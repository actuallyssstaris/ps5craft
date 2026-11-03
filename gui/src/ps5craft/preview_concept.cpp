// PS5Craft - host snapshot preview of the server list.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console app uses ServersScreen directly. This adapter lets the kit's
// host renderer (`make host-snapshots`) photograph the same screen without a
// backend, so layout and colour can be reviewed on a PC. The rows below are
// sample data and this adapter ignores start/stop requests; the real app
// supplies rows from Backend::list_snapshots().

#include "app/concept.hpp"

#include "ps5craft/servers_screen.hpp"

#include <memory>

namespace hui::concepts
{

namespace
{

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

class Ps5CraftPreview final : public app::Concept
{
  public:
    explicit Ps5CraftPreview(app::Context &context) : screen_(context.fonts)
    {
        screen_.set_rows({
            {"survival", "Survival SMP", "fabric", "1.21.4", "running",
             "Done (12.4s)! For help, type \"help\"", 4096, 8064000},
            {"creative", "Creative Flat", "vanilla", "1.21.4", "stopped",
             "Stopping server", 2048, 0},
            {"redstone", "Redstone Lab", "paper", "1.21.1", "starting",
             "Preparing spawn area: 42%", 3072, 0},
            {"expedition", "Modded Expedition", "fabric", "1.20.1", "crashed",
             "java.lang.OutOfMemoryError: Java heap space", 6144, 0},
        });
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "ps5craft",
            "PS5Craft Servers",
            "The console home of your Minecraft servers",
            "gui/src/ps5craft/servers_screen.cpp",
            audio::SoundSet::glass,
            gfx::Color::rgb(0xbaa6ff),
            kTechniques,
        };
        return kInfo;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        screen_.update(input, dt, feedback);
        // No backend in the preview: the request has nowhere to go.
        (void)screen_.take_request();
    }

    void draw(app::Frame &frame) const override
    {
        screen_.draw(frame.scene, frame.backdrop);
    }

    std::span<const app::TourStep> tour() const override
    {
        return kTour;
    }

  private:
    ServersScreen screen_;
};

} // namespace

std::unique_ptr<app::Concept> make_ps5craft(app::Context &context)
{
    return std::make_unique<Ps5CraftPreview>(context);
}

} // namespace hui::concepts
