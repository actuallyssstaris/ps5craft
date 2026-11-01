/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "monitor.hpp"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <ifaddrs.h>
#include <sstream>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

namespace ps5craft {
namespace {

bool read_file(const std::string &path, std::string &out) {
    std::FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        out.append(buf, n);
    std::fclose(f);
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
    std::string text;
    if (!read_file("/proc/self/stat", text)) return s;
    std::size_t end = text.rfind(')');
    if (end == std::string::npos) return s;
    std::istringstream iss(text.substr(end + 2));
    long state, ppid, pgrp, session, tty_nr, tpgid, flags;
    long minflt, cminflt, majflt, cmajflt;
    std::uint64_t utime, stime;
    long cutime, cstime, priority, nice, num_threads, itrealvalue;
    long long starttime;
    if (iss >> state >> ppid >> pgrp >> session >> tty_nr >> tpgid >> flags >>
          minflt >> cminflt >> majflt >> cmajflt >> utime >> stime >>
          cutime >> cstime >> priority >> nice >> num_threads >> itrealvalue >>
          starttime) {
        s.utime_ticks = utime;
        s.stime_ticks = stime;
    }
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        s.uptime_ms = static_cast<std::uint64_t>(ts.tv_sec) * 1000 +
                      static_cast<std::uint64_t>(ts.tv_nsec) / 1000000;
    return s;
}

double cpu_percent(const CpuSample &a, const CpuSample &b) {
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
}

std::vector<std::string> local_ips() {
    std::vector<std::string> out;
    struct ifaddrs *addrs = nullptr;
    if (getifaddrs(&addrs) != 0) return out;
    for (struct ifaddrs *a = addrs; a; a = a->ifa_next) {
        if (!a->ifa_addr) continue;
        char host[INET6_ADDRSTRLEN];
        const void *addr = nullptr;
        if (a->ifa_addr->sa_family == AF_INET)
            addr = &reinterpret_cast<struct sockaddr_in *>(a->ifa_addr)->sin_addr;
        else if (a->ifa_addr->sa_family == AF_INET6)
            addr = &reinterpret_cast<struct sockaddr_in6 *>(a->ifa_addr)->sin6_addr;
        else
            continue;
        if (inet_ntop(a->ifa_addr->sa_family, addr, host, sizeof host)) {
            std::string h = host;
            /* Loopback is not reachable from another machine; skip it. */
            if (h == "127.0.0.1" || h == "::1") continue;
            out.push_back(h);
        }
    }
    freeifaddrs(addrs);
    return out;
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
        double pct = cpu_percent(*prev_cpu, cur);
        if (pct >= 0) {
            out.proc_cpu_percent = pct;
            out.proc_cpu_present = true;
        }
        *prev_cpu = cur;
    } else {
        *prev_cpu = cur;
    }
    return out;
}

} // namespace ps5craft
