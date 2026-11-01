/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host HTTP harness: real Backend + minimal single-threaded socket server.
   Mirrors the MHD API routes to validate the shared backend end-to-end on host.
   Not shipping code; the PS5 title uses src/http_api.cpp (libmicrohttpd). */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../core/backend.hpp"
#include "../core/json.hpp"

using namespace ps5craft;

static volatile sig_atomic_t g_run = 1;
static void h(int) { g_run = 0; }

static std::string read_headers(int fd) {
    std::string out; char buf[1];
    while (out.size() < 8192) {
        ssize_t n = read(fd, buf, 1);
        if (n <= 0) break;
        out.push_back(buf[0]);
        if (out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0) break;
    }
    return out;
}

static std::string read_exact(int fd, size_t n) {
    std::string out; out.reserve(n);
    char buf[4096];
    while (out.size() < n) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r <= 0) break;
        out.append(buf, (size_t)r);
    }
    return out;
}

static std::string url_path(const std::string &req) {
    auto sp = req.find(' ');
    if (sp == std::string::npos) return "/";
    auto start = req.find_first_not_of(' ', sp + 1);
    if (start == std::string::npos) return "/";
    auto end = req.find(' ', start);
    if (end == std::string::npos) return req.substr(start);
    return req.substr(start, end - start);
}

static std::string query_param(const std::string &query, const std::string &key) {
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

static bool starts(const std::string &s, const std::string &p) {
    return s.compare(0, p.size(), p) == 0;
}

static void send_resp(int fd, int code, const std::string &ctype,
                      const std::string &body) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " OK\r\n"
                    "Content-Type: " + ctype + "\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n"
                    "Access-Control-Allow-Origin: *\r\n"
                    "Connection: close\r\n\r\n";
    std::string resp = h + body;
    write(fd, resp.data(), resp.size());
}

