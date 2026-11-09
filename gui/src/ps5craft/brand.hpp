// PS5Craft - shared colours and the logo mark for the console UI.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The palette comes from the title icon: slate, silver and charcoal bars with
// a lime status light. The mark is drawn from primitives instead of a bitmap,
// so it stays sharp at any size and needs no PNG decoder in the title.
#pragma once

#include "gfx/draw_list.hpp"
#include "ui/fonts.hpp"

namespace ps5craft::brand
{

using hui::gfx::Color;
using hui::gfx::DrawList;
using hui::gfx::Rect;

const Color kInk = Color::rgb(0xeef1ec);
const Color kDim = Color::rgb(0xb9c0b6);
const Color kFaint = Color::rgb(0x8b938a);
const Color kAccent = Color::rgb(0xb3d948);
const Color kAccentDeep = Color::rgb(0x7fa82a);
const Color kOk = Color::rgb(0xb3d948);
const Color kWarn = Color::rgb(0xe6c14f);
const Color kBad = Color::rgb(0xff7b6b);
const Color kPanelTop = Color::rgb(0x232a22);
const Color kPanelLow = Color::rgb(0x141813);
const Color kRowTop = Color::rgb(0x1e241d);
const Color kRowLow = Color::rgb(0x171c16);
const Color kFocusTop = Color::rgb(0x2c352a);
const Color kFocusLow = Color::rgb(0x1b211a);
const Color kLine = Color::rgb(0xffffff, 0.08f);
const Color kClear = Color::rgb(0x000000, 0.0f);
const Color kBackTop = Color::rgb(0x1b1f1b);
const Color kBackLow = Color::rgb(0x0a0c0a);

// Icon colours, kept exact so the mark reads as the same logo.
const Color kSlate = Color::rgb(0x666e71);
const Color kSilver = Color::rgb(0xc7cbca);
const Color kCharcoal = Color::rgb(0x2e352e);

// Three stacked server bars with the status lights. r is the square the mark
// occupies; nothing is drawn outside it.
inline void draw_logo(DrawList &scene, const Rect &r)
{
    const float bar_h = r.h * 0.25f;
    const float gap = r.h * 0.075f;
    const float radius = bar_h * 0.42f;
    const float dot_x = r.x + r.w * 0.15f;
    const float dot_r = r.h * 0.045f;

    const Color bars[3] = {kSlate, kSilver, kCharcoal};
    for (int i = 0; i < 3; ++i)
    {
        const float y = r.y + static_cast<float>(i) * (bar_h + gap);
        const Rect bar{r.x, y, r.w, bar_h};
        scene.rounded_rect(bar, radius, bars[i]);

        if (i > 0)
        {
            scene.circle(dot_x, bar.cy(), dot_r, kAccent);
            // The vents are part of the icon, but at header size they would
            // turn into noise, so they only appear on a large mark.
            if (r.h >= 56.0f)
            {
                const Color vents = i == 1 ? kCharcoal.with_alpha(0.55f)
                                           : Color::rgb(0x8b938a).with_alpha(0.75f);
                for (int row = 0; row < 2; ++row)
                {
                    for (int col = 0; col < 5 - row; ++col)
                    {
                        const float x = r.x + r.w * (0.5f + 0.075f * static_cast<float>(col) +
                                                     0.04f * static_cast<float>(row));
                        const float y2 = bar.cy() +
                                         (static_cast<float>(row) - 0.5f) * bar_h * 0.28f;
                        scene.circle(x, y2, r.h * 0.012f, vents);
                    }
                }
            }
        }
    }
}

// Logo plus the small uppercase wordmark that heads every screen.
inline void draw_header(DrawList &scene, const hui::ui::FontRef &font, float x, float baseline_y,
                        Color color)
{
    const float size = 40.0f;
    draw_logo(scene, {x, baseline_y - size + 11.0f, size, size});
    hui::ui::text(scene, font, "PS5CRAFT", x + size + 16.0f, baseline_y, 15.0f, color,
                  hui::gfx::Align::left, 2.6f);
}

} // namespace ps5craft::brand
