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
};

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

/* Parse a single query parameter value from "key=val&...". */
std::string query_param(const std::string &query, const std::string &key) {
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t sep = query.find('&', pos);
        if (sep == std::string::npos) sep = query.size();
        std::size_t eq = query.find('=', pos);
        if (eq != std::string::npos && eq < sep) {
            std::string k = query.substr(pos, eq - pos);
            if (k == key) return query.substr(eq + 1, sep - eq - 1);
        }
        pos = sep + 1;
    }
    return std::string();
}

std::string url_decode(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '%' && i + 2 < in.size()) {
            char hi = in[i + 1], lo = in[i + 2];
            auto hex = [](char h) -> int {
                if (h >= '0' && h <= '9') return h - '0';
                if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                return -1;
            };
            int a = hex(hi), b = hex(lo);
            if (a >= 0 && b >= 0) { out += static_cast<char>((a << 4) | b); i += 2; continue; }
        }
        out += c;
    }
    return out;
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
            ctx = new conn_ctx();
            *con_cls = ctx;
        }
        if (upload_data && *upload_data_size > 0) {
            ctx->body.append(upload_data, *upload_data_size);
            *upload_data_size = 0;
            return MHD_YES;
        }
    }

    std::string path = url ? url : "/";
    std::size_t q = path.find('?');
    std::string query;
    if (q != std::string::npos) { query = path.substr(q + 1); path = path.substr(0, q); }
    auto parts = path_parts(path);

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
        if (std::strcmp(method, "POST") == 0 && parts.size() == 4) {
            ps5craft::Instance *inst = g_backend->instance(parts[2]);
            if (!inst) {
                ps5craft::Json err = ps5craft::Json::object();
                err.set("error", "not found");
                return send_json(conn, MHD_HTTP_NOT_FOUND, err);
            }
            std::string action = parts[3];
            if (action == "start") {
                g_backend->start_instance(parts[2]);
                return send_json(conn, MHD_HTTP_OK, inst->snapshot());
            }
            if (action == "stop") {
                g_backend->stop_instance(parts[2]);
                return send_json(conn, MHD_HTTP_OK, inst->snapshot());
            }
            if (action == "restart") {
                g_backend->restart_instance(parts[2]);
                return send_json(conn, MHD_HTTP_OK, inst->snapshot());
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
            std::string action = query_param(query, "action");
            if (action == "write") {
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
                std::string to = url_decode(query_param(query, "to"));
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
                std::string to = url_decode(query_param(query, "to"));
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

    /* /api/status */
    if (parts.size() == 2 && parts[0] == "api" && parts[1] == "status" &&
        std::strcmp(method, "GET") == 0) {
        return send_json(conn, MHD_HTTP_OK, g_backend->status_snapshot());
    }

    /* /api/backup/<id>  (list) */
    if (parts.size() == 3 && parts[0] == "api" && parts[1] == "backup" &&
        std::strcmp(method, "GET") == 0) {
        auto backups = g_backend->backup_list(parts[2]);
        Json arr = Json::array();
        for (const auto &b : backups) {
            Json item = Json::object();
            item.set("id", b.id);
            item.set("file_name", b.file_name);
            item.set("path", b.path);
            item.set("size", static_cast<double>(b.size));
            item.set("modified", static_cast<double>(b.modified));
            item.set("instance_id", b.instance_id);
            item.set("full", b.full);
            arr.push(item);
        }
        Json root = Json::object();
        root.set("instance_id", parts[2]);
        root.set("backups", arr);
        return send_json(conn, MHD_HTTP_OK, root);
    }

    /* /api/backup/<id>/create  (POST ?full=1) */
    if (parts.size() == 4 && parts[0] == "api" && parts[1] == "backup" &&
        parts[3] == "create" && std::strcmp(method, "POST") == 0) {
        bool full = query_param(query, "full") == "1";
        std::string id = g_backend->backup_create(parts[2], full);
        if (id.empty()) {
            Json err = Json::object();
            err.set("error", "cannot create backup");
            return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
        }
        Json ok = Json::object();
        ok.set("ok", true);
        ok.set("id", id);
        return send_json(conn, MHD_HTTP_OK, ok);
    }

    /* /api/backup/<id>/<backup>/restore  (POST) */
    if (parts.size() == 5 && parts[0] == "api" && parts[1] == "backup" &&
        parts[4] == "restore" && std::strcmp(method, "POST") == 0) {
        if (!g_backend->backup_restore(parts[2], parts[3])) {
            Json err = Json::object();
            err.set("error", "cannot restore");
            return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
        }
        Json ok = Json::object();
        ok.set("ok", true);
        return send_json(conn, MHD_HTTP_OK, ok);
    }

    /* /api/backup/<id>/<backup>/delete  (POST) */
    if (parts.size() == 5 && parts[0] == "api" && parts[1] == "backup" &&
        parts[4] == "delete" && std::strcmp(method, "POST") == 0) {
        if (!g_backend->backup_remove(parts[2], parts[3])) {
            Json err = Json::object();
            err.set("error", "cannot delete");
            return send_json(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err);
        }
        Json ok = Json::object();
        ok.set("ok", true);
        return send_json(conn, MHD_HTTP_OK, ok);
    }

    /* /api/backup/<id>/<backup>/download  (GET) */
    if (parts.size() == 5 && parts[0] == "api" && parts[1] == "backup" &&
        parts[4] == "download" && std::strcmp(method, "GET") == 0) {
        std::string zip_path = "backups/" + parts[2] + "/" + parts[3] + ".zip";
        std::string abs_zip;
        if (!g_backend->sandbox().resolve(zip_path, abs_zip)) {
            Json err = Json::object();
            err.set("error", "not found");
            return send_json(conn, MHD_HTTP_NOT_FOUND, err);
        }
        std::string content;
        if (!g_backend->sandbox().read(zip_path, content, 1ull << 30)) {
            Json err = Json::object();
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
                      parts[3]);
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
    g_daemon = MHD_start_daemon(MHD_USE_SELECT_INTERNALLY | MHD_USE_DEBUG,
                                port, nullptr, nullptr, &api_handler, nullptr,
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
