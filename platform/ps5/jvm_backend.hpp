/* SPDX-License-Identifier: GPL-3.0-or-later */
/* In-process JVM backend for the console title. Replaces posix_spawn: a title
   cannot exec another program, so the HotSpot build is linked into eboot.bin
   and started with JNI_CreateJavaVM. */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "../process.hpp"

namespace ps5craft {

/* True when pid belongs to the embedded VM rather than a spawned process. */
bool jvm_backend_owns(int pid);

/* Start the embedded VM with a command line shaped like the host backend's
   java invocation. Returns a synthetic pid or -1. */
int jvm_backend_spawn(const std::vector<std::string> &argv, const std::string &cwd,
                      ProcessResult &result);

int jvm_backend_drain(int pid, LogRing *log);
int jvm_backend_signal(int pid, int sig);
int jvm_backend_write(int pid, const char *data, std::size_t len);
int jvm_backend_waitpid(int pid, int *status, int options, ProcessResult &result);

} // namespace ps5craft
