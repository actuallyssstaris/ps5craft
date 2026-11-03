// PS5Craft - Server list screen for the native console UI.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Portable: the screen owns layout, focus and input handling only. The caller
// supplies rows from the backend and performs the start/stop request it takes.
// The same class drives the console app and the host snapshot preview.

#pragma once

#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps5craft::screen
{

// One server as the list needs it. Empty optional fields are not drawn.
struct ServerRow
{
    std::string id;
    std::string name;
    std::string software;
    std::string version;
    std::string state; // stopped | starting | running | stopping | crashed | error
    std::string last_log;
    std::int64_t memory_max_mib = 0;
    std::uint64_t uptime_ms = 0;
};

enum class RequestKind : std::uint8_t
{
    none,
    start,
    stop,
};

struct Request
{
    RequestKind kind = RequestKind::none;
    std::string id;
};

class ServersScreen
{
  public:
    explicit ServersScreen(const hui::ui::Fonts &fonts);

    // Replaces the list. The selection stays on the same server id when it is
    // still present, otherwise it is clamped to the new size.
    void set_rows(std::vector<ServerRow> rows);

    void update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::gfx::DrawList &scene, hui::gfx::BackdropSpec &backdrop) const;

    // The action the player asked for; the caller performs it and refreshes.
    Request take_request();

    std::size_t selected() const
    {
        return selected_;
    }
    bool empty() const
    {
        return rows_.empty();
    }

  private:
    void move_selection(int delta, hui::ui::Feedback &feedback);
    void clamp_selection();
    void ensure_visible();

    const hui::ui::Fonts &fonts_;
    std::vector<ServerRow> rows_;
    std::size_t selected_ = 0;
    std::size_t top_ = 0;
    float scroll_px_ = 0.0f;
    float focus_px_ = 0.0f;
    float time_ = 0.0f;
    Request request_;
};

} // namespace ps5craft::screen
