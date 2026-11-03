// PS5Craft - Server list screen for the native console UI.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ps5craft/servers_screen.hpp"

#include "audio/cues.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>

namespace ps5craft::screen
{

namespace
{

using hui::gfx::Align;
using hui::gfx::Color;
using hui::gfx::Rect;
using hui::ui::text;

// ---- the design language --------------------------------------------------

const Color kInk = Color::rgb(0xf4f1ff);
const Color kDim = Color::rgb(0xb8b0d1);
const Color kFaint = Color::rgb(0x8c87a8);
const Color kAccent = Color::rgb(0xbaa6ff);
const Color kAccentDeep = Color::rgb(0x8c6bff);
const Color kOk = Color::rgb(0x5cf2a0);
const Color kWarn = Color::rgb(0xffcf7a);
const Color kBad = Color::rgb(0xff6b6b);
const Color kPanelTop = Color::rgb(0x171735);
const Color kPanelLow = Color::rgb(0x0b0b1e);
const Color kRowTop = Color::rgb(0x14142c);
const Color kRowLow = Color::rgb(0x0c0c20);
const Color kLine = Color::rgb(0xffffff, 0.08f);
const Color kClear = Color::rgb(0x000000, 0.0f);

constexpr float kMargin = 96.0f;
constexpr float kListX = kMargin;
constexpr float kListW = 1152.0f;
constexpr float kListY = 236.0f;
constexpr float kListH = 788.0f;
constexpr float kRowH = 96.0f;
constexpr float kRowGap = 12.0f;
constexpr float kRowPitch = kRowH + kRowGap;
constexpr float kRowRadius = 16.0f;
constexpr float kPanelX = 1300.0f;
constexpr float kPanelW = 524.0f;
constexpr float kPanelRadius = 22.0f;

constexpr float kHeaderLabelY = 92.0f;
constexpr float kHeaderTitleY = 166.0f;
constexpr float kHeaderSubY = 206.0f;

constexpr float kFooterY = 1052.0f;

constexpr std::size_t kVisibleRows = 7;

bool active(std::string_view state)
{
    return state == "running" || state == "starting";
}

bool busy(std::string_view state)
{
    return state == "stopping";
}

Color state_color(std::string_view state)
{
    if (state == "running")
        return kOk;
    if (state == "starting" || state == "stopping")
        return kWarn;
    if (state == "crashed" || state == "error")
        return kBad;
    return kFaint;
}

std::string upper(std::string_view value)
{
    std::string out(value);
    for (char &c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return out;
}

// Cuts a line to fit width, with three dots. The font is proportional, so the
// cut point is found by measurement, not by character count.
std::string fit(const hui::ui::FontRef &font, std::string_view value, float size, float width)
{
    if (font.measure(value, size) <= width)
        return std::string(value);
    std::string out(value);
    while (!out.empty() && font.measure(out + "...", size) > width)
        out.pop_back();
    return out + "...";
}

std::string format_uptime(std::uint64_t milliseconds)
{
    const std::uint64_t seconds = milliseconds / 1000;
    const std::uint64_t hours = seconds / 3600;
    const std::uint64_t minutes = (seconds / 60) % 60;
    char buffer[32];
    if (hours > 0)
        std::snprintf(buffer, sizeof(buffer), "%lluh %02llum",
                      static_cast<unsigned long long>(hours),
                      static_cast<unsigned long long>(minutes));
    else
        std::snprintf(buffer, sizeof(buffer), "%llum %02llus",
                      static_cast<unsigned long long>(minutes),
                      static_cast<unsigned long long>(seconds % 60));
    return buffer;
}

std::string memory_label(const ServerRow &row)
{
    if (row.memory_max_mib <= 0)
        return {};
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld MiB",
                  static_cast<long long>(row.memory_max_mib));
    return buffer;
}

void label(hui::gfx::DrawList &scene, const hui::ui::FontRef &font, std::string_view value,
           float x, float y, Color color)
{
    text(scene, font, upper(value), x, y, 15.0f, color, Align::left, 2.6f);
}

void badge(hui::gfx::DrawList &scene, const hui::ui::Fonts &fonts, const Rect &rect,
           std::string_view state)
{
    const Color color = state_color(state);
    scene.rounded_rect(rect, rect.h * 0.5f, color.with_alpha(0.14f));
    text(scene, fonts.semibold, upper(state), rect.cx(), rect.cy() + 5.0f, 15.0f, color,
         Align::center, 2.0f);
}

void draw_empty(hui::gfx::DrawList &scene, const hui::ui::Fonts &fonts)
{
    const Rect box{kListX, kListY, kListW, kListH};
    scene.gradient_rect(box, kPanelRadius, kPanelTop, kPanelLow);
    scene.bordered_rect(box, kPanelRadius, kClear, 1.0f, kLine);

    text(scene, fonts.display, "No servers yet", box.cx(), box.cy() - 42.0f, 46.0f, kInk,
         Align::center);
    text(scene, fonts.regular, "Create one in the PS5Craft dashboard, then come back.",
         box.cx(), box.cy() + 8.0f, 22.0f, kDim, Align::center);
    text(scene, fonts.mono, "http://<console-ip>:8080", box.cx(), box.cy() + 52.0f, 22.0f,
         kAccent, Align::center);
}

} // namespace

ServersScreen::ServersScreen(const hui::ui::Fonts &fonts) : fonts_(fonts) {}

void ServersScreen::set_rows(std::vector<ServerRow> rows)
{
    const std::string previous = rows_.empty() ? std::string() : rows_[selected_].id;
    rows_ = std::move(rows);
    selected_ = 0;
    if (!previous.empty())
    {
        const auto found = std::find_if(rows_.begin(), rows_.end(),
                                        [&](const ServerRow &row) { return row.id == previous; });
        if (found != rows_.end())
            selected_ = static_cast<std::size_t>(found - rows_.begin());
    }
    clamp_selection();
    ensure_visible();
    scroll_px_ = static_cast<float>(top_) * kRowPitch;
    focus_px_ = static_cast<float>(selected_) * kRowPitch;
}

void ServersScreen::clamp_selection()
{
    if (rows_.empty())
    {
        selected_ = 0;
        top_ = 0;
        return;
    }
    selected_ = std::min(selected_, rows_.size() - 1);
    const std::size_t max_top = rows_.size() > kVisibleRows ? rows_.size() - kVisibleRows : 0;
    top_ = std::min(top_, max_top);
}

void ServersScreen::ensure_visible()
{
    if (rows_.empty())
        return;
    if (selected_ < top_)
        top_ = selected_;
    else if (selected_ >= top_ + kVisibleRows)
        top_ = selected_ - kVisibleRows + 1;
    const std::size_t max_top = rows_.size() > kVisibleRows ? rows_.size() - kVisibleRows : 0;
    top_ = std::min(top_, max_top);
}

void ServersScreen::move_selection(int delta, hui::ui::Feedback &feedback)
{
    if (rows_.empty())
    {
        feedback.play(hui::audio::Cue::error);
        return;
    }
    const int count = static_cast<int>(rows_.size());
    int next = static_cast<int>(selected_) + delta;
    if (next < 0)
    {
        next = 0;
        feedback.play(hui::audio::Cue::error);
    }
    else if (next >= count)
    {
        next = count - 1;
        feedback.play(hui::audio::Cue::error);
    }
    if (static_cast<std::size_t>(next) != selected_)
    {
        selected_ = static_cast<std::size_t>(next);
        ensure_visible();
        feedback.play(hui::audio::Cue::focus, 1.0f, (static_cast<float>(selected_) / count) - 0.5f);
    }
}

void ServersScreen::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    time_ += dt;

