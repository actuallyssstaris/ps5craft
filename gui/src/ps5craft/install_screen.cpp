// PS5Craft - Server software install screen for the native console UI.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ps5craft/install_screen.hpp"

#include "audio/cues.hpp"
#include "ps5craft/brand.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
constexpr float kTop = 236.0f;
constexpr float kHeight = 788.0f;
constexpr float kLeftX = kMargin;
constexpr float kLeftW = 760.0f;
constexpr float kRightX = 916.0f;
constexpr float kRightW = 908.0f;
constexpr float kSoftPitch = 96.0f;
constexpr float kSoftH = 84.0f;
constexpr float kVerPitch = 64.0f;
constexpr float kVerH = 54.0f;
constexpr float kFooterY = 1052.0f;
constexpr std::size_t kVisibleVersions = 8;

std::string upper(std::string_view value)
{
    std::string out(value);
    for (char &c : out)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return out;
}

void label(hui::gfx::DrawList &scene, const hui::ui::FontRef &font, std::string_view value,
           float x, float y, Color color)
{
    text(scene, font, upper(value), x, y, 15.0f, color, Align::left, 2.6f);
}

std::string fit(const hui::ui::FontRef &font, std::string_view value, float size, float width)
{
    if (font.measure(value, size) <= width)
        return std::string(value);
    std::string out(value);
    while (!out.empty() && font.measure(out + "...", size) > width)
        out.pop_back();
    return out + "...";
}

} // namespace

InstallScreen::InstallScreen(const hui::ui::Fonts &fonts) : fonts_(fonts) {}

void InstallScreen::set_software(std::vector<InstallSoftware> software)
{
    const std::string previous = selected_software() ? selected_software()->id : std::string();
    software_ = std::move(software);
    software_sel_ = 0;
    for (std::size_t i = 0; i < software_.size(); ++i)
        if (software_[i].id == previous)
            software_sel_ = i;
    if (software_.empty())
        software_sel_ = 0;
    if (version_sel_ >= (selected_versions() ? selected_versions()->versions.size() : 0))
        version_sel_ = 0;
}

void InstallScreen::set_versions(const std::string &software, std::vector<std::string> versions,
                                 const std::string &message)
{
    VersionList list;
    list.versions = std::move(versions);
    list.message = message;
    list.loaded = true;
    versions_[software] = std::move(list);
    if (selected_software() && selected_software()->id == software)
        version_sel_ = 0;
}

void InstallScreen::set_install(bool active, bool done, bool failed, std::uint64_t received,
                                std::uint64_t total, const std::string &version,
                                const std::string &error)
{
    install_active_ = active;
    install_done_ = done;
    install_failed_ = failed;
    received_ = received;
    total_ = total;
    install_version_ = version;
    install_error_ = error;
}

const InstallSoftware *InstallScreen::selected_software() const
{
    if (software_.empty() || software_sel_ >= software_.size())
        return nullptr;
    return &software_[software_sel_];
}

const InstallScreen::VersionList *InstallScreen::selected_versions() const
{
    const InstallSoftware *software = selected_software();
    if (!software)
        return nullptr;
    const auto it = versions_.find(software->id);
    return it == versions_.end() ? nullptr : &it->second;
}

void InstallScreen::move(int delta, hui::ui::Feedback &feedback)
{
    if (pane_ == 0)
    {
        if (software_.empty())
        {
            feedback.play(hui::audio::Cue::error);
            return;
        }
        const int count = static_cast<int>(software_.size());
        int next = static_cast<int>(software_sel_) + delta;
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
        if (static_cast<std::size_t>(next) != software_sel_)
        {
            software_sel_ = static_cast<std::size_t>(next);
            version_sel_ = 0;
            top_ = 0;
            feedback.play(hui::audio::Cue::focus);
        }
        return;
    }

    const VersionList *list = selected_versions();
    if (!list || list->versions.empty())
    {
        feedback.play(hui::audio::Cue::error);
        return;
    }
    const int count = static_cast<int>(list->versions.size());
    int next = static_cast<int>(version_sel_) + delta;
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
    if (static_cast<std::size_t>(next) != version_sel_)
    {
        version_sel_ = static_cast<std::size_t>(next);
        if (version_sel_ < top_)
            top_ = version_sel_;
        else if (version_sel_ >= top_ + kVisibleVersions)
            top_ = version_sel_ - kVisibleVersions + 1;
        feedback.play(hui::audio::Cue::focus);
    }
}