static void handle(int cfd, Backend &backend) {
    std::string req = read_headers(cfd);
    if (req.empty()) { close(cfd); return; }
    auto method_end = req.find(' ');
    std::string method = req.substr(0, method_end);
    std::string path = url_path(req);
    auto q = path.find('?');
    std::string query;
    if (q != std::string::npos) { query = path.substr(q + 1); path = path.substr(0, q); }

    std::string body;
    if (starts(method, "POST") || starts(method, "PUT")) {
        std::string clen;
        auto cl = req.find("Content-Length: ");
        if (cl != std::string::npos) {
            auto clend = req.find("\r\n", cl);
            clen = req.substr(cl + 16, clend - cl - 16);
        }
        size_t n = clen.empty() ? 0 : std::strtoul(clen.c_str(), nullptr, 10);
        body = read_exact(cfd, n);
    }

    if (method == "GET" && path == "/") {
        std::string html;
        if (fs_read_file("/root/projects/ps5craft/web/index.html", html))
            send_resp(cfd, 200, "text/html", html);
        else
            send_resp(cfd, 200, "text/html", "<html>ok</html>");
        close(cfd);
        return;
    }
    if (method == "GET" && path == "/api/instances") {
        send_resp(cfd, 200, "application/json", backend.status_snapshot().dump());
        close(cfd);
        return;
    }
    if (method == "GET" && starts(path, "/api/instances/")) {
        auto id = path.substr(std::string("/api/instances/").size());
        auto slash = id.find('/');
        if (slash != std::string::npos) id = id.substr(0, slash);
        auto inst = backend.instance(id);
        if (!inst) { send_resp(cfd, 404, "application/json", "{\"error\":\"not found\"}"); close(cfd); return; }
        send_resp(cfd, 200, "application/json", inst->snapshot().dump());
        close(cfd);
        return;
    }
    if (method == "POST" && starts(path, "/api/instances/")) {
        auto rest = path.substr(std::string("/api/instances/").size());
        auto slash = rest.find('/');
        if (slash == std::string::npos) { send_resp(cfd, 404, "application/json", "{\"error\":\"not found\"}"); close(cfd); return; }
        auto id = rest.substr(0, slash);
        auto action = rest.substr(slash + 1);
        auto inst = backend.instance(id);
        if (!inst) { send_resp(cfd, 404, "application/json", "{\"error\":\"not found\"}"); close(cfd); return; }
        bool ok = false;
        if (action == "start") ok = backend.start_instance(id);
        else if (action == "stop") ok = backend.stop_instance(id);
        else if (action == "restart") ok = backend.restart_instance(id);
        if (!ok) { send_resp(cfd, 400, "application/json", "{\"error\":\"cannot\"}"); close(cfd); return; }
        send_resp(cfd, 200, "application/json", inst->snapshot().dump());
        close(cfd);
        return;
    }
    if (method == "GET" && starts(path, "/api/fs/")) {
        auto relative = path.substr(std::string("/api/fs/").size());
        FileEntry entry;
        if (!backend.sandbox().entry_stat(relative, entry)) {
            send_resp(cfd, 404, "application/json", "{\"error\":\"not found\"}"); close(cfd); return;
        }
        if (entry.directory) {
            std::vector<FileEntry> list;
            if (!backend.sandbox().list(relative, list)) {
                send_resp(cfd, 500, "application/json", "{\"error\":\"cannot list\"}"); close(cfd); return;
            }
            Json arr = Json::array();
            for (const auto &e : list) {
                Json item = Json::object();
                item.set("name", e.name); item.set("path", e.path);
                item.set("directory", e.directory); item.set("size", (double)e.size);
                item.set("modified", (double)e.modified);
                arr.push(item);
            }
            Json root = Json::object();
            root.set("path", relative); root.set("entries", arr);
            send_resp(cfd, 200, "application/json", root.dump());
        } else {
            std::string content;
            if (!backend.sandbox().read(relative, content, 1u << 20)) {
                send_resp(cfd, 500, "application/json", "{\"error\":\"cannot read\"}"); close(cfd); return;
            }
            send_resp(cfd, 200, "text/plain", content);
        }
        close(cfd);
        return;
    }
    if (method == "POST" && starts(path, "/api/fs/")) {
        auto relative = path.substr(std::string("/api/fs/").size());
        std::string action = query_param(query, "action");
        bool ok = false;
        if (action == "write") {
            ok = backend.fs_write(relative, body);
        } else if (action == "mkdir") {
            ok = backend.fs_mkdir(relative);
        } else if (action == "rename") {
            std::string to = query_param(query, "to");
            ok = backend.fs_rename(relative, to);
        } else if (action == "delete") {
            ok = backend.fs_remove(relative);
        } else if (action == "copy") {
            std::string to = query_param(query, "to");
            ok = backend.fs_copy(relative, to);
        }
        if (!ok) { send_resp(cfd, 500, "application/json", "{\"error\":\"op failed\"}"); close(cfd); return; }
        send_resp(cfd, 200, "application/json", "{\"ok\":true}");
        close(cfd);
        return;
    }
    if (method == "GET" && path == "/api/status") {
        send_resp(cfd, 200, "application/json", backend.status_snapshot().dump());
        close(cfd);
        return;
    }
    send_resp(cfd, 404, "text/plain", "not found");
    close(cfd);
}

int main() {
    signal(SIGINT, h); signal(SIGTERM, h); signal(SIGPIPE, SIG_IGN);
    Backend backend("/tmp/ps5craft_e2e");
    InstanceConfig c;
    c.name = "E2E Server";
    c.software = "paper";
    c.memory.initial_mib = 16;
    c.memory.max_mib = 224;
    std::string id = backend.create_instance(c);
    std::printf("created instance %s\n", id.c_str());
    fflush(stdout);

    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(18090);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sfd, (sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(sfd, 16) < 0) { perror("listen"); return 1; }
    std::printf("listening on 127.0.0.1:18090\n");
    fflush(stdout);

    while (g_run) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(sfd, &rf);
        timeval tv{2, 0};
        int rc = select(sfd + 1, &rf, 0, 0, &tv);
        if (rc < 0) { if (errno == EINTR) continue; break; }
        if (rc == 0) continue;
        int cfd = accept(sfd, 0, 0);
        if (cfd < 0) continue;
        handle(cfd, backend);
    }
    close(sfd);
    return 0;
}
