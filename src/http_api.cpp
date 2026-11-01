/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HTTP management API over libmicrohttpd (websrv model). PS5 build only. */
#include "http_api.hpp"

#include <microhttpd.h>

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <string>

#include "../core/backend.hpp"
#include "../core/instance.hpp"
#include "../core/json.hpp"
#include "../core/fs.hpp"

namespace {

ps5craft::Backend *g_backend = nullptr;
struct MHD_Daemon *g_daemon = nullptr;

/* Per-connection POST body accumulator. */
struct conn_ctx {
    std::string body;
    bool body_done = false;
    bool too_large = false;
};

/* Uploads are buffered in memory; cap them so a bad request cannot eat the
   console's RAM. A Minecraft server JAR fits comfortably. */
constexpr std::size_t kMaxUploadBytes = 256u * 1024u * 1024u;

void add_cors(struct MHD_Response *resp) {
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN, "*");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_METHODS, "GET,POST,OPTIONS");
    MHD_add_response_header(resp, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_HEADERS, "Content-Type");
}

enum MHD_Result send_json(struct MHD_Connection *conn, unsigned int status,
                          const ps5craft::Json &j) {
    std::string body = j.dump();
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(body.size(), (void *)body.data(),
                                        MHD_RESPMEM_MUST_COPY);
    if (!resp) return MHD_NO;
    add_cors(resp);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json");
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

enum MHD_Result send_text(struct MHD_Connection *conn, unsigned int status,
                          const std::string &text) {
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(text.size(), (void *)text.data(),
                                        MHD_RESPMEM_MUST_COPY);
    if (!resp) return MHD_NO;
    add_cors(resp);
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/plain");
    enum MHD_Result ret = MHD_queue_response(conn, status, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* Query parameter value. libmicrohttpd has already URL-decoded it. */
std::string query_param(struct MHD_Connection *conn, const char *key) {
    const char *value =
        MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, key);
    return value ? std::string(value) : std::string();
}

/* Split path "/a/b/c" into components ["a","b","c"]. */
std::vector<std::string> path_parts(const std::string &uri) {
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i < uri.size()) {
        while (i < uri.size() && uri[i] == '/') ++i;
        if (i >= uri.size()) break;
        std::size_t j = uri.find('/', i);
        if (j == std::string::npos) j = uri.size();
        parts.push_back(uri.substr(i, j - i));
        i = j;
    }
    return parts;
}

/* Return the POST body accumulated in con_cls (empty for GET). */
std::string post_body(void **con_cls) {
    conn_ctx *ctx = static_cast<conn_ctx *>(*con_cls);
    if (!ctx) {
        ctx = new conn_ctx();
        *con_cls = ctx;
    }
    return ctx->body;
}

/* Called once the full body has been received. */
void finish_body(void **con_cls) {
    conn_ctx *ctx = static_cast<conn_ctx *>(*con_cls);
    if (ctx) ctx->body_done = true;
}

/* Release the per-connection context when the request finishes. */
void request_completed(void *, struct MHD_Connection *, void **con_cls,
                       enum MHD_RequestTerminationCode) {
    conn_ctx *ctx = static_cast<conn_ctx *>(*con_cls);
    delete ctx;
    *con_cls = nullptr;
}

} // namespace

/* MHD request iterator: headers first, then body chunks, then a final call
   with upload_data_size==0 to signal the body is complete. */
