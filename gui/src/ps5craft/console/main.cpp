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
#include "ps5craft/core/catalog.hpp"
#include "ps5craft/core/http_client.hpp"
#include "ps5craft/core/storage.hpp"
#include "ps5craft/core/trace.hpp"
#include "ps5craft/install_screen.hpp"
#include "ps5craft/logs_screen.hpp"
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
#include <atomic>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#if defined(__PROSPERO__)
extern "C" int sceNetInit(void);
#endif

namespace
{

constexpr const char *kAssets = "/app0/assets";
constexpr int kHttpPort = 8080;
constexpr double kRowRefreshSeconds = 0.5;
constexpr double kPollSeconds = 0.2;

/* ShadowMountPlus mounts the host /data into the title sandbox, so this is
   the same directory the file manager sees. Fall back to the title folder
   when the mount is absent. */
std::string open_title_log()
{
    ::mkdir("/data/ps5craft", 0777);
    if (!ps5craft::trace_init("/data/ps5craft/title.log"))
        ps5craft::trace_init("/data/homebrew/PPSA90104/title.log");
    return "/data/ps5craft/title.log";
}

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

// Version lists come over the network, so they load off the frame thread.
struct VersionFetch
{
    std::mutex mutex;
    std::thread thread;
    std::atomic<bool> done{false};
    std::string software;
    std::vector<std::string> versions;
    std::string message;
};

std::vector<ps5craft::screen::InstallSoftware> install_software()
{
    std::vector<ps5craft::screen::InstallSoftware> out;
    for (const auto &s : ps5craft::catalog_software())
        out.push_back({s.id, s.name, s.note, s.supported});
    return out;
}

} // namespace

