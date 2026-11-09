/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HTTP management API over libmicrohttpd (websrv model). PS5 build only. */
#include "http_api.hpp"

#include <microhttpd.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "../core/backend.hpp"
#include "../core/catalog.hpp"
#include "../core/instance.hpp"
#include "../core/json.hpp"
#include "../core/fs.hpp"
#include "../core/http_client.hpp"
#include "../core/monitor.hpp"
#include "../core/storage.hpp"
#include "../core/trace.hpp"

namespace {

ps5craft::Backend *g_backend = nullptr;
struct MHD_Daemon *g_daemon = nullptr;

/* The embedded JVM runtime keeps the SDK resolver object under this name. */
extern "C" int ps5_sdk_getaddrinfo(const char *, const char *,
                                   const struct addrinfo *,
                                   struct addrinfo **)
    __attribute__((weak));

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

    /* Temporary: proves whether a request reaches the handler while the
       console test is running. */
    ps5craft::trace("[http] req %s %s\n", method, url);

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

    /* Dashboard: serve index.html for "/" and "/index.html". The title has no
       fixed working directory, so try the app folder first, then the
       development layout. */
    if (std::strcmp(method, "GET") == 0 &&
        (path == "/" || path == "/index.html")) {
        std::string html;
        bool found = false;
        if (const char *root = std::getenv("PS5CRAFT_WEB_ROOT")) {
            found = ps5craft::fs_read_file(std::string(root) + "/index.html", html);
        }
        for (const char *dir : {"/app0/assets/web", "web"}) {
            if (found) break;
            found = ps5craft::fs_read_file(std::string(dir) + "/index.html", html);
        }
        /* A build run as a payload has no /app0, so the data root keeps a
           copy of the page as well. */
        if (!found && g_backend)
            found = ps5craft::fs_read_file(
                g_backend->data_root() + "/web/index.html", html);
        if (found) {
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
            parts[3] == "install") {
            if (!g_backend->instance(parts[2])) {
                ps5craft::Json err = ps5craft::Json::object();
                err.set("error", "not found");
                return send_json(conn, MHD_HTTP_NOT_FOUND, err);
            }
            return send_json(conn, MHD_HTTP_OK,
                             g_backend->install_status(parts[2]));
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
        if (std::strcmp(method, "GET") == 0 && parts.size() == 5 &&
            parts[3] == "mods" && parts[4] == "status") {
            if (!g_backend->instance(parts[2])) {
                ps5craft::Json err = ps5craft::Json::object();
                err.set("error", "not found");
                return send_json(conn, MHD_HTTP_NOT_FOUND, err);
            }
            return send_json(conn, MHD_HTTP_OK,
                             g_backend->mod_install_status(parts[2]));
        }
        if (std::strcmp(method, "GET") == 0 && parts.size() == 4 &&
            parts[3] == "mods") {
            if (!g_backend->instance(parts[2])) {
                ps5craft::Json err = ps5craft::Json::object();
                err.set("error", "not found");
                return send_json(conn, MHD_HTTP_NOT_FOUND, err);
            }
            return send_json(conn, MHD_HTTP_OK, g_backend->mods_list(parts[2]));
        }
        if (std::strcmp(method, "DELETE") == 0 && parts.size() == 4 &&
            parts[3] == "mods") {
            const std::string name = query_param(conn, "name");
            const bool ok = g_backend->mod_remove(parts[2], name);
            ps5craft::Json out = ps5craft::Json::object();
            out.set("ok", ok);
            return send_json(conn, ok ? MHD_HTTP_OK : MHD_HTTP_CONFLICT, out);
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
            if (action == "install") {
                ps5craft::Json body;
                if (!ps5craft::Json::parse(post_body(con_cls), body) ||
                    !body.is_object()) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "expected a JSON body");
                    return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
                }
                const std::string software = body["software"].as_string();
                const std::string version = body["version"].as_string("latest");
                if (software.empty()) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "software is required");
                    return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
                }
                const std::string error =
                    g_backend->install_start(parts[2], software, version);
                if (!error.empty()) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", error);
                    return send_json(conn, MHD_HTTP_CONFLICT, err);
                }
                return send_json(conn, MHD_HTTP_ACCEPTED,
                                 g_backend->install_status(parts[2]));
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
            if (action == "mods") {
                ps5craft::Json body;
                if (!ps5craft::Json::parse(post_body(con_cls), body) ||
                    !body.is_object()) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "expected a JSON body");
                    return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
                }
                const std::string project = body["project"].as_string();
                if (project.empty()) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", "project is required");
                    return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
                }
                const ps5craft::ModSource source =
                    ps5craft::mod_source_from_string(
                        body["source"].as_string("modrinth"));
                const std::string version = body["version"].as_string();
                const std::string error = g_backend->mod_install_start(
                    parts[2], source, project, version);
                if (!error.empty()) {
                    ps5craft::Json err = ps5craft::Json::object();
                    err.set("error", error);
                    return send_json(conn, MHD_HTTP_CONFLICT, err);
                }
                return send_json(conn, MHD_HTTP_ACCEPTED,
                                 g_backend->mod_install_status(parts[2]));
            }
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "unknown action");
            return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
        }
    }

    /* /api/mods: search the public content stores for this server software. */
    if (std::strcmp(method, "GET") == 0 && parts.size() == 2 &&
        parts[0] == "api" && parts[1] == "mods") {
        const std::string software = query_param(conn, "software");
        if (software.empty()) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "software is required");
            return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
        }
        const std::string game_version = query_param(conn, "version");
        const std::string query = query_param(conn, "query");
        int offset = std::atoi(query_param(conn, "offset").c_str());
        if (offset < 0) offset = 0;
        const std::string source_name = query_param(conn, "source");
        const ps5craft::ModSource source =
            source_name.empty() ? ps5craft::mod_default_source(software)
                                : ps5craft::mod_source_from_string(source_name);
        std::vector<ps5craft::ModProject> results;
        std::string error;
        if (!ps5craft::mod_search(source, software, game_version, query, offset,
                                  results, error)) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", error);
            return send_json(conn, MHD_HTTP_BAD_GATEWAY, err);
        }
        ps5craft::Json arr = ps5craft::Json::array();
        for (const auto &project : results) {
            ps5craft::Json item = ps5craft::Json::object();
            item.set("source", ps5craft::mod_source_name(project.source));
            item.set("id", project.id);
            item.set("slug", project.slug);
            item.set("title", project.title);
            item.set("description", project.description);
            item.set("icon_url", project.icon_url);
            item.set("project_type", project.project_type);
            item.set("downloads", static_cast<double>(project.downloads));
            ps5craft::Json loaders = ps5craft::Json::array();
            for (const auto &loader : project.loaders) loaders.push(loader);
            item.set("loaders", loaders);
            ps5craft::Json versions = ps5craft::Json::array();
            for (const auto &version : project.game_versions) versions.push(version);
            item.set("game_versions", versions);
            arr.push(item);
        }
        ps5craft::Json out = ps5craft::Json::object();
        out.set("source", ps5craft::mod_source_name(source));
        out.set("software", software);
        out.set("offset", offset);
        out.set("results", arr);
        return send_json(conn, MHD_HTTP_OK, out);
    }

    /* /api/mods/versions: versions of one project that fit the server. */
    if (std::strcmp(method, "GET") == 0 && parts.size() == 3 &&
        parts[0] == "api" && parts[1] == "mods" && parts[2] == "versions") {
        const std::string project = query_param(conn, "project");
        const std::string software = query_param(conn, "software");
        if (project.empty() || software.empty()) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "project and software are required");
            return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
        }
        const std::string game_version = query_param(conn, "version");
        const std::string source_name = query_param(conn, "source");
        const ps5craft::ModSource source =
            source_name.empty() ? ps5craft::mod_default_source(software)
                                : ps5craft::mod_source_from_string(source_name);
        std::vector<ps5craft::ModFile> versions;
        std::string error;
        if (!ps5craft::mod_versions(source, project, software, game_version,
                                    versions, error)) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", error);
            return send_json(conn, MHD_HTTP_BAD_GATEWAY, err);
        }
        ps5craft::Json arr = ps5craft::Json::array();
        for (const auto &file : versions) {
            ps5craft::Json item = ps5craft::Json::object();
            item.set("source", ps5craft::mod_source_name(file.source));
            item.set("id", file.version_id);
            item.set("name", file.version_name);
            item.set("channel", file.channel);
            item.set("file_name", file.file_name);
            item.set("size", static_cast<double>(file.size));
            item.set("sha1", file.sha1);
            item.set("sha256", file.sha256);
            ps5craft::Json games = ps5craft::Json::array();
            for (const auto &game : file.game_versions) games.push(game);
            item.set("game_versions", games);
            ps5craft::Json loaders = ps5craft::Json::array();
            for (const auto &loader : file.loaders) loaders.push(loader);
            item.set("loaders", loaders);
            ps5craft::Json deps = ps5craft::Json::array();
            for (const auto &dep : file.dependencies) {
                ps5craft::Json d = ps5craft::Json::object();
                d.set("project_id", dep.project_id);
                d.set("version_id", dep.version_id);
                d.set("name", dep.name);
                d.set("kind", dep.kind);
                deps.push(d);
            }
            item.set("dependencies", deps);
            arr.push(item);
        }
        ps5craft::Json out = ps5craft::Json::object();
        out.set("project", project);
        out.set("source", ps5craft::mod_source_name(source));
        out.set("versions", arr);
        return send_json(conn, MHD_HTTP_OK, out);
    }

    /* /api/catalog  (server software catalog) */
    if (std::strcmp(method, "GET") == 0 && parts.size() >= 2 &&
        parts[0] == "api" && parts[1] == "catalog") {
        if (parts.size() == 2) {
            ps5craft::Json arr = ps5craft::Json::array();
            for (const auto &s : ps5craft::catalog_software()) {
                ps5craft::Json item = ps5craft::Json::object();
                item.set("id", s.id);
                item.set("name", s.name);
                item.set("description", s.description);
                item.set("supported", s.supported);
                item.set("note", s.note);
                arr.push(item);
            }
            ps5craft::Json root = ps5craft::Json::object();
            root.set("software", arr);
            return send_json(conn, MHD_HTTP_OK, root);
        }
        const std::string software = parts[2];
        if (parts.size() == 4 && parts[3] == "versions") {
            std::vector<ps5craft::CatalogVersion> versions;
            std::string error;
            if (!ps5craft::catalog_versions(software, versions, error)) {
                ps5craft::trace("[catalog] %s versions failed: %s\n",
                                software.c_str(), error.c_str());
                ps5craft::Json err = ps5craft::Json::object();
                err.set("error", error);
                return send_json(conn, MHD_HTTP_BAD_GATEWAY, err);
            }
            ps5craft::Json arr = ps5craft::Json::array();
            for (const auto &v : versions) {
                ps5craft::Json item = ps5craft::Json::object();
                item.set("id", v.id);
                item.set("channel", v.channel);
                item.set("released", v.released);
                item.set("installable", v.installable);
                item.set("note", v.note);
                arr.push(item);
            }
            ps5craft::Json root = ps5craft::Json::object();
            root.set("software", software);
            root.set("versions", arr);
            return send_json(conn, MHD_HTTP_OK, root);
        }
        if (parts.size() == 4) {
            ps5craft::SoftwareArtifact artifact;
            std::string error;
            if (!ps5craft::catalog_resolve(software, parts[3], artifact,
                                           error)) {
                ps5craft::Json err = ps5craft::Json::object();
                err.set("error", error);
                return send_json(conn, MHD_HTTP_BAD_GATEWAY, err);
            }
            ps5craft::Json root = ps5craft::Json::object();
            root.set("software", artifact.software);
            root.set("version", artifact.version);
            root.set("url", artifact.url);
            root.set("file_name", artifact.file_name);
            root.set("sha1", artifact.sha1);
            root.set("sha256", artifact.sha256);
            root.set("size", static_cast<std::int64_t>(artifact.size));
            root.set("java_major", artifact.java_major);
            root.set("note", artifact.note);
            return send_json(conn, MHD_HTTP_OK, root);
        }
        ps5craft::Json err = ps5craft::Json::object();
        err.set("error", "unknown catalog route");
        return send_json(conn, MHD_HTTP_NOT_FOUND, err);
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
        out.set("data_root", g_backend->data_root());
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

        /* Where the title may write. /data is host-backed only when
           ShadowMountPlus mounts it into this title. */
        ps5craft::Json volumes = ps5craft::Json::array();
        for (const auto &volume : ps5craft::storage_volumes()) {
            ps5craft::Json item = ps5craft::Json::object();
            item.set("path", volume.path);
            item.set("label", volume.label);
            item.set("present", volume.present);
            item.set("writable", volume.writable);
            if (volume.total > 0) {
                item.set("total", static_cast<double>(volume.total));
                item.set("free", static_cast<double>(volume.free));
            }
            volumes.push(item);
        }
        out.set("storage", volumes);
        out.set("shadowmount", ps5craft::shadowmount_present());

        /* Read-only measured monitoring values (process/system). */
        ps5craft::Json mon = g_backend->monitor_snapshot();
        if (mon.is_object()) out.set("monitor", mon);

        return send_json(conn, MHD_HTTP_OK, out);
    }

    /* /api/system/root: remember where the title keeps its data. Takes effect
       on the next title launch. */
    if (parts.size() == 4 && parts[0] == "api" && parts[1] == "system" &&
        parts[2] == "root" && std::strcmp(method, "POST") == 0) {
        ps5craft::Json body;
        if (!ps5craft::Json::parse(post_body(con_cls), body) || !body.is_object()) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "expected a JSON body");
            return send_json(conn, MHD_HTTP_BAD_REQUEST, err);
        }
        const std::string root =
            ps5craft::storage_root_for(body["volume"].as_string());
        if (root.empty()) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "volume is not mounted");
            return send_json(conn, MHD_HTTP_CONFLICT, err);
        }
        if (!ps5craft::storage_save_root(root)) {
            ps5craft::Json err = ps5craft::Json::object();
            err.set("error", "cannot write to that volume");
            return send_json(conn, MHD_HTTP_CONFLICT, err);
        }
        ps5craft::Json out = ps5craft::Json::object();
        out.set("root", root);
        out.set("restart", true);
        return send_json(conn, MHD_HTTP_OK, out);
    }

    /* /api/diag/net/<step>: temporary network isolation probes. */
    if (parts.size() == 4 && parts[0] == "api" && parts[1] == "diag" &&
        parts[2] == "net" && std::strcmp(method, "GET") == 0) {
        const std::string step = parts[3];
        ps5craft::Json out = ps5craft::Json::object();
        out.set("step", step);
        if (step == "dns" || step == "sdk") {
            struct addrinfo hints;
            std::memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            struct addrinfo *res = nullptr;
            int rc = EAI_FAIL;
            if (step == "dns") {
                rc = ::getaddrinfo("piston-meta.mojang.com", "443", &hints, &res);
            } else {
                if (ps5_sdk_getaddrinfo)
                    rc = ps5_sdk_getaddrinfo("piston-meta.mojang.com", "443",
                                             &hints, &res);
                else
                    out.set("error", "ps5_sdk_getaddrinfo missing");
            }
            out.set("result", static_cast<double>(rc));
            ps5craft::Json addrs = ps5craft::Json::array();
            if (rc == 0) {
                for (struct addrinfo *p = res; p; p = p->ai_next) {
                    char text[INET_ADDRSTRLEN] = {0};
                    auto *sa = reinterpret_cast<struct sockaddr_in *>(p->ai_addr);
                    if (inet_ntop(AF_INET, &sa->sin_addr, text, sizeof(text)))
                        addrs.push(ps5craft::Json(text));
                }
            }
            if (res) ::freeaddrinfo(res);
            out.set("addresses", addrs);
            return send_json(conn, MHD_HTTP_OK, out);
        }
        if (step == "tcp") {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            int rc = -1;
            int err = 0;
            if (fd >= 0) {
                struct sockaddr_in sa;
                std::memset(&sa, 0, sizeof(sa));
                sa.sin_family = AF_INET;
                sa.sin_port = htons(443);
                inet_pton(AF_INET, "1.1.1.1", &sa.sin_addr);
                rc = ::connect(fd, reinterpret_cast<struct sockaddr *>(&sa),
                               sizeof(sa));
                err = errno;
                ::close(fd);
            }
            out.set("result", static_cast<double>(rc));
            out.set("errno", static_cast<double>(err));
            return send_json(conn, MHD_HTTP_OK, out);
        }
        if (step == "ip" || step == "host") {
            const bool by_ip = (step == "ip");
            std::string url;
            if (by_ip) {
                const auto ips = ps5craft::local_ips();
                if (ips.empty()) {
                    out.set("ok", false);
                    out.set("error", "no local IPv4 address");
                    return send_json(conn, MHD_HTTP_OK, out);
                }
                url = "http://" + ips.front() + ":8080/api/status";
            } else {
                url = "https://piston-meta.mojang.com/mc/game/"
                      "version_manifest_v2.json";
            }
            std::string body, error;
            ps5craft::HttpLimits limits;
            limits.timeout_s = by_ip ? 8 : 15;
            limits.connect_timeout_s = 5;
            limits.max_bytes = 4ull * 1024 * 1024;
            const bool ok = ps5craft::http_get(url, body, error, limits);
            out.set("ok", ok);
            out.set("bytes", static_cast<double>(body.size()));
            if (!ok) out.set("error", error);
            return send_json(conn, MHD_HTTP_OK, out);
        }
        out.set("error", "unknown step");
        return send_json(conn, MHD_HTTP_BAD_REQUEST, out);
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

namespace {

/* Temporary watchdog: a line every few seconds shows the process is still
   scheduled even when a request gets no answer. */
void http_watchdog() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        ps5craft::trace("[http] tick\n");
    }
}

} // namespace