extern "C" enum MHD_Result api_handler(void *cls, struct MHD_Connection *conn,
                                       const char *url, const char *method,
                                       const char *version,
                                       const char *upload_data,
                                       size_t *upload_data_size,
                                       void **con_cls) {
    (void)cls; (void)version;

    if (!g_backend) return MHD_NO;

    if (std::strcmp(method, "OPTIONS") == 0) {
        struct MHD_Response *resp = MHD_create_response_from_buffer(0, nullptr, MHD_RESPMEM_PERSISTENT);
        if (!resp) return MHD_NO;
        add_cors(resp);
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* Accumulate POST body in per-connection context. */
    if (std::strcmp(method, "POST") == 0) {
        conn_ctx *ctx = static_cast<conn_ctx *>(*con_cls);
        if (!ctx) {
            *con_cls = new conn_ctx();
            return MHD_YES; /* headers only so far */
        }
        if (upload_data && *upload_data_size > 0) {
            if (!ctx->too_large &&
                ctx->body.size() + *upload_data_size > kMaxUploadBytes) {
                ctx->too_large = true;
                ctx->body.clear();
                ctx->body.shrink_to_fit();
            }
            if (!ctx->too_large) ctx->body.append(upload_data, *upload_data_size);
            *upload_data_size = 0;
            return MHD_YES;
        }
        if (!ctx->body_done) {
            finish_body(con_cls);
            return MHD_YES;
        }
    }

    if (std::strcmp(method, "POST") == 0) {
        conn_ctx *ctx = static_cast<conn_ctx *>(*con_cls);
        if (ctx && ctx->too_large) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "upload too large; limit is 256 MiB");
            return send_json(conn, MHD_HTTP_CONTENT_TOO_LARGE, err);
        }
    }

    std::string path = url ? url : "/";
    auto parts = path_parts(path);

    /* Dashboard: serve web/index.html for "/" and "/index.html". */
    if (std::strcmp(method, "GET") == 0 &&
        (path == "/" || path == "/index.html")) {
        const char *root = std::getenv("PS5CRAFT_WEB_ROOT");
        std::string file = std::string(root ? root : "web") + "/index.html";
        std::string html;
        if (ps5craft::fs_read_file(file, html)) {
            struct MHD_Response *resp =
                MHD_create_response_from_buffer(html.size(), (void *)html.data(),
                                                MHD_RESPMEM_MUST_COPY);
            if (!resp) return MHD_NO;
            MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                                    "text/html; charset=utf-8");
            enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
            MHD_destroy_response(resp);
            return ret;
        }
        return send_text(conn, MHD_HTTP_NOT_FOUND,
                         "dashboard not found (set PS5CRAFT_WEB_ROOT)");
    }

    /* Create an instance: POST /api/instances with JSON body. */
    if (parts.size() == 2 && parts[0] == "api" && parts[1] == "instances" &&
        std::strcmp(method, "POST") == 0) {
        ps5craft::Json body;
        if (!ps5craft::Json::parse(post_body(con_cls), body) ||
            !body.is_object()) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "invalid JSON body");
            return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
        }
        ps5craft::InstanceConfig cfg;
        cfg.name = body["name"].as_string("New Server");
        cfg.software = body["software"].as_string("vanilla");
        cfg.version = body["version"].as_string("26.3");
        cfg.jar_path = body["jar_path"].as_string("server.jar");
        if (body["memory_max_mib"].as_int(0) > 0)
            cfg.memory.max_mib = body["memory_max_mib"].as_int();
        if (body["memory_initial_mib"].as_int(0) > 0)
            cfg.memory.initial_mib = body["memory_initial_mib"].as_int();
        std::string id = g_backend->create_instance(cfg);
        if (id.empty()) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error",
                    "unsupported software; tested on PS5: vanilla, fabric, paper");
            return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
        }
        ps5craft::Json out = ps5craft::Json::object();
        out.set("ok", true);
        out.set("id", id);
        return send_json(conn, MHD_HTTP_OK, out);
    }

    /* /api/instances */
    if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "instances") {
        if (std::strcmp(method, "GET") == 0 && parts.size() == 2) {
            return send_json(conn, MHD_HTTP_OK, g_backend->status_snapshot());
        }
        if (std::strcmp(method, "GET") == 0 && parts.size() == 3) {
            const ps5craft::Instance *inst = g_backend->instance(parts[2]);
            if (!inst) {
                ps5craft::Json err = ps5craft::Json::object();
                err.set("error", "not found");
                return send_json(conn, MHD_HTTP_NOT_FOUND, err);
            }
            return send_json(conn, MHD_HTTP_OK, inst->snapshot());
        }
        if (std::strcmp(method, "DELETE") == 0 && parts.size() == 3) {
            bool ok = g_backend->remove_instance(parts[2]);
            ps5craft::Json out = ps5craft::Json::object();
            out.set("ok", ok);
            return send_json(conn, ok ? MHD_HTTP_OK : MHD_HTTP_CONFLICT, out);
        }
        if (std::strcmp(method, "GET") == 0 && parts.size() == 4 &&
            parts[3] == "logs") {
            if (!g_backend->instance(parts[2])) {
                ps5craft::Json err = ps5craft::Json::object();
                err.set("error", "not found");
                return send_json(conn, MHD_HTTP_NOT_FOUND, err);
            }
            ps5craft::Json root = ps5craft::Json::object();
            root.set("id", parts[2]);
            root.set("logs", g_backend->instance_logs(parts[2]));
            return send_json(conn, MHD_HTTP_OK, root);
        }
        if (std::strcmp(method, "POST") == 0 && parts.size() == 4) {
            ps5craft::Instance *inst = g_backend->instance(parts[2]);
            if (!inst) {
                ps5craft::Json err = ps5craft::Json::object();
                err.set("error", "not found");
                return send_json(conn, MHD_HTTP_NOT_FOUND, err);
            }
            std::string action = parts[3];
            if (action == "start") {
                bool ok = g_backend->start_instance(parts[2]);
                return send_json(conn, ok ? MHD_HTTP_OK : MHD_HTTP_CONFLICT,
                                 inst->snapshot());
            }
            if (action == "stop") {
                bool ok = g_backend->stop_instance(parts[2]);
                return send_json(conn, ok ? MHD_HTTP_OK : MHD_HTTP_CONFLICT,
                                 inst->snapshot());
            }
            if (action == "restart") {
                bool ok = g_backend->restart_instance(parts[2]);
                return send_json(conn, ok ? MHD_HTTP_OK : MHD_HTTP_CONFLICT,
                                 inst->snapshot());
            }
            if (action == "eula") {
                bool ok = g_backend->accept_eula(parts[2]);
                ps5craft::Json out = ps5craft::Json::object();
                out.set("ok", ok);
                return send_json(conn, ok ? MHD_HTTP_OK : MHD_HTTP_INTERNAL_SERVER_ERROR,
                                 out);
            }
            if (action == "command") {
                std::string line = post_body(con_cls);
                if (line.empty()) line = query_param(conn, "cmd");
                if (line.empty()) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "empty command");
                    return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
                }
                bool ok = g_backend->instance_command(parts[2], line);
                ps5craft::Json root = ps5craft::Json::object();
                root.set("ok", ok);
                return send_json(conn, ok ? MHD_HTTP_OK : MHD_HTTP_CONFLICT,
                                 root);
            }
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "unknown action");
            return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
        }
    }

    /* /api/fs/:...  (sandboxed file operations) */
    if (parts.size() >= 2 && parts[0] == "api" && parts[1] == "fs") {
        std::string relative;
        for (std::size_t i = 2; i < parts.size(); ++i) {
            if (i > 2) relative += "/";
            relative += parts[i];
        }

        if (std::strcmp(method, "GET") == 0 && parts.size() >= 3) {
            ps5craft::FileEntry entry;
            if (!g_backend->sandbox().entry_stat(relative, entry)) {
                ps5craft::Json err = ps5craft::Json::object();
                err.set("error", "not found");
                return send_json(conn, MHD_HTTP_NOT_FOUND, err);
            }
            if (entry.directory) {
                std::vector<ps5craft::FileEntry> list;
                if (!g_backend->sandbox().list(relative, list)) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "cannot list");
                    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
                }
                ps5craft::Json arr = ps5craft::Json::array();
                for (const auto &e : list) {
                    ps5craft::Json item = ps5craft::Json::object();
                    item.set("name", e.name);
                    item.set("path", e.path);
                    item.set("directory", e.directory);
                    item.set("size", static_cast<double>(e.size));
                    item.set("modified", static_cast<double>(e.modified));
                    arr.push(item);
                }
                ps5craft::Json root = ps5craft::Json::object();
                root.set("path", relative);
                root.set("entries", arr);
                return send_json(conn, MHD_HTTP_OK, root);
            } else {
                std::string content;
                if (!g_backend->sandbox().read(relative, content, 1u << 20)) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "cannot read");
                    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
                }
                return send_text(conn, MHD_HTTP_OK, content);
            }
        }

        if (std::strcmp(method, "POST") == 0 && parts.size() >= 3) {
            std::string action = query_param(conn, "action");
            if (action == "write" || action == "upload") {
                std::string data = post_body(con_cls);
                if (!g_backend->fs_write(relative, data)) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "cannot write");
                    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
                }
                ps5craft::Json ok = ps5craft::Json::object();
                ok.set("ok", true);
                return send_json(conn, MHD_HTTP_OK, ok);
            }
            if (action == "mkdir") {
                if (!g_backend->fs_mkdir(relative)) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "cannot mkdir");
                    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
                }
                ps5craft::Json ok = ps5craft::Json::object();
                ok.set("ok", true);
                return send_json(conn, MHD_HTTP_OK, ok);
            }
            if (action == "rename") {
                std::string to = query_param(conn, "to");
                if (to.empty()) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "missing target");
                    return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
                }
                if (!g_backend->fs_rename(relative, to)) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "cannot rename");
                    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
                }
                ps5craft::Json ok = ps5craft::Json::object();
                ok.set("ok", true);
                return send_json(conn, MHD_HTTP_OK, ok);
            }
            if (action == "delete") {
                if (!g_backend->fs_remove(relative)) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "cannot delete");
                    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
                }
                ps5craft::Json ok = ps5craft::Json::object();
                ok.set("ok", true);
                return send_json(conn, MHD_HTTP_OK, ok);
            }
            if (action == "copy") {
                std::string to = query_param(conn, "to");
                if (to.empty()) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "missing target");
                    return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
                }
                if (!g_backend->fs_copy(relative, to)) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "cannot copy");
                    return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
                }
                ps5craft::Json ok = ps5craft::Json::object();
                ok.set("ok", true);
                return send_json(conn, MHD_HTTP_OK, ok);
            }
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "unknown action");
            return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
        }
    }

    /* /api/system: read-only host capacity. */
    if (parts.size() == 2 && parts[0] == "api" && parts[1] == "system" &&
        std::strcmp(method, "GET") == 0) {
        ps5craft::DiskSpace disk;
        ps5craft::Json out = ps5craft::Json::object();
        if (ps5craft::fs_disk_space(g_backend->data_root(), disk)) {
            ps5craft::Json d = ps5craft::Json::object();
            d.set("total", static_cast<double>(disk.total));
            d.set("free", static_cast<double>(disk.free));
            d.set("used", static_cast<double>(disk.total - disk.free));
            out.set("disk", d);
        }
        ps5craft::Json sizes = ps5craft::Json::object();
        for (const auto &snap : g_backend->list_snapshots()) {
            const std::string id = snap["id"].as_string("");
            if (!id.empty())
                sizes.set(id, static_cast<double>(g_backend->instance_disk_bytes(id)));
        }
        out.set("instances", sizes);
        return send_json(conn, MHD_HTTP_OK, out);
    }

    /* /api/status */
    if (parts.size() == 2 && parts[0] == "api" && parts[1] == "status" &&
        std::strcmp(method, "GET") == 0) {
        return send_json(conn, MHD_HTTP_OK, g_backend->status_snapshot());
    }

    /* /api/backup/<id>  (list) */
    if (parts.size() == 3 && parts[0] == "api" && parts[1] == "backup" &&
        std::strcmp(method, "GET") == 0) {
        auto backups = g_backend->backup_list(parts[2]);
        ps5craft::Json arr = ps5craft::Json::array();
        for (const auto &b : backups) {
            ps5craft::Json item = ps5craft::Json::object();
            item.set("id", b.id);
            item.set("file_name", b.file_name);
            item.set("path", b.path);
            item.set("size", static_cast<double>(b.size));
            item.set("modified", static_cast<double>(b.modified));
            item.set("instance_id", b.instance_id);
            item.set("full", b.full);
            arr.push(item);
        }
        ps5craft::Json root = ps5craft::Json::object();
        root.set("instance_id", parts[2]);
        root.set("backups", arr);
        return send_json(conn, MHD_HTTP_OK, root);
    }

    /* /api/backup/<id>/create  (POST ?full=1) */
    if (parts.size() == 4 && parts[0] == "api" && parts[1] == "backup" &&
        parts[3] == "create" && std::strcmp(method, "POST") == 0) {
        bool full = query_param(conn, "full") == "1";
        std::string id = g_backend->backup_create(parts[2], full);
        if (id.empty()) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "cannot create backup");
            return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
        }
        ps5craft::Json ok = ps5craft::Json::object();
        ok.set("ok", true);
        ok.set("id", id);
        return send_json(conn, MHD_HTTP_OK, ok);
    }

    /* /api/backup/<id>/<backup>/restore  (POST) */
    if (parts.size() == 5 && parts[0] == "api" && parts[1] == "backup" &&
        parts[4] == "restore" && std::strcmp(method, "POST") == 0) {
        if (!g_backend->backup_restore(parts[2], parts[3])) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "cannot restore");
            return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
        }
        ps5craft::Json ok = ps5craft::Json::object();
        ok.set("ok", true);
        return send_json(conn, MHD_HTTP_OK, ok);
    }

    /* /api/backup/<id>/<backup>/delete  (POST) */
    if (parts.size() == 5 && parts[0] == "api" && parts[1] == "backup" &&
        parts[4] == "delete" && std::strcmp(method, "POST") == 0) {
        if (!g_backend->backup_remove(parts[2], parts[3])) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "cannot delete");
            return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
        }
        ps5craft::Json ok = ps5craft::Json::object();
        ok.set("ok", true);
        return send_json(conn, MHD_HTTP_OK, ok);
    }

    /* /api/backup/<id>/<backup>/download  (GET) */
    if (parts.size() == 5 && parts[0] == "api" && parts[1] == "backup" &&
        parts[4] == "download" && std::strcmp(method, "GET") == 0) {
        std::string zip_path = "backups/" + parts[2] + "/" + parts[3] + ".zip";
        std::string abs_zip;
        if (!g_backend->sandbox().resolve(zip_path, abs_zip)) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "not found");
            return send_json(conn, MHD_HTTP_NOT_FOUND, err);
        }
        std::string content;
        if (!g_backend->sandbox().read(zip_path, content, 1ull << 30)) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "cannot read");
            return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
        }
        struct MHD_Response *resp =
            MHD_create_response_from_buffer(content.size(),
                                            (void *)content.data(),
                                            MHD_RESPMEM_MUST_COPY);
        if (!resp) return MHD_NO;
        add_cors(resp);
        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                                "application/zip");
        char disp[256];
        std::snprintf(disp, sizeof disp, "attachment; filename=\"%s.zip\"",
                      parts[3].c_str());
        MHD_add_response_header(resp, "Content-Disposition", disp);
        enum MHD_Result ret = MHD_queue_response(conn, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    ps5craft::Json err = ps5craft::Json::object();
    err.set("error", "not found");
    return send_json(conn, MHD_HTTP_NOT_FOUND, err);
}

int http_api_start(int port, void *backend) {
    g_backend = static_cast<ps5craft::Backend *>(backend);
    g_daemon = MHD_start_daemon(MHD_USE_INTERNAL_POLLING_THREAD, port,
                                nullptr, nullptr, &api_handler, nullptr,
                                MHD_OPTION_NOTIFY_COMPLETED, &request_completed,
                                nullptr,
                                MHD_OPTION_END);
    if (!g_daemon) return -1;
    return 0;
}

void http_api_stop(void) {
    if (g_daemon) {
        MHD_stop_daemon(g_daemon);
        g_daemon = nullptr;
    }
    g_backend = nullptr;
}
