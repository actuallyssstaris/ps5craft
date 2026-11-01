/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Instance record and state machine. One instance per JVM per process. */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "json.hpp"

namespace ps5craft {

class Sandbox;
class Server;
class LogRing;

enum class InstanceState : std::uint8_t {
    kStopped = 0,
    kStarting = 1,
    kRunning = 2,
    kStopping = 3,
    kCrashed = 4,
    kError = 5,
};

struct MemorySettings {
    std::int64_t initial_mib = 16;
    std::int64_t max_mib = 224;
};

struct InstanceConfig {
    std::string id;                  /* stable, filesystem-safe */
    std::string name;
    std::string version = "26.3";
    std::string software = "vanilla"; /* vanilla | fabric | paper | ... */
    std::string jar_path;            /* relative to instance dir */
    std::string java_path = "java";  /* bundled runtime entry on PS5 */
    MemorySettings memory;
    std::vector<std::string> jvm_args;
    std::vector<std::string> server_args;
    bool eula_accepted = false;
    bool online_mode = true;
    std::string workdir;             /* instance dir */
};

struct InstanceSnapshot {
    InstanceConfig config;
    InstanceState state = InstanceState::kStopped;
    std::int64_t pid = 0;
    std::int64_t started_at = 0;
    std::int64_t stopped_at = 0;
    int exit_code = 0;
    std::string exit_reason;
    std::uint64_t uptime_ms = 0;
    std::string last_log;
};

class Instance {
public:
    Instance(InstanceConfig config);
    ~Instance();

    /* Bind the instance to the data sandbox. Must be called before start. */
    void attach(Sandbox &sandbox);

    const InstanceConfig &config() const { return config_; }
    InstanceState state() const { return state_.load(); }
    std::int64_t pid() const { return pid_.load(); }
    std::int64_t started_at() const { return started_at_.load(); }
    int exit_code() const { return exit_code_.load(); }
    const std::string &exit_reason() const { return exit_reason_; }
    std::uint64_t uptime_ms() const;

    /* Start spawns the server process synchronously. */
    bool request_start();
    bool request_stop();
    bool request_restart();

    /* Drain output and reap an exited process. Returns 1 when reaped. */
    int poll();

    /* Send one command line to the server stdin. Returns bytes written. */
    int send_command(const std::string &line);

    /* Write eula.txt for this instance and remember the flag. */
    bool write_eula();

    /* Record the installed server software after a verified download. */
    void apply_software(const std::string &version, const std::string &software,
                        const std::string &jar_path);

    /* Recent process output (bounded ring buffer). */
    std::string logs() const;

    Json snapshot() const;
    std::string state_name() const;

private:
    InstanceConfig config_;
    Sandbox *sandbox_ = nullptr;
    mutable std::mutex mutex_;
    std::unique_ptr<LogRing> log_;
    std::unique_ptr<Server> server_;
    std::atomic<InstanceState> state_{InstanceState::kStopped};
    std::atomic<std::int64_t> pid_{0};
    std::atomic<std::int64_t> started_at_{0};
    std::atomic<std::int64_t> stopped_at_{0};
    std::atomic<int> exit_code_{-1};
    std::string exit_reason_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> restart_requested_{false};
};

} // namespace ps5craft
