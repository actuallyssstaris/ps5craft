// PS5Craft - host snapshot preview of the console screens.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console app uses ServersScreen, LogsScreen and InstallScreen directly.
// This adapter lets the kit's host renderer (`make host-snapshots`) photograph
// the same screens without a backend, so layout and colour can be reviewed on
// a PC. PS5CRAFT_PREVIEW=servers|logs|install picks which one is drawn; the
// rows and log text below are sample data and no request goes anywhere.

#include "app/concept.hpp"

#include "ps5craft/install_screen.hpp"
#include "ps5craft/logs_screen.hpp"
#include "ps5craft/servers_screen.hpp"

#include <cstdlib>
#include <memory>
#include <string>

namespace hui::concepts
{

namespace
{

using ps5craft::screen::InstallScreen;
using ps5craft::screen::InstallSoftware;
using ps5craft::screen::LogsScreen;
using ps5craft::screen::ServersScreen;

constexpr const char *kTechniques[] = {
    "Rows come from the real backend in the console app; this design is its host preview",
    "Focus glides between rows and the list scrolls to keep the selection visible",
    "State colour and badge are driven by the instance state machine",
    "Detail panel shows software, version, memory, uptime and the last log line",
    "Cross asks to start or stop the selected server; the caller owns the request",
};

constexpr app::TourStep kTour[] = {
    {0.6f, 0, Direction::none, "servers"},
    {0.35f, 0, Direction::down},
    {0.35f, 0, Direction::down},
    {0.8f, 0, Direction::none, "selection"},
};

constexpr const char *kLogText =
    "[12:04:11] [main/INFO]: Loading Minecraft 1.21.4 with Fabric Loader 0.19.5\n"
    "[12:04:11] [main/INFO]: Loading 62 mods:\n"
    "\t- fabric-api 0.114.0+1.21.4\n"
    "\t- fabric-loader 0.19.5\n"
    "\t- lithium 0.14.3+mc1.21.4\n"
    "\t- sodium 0.6.9+mc1.21.4\n"
    "[12:04:13] [main/INFO]: SpongePowered MIXIN Subsystem started\n"
    "[12:04:18] [Worker-Main-4/INFO]: Preparing spawn area: 42%\n"
    "[12:04:21] [Server thread/INFO]: Starting minecraft server version 1.21.4\n"
    "[12:04:21] [Server thread/INFO]: Loading properties\n"
    "[12:04:21] [Server thread/INFO]: Default game type: SURVIVAL\n"
    "[12:04:21] [Server thread/INFO]: Generating keypair\n"
    "[12:04:22] [Server thread/INFO]: Starting Minecraft server on *:25565\n"
    "[12:04:22] [Server thread/INFO]: Using epoll channel type\n"
    "[12:04:23] [Server thread/INFO]: Preparing level \"world\"\n"
    "[12:04:29] [Server thread/INFO]: Preparing spawn area: 88%\n"
    "[12:04:31] [Server thread/INFO]: Done (12.418s)! For help, type \"help\"\n"
    "[12:05:02] [Server thread/INFO]: Steve joined the game\n"
    "[12:05:19] [Server thread/INFO]: <Steve> the render distance is holding up well\n"
    "[12:06:40] [Server thread/INFO]: Alex joined the game\n"
    "[12:07:14] [Server thread/WARN]: Can't keep up! Is the server overloaded? "
    "Running 1042ms or 20 ticks behind\n"
    "[12:07:31] [Server thread/INFO]: Saving chunks for level 'ServerLevel[world]'/minecraft:overworld\n";

class Ps5CraftPreview final : public app::Concept
{
  public:
    explicit Ps5CraftPreview(app::Context &context)
        : servers_(context.fonts), logs_(context.fonts), install_(context.fonts)
    {
        const char *mode = std::getenv("PS5CRAFT_PREVIEW");
        mode_ = mode != nullptr ? mode : "servers";

        servers_.set_rows({
            {"survival", "Survival SMP", "fabric", "1.21.4", "running",
             "Done (12.4s)! For help, type \"help\"", "", 4096, 8064000},
            {"creative", "Creative Flat", "vanilla", "1.21.4", "stopped",
             "Stopping server", "", 2048, 0},
            {"redstone", "Redstone Lab", "paper", "1.21.1", "starting",
             "Preparing spawn area: 42%", "", 3072, 0},
            {"expedition", "Modded Expedition", "fabric", "1.20.1", "crashed",
             "java.lang.OutOfMemoryError: Java heap space", "exit code 1",
             6144, 0},
        });

        logs_.set_text("Survival SMP", kLogText);

        install_.set_software({
            {"vanilla", "Vanilla", "Mojang server, no loader", true},
            {"fabric", "Fabric", "Loader 0.19.5", true},
            {"paper", "Paper", "Build 207", true},
            {"forge", "Forge", "Not tested on this build", false},
            {"quilt", "Quilt", "Not tested on this build", false},
        });
        install_.set_versions("vanilla", {"1.21.4", "1.21.1", "1.20.6", "1.20.1"},
                              "4 versions fit this server.");
        install_.set_versions("fabric", {"1.21.4", "1.21.1", "1.20.6", "1.20.1"},
                              "4 versions fit this server.");
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "ps5craft",
            "PS5Craft Servers",
            "The console home of your Minecraft servers",
            "gui/src/ps5craft/servers_screen.cpp",
            audio::SoundSet::glass,
            gfx::Color::rgb(0xb3d948),
            kTechniques,
        };
        return kInfo;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        if (mode_ == "logs")
        {
            logs_.update(input, dt, feedback);
            return;
        }
        if (mode_ == "install")
        {
            install_.update(input, dt, feedback);
            (void)install_.take_request();
            return;
        }
        servers_.update(input, dt, feedback);
        // No backend in the preview: the request has nowhere to go.
        (void)servers_.take_request();
    }

    void draw(app::Frame &frame) const override
    {
        if (mode_ == "logs")
            logs_.draw(frame.scene, frame.backdrop);
        else if (mode_ == "install")
            install_.draw(frame.scene, frame.backdrop);
        else
            servers_.draw(frame.scene, frame.backdrop);
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    ServersScreen servers_;
    LogsScreen logs_;
    InstallScreen install_;
    std::string mode_ = "servers";
};

} // namespace

std::unique_ptr<app::Concept> make_ps5craft(app::Context &context)
{
    return std::make_unique<Ps5CraftPreview>(context);
}

} // namespace hui::concepts
