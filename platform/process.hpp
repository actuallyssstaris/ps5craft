/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Process supervisor: spawn, capture output, stop/kill, exit detection. */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ps5craft {

/* Ring buffer of log lines with a byte cap. */
class LogRing {
public:
    explicit LogRing(std::size_t max_bytes);

    /* Append one line (caller supplies newline). Returns false on allocation failure. */
    bool append(const char *data, std::size_t len);

    /* Snapshot current contents as a single string (with newlines). */
    std::string snapshot() const;

    std::size_t size() const;
    std::size_t capacity() const;

private:
    std::string data_;
    std::size_t head_ = 0;      /* start of oldest complete line */
    std::size_t max_bytes_ = 0;
};

/* Result of a process run. */
struct ProcessResult {
    int exit_code = -1;
    bool signaled = false;
    int signal = 0;
    std::string reason;         /* human readable */
    std::int64_t started_at = 0;
    std::int64_t finished_at = 0;
    std::uint64_t runtime_ms = 0;
};

/* Spawn argv[0] with args, env, cwd. Returns pid (>0) on success, -1 with
   reason set otherwise. The caller owns reaping via process_wait/process_waitpid.
   Output is captured through process_drain. */
int process_spawn(const std::vector<std::string> &argv,
                  const std::vector<std::string> &env,
                  const std::string &cwd,
                  ProcessResult &result);

/* Read all currently-available output from a spawned process into log.
   Non-blocking. Returns bytes read, or -1 on error. */
int process_drain(int pid, LogRing *log);

/* Send signal to pid. Returns 0 on success. */
int process_signal(int pid, int sig);

/* Write bytes to the child's stdin. Returns bytes written, or -1 on error. */
int process_write(int pid, const char *data, std::size_t len);

/* Wait (non-blocking) for pid; fills result if exited. Returns 1 if reaped, 0 if still running, -1 on error. */
int process_waitpid(int pid, int *status, int options, ProcessResult &result);

/* Blocking wait. */
int process_wait(int pid, ProcessResult &result);

} // namespace ps5craft
