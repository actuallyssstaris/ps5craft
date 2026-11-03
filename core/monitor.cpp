/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "monitor.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace ps5craft {
namespace {

bool read_file(const std::string &path, std::string &out) {
    /* Raw descriptor I/O: the title's runtime provides open/read, while the
       stdio wrappers are not part of its compatibility contract. */
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    out.clear();
    char buf[4096];
    for (;;) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (n == 0) break;
        out.append(buf, static_cast<std::size_t>(n));
        if (out.size() > (1u << 20)) break;
    }
    close(fd);
    return true;
}

/* First "Key: value" int match in /proc/self/status-style text. */
std::int64_t status_int(const std::string &text, const char *key) {
    const std::string k = std::string(key) + ":";
    std::size_t pos = 0;
    while ((pos = text.find(k, pos)) != std::string::npos) {
        if (pos != 0 && text[pos - 1] != '\n') {
            pos += k.size();
            continue;
        }
        pos += k.size();
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'))
            ++pos;
        std::int64_t v = 0;
        bool any = false;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            v = v * 10 + (text[pos] - '0');
            ++pos;
            any = true;
        }
        if (any) return v;
        pos += k.size();
    }
    return -1;
}

std::int64_t meminfo_int(const std::string &text, const char *key) {
    const std::string k = std::string(key) + ":";
    std::size_t pos = 0;
    while ((pos = text.find(k, pos)) != std::string::npos) {
        if (pos != 0 && text[pos - 1] != '\n' && text[pos - 1] != '\r') {
            pos += k.size();
            continue;
        }
        pos += k.size();
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t'))
            ++pos;
        std::int64_t v = 0;
        bool any = false;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            v = v * 10 + (text[pos] - '0');
            ++pos;
            any = true;
        }
        if (any) return v;
        pos += k.size();
    }
    return -1;
}

} // namespace

ProcMem read_proc_mem() {
    ProcMem m;
    std::string text;
    if (!read_file("/proc/self/status", text)) return m;
    m.vms_kb = status_int(text, "VmSize");
    m.rss_kb = status_int(text, "VmRSS");
    m.present = m.vms_kb >= 0 || m.rss_kb >= 0;
    return m;
}

SysMem read_sys_mem() {
    SysMem m;
    std::string text;
    if (!read_file("/proc/meminfo", text)) return m;
    m.total_kb = meminfo_int(text, "MemTotal");
    m.available_kb = meminfo_int(text, "MemAvailable");
    m.present = m.total_kb >= 0 || m.available_kb >= 0;
    return m;
}

CpuSample cpu_sample() {
    CpuSample s;
#if defined(__PROSPERO__)
    /* getrusage is part of the runtime's import contract and reports CPU time
       in microseconds, so no sysconf tick-rate lookup is needed. */
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return s;
    s.cpu_us = static_cast<std::uint64_t>(usage.ru_utime.tv_sec) * 1000000u +
               static_cast<std::uint64_t>(usage.ru_utime.tv_usec) +
               static_cast<std::uint64_t>(usage.ru_stime.tv_sec) * 1000000u +
               static_cast<std::uint64_t>(usage.ru_stime.tv_usec);
#else
    std::string text;
    if (!read_file("/proc/self/stat", text)) return s;
    /* Fields after the parenthesised comm: state is field 3, utime 14,
       stime 15. Parsed by hand: libc++ streams drag in locale internals the
       PS5 libc does not provide. */
    std::size_t pos = text.rfind(')');
    if (pos == std::string::npos) return s;
    ++pos;
    int field = 3;
    while (field <= 15) {
        while (pos < text.size() && text[pos] == ' ') ++pos;
        const std::size_t start = pos;
        while (pos < text.size() && text[pos] != ' ' && text[pos] != '\n') ++pos;
        if (start == pos) return s;
        if (field == 14 || field == 15) {
            std::uint64_t v = 0;
            for (std::size_t i = start; i < pos; ++i) {
                if (text[i] < '0' || text[i] > '9') return s;
                v = v * 10 + static_cast<std::uint64_t>(text[i] - '0');
            }
            (field == 14 ? s.utime_ticks : s.stime_ticks) = v;
        }
        ++field;
    }
#endif
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return s;
    s.uptime_ms = static_cast<std::uint64_t>(ts.tv_sec) * 1000 +
                  static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
    s.valid = true;
    return s;
}

