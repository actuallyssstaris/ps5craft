/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "trace.hpp"

#include <cstdarg>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

namespace ps5craft {
namespace {
int g_fd = -1;
}

bool trace_init(const std::string &path) {
    if (g_fd >= 0) return true;
    g_fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0777);
    return g_fd >= 0;
}

void trace(const char *format, ...) {
    if (g_fd < 0) return;
    char line[512];
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    if (length <= 0) return;
    const std::size_t used =
        static_cast<std::size_t>(length) < sizeof line
            ? static_cast<std::size_t>(length)
            : sizeof line - 1;
    (void)::write(g_fd, line, used);
}

} // namespace ps5craft