int main()
{
    using namespace hui;

    open_title_log();
    ps5craft::trace("[PS5Craft] entry\n");
    sys::log("[PS5Craft] entry");
#if defined(__PROSPERO__)
    /* The console resolver pool only exists after sceNetInit. Call it on the
       main thread: socket() and libcurl use the caller's network state. */
    ps5craft::trace("[PS5Craft] network init start\n");
    const int net_result = sceNetInit();
    ps5craft::trace("[PS5Craft] network init result=%d\n", net_result);
#endif
    ps5craft::trace("[PS5Craft] data root probe\n");
    const std::string data_root = ps5craft::console::choose_data_root();
    ps5craft::trace("[PS5Craft] data root %s shadowmount=%d\n", data_root.c_str(),
                    ps5craft::shadowmount_present() ? 1 : 0);
    for (const auto &volume : ps5craft::storage_volumes())
    {
        if (volume.present)
            ps5craft::trace("[PS5Craft] volume %s writable=%d free=%llu\n",
                            volume.path.c_str(), volume.writable ? 1 : 0,
                            static_cast<unsigned long long>(volume.free));
    }
    ps5craft::Backend backend(data_root);
    ps5craft::trace("[PS5Craft] backend ready\n");
    ps5craft::console::configure_ca_bundle(data_root);
    sys::log("[PS5Craft] backend data_root=%s instances=%zu", data_root.c_str(),
             backend.list_snapshots().size());

    // Keep the dashboard available in the same process, exactly as v1 did.
    ps5craft::trace("[PS5Craft] http start\n");
    const int http_result = http_api_start(kHttpPort, &backend);
    ps5craft::trace("[PS5Craft] http result=%d\n", http_result);
    sys::log("[PS5Craft] http api :%d result=%d", kHttpPort, http_result);

    ps5craft::trace("[PS5Craft] display open\n");
    ps5::Display display;
    if (!display.open(1920, 1080))
    {
        ps5craft::trace("[PS5Craft] fatal: display open failed\n");
        sys::log("[PS5Craft] fatal: display open failed");
        sys::park();
    }
    ps5craft::trace("[PS5Craft] display ready\n");

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

    ps5craft::screen::ServersScreen servers(fonts);
    ps5craft::screen::LogsScreen logs(fonts);
    ps5craft::screen::InstallScreen install(fonts);
    install.set_software(install_software());

    std::vector<ps5craft::screen::ServerRow> rows =
        ps5craft::console::rows_from_backend(backend);
    servers.set_rows(rows);

    enum class Mode
    {
        servers,
        logs,
        install,
    };
    Mode mode = Mode::servers;
    std::string logs_id;
    std::string install_id;
    std::shared_ptr<VersionFetch> fetch;
    std::string fetch_applied;
    std::int64_t last_log_fetch = 0;
    std::int64_t last_install_poll = 0;
    bool install_done_seen = false;

    const auto refresh_rows = [&]()
    {
        rows = ps5craft::console::rows_from_backend(backend);
        servers.set_rows(rows);
    };

    const auto start_fetch = [&](const std::string &software)
    {
        if (fetch && fetch->thread.joinable())
        {
            if (!fetch->done.load())
                return;
            fetch->thread.join();
        }
        auto job = std::make_shared<VersionFetch>();
        job->software = software;
        fetch = job;
        job->thread = std::thread([job, software]()
        {
            std::vector<ps5craft::CatalogVersion> list;
            std::string error;
            std::vector<std::string> ids;
            std::string message;
            if (ps5craft::catalog_versions(software, list, error))
            {
                for (const auto &version : list)
                    ids.push_back(version.id);
                if (ids.empty())
                    message = "No versions listed.";
            }
            else
            {
                message = "Version list failed: " + error;
            }
            std::lock_guard<std::mutex> lock(job->mutex);
            job->versions = std::move(ids);
            job->message = std::move(message);
            job->done = true;
        });
    };

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
        if (mode == Mode::servers)
        {
            servers.update(input, dt, feedback);
            if (!rows.empty())
            {
                const ps5craft::screen::ServerRow &row = rows[servers.selected()];
                if (input.is_pressed(hui::Action::west))
                {
                    logs_id = row.id;
                    logs.set_text(row.name, backend.instance_logs(row.id));
                    last_log_fetch = now;
                    mode = Mode::logs;
                    feedback.play(hui::audio::Cue::launch);
                }
                else if (input.is_pressed(hui::Action::north))
                {
                    install_id = row.id;
                    install.set_software(install_software());
                    install.set_install(false, false, false, 0, 0, std::string(), std::string());
                    last_install_poll = 0;
                    install_done_seen = false;
                    mode = Mode::install;
                    feedback.play(hui::audio::Cue::launch);
                }
            }
        }
        else if (mode == Mode::logs)
        {
            if (logs.update(input, dt, feedback))
            {
                mode = Mode::servers;
                refresh_rows();
            }
        }
        else
        {
            install.update(input, dt, feedback);
        }

        for (const audio::CueEvent &event : feedback.cues)
            sounds.play(mixer, audio::SoundSet::glass, event);
        if (feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);

        const ps5craft::screen::Request request = servers.take_request();
        if (request.kind != ps5craft::screen::RequestKind::none)
        {
            const bool start = request.kind == ps5craft::screen::RequestKind::start;
            const bool ok = start ? backend.start_instance(request.id)
                                  : backend.stop_instance(request.id);
            sys::log("[PS5Craft] %s %s ok=%d", start ? "start" : "stop", request.id.c_str(),
                     ok ? 1 : 0);
            refresh_rows();
        }

        const ps5craft::screen::InstallRequest want = install.take_request();
        if (want.kind == ps5craft::screen::InstallRequest::Kind::back)
        {
            mode = Mode::servers;
            refresh_rows();
        }
        else if (want.kind == ps5craft::screen::InstallRequest::Kind::load_versions)
        {
            start_fetch(want.software);
        }
        else if (want.kind == ps5craft::screen::InstallRequest::Kind::install)
        {
            const std::string error =
                backend.install_start(install_id, want.software, want.version);
            if (!error.empty())
            {
                install.set_install(false, false, true, 0, 0, want.version, error);
                sys::log("[PS5Craft] install start failed: %s", error.c_str());
            }
            else
            {
                install_done_seen = false;
            }
            last_install_poll = 0;
        }

        if (fetch && fetch->done.load() && fetch_applied != fetch->software)
        {
            std::vector<std::string> versions;
            std::string message;
            {
                std::lock_guard<std::mutex> lock(fetch->mutex);
                versions = fetch->versions;
                message = fetch->message;
            }
            install.set_versions(fetch->software, std::move(versions), message);
            fetch_applied = fetch->software;
        }

        if (mode == Mode::install && !install_id.empty() &&
            now - last_install_poll >= static_cast<std::int64_t>(0.3 * 1e6))
        {
            const ps5craft::Json status = backend.install_status(install_id);
            const bool active = status["active"].as_bool(false);
            const bool done = status["done"].as_bool(false);
            if (active || done)
            {
                install.set_install(
                    active, done, status["failed"].as_bool(false),
                    static_cast<std::uint64_t>(status["received"].as_number()),
                    static_cast<std::uint64_t>(status["total"].as_number()),
                    status["version"].as_string(), status["error"].as_string());
                if (done && !install_done_seen)
                {
                    install_done_seen = true;
                    refresh_rows();
                }
            }
            last_install_poll = now;
        }

        if (mode == Mode::logs && now - last_log_fetch >= static_cast<std::int64_t>(0.5 * 1e6))
        {
            std::string name = logs_id;
            for (const auto &row : rows)
                if (row.id == logs_id)
                    name = row.name;
            logs.set_text(name, backend.instance_logs(logs_id));
            last_log_fetch = now;
        }

        if (now - last_poll >= static_cast<std::int64_t>(kPollSeconds * 1e6))
        {
            backend.poll_all();
            last_poll = now;
        }
        if (now - last_refresh >= static_cast<std::int64_t>(kRowRefreshSeconds * 1e6))
        {
            refresh_rows();
            last_refresh = now;
        }

        pad.tick(dt);
        pad.set_light_bar(186, 166, 255);

        gfx::DrawList scene;
        gfx::BackdropSpec backdrop;
        if (mode == Mode::servers)
            servers.draw(scene, backdrop);
        else if (mode == Mode::logs)
            logs.draw(scene, backdrop);
        else
            install.draw(scene, backdrop);
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
