/* SPDX-License-Identifier: GPL-3.0-or-later */
/* HTTPS client used by the server catalog. libcurl is available in the PS5
   sysroot (static, OpenSSL backend), so the same code runs on console. */
#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ps5craft {

/* Progress callback: received and total bytes (total 0 when unknown).
   Return false to abort the transfer. */
using HttpProgress = std::function<bool(std::uint64_t received,
                                        std::uint64_t total)>;

struct HttpLimits {
    int timeout_s = 30;            /* whole transfer */
    int connect_timeout_s = 15;
    std::uint64_t max_bytes = 8ull * 1024 * 1024;
};

/* GET into memory. */
bool http_get(const std::string &url, std::string &out, std::string &error,
              const HttpLimits &limits = HttpLimits());

/* GET into a file. The caller decides the final path; write to a temporary
   name and rename after the hash check. */
bool http_download(const std::string &url, const std::string &path,
                   const HttpProgress &progress, std::string &error,
                   int timeout_s = 600);

/* Where the CA bundle lives, or empty to use libcurl's default. */
std::string http_ca_bundle();

/* Select the CA bundle explicitly. Used by the title entry point after it has
   chosen a writable data root; PS5CRAFT_CA_BUNDLE still wins. */
void http_set_ca_bundle(std::string path);

} // namespace ps5craft