int http_api_start(int port, void *backend) {
    g_backend = static_cast<ps5craft::Backend *>(backend);
    /* One polling thread. The console build stops accepting connections
       after the first request when MHD spawns a thread per connection, so the
       daemon stays single-threaded here; slow routes run their work in the
       backend instead of in this loop. */
    g_daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD, port, nullptr, nullptr, &api_handler,
        nullptr, MHD_OPTION_NOTIFY_COMPLETED, &request_completed, nullptr,
        MHD_OPTION_END);
    if (g_daemon) {
        ps5craft::trace("[http] one thread on :%d\n", port);
        std::thread(http_watchdog).detach();
        return 0;
    }
    ps5craft::trace("[http] start failed on :%d, retrying with poll\n", port);
    g_daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_USE_POLL, port, nullptr, nullptr,
        &api_handler, nullptr, MHD_OPTION_NOTIFY_COMPLETED,
        &request_completed, nullptr, MHD_OPTION_END);
    if (!g_daemon) {
        ps5craft::trace("[http] start failed on :%d\n", port);
        return -1;
    }
    ps5craft::trace("[http] single-thread on :%d\n", port);
    return 0;
}

void http_api_stop(void) {
    if (g_daemon) {
        MHD_stop_daemon(g_daemon);
        g_daemon = nullptr;
    }
    g_backend = nullptr;
}
