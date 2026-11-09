// PS5Craft - Server log screen for the native console UI.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shows the tail of one server's output. Caller passes the same text the
// dashboard reads from /api/instances/<id>/logs.

#pragma once

#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace ps5craft::screen
{

class LogsScreen
{
  public:
    explicit LogsScreen(const hui::ui::Fonts &fonts);

    // Name is the server title, text the whole tail. Sticks to the end until
    // the player scrolls up.
    void set_text(const std::string &name, const std::string &text);

    // Returns true when the player asked to go back.
    bool update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::gfx::DrawList &scene, hui::gfx::BackdropSpec &backdrop) const;

  private:
    void rebuild();
    void scroll_by(int delta, hui::ui::Feedback &feedback);
    void scroll_to_end(hui::ui::Feedback &feedback);

    const hui::ui::Fonts &fonts_;
    std::string name_;
    std::string text_;
    std::vector<std::string> lines_;
    std::size_t top_ = 0;
    float scroll_px_ = 0.0f;
    float time_ = 0.0f;
    bool follow_ = true;
    bool dirty_ = true;
};

} // namespace ps5craft::screen
