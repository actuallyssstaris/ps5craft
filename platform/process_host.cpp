/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "process.hpp"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ps5craft {

namespace {

std::map<int, int> g_pipe_fd;   /* pid -> read end of its output pipe */
std::map<int, int> g_stdin_fd;  /* pid -> write end of its input pipe */

void set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* execve() does not search PATH. Resolve a bare command name first, using the
   PATH we are about to hand the child. */
std::string resolve_executable(const std::string &name,
                               const std::vector<std::string> &env) {
    if (name.empty() || name.find('/') != std::string::npos) return name;
    std::string path = "/usr/bin:/bin";
    for (const auto &e : env) {
        if (e.compare(0, 5, "PATH=") == 0) { path = e.substr(5); break; }
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        std::string dir = path.substr(start, end - start);
        if (dir.empty()) dir = ".";
        std::string candidate = dir + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
        start = end + 1;
    }
    return name;
}

} // namespace

LogRing::LogRing(std::size_t max_bytes) : max_bytes_(max_bytes) {}

bool LogRing::append(const char *data, std::size_t len) {
    if (len == 0) return true;
    if (len > max_bytes_) {
        /* single append larger than cap: keep only the tail */
        data_ = std::string(data + (len - max_bytes_), max_bytes_);
        head_ = 0;
        return true;
    }
    data_.append(data, len);
    while (data_.size() > max_bytes_ && head_ < data_.size()) {
        std::size_t nl = data_.find('\n', head_);
        if (nl == std::string::npos) {
            head_ = data_.size(); /* drop partial */
            break;
        }
        head_ = nl + 1;
    }
    return true;
}

std::string LogRing::snapshot() const {
    if (head_ >= data_.size()) return "";
    return data_.substr(head_);
}

std::size_t LogRing::size() const {
    return head_ >= data_.size() ? 0 : data_.size() - head_;
}

std::size_t LogRing::capacity() const { return max_bytes_; }

int process_spawn(const std::vector<std::string> &argv,
                  const std::vector<std::string> &env,
                  const std::string &cwd,
                  ProcessResult &result) {
    int out_pipe[2] = {-1, -1};
    int in_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0) {
        result.reason = "pipe failed";
        return -1;
    }
    if (pipe(in_pipe) != 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        result.reason = "pipe failed";
        return -1;
    }
    set_nonblock(out_pipe[0]);

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        result.reason = "fork failed";
        return -1;
    }

    if (pid == 0) {
        /* child */
        close(out_pipe[0]);
        if (dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        if (dup2(out_pipe[1], STDERR_FILENO) < 0) _exit(127);
        close(out_pipe[1]);

        close(in_pipe[1]);
        if (dup2(in_pipe[0], STDIN_FILENO) < 0) _exit(127);
        close(in_pipe[0]);

        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);

        std::vector<char *> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto &a : argv) cargv.push_back(const_cast<char *>(a.c_str()));
        cargv.push_back(nullptr);

        std::vector<char *> cenv;
        cenv.reserve(env.size() + 1);
        for (const auto &e : env) cenv.push_back(const_cast<char *>(e.c_str()));
        cenv.push_back(nullptr);

        const std::string exe = resolve_executable(argv[0], env);
        execve(exe.c_str(), cargv.data(), cenv.data());
        _exit(127);
    }

    /* parent */
    close(out_pipe[1]);
    close(in_pipe[0]);
    set_nonblock(in_pipe[1]);
    g_pipe_fd[static_cast<int>(pid)] = out_pipe[0];
    g_stdin_fd[static_cast<int>(pid)] = in_pipe[1];
    result.started_at = 0; /* set by caller */
    return static_cast<int>(pid);
}

int process_drain(int pid, LogRing *log) {
    auto it = g_pipe_fd.find(pid);
    if (it == g_pipe_fd.end()) return -1;
    int fd = it->second;
    char buffer[4096];
    std::size_t total = 0;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n > 0) {
            if (log) log->append(buffer, static_cast<std::size_t>(n));
            total += static_cast<std::size_t>(n);
            continue;
        }
        if (n == 0) break; /* EOF: child closed output */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        return -1;
    }
    return static_cast<int>(total);
}

int process_signal(int pid, int sig) {
    return kill(pid, sig);
}

int process_write(int pid, const char *data, std::size_t len) {
    auto it = g_stdin_fd.find(pid);
    if (it == g_stdin_fd.end()) return -1;
    int fd = it->second;
    std::size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return static_cast<int>(written);
        }
        written += static_cast<std::size_t>(n);
    }
    return static_cast<int>(written);
}

int process_waitpid(int pid, int *status, int options, ProcessResult &result) {
    pid_t reaped = waitpid(pid, status, options);
    if (reaped < 0) {
        if (errno == EINTR) return 0;
        if (errno == ECHILD) {
            /* child already reaped; clean up pipe fd */
            g_pipe_fd.erase(pid);
            return -1;
        }
        return 0;
    }
    if (reaped == 0) return 0; /* still running (WNOHANG) */
    /* reaped */
    auto pipe_it = g_pipe_fd.find(pid);
    if (pipe_it != g_pipe_fd.end()) {
        close(pipe_it->second);
        g_pipe_fd.erase(pipe_it);
    }
    auto in_it = g_stdin_fd.find(pid);
    if (in_it != g_stdin_fd.end()) {
        close(in_it->second);
        g_stdin_fd.erase(in_it);
    }
    if (WIFEXITED(*status)) {
        result.exit_code = WEXITSTATUS(*status);
        result.signaled = false;
        result.reason = "exited";
    } else if (WIFSIGNALED(*status)) {
        result.signaled = true;
        result.signal = WTERMSIG(*status);
        result.exit_code = -1;
        result.reason = "signaled";
    }
    return 1;
}

int process_wait(int pid, ProcessResult &result) {
    int status = 0;
    return process_waitpid(pid, &status, 0, result);
}

} // namespace ps5craft
