// PS5Craft native UI - runtime paths.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ps5craft/console/runtime.hpp"

#include "ps5craft/core/fs.hpp"
#include "ps5craft/core/http_client.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace ps5craft::console
{

namespace
{

// A root counts as usable only when a file can be written and read back.
// Directory existence alone is not enough: /download0 is absent on some
// loader setups and mkdir can appear to succeed on a read-only mount.
bool root_is_writable(const std::string &root)
{
    if (root.empty() || !fs_mkdirs(root))
        return false;
    const std::string probe = root + "/.write-probe";
    const std::string token = "ps5craft";
    if (!fs_write_file(probe, token))
        return false;
    std::string back;
    const bool ok = fs_read_file(probe, back) && back == token;
    fs_remove_tree(probe);
    return ok;
}

} // namespace

std::string choose_data_root()
{
    std::vector<std::string> candidates;
    if (const char *env = std::getenv("PS5CRAFT_DATA_ROOT"); env && *env)
        candidates.push_back(env);
#if defined(__PROSPERO__)
    // /download0 is the conventional per-title data mount, but it is not
    // present on every loader. Fall back to the title folder itself, then to
    // the volatile app temp mount.
    candidates.push_back("/download0/PS5Craft");
    candidates.push_back("/data/homebrew/PPSA90104/data");
    candidates.push_back("/app_temp0/ps5craft");
#else
    candidates.push_back("/download0/PS5Craft");
#endif
    for (const auto &candidate : candidates)
    {
        if (root_is_writable(candidate))
            return candidate;
        std::fprintf(stderr, "data root %s is not writable\n", candidate.c_str());
    }
    // Nothing probed clean: use the first choice so the failure is reported
    // by the API instead of silently writing somewhere unexpected.
    return candidates.front();
}

void configure_ca_bundle(const std::string &data_root)
{
    if (std::getenv("PS5CRAFT_CA_BUNDLE"))
        return;
    const std::string target = data_root + "/ca-bundle.crt";
    std::string text;
    if (fs_read_file(target, text))
    {
        http_set_ca_bundle(target);
        return;
    }
    // The title ships the bundle in its app folder. Copy it into the writable
    // data root once, so libcurl has a stable path on every launch.
    if (fs_read_file("/app0/assets/ca-bundle.crt", text) && fs_write_file(target, text))
    {
        http_set_ca_bundle(target);
        std::printf("CA bundle installed at %s\n", target.c_str());
        return;
    }
    std::fprintf(stderr,
                 "NOTICE: no CA bundle configured (PS5CRAFT_CA_BUNDLE or %s).\n"
                 "        Catalog HTTPS downloads will fail until one is provided.\n",
                 target.c_str());
}

} // namespace ps5craft::console
