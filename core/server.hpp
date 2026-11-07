/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minecraft Java server wrapper over a supervised process. */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "../platform/process.hpp"

namespace ps5craft {

class Instance;
class Sandbox;

class Server {
public:
    Server(Sandbox &sandbox, Instance &instance, LogRing &log);

    /* Build the JVM command line from instance config. workdir_abs must be
       the absolute instance directory and java_abs the resolved runtime. */
    std::vector<std::string> build_command(const std::string &workdir_abs,
                                           const std::string &java_abs) const;

    /* Start: spawn the JVM process. Returns pid, or -1 on failure. */
    int start();

    /* Ask the process to stop. Sends the Minecraft "stop" command, then
       SIGTERM at the deadline, then SIGKILL. Non-blocking; poll() completes
       the transition. Returns true when a stop was initiated. */
    bool request_stop(std::int64_t timeout_ms);

    /* Write one command line to the server's stdin. Returns bytes written. */
    int send_command(const std::string &line);

    /* Drain output and reap the child if it exited.
       Returns 1 when reaped, 0 while running, -1 on error. */
    int poll();

    /* Blocking wait for exit. */
    int wait();

    bool alive() const { return pid_ > 0 && !exited_; }
    int pid() const { return pid_; }
    bool exited() const { return exited_; }
    int exit_code() const { return exit_code_.load(); }
    bool signaled() const { return result_.signaled; }
    const std::string &exit_reason() const { return exit_reason_; }

    /* Write server.properties from config. */
    bool write_properties() const;

    /* Accept EULA: write eula=true. */
    bool accept_eula();

    bool eula_accepted() const;

private:
    Sandbox &sandbox_;
    Instance &instance_;
    LogRing &log_;
    mutable std::mutex mutex_;
    ProcessResult result_;
    int pid_ = 0;
    std::atomic<bool> exited_{false};
    std::atomic<int> exit_code_{-1};
    std::string exit_reason_;
    std::int64_t stop_deadline_ms_ = 0;
    bool term_sent_ = false;
    bool kill_sent_ = false;
    bool eula_written_ = false;
};

} // namespace ps5craft
