/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal file logger for the console title.
   The launcher gives the title no usable stdout, and the payload libc's
   dup2 is a stub, so logs are written straight to an fd. */
#pragma once

#include <string>

namespace ps5craft {

/* Open the log for this launch. Safe to call once; later calls are ignored.
   Returns false when the path cannot be opened. */
bool trace_init(const std::string &path);

/* Append one formatted line. Does nothing when trace_init failed. */
void trace(const char *format, ...) __attribute__((format(printf, 1, 2)));

} // namespace ps5craft