void InstallScreen::choose(hui::ui::Feedback &feedback)
{
    const InstallSoftware *software = selected_software();
    if (!software)
    {
        feedback.play(hui::audio::Cue::error);
        return;
    }
    if (pane_ == 0)
    {
        if (!software->supported)
        {
            feedback.play(hui::audio::Cue::error);
            return;
        }
        pane_ = 1;
        top_ = 0;
        version_sel_ = 0;
        request_.kind = InstallRequest::Kind::load_versions;
        request_.software = software->id;
        feedback.play(hui::audio::Cue::launch);
        return;
    }

    const VersionList *list = selected_versions();
    if (!list || list->versions.empty() || install_active_)
    {
        feedback.play(hui::audio::Cue::error);
        return;
    }
    request_.kind = InstallRequest::Kind::install;
    request_.software = software->id;
    request_.version = list->versions[version_sel_];
    feedback.play(hui::audio::Cue::launch);
    feedback.rumble(0.25f, 0.08f);
}

void InstallScreen::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    time_ += dt;

    if (input.is_pressed(hui::Action::back))
    {
        if (pane_ == 1)
        {
            pane_ = 0;
            feedback.play(hui::audio::Cue::back);
        }
        else
        {
            request_.kind = InstallRequest::Kind::back;
            feedback.play(hui::audio::Cue::back);
        }
        return;
    }
    if (input.nav == hui::Direction::down)
        move(1, feedback);
    else if (input.nav == hui::Direction::up)
        move(-1, feedback);
    else if (input.nav == hui::Direction::left)
    {
        if (pane_ == 1)
        {
            pane_ = 0;
            feedback.play(hui::audio::Cue::focus);
        }
    }
    else if (input.nav == hui::Direction::right)
    {
        if (pane_ == 0 && selected_software() && selected_software()->supported)
        {
            pane_ = 1;
            feedback.play(hui::audio::Cue::focus);
        }
    }
    else if (input.is_pressed(hui::Action::page_next))
        move(static_cast<int>(kVisibleVersions), feedback);
    else if (input.is_pressed(hui::Action::page_prev))
        move(-static_cast<int>(kVisibleVersions), feedback);

    if (input.is_pressed(hui::Action::confirm))
        choose(feedback);

    const float target = static_cast<float>(top_) * kVerPitch;
    const float k = 1.0f - std::exp(-14.0f * dt);
    scroll_px_ += (target - scroll_px_) * k;
}

InstallRequest InstallScreen::take_request()
{
    InstallRequest request = request_;
    request_ = {};
    return request;
}

