// PS5Craft - Server log screen for the native console UI.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ps5craft/logs_screen.hpp"

#include "audio/cues.hpp"
#include "ps5craft/brand.hpp"

#include <algorithm>
#include <cmath>

namespace ps5craft::screen
{

namespace
{

using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::Rect;
using hui::ui::text;

// Palette and logo mark live in brand.hpp so every screen matches the icon.
using ps5craft::brand::kAccent;
using ps5craft::brand::kAccentDeep;
using ps5craft::brand::kBad;
using ps5craft::brand::kBackLow;
using ps5craft::brand::kBackTop;
using ps5craft::brand::kClear;
using ps5craft::brand::kDim;
using ps5craft::brand::kFaint;
using ps5craft::brand::kFocusLow;
using ps5craft::brand::kFocusTop;
using ps5craft::brand::kInk;
using ps5craft::brand::kLine;
using ps5craft::brand::kOk;
using ps5craft::brand::kPanelLow;
using ps5craft::brand::kPanelTop;
using ps5craft::brand::kRowLow;
using ps5craft::brand::kRowTop;
using ps5craft::brand::kWarn;

constexpr float kMargin = 96.0f;
constexpr float kBoxY = 236.0f;
constexpr float kBoxH = 788.0f;
constexpr float kPad = 26.0f;
constexpr float kLineH = 26.0f;
constexpr float kFooterY = 1052.0f;
constexpr std::size_t kVisibleLines = 27;

} // namespace

LogsScreen::LogsScreen(const hui::ui::Fonts &fonts) : fonts_(fonts) {}

void LogsScreen::set_text(const std::string &name, const std::string &text)
{
    if (name == name_ && text == text_)
        return;
    name_ = name;
    text_ = text;
    dirty_ = true;
}

void LogsScreen::rebuild()
{
    lines_.clear();
    std::string current;
    for (char c : text_)
    {
        if (c == '\n')
        {
            lines_.push_back(current);
            current.clear();
        }
        else if (c == '\t')
        {
            /* Server logs indent mod lists with tabs. The UI font has no tab
               glyph, so a raw tab would draw as a box. */
            current += "    ";
        }
        else if (c != '\r')
        {
            current += c;
        }
    }
    if (!current.empty())
        lines_.push_back(current);
    if (lines_.empty())
        lines_.push_back("No output yet.");
    dirty_ = false;
    if (follow_)
        top_ = lines_.size() > kVisibleLines ? lines_.size() - kVisibleLines : 0;
    else
        top_ = std::min(top_, lines_.size() > kVisibleLines ? lines_.size() - kVisibleLines : 0);
}

void LogsScreen::scroll_by(int delta, hui::ui::Feedback &feedback)
{
    if (lines_.empty())
        return;
    const std::size_t max_top = lines_.size() > kVisibleLines ? lines_.size() - kVisibleLines : 0;
    long long next = static_cast<long long>(top_) + delta;
    if (next < 0)
    {
        next = 0;
        feedback.play(hui::audio::Cue::error);
    }
    else if (next > static_cast<long long>(max_top))
    {
        next = static_cast<long long>(max_top);
        feedback.play(hui::audio::Cue::error);
    }
    if (static_cast<std::size_t>(next) != top_)
    {
        top_ = static_cast<std::size_t>(next);
        follow_ = top_ == max_top;
        feedback.play(hui::audio::Cue::focus);
    }
}

void LogsScreen::scroll_to_end(hui::ui::Feedback &feedback)
{
    follow_ = true;
    const std::size_t max_top = lines_.size() > kVisibleLines ? lines_.size() - kVisibleLines : 0;
    if (top_ != max_top)
    {
        top_ = max_top;
        feedback.play(hui::audio::Cue::focus);
    }
}

bool LogsScreen::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    time_ += dt;
    if (dirty_)
        rebuild();

    if (input.is_pressed(hui::Action::back))
    {
        feedback.play(hui::audio::Cue::back);
        return true;
    }
    if (input.nav == hui::Direction::down)
        scroll_by(1, feedback);
    else if (input.nav == hui::Direction::up)
        scroll_by(-1, feedback);
    else if (input.is_pressed(hui::Action::page_next))
        scroll_by(static_cast<int>(kVisibleLines), feedback);
    else if (input.is_pressed(hui::Action::page_prev))
        scroll_by(-static_cast<int>(kVisibleLines), feedback);
    else if (input.is_pressed(hui::Action::jump_next))
        scroll_to_end(feedback);

    const float target = static_cast<float>(top_) * kLineH;
    const float k = 1.0f - std::exp(-16.0f * dt);
    scroll_px_ += (target - scroll_px_) * k;
    return false;
}

void LogsScreen::draw(hui::gfx::DrawList &scene, hui::gfx::BackdropSpec &backdrop) const
{
    backdrop.mode = hui::gfx::BackdropMode::gradient;
    backdrop.colors[0] = kBackTop;
    backdrop.colors[1] = kBackLow;
    backdrop.colors[2] = kAccentDeep;
    backdrop.params[0] = 0.10f;
    backdrop.params[1] = 0.06f;
    backdrop.params[2] = 0.13f;
    backdrop.time = time_;

    brand::draw_header(scene, fonts_.semibold, kMargin, 92.0f, kAccent);
    text(scene, fonts_.display, "Logs", kMargin, 166.0f, 64.0f, kInk);
    if (!name_.empty())
        text(scene, fonts_.regular, name_, kMargin, 206.0f, 22.0f, kDim);

    const Rect box{kMargin, kBoxY, 1920.0f - kMargin * 2.0f, kBoxH};
    scene.gradient_rect(box, 22.0f, kPanelTop, kPanelLow);
    scene.bordered_rect(box, 22.0f, kClear, 1.0f, kLine);

    scene.push_clip({box.x + kPad, box.y + kPad, box.w - kPad * 2.0f, box.h - kPad * 2.0f});
    for (std::size_t i = 0; i < lines_.size(); ++i)
    {
        const float y = box.y + kPad + static_cast<float>(i) * kLineH - scroll_px_;
        if (y > box.y + box.h - kPad || y + kLineH < box.y + kPad)
            continue;
        text(scene, fonts_.mono, lines_[i], box.x + kPad, y + 18.0f, 19.0f, kDim);
    }
    scene.pop_clip();

    if (!follow_)
    {
        text(scene, fonts_.mono, "SCROLLED BACK", box.x + box.w - kPad, box.y + 34.0f, 15.0f,
             kAccent, Align::right, 2.0f);
    }

    text(scene, fonts_.regular, "D-pad  Scroll", kMargin, kFooterY, 20.0f, kFaint);
    text(scene, fonts_.regular, "L1 / R1  Page", kMargin + 200.0f, kFooterY, 20.0f, kFaint);
    text(scene, fonts_.regular, "R2  Jump to end", kMargin + 430.0f, kFooterY, 20.0f, kFaint);
    text(scene, fonts_.regular, "Circle  Back", 1920.0f - kMargin, kFooterY, 20.0f, kFaint,
         Align::right);
}

} // namespace ps5craft::screen