    if (input.nav == hui::Direction::down)
        move_selection(1, feedback);
    else if (input.nav == hui::Direction::up)
        move_selection(-1, feedback);
    else if (input.is_pressed(hui::Action::page_next))
        move_selection(static_cast<int>(kVisibleRows), feedback);
    else if (input.is_pressed(hui::Action::page_prev))
        move_selection(-static_cast<int>(kVisibleRows), feedback);

    if (input.is_pressed(hui::Action::confirm) && !rows_.empty())
    {
        const ServerRow &row = rows_[selected_];
        if (busy(row.state))
        {
            feedback.play(hui::audio::Cue::error);
        }
        else
        {
            const bool is_active = active(row.state);
            request_.kind = is_active ? RequestKind::stop : RequestKind::start;
            request_.id = row.id;
            feedback.play(is_active ? hui::audio::Cue::back : hui::audio::Cue::launch);
            feedback.rumble(0.25f, 0.08f);
        }
    }

    // Animate the highlight and the scroll to their targets. Both are plain
    // exponential approaches, frame-rate independent.
    const float focus_target = static_cast<float>(selected_) * kRowPitch;
    const float scroll_target = static_cast<float>(top_) * kRowPitch;
    const float focus_k = 1.0f - std::exp(-18.0f * dt);
    const float scroll_k = 1.0f - std::exp(-14.0f * dt);
    focus_px_ += (focus_target - focus_px_) * focus_k;
    scroll_px_ += (scroll_target - scroll_px_) * scroll_k;
}

