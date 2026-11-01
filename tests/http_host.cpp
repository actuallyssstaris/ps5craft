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
    while (out.size() < 4096) {
        ssize_t n = read(fd, buf, 1);
        if (n <= 0) break;
        out.push_back(buf[0]);
        if (out.size() >= 4 && out.compare(out.size() - 4, 4, "\r\n\r\n") == 0) break;
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

static void send_resp(int fd, int code, const std::string &ctype, const std::string &body) {
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
    auto q = path.find('?'); if (q != std::string::npos) path = path.substr(0, q);

    if (method == "GET" && path == "/") {
        std::string body;
        if (fs_read_file("/root/projects/ps5craft/web/index.html", body))
            send_resp(cfd, 200, "text/html", body);
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
    if (path.compare(0, 15, "/api/instances/") == 0) {
        std::string rest = path.substr(15);
        auto slash = rest.find('/');
        std::string id = rest.substr(0, slash);
        std::string action;
        if (slash != std::string::npos) action = rest.substr(slash + 1);
        auto inst = backend.instance(id);
        if (!inst) { send_resp(cfd, 404, "application/json", "{\"error\":\"not found\"}"); close(cfd); return; }
        if (method == "GET" && action.empty()) {
            send_resp(cfd, 200, "application/json", inst->snapshot().dump());
        } else if (method == "POST" && !action.empty()) {
            bool ok = false;
            if (action == "start") ok = backend.start_instance(id);
            else if (action == "stop") ok = backend.stop_instance(id);
            else if (action == "restart") ok = backend.restart_instance(id);
            if (!ok) { send_resp(cfd, 400, "application/json", "{\"error\":\"cannot\"}"); close(cfd); return; }
            send_resp(cfd, 200, "application/json", inst->snapshot().dump());
        } else {
            send_resp(cfd, 404, "application/json", "{\"error\":\"not found\"}");
        }
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