double cpu_percent(const CpuSample &a, const CpuSample &b) {
#if defined(__PROSPERO__)
    const std::int64_t dcpu =
        static_cast<std::int64_t>(b.cpu_us - a.cpu_us);
    const std::int64_t dwall =
        static_cast<std::int64_t>(b.uptime_ms - a.uptime_ms);
    if (dcpu < 0 || dwall <= 0) return -1;
    const double pct = (static_cast<double>(dcpu) / 1000.0) /
                       static_cast<double>(dwall) * 100.0;
    return pct < 0 ? 0 : (pct > 100.0 ? 100.0 : pct);
#else
    const long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    const long clk = sysconf(_SC_CLK_TCK);
    if (ncpu < 1 || clk <= 0) return -1;
    const std::int64_t dticks =
        static_cast<std::int64_t>((b.utime_ticks + b.stime_ticks) -
                                  (a.utime_ticks + a.stime_ticks));
    const std::int64_t dwall =
        static_cast<std::int64_t>(b.uptime_ms - a.uptime_ms);
    if (dticks < 0 || dwall <= 0) return -1;
    const double pct = (static_cast<double>(dticks) / clk) /
                       (static_cast<double>(dwall) / 1000.0) * 100.0 *
                       static_cast<double>(ncpu);
    if (pct < 0) return 0;
    const double cap = 100.0 * static_cast<double>(ncpu);
    return pct > cap ? cap : pct;
#endif
}

std::vector<std::string> local_ips() {
    std::vector<std::string> out;
#if !defined(__PROSPERO__)
    struct ifaddrs *addrs = nullptr;
    if (getifaddrs(&addrs) != 0) return out;
    for (struct ifaddrs *a = addrs; a; a = a->ifa_next) {
        if (!a->ifa_addr) continue;
        char text[INET6_ADDRSTRLEN];
        const void *addr = nullptr;
        if (a->ifa_addr->sa_family == AF_INET)
            addr = &reinterpret_cast<struct sockaddr_in *>(a->ifa_addr)->sin_addr;
        else if (a->ifa_addr->sa_family == AF_INET6)
            addr = &reinterpret_cast<struct sockaddr_in6 *>(a->ifa_addr)->sin6_addr;
        else
            continue;
        if (!inet_ntop(a->ifa_addr->sa_family, addr, text, sizeof text)) continue;
        std::string h = text;
        /* Loopback is not reachable from another machine; skip it. */
        if (h == "127.0.0.1" || h == "::1") continue;
        out.push_back(h);
    }
    freeifaddrs(addrs);
    return out;
#else
    /* getifaddrs() and getaddrinfo() are not usable in the title's runtime.
       A UDP socket connected to a public address selects the console's
       primary IPv4 route; no packet is sent by connect(). */
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return out;
    struct sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port = htons(53);
    target.sin_addr.s_addr = htonl(0x08080808u); /* 8.8.8.8 */
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&target),
                sizeof target) == 0) {
        struct sockaddr_in local {};
        socklen_t len = sizeof local;
        if (getsockname(fd, reinterpret_cast<struct sockaddr *>(&local),
                        &len) == 0 &&
            local.sin_family == AF_INET) {
            const unsigned char *b =
                reinterpret_cast<const unsigned char *>(&local.sin_addr);
            char text[16];
            std::snprintf(text, sizeof text, "%u.%u.%u.%u",
                          static_cast<unsigned>(b[0]),
                          static_cast<unsigned>(b[1]),
                          static_cast<unsigned>(b[2]),
                          static_cast<unsigned>(b[3]));
            out.push_back(text);
        }
    }
    close(fd);
    return out;
#endif
}

MonitorSnapshot monitor_sample(CpuSample *prev_cpu) {
    MonitorSnapshot out;
    out.proc_mem = read_proc_mem();
    out.proc_mem_present = out.proc_mem.present;
    out.sys_mem = read_sys_mem();
    out.sys_mem_present = out.sys_mem.present;
    out.ips = local_ips();
    out.net_present = !out.ips.empty();

    CpuSample cur = cpu_sample();
    if (prev_cpu) {
        /* A percentage needs two valid samples; the first call only seeds. */
        if (prev_cpu->valid && cur.valid) {
            double pct = cpu_percent(*prev_cpu, cur);
            if (pct >= 0) {
                out.proc_cpu_percent = pct;
                out.proc_cpu_present = true;
            }
        }
        *prev_cpu = cur;
    }
    return out;
}

} // namespace ps5craft