Request ServersScreen::take_request()
{
    Request request = request_;
    request_ = {};
    return request;
}

void ServersScreen::draw(hui::gfx::DrawList &scene, hui::gfx::BackdropSpec &backdrop) const
{
    backdrop.mode = hui::gfx::BackdropMode::gradient;
    backdrop.colors[0] = Color::rgb(0x08091a);
    backdrop.colors[1] = Color::rgb(0x030307);
    backdrop.colors[2] = Color::rgb(0x8c6bff);
    backdrop.params[0] = 0.18f;
    backdrop.params[1] = 0.12f;
    backdrop.params[2] = 0.22f;
    backdrop.time = time_;

    label(scene, fonts_.semibold, "PS5CRAFT", kMargin, kHeaderLabelY, kAccent);
    text(scene, fonts_.display, "Servers", kMargin, kHeaderTitleY, 64.0f, kInk);

    int running_count = 0;
    for (const ServerRow &row : rows_)
        if (row.state == "running")
            ++running_count;

    if (!rows_.empty())
    {
        char summary[64];
        std::snprintf(summary, sizeof(summary), "%zu SERVERS  /  %d RUNNING", rows_.size(), running_count);
        text(scene, fonts_.mono, summary, kListX + kListW, kHeaderSubY, 19.0f, kDim, Align::right,
             1.0f);
    }

    if (rows_.empty())
    {
        draw_empty(scene, fonts_);
        text(scene, fonts_.regular, "The dashboard stays available while the console runs.",
             kListX, kFooterY, 20.0f, kFaint);
        return;
    }

    const Rect viewport{kListX, kListY, kListW, kListH};

    // The highlight travels between rows, so the eye can follow the selection
    // instead of re-finding it.
    const float highlight_y = kListY + focus_px_ - scroll_px_;
    const Rect highlight{kListX + 4.0f, highlight_y + 4.0f, kListW - 8.0f, kRowH - 8.0f};
    scene.glow(highlight, kRowRadius, 26.0f, kAccentDeep.with_alpha(0.20f));

    scene.push_clip(viewport);
    for (std::size_t i = 0; i < rows_.size(); ++i)
    {
        const float y = kListY + static_cast<float>(i) * kRowPitch - scroll_px_;
        if (y > kListY + kListH || y + kRowH < kListY)
            continue;

        const ServerRow &row = rows_[i];
        const bool focused = i == selected_;
        const Rect rect{kListX, y, kListW, kRowH};

        if (focused)
        {
            scene.gradient_rect(rect, kRowRadius, Color::rgb(0x262452), Color::rgb(0x171735));
            scene.bordered_rect(rect, kRowRadius, kClear, 1.5f, kAccent.with_alpha(0.55f));
            scene.rounded_rect({rect.x, rect.y + 22.0f, 4.0f, rect.h - 44.0f}, 2.0f, kAccent);
        }
        else
        {
            scene.gradient_rect(rect, kRowRadius, kRowTop, kRowLow);
            scene.bordered_rect(rect, kRowRadius, kClear, 1.0f, kLine);
        }

        const Color state = state_color(row.state);
        scene.circle(rect.x + 36.0f, rect.cy(), 6.0f, state);
        if (row.state == "running")
            scene.ring(rect.x + 36.0f, rect.cy(), 13.0f, 1.5f, state.with_alpha(0.30f));

        const float text_x = rect.x + 66.0f;
        const float name_width = rect.w - 66.0f - 190.0f;
        text(scene, fonts_.semibold, fit(fonts_.semibold, row.name, 29.0f, name_width), text_x,
             rect.y + 44.0f, 29.0f, kInk);

        std::string meta;
        if (!row.software.empty())
            meta += row.software;
        if (!row.version.empty())
        {
            if (!meta.empty())
                meta += "  ";
            meta += row.version;
        }
        if (!row.last_log.empty())
        {
            if (!meta.empty())
                meta += "  /  ";
            meta += fit(fonts_.regular, row.last_log, 18.0f, name_width - 180.0f);
        }
        text(scene, fonts_.regular, meta, text_x, rect.y + 73.0f, 18.0f, kDim);

        badge(scene, fonts_, {rect.x + rect.w - 176.0f, rect.y + 31.0f, 152.0f, 34.0f},
              row.state);
    }
    scene.pop_clip();

    // ---- the detail panel -------------------------------------------------

    const Rect panel{kPanelX, kListY, kPanelW, kListH};
    scene.gradient_rect(panel, kPanelRadius, kPanelTop, kPanelLow);
    scene.bordered_rect(panel, kPanelRadius, kClear, 1.0f, kLine);

    const ServerRow &row = rows_[selected_];
    label(scene, fonts_.semibold, "Selected server", panel.x + 40.0f, panel.y + 54.0f, kAccent);

    const std::string title = fit(fonts_.display, row.name, 40.0f, panel.w - 80.0f);
    text(scene, fonts_.display, title, panel.x + 40.0f, panel.y + 112.0f, 40.0f, kInk);
    badge(scene, fonts_, {panel.x + 40.0f, panel.y + 132.0f, 150.0f, 34.0f}, row.state);

    scene.line(panel.x + 40.0f, panel.y + 196.0f, panel.x + panel.w - 40.0f, panel.y + 196.0f,
               1.0f, kLine);

    const auto fact = [&](float y, std::string_view name, const std::string &value)
    {
        label(scene, fonts_.semibold, name, panel.x + 40.0f, y, kFaint);
        text(scene, fonts_.regular, value, panel.x + panel.w - 40.0f, y, 20.0f, kInk,
             Align::right);
    };

    fact(panel.y + 244.0f, "Software", row.software.empty() ? "-" : row.software);
    fact(panel.y + 288.0f, "Version", row.version.empty() ? "-" : row.version);
    fact(panel.y + 332.0f, "Memory", memory_label(row).empty() ? "-" : memory_label(row));
    fact(panel.y + 376.0f, "Uptime",
         row.state == "running" ? format_uptime(row.uptime_ms) : "-");

    label(scene, fonts_.semibold, "Last log", panel.x + 40.0f, panel.y + 440.0f, kFaint);
    if (row.last_log.empty())
    {
        text(scene, fonts_.regular, "No output yet.", panel.x + 40.0f, panel.y + 478.0f, 20.0f,
             kFaint);
    }
    else
    {
        const std::string line = fit(fonts_.mono, row.last_log, 17.0f, panel.w - 80.0f);
        text(scene, fonts_.mono, line, panel.x + 40.0f, panel.y + 478.0f, 17.0f, kDim);
    }

    text(scene, fonts_.regular, "Files, backups and installs are on the dashboard.",
         panel.x + 40.0f, panel.y + panel.h - 176.0f, 19.0f, kFaint);

    // The one action the player can take here, spelled out.
    const bool is_active = active(row.state);
    const bool is_busy = busy(row.state);
    const Color action_color = is_busy ? kFaint : (is_active ? kBad : kOk);
    const Rect action{panel.x + 40.0f, panel.y + panel.h - 118.0f, panel.w - 80.0f, 64.0f};
    scene.rounded_rect(action, 14.0f, action_color.with_alpha(0.13f));
    scene.bordered_rect(action, 14.0f, kClear, 1.0f, action_color.with_alpha(0.40f));
    const char *action_label = is_busy ? "Stopping..." : (is_active ? "Stop server" : "Start server");
    text(scene, fonts_.semibold, action_label, action.cx(), action.cy() + 7.0f, 24.0f,
         action_color, Align::center);

    // ---- the footer -------------------------------------------------------

    text(scene, fonts_.regular, "D-pad  Move", kMargin, kFooterY, 20.0f, kFaint);
    text(scene, fonts_.regular, "Cross  Start / stop", kMargin + 190.0f, kFooterY, 20.0f, kFaint);
    text(scene, fonts_.regular, "L1 / R1  Page", kMargin + 460.0f, kFooterY, 20.0f, kFaint);
    text(scene, fonts_.regular, "Dashboard  :8080", kListX + kListW, kFooterY, 20.0f, kFaint,
         Align::right);
}

} // namespace ps5craft::screen
