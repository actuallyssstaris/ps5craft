/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minecraft Java server wrapper over a supervised process. */
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "instance.hpp"
#include "json.hpp"
#include "fs.hpp"
#include "../platform/process.hpp"

namespace ps5craft {

class Server {
public:
    Server(Sandbox &sandbox, Instance &instance, LogRing &log);

    /* Build the JVM command line from instance config. */
    std::vector<std::string> build_command() const;

    /* Start: spawn JVM process. Returns pid or -1. */
    int start();

    /* Stop gracefully, then kill after timeout_ms. Returns true on clean stop. */
    bool stop(std::int64_t timeout_ms);

    /* Whether the supervised process is currently alive. */
    bool alive() const { return pid_ > 0 && !exited_; }

    int pid() const { return pid_; }
    int exit_code() const { return exit_code_.load(); }
    const std::string &exit_reason() const { return exit_reason_; }

    /* Poll the process (non-blocking). Returns 1 if reaped. */
    int poll();

    /* Blocking wait. */
    int wait();

    /* Write server.properties from config. */
    bool write_properties() const;

    /* Accept EULA: write eula=true. */
    bool accept_eula();

    bool eula_accepted() const;

private:
    Sandbox &sandbox_;
    Instance &instance_;
    LogRing &log_;
    std::mutex mutex_;
    int pid_ = 0;
    std::atomic<bool> exited_{false};
    std::atomic<int> exit_code_{-1};
    std::string exit_reason_;
    bool eula_written_ = false;
};

} // namespace ps5craft
