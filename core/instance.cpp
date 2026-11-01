/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "instance.hpp"

#include <chrono>
#include <cstdio>

#include "server.hpp"
#include "../platform/process.hpp"

namespace ps5craft {

namespace {
std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

std::string last_line(const std::string &text) {
    std::size_t end = text.find_last_not_of("\r\n");
    if (end == std::string::npos) return std::string();
    std::size_t begin = text.find_last_of('\n', end);
    begin = (begin == std::string::npos) ? 0 : begin + 1;
    return text.substr(begin, end - begin + 1);
}
} // namespace

Instance::Instance(InstanceConfig config) : config_(std::move(config)) {}

Instance::~Instance() = default;

void Instance::attach(Sandbox &sandbox) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (server_) return;
    sandbox_ = &sandbox;
    log_.reset(new LogRing(256 * 1024));
    server_.reset(new Server(sandbox, *this, *log_));
}

std::uint64_t Instance::uptime_ms() const {
    auto s = state_.load();
    if (s != InstanceState::kRunning && s != InstanceState::kStarting &&
        s != InstanceState::kStopping)
        return 0;
    auto start = started_at_.load();
    if (start <= 0) return 0;
    return static_cast<std::uint64_t>(now_ms() - start);
}

bool Instance::request_start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!server_) {
        exit_reason_ = "instance not attached to a data root";
        state_.store(InstanceState::kError);
        return false;
    }
    InstanceState expected = InstanceState::kStopped;
    if (!state_.compare_exchange_strong(expected, InstanceState::kStarting)) {
        return false;
    }
    stop_requested_.store(false);
    restart_requested_.store(false);
    exit_code_.store(-1);
    exit_reason_.clear();
    started_at_.store(now_ms());
    stopped_at_.store(0);

    int pid = server_->start();
    if (pid <= 0) {
        pid_.store(0);
        exit_reason_ = server_->exit_reason().empty()
                           ? "spawn failed"
                           : server_->exit_reason();
        state_.store(InstanceState::kError);
        return false;
    }
    pid_.store(pid);
    state_.store(InstanceState::kRunning);
    return true;
}

bool Instance::request_stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    InstanceState current = state_.load();
    if (current != InstanceState::kRunning &&
        current != InstanceState::kStarting)
        return false;
    if (!server_) return false;

    server_->request_stop(20000);
    stop_requested_.store(true);
    state_.store(InstanceState::kStopping);
    return true;
}

bool Instance::request_restart() {
    bool start_now = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        InstanceState current = state_.load();
        if (current == InstanceState::kCrashed) {
            /* Nothing to stop; start directly. */
            restart_requested_.store(false);
            state_.store(InstanceState::kStopped);
            start_now = true;
        } else if (current == InstanceState::kRunning) {
            if (!server_) return false;
            server_->request_stop(20000);
            stop_requested_.store(true);
            restart_requested_.store(true);
            state_.store(InstanceState::kStopping);
            return true;
        } else {
            return false;
        }
    }
    return start_now ? request_start() : true;
}

int Instance::poll() {
    bool restart = false;
    int reaped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!server_) return 0;
        reaped = server_->poll();
        if (reaped == 1) {
            pid_.store(0);
            stopped_at_.store(now_ms());
            exit_code_.store(server_->exit_code());
            exit_reason_ = server_->exit_reason();
            bool clean = !server_->signaled() && server_->exit_code() == 0;
            state_.store(clean ? InstanceState::kStopped
                               : InstanceState::kCrashed);
            restart = restart_requested_.exchange(false);
        } else if (server_->alive()) {
            pid_.store(server_->pid());
        }
    }
    if (reaped == 1 && restart) {
        (void)request_start();
    }
    return reaped;
}

int Instance::send_command(const std::string &line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!server_) return -1;
    return server_->send_command(line);
}

bool Instance::write_eula() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!server_) return false;
    if (!server_->accept_eula()) return false;
    config_.eula_accepted = true;
    return true;
}

std::string Instance::logs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!log_) return std::string();
    return log_->snapshot();
}

Json Instance::snapshot() const {
    Json j = Json::object();
    j.set("id", config_.id);
    j.set("name", config_.name);
    j.set("version", config_.version);
    j.set("software", config_.software);
    j.set("state", state_name());
    j.set("pid", pid_.load());
    j.set("started_at", started_at_.load());
    j.set("stopped_at", stopped_at_.load());
    j.set("exit_code", exit_code_.load());
    j.set("exit_reason", exit_reason_);
    j.set("uptime_ms", static_cast<double>(uptime_ms()));
    j.set("eula_accepted", config_.eula_accepted);
    j.set("online_mode", config_.online_mode);
    j.set("memory_initial_mib", config_.memory.initial_mib);
    j.set("memory_max_mib", config_.memory.max_mib);
    j.set("last_log", last_line(logs()));
    Json args = Json::array();
    for (const auto &a : config_.jvm_args) args.push(a);
    j.set("jvm_args", args);
    Json sargs = Json::array();
    for (const auto &a : config_.server_args) sargs.push(a);
    j.set("server_args", sargs);
    return j;
}

std::string Instance::state_name() const {
    switch (state_.load()) {
    case InstanceState::kStopped: return "stopped";
    case InstanceState::kStarting: return "starting";
    case InstanceState::kRunning: return "running";
    case InstanceState::kStopping: return "stopping";
    case InstanceState::kCrashed: return "crashed";
    case InstanceState::kError: return "error";
    }
    return "unknown";
}

} // namespace ps5craft
