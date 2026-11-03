// PS5Craft - native console UI entry point.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One title, two front ends: the shared Backend (instances, catalog, files,
// backups) and the HTTP dashboard on :8080, plus this native screen. The web
// dashboard stays available; the pad drives the same state in-process.
//
// Frame loop: read pad, update the screen, apply the start/stop request it
// returns, draw, present.

#include "ps5craft/console/model.hpp"
#include "ps5craft/console/runtime.hpp"
#include "ps5craft/core/backend.hpp"
#include "ps5craft/core/http_client.hpp"
#include "ps5craft/servers_screen.hpp"
#include "ps5craft/src/http_api.hpp"

#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "core/frame_stats.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "gfx/draw_list.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"

#include <GL/glcorearb.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

namespace
{

constexpr const char *kAssets = "/app0/assets";
constexpr int kHttpPort = 8080;
constexpr double kRowRefreshSeconds = 0.5;
constexpr double kPollSeconds = 0.2;

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[PS5Craft] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

void stop_all(ps5craft::Backend &backend)
{
    for (const auto &snapshot : backend.list_snapshots())
    {
        const std::string id = snapshot["id"].as_string();
        if (!id.empty())
            backend.stop_instance(id);
    }
    for (int i = 0; i < 100; ++i)
    {
        backend.poll_all();
        bool running = false;
        for (const auto &snapshot : backend.list_snapshots())
        {
            const std::string state = snapshot["state"].as_string();
            if (state == "running" || state == "stopping")
                running = true;
        }
        if (!running)
            break;
        hui::sys::sleep_us(100000);
    }
}

} // namespace

int main()
{
    using namespace hui;

    sys::log("[PS5Craft] entry");
    const std::string data_root = ps5craft::console::choose_data_root();
    ps5craft::Backend backend(data_root);
    ps5craft::console::configure_ca_bundle(data_root);
    sys::log("[PS5Craft] backend data_root=%s instances=%zu", data_root.c_str(),
             backend.list_snapshots().size());

    // Keep the dashboard available in the same process, exactly as v1 did.
    const int http_result = http_api_start(kHttpPort, &backend);
    sys::log("[PS5Craft] http api :%d result=%d", kHttpPort, http_result);

    ps5::Display display;
    if (!display.open(1920, 1080))
    {
        sys::log("[PS5Craft] fatal: display open failed");
        sys::park();
    }

    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font semibold;
    gfx::Font display_font;
    gfx::Font mono;
    ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, "inter-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "inter-semibold.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "montserrat-medium.huifont", &display_font, &fonts.display) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono))
    {
        sys::log("[PS5Craft] fatal: renderer init failed");
        sys::park();
    }

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;
    audio::Mixer mixer;
    mixer.set_bus_gain(audio::Bus::ui, 0.7f);
    mixer.set_bus_gain(audio::Bus::sfx, 0.7f);
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(std::string(kAssets) + "/audio/sfx");
    sys::log("[PS5Craft] sounds files=%d rejected=%d", bank.files, bank.rejected);

    ps5craft::screen::ServersScreen screen(fonts);
    screen.set_rows(ps5craft::console::rows_from_backend(backend));

    std::int64_t previous = sys::monotonic_us();
    std::int64_t last_frame_start = previous;
    std::int64_t last_refresh = previous;
    std::int64_t last_poll = previous;
    std::uint64_t frames = 0;
    FrameStats stats;
    PadSample samples[64];

    for (;;)
    {
        const std::int64_t now = sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_frame_start) / 1e6f;
        last_frame_start = now;
        if (dt > 0.05f)
            dt = 0.05f;

        const std::size_t count = pad.read(samples);
        const InputFrame input = tracker.update(
            std::span<const PadSample>(samples, count), static_cast<std::uint64_t>(now));

        ui::Feedback feedback;
        screen.update(input, dt, feedback);
        for (const audio::CueEvent &event : feedback.cues)
            sounds.play(mixer, audio::SoundSet::glass, event);
        if (feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);

        const ps5craft::screen::Request request = screen.take_request();
        if (request.kind != ps5craft::screen::RequestKind::none)
        {
            const bool start = request.kind == ps5craft::screen::RequestKind::start;
            const bool ok = start ? backend.start_instance(request.id)
                                  : backend.stop_instance(request.id);
            sys::log("[PS5Craft] %s %s ok=%d", start ? "start" : "stop", request.id.c_str(),
                     ok ? 1 : 0);
            screen.set_rows(ps5craft::console::rows_from_backend(backend));
        }

        if (now - last_poll >= static_cast<std::int64_t>(kPollSeconds * 1e6))
        {
            backend.poll_all();
            last_poll = now;
        }
        if (now - last_refresh >= static_cast<std::int64_t>(kRowRefreshSeconds * 1e6))
        {
            screen.set_rows(ps5craft::console::rows_from_backend(backend));
            last_refresh = now;
        }

        pad.tick(dt);
        pad.set_light_bar(186, 166, 255);

        gfx::DrawList scene;
        gfx::BackdropSpec backdrop;
        screen.draw(scene, backdrop);
        renderer.begin();
        renderer.backdrop(backdrop);
        renderer.draw(scene);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());

        if (!display.swap())
        {
            sys::log("[PS5Craft] fatal: swap failed frame=%llu error=%s",
                     static_cast<unsigned long long>(frames),
                     ps5::egl_error_name(display.last_error()));
            sys::park();
        }

        ++frames;
        const std::int64_t presented = sys::monotonic_us();
        const double frame_ms = static_cast<double>(presented - previous) / 1000.0;
        stats.add(frame_ms);
        previous = presented;

        if (frames == 1)
        {
            sys::log("[PS5Craft] first-swap ok shapes=%zu draws=%zu",
                     renderer.last_instances(), renderer.last_draw_calls());
            sys::log("[PS5Craft] ready splash_hidden=%d", sys::hide_splash_screen() ? 1 : 0);
        }
        if (stats.count() == 600)
        {
            char summary[160];
            stats.format(summary, sizeof(summary));
            sys::log("[PS5Craft] %s draws=%zu shapes=%zu", summary, renderer.last_draw_calls(),
                     renderer.last_instances());
            stats.reset();
        }
    }

    // The loop above is the title's life; this is reached only if a future
    // change adds a quit path.
    http_api_stop();
    stop_all(backend);
    pad.close();
    sys::quit();
}
