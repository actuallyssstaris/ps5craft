// PS5Craft - backend snapshots to server-list rows.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console app owns one Backend. This is the only place the UI touches its
// JSON snapshot shape, so a backend change breaks here and not in the drawing
// code.

#pragma once

#include "ps5craft/core/backend.hpp"
#include "ps5craft/servers_screen.hpp"

#include <vector>

namespace ps5craft::console
{

std::vector<screen::ServerRow> rows_from_backend(const Backend &backend);

} // namespace ps5craft::console
