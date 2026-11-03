/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Read-only measured monitoring. Values come from /proc, getifaddrs, or
   getrusage, and are absent when the platform does not provide them. Nothing
   simulated. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps5craft {

struct ProcMem {
    std::int64_t vms_kb = -1;
    std::int64_t rss_kb = -1;
    bool present = false;
};

struct SysMem {
    std::int64_t total_kb = -1;
    std::int64_t available_kb = -1;
    bool present = false;
};

struct CpuSample {
    std::uint64_t utime_ticks = 0;
    std::uint64_t stime_ticks = 0;
    std::uint64_t uptime_ms = 0;
    std::uint64_t cpu_us = 0; /* PS5: getrusage user+system microseconds */
    bool valid = false;
};

struct MonitorSnapshot {
    ProcMem proc_mem;
    SysMem sys_mem;
    double proc_cpu_percent = -1;
    std::vector<std::string> ips;
    bool proc_cpu_present = false;
    bool sys_mem_present = false;
    bool proc_mem_present = false;
    bool net_present = false;
};

ProcMem read_proc_mem();
SysMem read_sys_mem();
CpuSample cpu_sample();
double cpu_percent(const CpuSample &a, const CpuSample &b);
std::vector<std::string> local_ips();

/* One snapshot; prev_cpu holds the previous sample for the CPU delta. */
MonitorSnapshot monitor_sample(CpuSample *prev_cpu);

} // namespace ps5craft
