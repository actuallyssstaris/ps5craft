// PS5Craft - Server software install screen for the native console UI.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Caller resolves versions and runs the download; the screen only draws and
// handles input. Software that has not been tested cannot be selected.

#pragma once

#include "core/input.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ps5craft::screen
{

struct InstallSoftware
{
    std::string id;
    std::string name;
    std::string note;
    bool supported = false;
};

struct InstallRequest
{
    enum class Kind : std::uint8_t
    {
        none,
        load_versions,
        install,
        back,
    };

    Kind kind = Kind::none;
    std::string software;
    std::string version;
};

class InstallScreen
{
  public:
    explicit InstallScreen(const hui::ui::Fonts &fonts);

    void set_software(std::vector<InstallSoftware> software);
    void set_versions(const std::string &software, std::vector<std::string> versions,
                      const std::string &message);
    // Progress of the running install, if any.
    void set_install(bool active, bool done, bool failed, std::uint64_t received,
                     std::uint64_t total, const std::string &version,
                     const std::string &error);

    void update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::gfx::DrawList &scene, hui::gfx::BackdropSpec &backdrop) const;

    InstallRequest take_request();

  private:
    struct VersionList
    {
        std::vector<std::string> versions;
        std::string message;
        bool loaded = false;
    };

    void move(int delta, hui::ui::Feedback &feedback);
    void choose(hui::ui::Feedback &feedback);
    const InstallSoftware *selected_software() const;
    const VersionList *selected_versions() const;

    const hui::ui::Fonts &fonts_;
    std::vector<InstallSoftware> software_;
    std::unordered_map<std::string, VersionList> versions_;
    std::size_t software_sel_ = 0;
    std::size_t version_sel_ = 0;
    std::size_t top_ = 0;
    int pane_ = 0; // 0 software, 1 versions
    float scroll_px_ = 0.0f;
    float time_ = 0.0f;
    bool install_active_ = false;
    bool install_done_ = false;
    bool install_failed_ = false;
    std::uint64_t received_ = 0;
    std::uint64_t total_ = 0;
    std::string install_version_;
    std::string install_error_;
    InstallRequest request_;
};

} // namespace ps5craft::screen
