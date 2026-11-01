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
#include <chrono>

#include "../core/backend.hpp"
#include "../src/http_api.hpp"

using namespace std::chrono_literals;

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int) { g_running = 0; }

int main() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    /* Data root: PS5Craft data directory. On console this lives under
       /download0 or /data/homebrew/<title>/data depending on the loader. */
    std::string data_root = getenv("PS5CRAFT_DATA_ROOT")
                                ? getenv("PS5CRAFT_DATA_ROOT")
                                : "/download0/PS5Craft";

    ps5craft::Backend backend(data_root);
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