void InstallScreen::draw(hui::gfx::DrawList &scene, hui::gfx::BackdropSpec &backdrop) const
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
    text(scene, fonts_.display, "Install software", kMargin, 166.0f, 64.0f, kInk);
    text(scene, fonts_.regular, "Only tested server software can be installed here.",
         kMargin, 206.0f, 22.0f, kDim);

    // ---- software ---------------------------------------------------------

    const Rect left{kLeftX, kTop, kLeftW, kHeight};
    scene.gradient_rect(left, 22.0f, kPanelTop, kPanelLow);
    scene.bordered_rect(left, 22.0f, kClear, pane_ == 0 ? 1.5f : 1.0f,
                        pane_ == 0 ? kAccent.with_alpha(0.55f) : kLine);
    label(scene, fonts_.semibold, "Server software", left.x + 40.0f, left.y + 50.0f, kFaint);

    scene.push_clip({left.x + 12.0f, left.y + 76.0f, left.w - 24.0f, left.h - 96.0f});
    for (std::size_t i = 0; i < software_.size(); ++i)
    {
        const float y = left.y + 84.0f + static_cast<float>(i) * kSoftPitch;
        const Rect rect{left.x + 20.0f, y, left.w - 40.0f, kSoftH};
        const bool focused = pane_ == 0 && i == software_sel_;
        const bool enabled = software_[i].supported;
        if (focused)
        {
            scene.gradient_rect(rect, 14.0f, kFocusTop, kFocusLow);
            scene.bordered_rect(rect, 14.0f, kClear, 1.5f, kAccent.with_alpha(0.55f));
        }
        else
        {
            scene.gradient_rect(rect, 14.0f, kRowTop, kRowLow);
            scene.bordered_rect(rect, 14.0f, kClear, 1.0f, kLine);
        }
        const Color ink = enabled ? kInk : kFaint;
        text(scene, fonts_.semibold, software_[i].name, rect.x + 28.0f, rect.y + 36.0f, 26.0f,
             ink);
        text(scene, fonts_.regular,
             fit(fonts_.regular, software_[i].note, 17.0f, rect.w - 200.0f), rect.x + 28.0f,
             rect.y + 64.0f, 17.0f, enabled ? kDim : kFaint);
        if (!enabled)
            text(scene, fonts_.semibold, "UNTESTED", rect.x + rect.w - 24.0f, rect.y + 40.0f,
                 15.0f, kBad, Align::right, 2.0f);
    }
    scene.pop_clip();

    // ---- versions ---------------------------------------------------------

    const Rect right{kRightX, kTop, kRightW, kHeight};
    scene.gradient_rect(right, 22.0f, kPanelTop, kPanelLow);
    scene.bordered_rect(right, 22.0f, kClear, pane_ == 1 ? 1.5f : 1.0f,
                        pane_ == 1 ? kAccent.with_alpha(0.55f) : kLine);

    const InstallSoftware *software = selected_software();
    label(scene, fonts_.semibold, "Version", right.x + 40.0f, right.y + 50.0f, kFaint);

    const VersionList *list = selected_versions();
    if (!software)
    {
        text(scene, fonts_.regular, "No software list.", right.x + 40.0f, right.y + 110.0f,
             22.0f, kFaint);
    }
    else if (!software->supported)
    {
        text(scene, fonts_.regular, "This software is not tested on the console yet.",
             right.x + 40.0f, right.y + 110.0f, 22.0f, kFaint);
        text(scene, fonts_.regular, "Vanilla, Fabric and Paper are available.",
             right.x + 40.0f, right.y + 148.0f, 20.0f, kFaint);
    }
    else if (!list || !list->loaded)
    {
        text(scene, fonts_.regular, "Loading versions...", right.x + 40.0f, right.y + 110.0f,
             22.0f, kDim);
    }
    else if (list->versions.empty())
    {
        text(scene, fonts_.regular,
             list->message.empty() ? "No versions available." : list->message,
             right.x + 40.0f, right.y + 110.0f, 22.0f, kBad);
    }
    else
    {
        scene.push_clip({right.x + 20.0f, right.y + 76.0f, right.w - 40.0f, right.h - 300.0f});
        for (std::size_t i = 0; i < list->versions.size(); ++i)
        {
            const float y = right.y + 84.0f + static_cast<float>(i) * kVerPitch - scroll_px_;
            if (y > right.y + right.h - 230.0f || y + kVerH < right.y + 76.0f)
                continue;
            const Rect rect{right.x + 24.0f, y, right.w - 48.0f, kVerH};
            const bool focused = pane_ == 1 && i == version_sel_;
            if (focused)
            {
                scene.rounded_rect(rect, 12.0f, kAccent.with_alpha(0.18f));
                scene.bordered_rect(rect, 12.0f, kClear, 1.5f, kAccent.with_alpha(0.55f));
            }
            else
            {
                scene.rounded_rect(rect, 12.0f, kRowLow);
                scene.bordered_rect(rect, 12.0f, kClear, 1.0f, kLine);
            }
            text(scene, fonts_.semibold, list->versions[i], rect.x + 26.0f, rect.y + 34.0f,
                 22.0f, kInk);
        }
        scene.pop_clip();
    }

    // ---- install progress -------------------------------------------------

    const float status_y = right.y + right.h - 176.0f;
    scene.line(right.x + 40.0f, status_y - 26.0f, right.x + right.w - 40.0f, status_y - 26.0f,
               1.0f, kLine);

    if (install_active_ || install_done_)
    {
        const float pct = total_ > 0 ? std::min(1.0f, static_cast<float>(received_) /
                                                          static_cast<float>(total_))
                                     : 0.0f;        const Rect bar{right.x + 40.0f, status_y, right.w - 80.0f, 14.0f};
        scene.rounded_rect(bar, 7.0f, kLine);
        const Color fill = install_failed_ ? kBad : (install_done_ ? kOk : kAccent);
        scene.rounded_rect({bar.x, bar.y, std::max(14.0f, bar.w * pct), bar.h}, 7.0f, fill);

        char line[128];
        if (install_failed_)
            std::snprintf(line, sizeof(line), "FAILED  %s", install_error_.c_str());
        else if (install_done_)
            std::snprintf(line, sizeof(line), "INSTALLED  %s", install_version_.c_str());
        else
            std::snprintf(line, sizeof(line), "INSTALLING  %s  %d%%",
                          install_version_.c_str(), static_cast<int>(pct * 100.0f));
        text(scene, fonts_.mono, fit(fonts_.mono, line, 17.0f, right.w - 80.0f),
             right.x + 40.0f, status_y + 48.0f, 17.0f,
             install_failed_ ? kBad : (install_done_ ? kOk : kAccent));
    }
    else
    {
        text(scene, fonts_.regular, "Cross installs the selected version.",
             right.x + 40.0f, status_y + 8.0f, 20.0f, kFaint);
        text(scene, fonts_.regular, "The server must be stopped.",
             right.x + 40.0f, status_y + 40.0f, 20.0f, kFaint);
    }

    text(scene, fonts_.regular, "D-pad  Move", kMargin, kFooterY, 20.0f, kFaint);
    text(scene, fonts_.regular, "Left / Right  Pane", kMargin + 200.0f, kFooterY, 20.0f, kFaint);
    text(scene, fonts_.regular, "Cross  Select", kMargin + 440.0f, kFooterY, 20.0f, kFaint);
    text(scene, fonts_.regular, "Circle  Back", kMargin + 650.0f, kFooterY, 20.0f, kFaint);
    text(scene, fonts_.regular, "Dashboard  :8080", kLeftX + kLeftW + 60.0f + kRightW, kFooterY,
         20.0f, kFaint, Align::right);
}

} // namespace ps5craft::screen
