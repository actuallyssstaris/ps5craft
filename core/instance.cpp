/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "instance.hpp"

#include <chrono>
#include <cstdio>

namespace ps5craft {

namespace {
std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}
}

Instance::Instance(InstanceConfig config) : config_(std::move(config)) {}

std::uint64_t Instance::uptime_ms() const {
    auto s = state_.load();
    if (s != InstanceState::kRunning && s != InstanceState::kStarting) return 0;
    auto start = started_at_.load();
    if (start <= 0) return 0;
    return static_cast<std::uint64_t>(now_ms() - start);
}

bool Instance::request_start() {
    InstanceState expected = InstanceState::kStopped;
    if (!state_.compare_exchange_strong(expected, InstanceState::kStarting)) {
        return false;
    }
    /* Actual process spawn happens in the supervisor; this only flips state. */
    pid_.store(0);
    exit_code_.store(-1);
    exit_reason_.clear();
    started_at_.store(now_ms());
    state_.store(InstanceState::kStarting);
    return true;
}

bool Instance::request_stop() {
    InstanceState expected = state_.load();
    while (expected == InstanceState::kRunning ||
           expected == InstanceState::kStarting ||
           expected == InstanceState::kCrashed) {
        if (state_.compare_exchange_weak(expected, InstanceState::kStopping)) {
            return true;
        }
    }
    return false;
}

bool Instance::request_restart() {
    if (state_.load() == InstanceState::kRunning ||
        state_.load() == InstanceState::kCrashed ||
        state_.load() == InstanceState::kError) {
        request_stop();
        /* restart completion handled by supervisor observing stopped state. */
        return true;
    }
    return false;
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
    j.set("uptime_ms", static_cast<double>(uptime_ms_));
    j.set("eula_accepted", config_.eula_accepted);
    j.set("online_mode", config_.online_mode);
    j.set("memory_initial_mib", config_.memory.initial_mib);
    j.set("memory_max_mib", config_.memory.max_mib);
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
