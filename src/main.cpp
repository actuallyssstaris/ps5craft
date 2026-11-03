/* SPDX-License-Identifier: GPL-3.0-or-later */
/* PS5Craft title entry point.
 *
 * Runs the shared backend and the HTTP management API. The native Vulkan UI
 * will share this same backend when the PS5 graphics dependencies (pinned
 * PS5_PayloadSDK fork + RADV archive) are buildable.
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

#include "../core/backend.hpp"
#include "../core/fs.hpp"
#include "../core/http_client.hpp"
#include "../src/http_api.hpp"

using namespace std::chrono_literals;

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int) { g_running = 0; }

/* The catalog downloads server software over HTTPS. libcurl needs a CA
   bundle to verify certificates. On the host the system store is used
   (http_ca_bundle() returns empty). On PS5 there is no system store, so the
   operator must place assets/ca-bundle.crt (shipped in the title app folder)
   into the data root, or set PS5CRAFT_CA_BUNDLE. Print a notice if neither is
   present so the failure mode is clear instead of a silent TLS error. */
static void note_ca_bundle(const std::string &data_root) {
    if (getenv("PS5CRAFT_CA_BUNDLE")) return;
    const std::string target = data_root + "/ca-bundle.crt";
    std::string text;
    if (ps5craft::fs_read_file(target, text)) {
        ps5craft::http_set_ca_bundle(target);
        return;
    }
    /* The title ships the bundle in its app folder. Copy it into the writable
       data root once, so libcurl has a stable path on every launch. */
    if (ps5craft::fs_read_file("/app0/assets/ca-bundle.crt", text) &&
        ps5craft::fs_write_file(target, text)) {
        ps5craft::http_set_ca_bundle(target);
        std::printf("CA bundle installed at %s\n", target.c_str());
        return;
    }
    std::fprintf(stderr,
        "NOTICE: no CA bundle configured (PS5CRAFT_CA_BUNDLE or %s).\n"
        "        Catalog HTTPS downloads will fail until one is provided.\n"
        "        Ship assets/ca-bundle.crt into the data root.\n", target.c_str());
}

/* A root counts as usable only when a file can be written and read back.
   Directory existence alone is not enough: /download0 is absent on some
   loader setups and mkdir can appear to succeed on a read-only mount. */
static bool root_is_writable(const std::string &root) {
    if (root.empty() || !ps5craft::fs_mkdirs(root)) return false;
    const std::string probe = root + "/.write-probe";
    const std::string token = "ps5craft";
    if (!ps5craft::fs_write_file(probe, token)) return false;
    std::string back;
    const bool ok = ps5craft::fs_read_file(probe, back) && back == token;
    ps5craft::fs_remove_tree(probe);
    return ok;
}

static std::string choose_data_root() {
    std::vector<std::string> candidates;
    if (const char *env = std::getenv("PS5CRAFT_DATA_ROOT"); env && *env)
        candidates.push_back(env);
#if defined(__PROSPERO__)
    /* /download0 is the conventional per-title data mount, but it is not
       present on every loader. Fall back to the title folder itself, then to
       the volatile app temp mount. */
    candidates.push_back("/download0/PS5Craft");
    candidates.push_back("/data/homebrew/PPSA90104/data");
    candidates.push_back("/app_temp0/ps5craft");
#else
    candidates.push_back("/download0/PS5Craft");
#endif
    for (const auto &candidate : candidates) {
        if (root_is_writable(candidate)) return candidate;
        std::fprintf(stderr, "data root %s is not writable\n", candidate.c_str());
    }
    /* Nothing probed clean: use the first choice so the failure is reported by
       the API instead of silently writing somewhere unexpected. */
    return candidates.front();
}

int main() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    /* Data root: PS5Craft data directory. Selected by write probe because the
       loader decides which mounts the title can actually use. */
    std::string data_root = choose_data_root();

    ps5craft::Backend backend(data_root);
    note_ca_bundle(data_root);
    std::printf("PS5Craft backend ready at %s\n", data_root.c_str());

    /* Create a default instance on first run so the dashboard is not empty. */
    if (backend.list_snapshots().empty()) {
        ps5craft::InstanceConfig def;
        def.name = "Example Server";
        def.software = "vanilla";
        def.memory.initial_mib = 16;
        def.memory.max_mib = 224;
        std::string id = backend.create_instance(def);
        std::printf("default instance: %s\n", id.c_str());
    }

    /* Start the HTTP management API (port 8080 unless overridden). */
    int port = 8080;
    if (const char *p = getenv("PS5CRAFT_PORT")) port = std::atoi(p);
    if (http_api_start(port, &backend) != 0) {
        std::fprintf(stderr, "failed to start HTTP API\n");
        return 1;
    }
    std::printf("HTTP API on :%d\n", port);

    /* Main loop: poll instances until terminated. */
    while (g_running) {
        backend.poll_all();
        std::this_thread::sleep_for(200ms);
    }

    http_api_stop();

    /* Stop running servers cleanly before exiting. */
    for (const auto &snap : backend.list_snapshots()) {
        std::string id = snap["id"].as_string();
        if (!id.empty()) backend.stop_instance(id);
    }
    for (int i = 0; i < 100; ++i) {
        backend.poll_all();
        bool running = false;
        for (const auto &snap : backend.list_snapshots()) {
            std::string st = snap["state"].as_string();
            if (st == "running" || st == "stopping") running = true;
        }
        if (!running) break;
        std::this_thread::sleep_for(100ms);
    }
    std::printf("PS5Craft stopped\n");
    return 0;
}
