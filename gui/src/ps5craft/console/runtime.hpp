// PS5Craft native UI - runtime paths.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same policy as the v1 title: the data root is chosen by a write probe, not
// by assuming a mount exists, and the CA bundle is copied from the app folder
// into the writable data root once.

#pragma once

#include <string>

namespace ps5craft::console
{

std::string choose_data_root();
void configure_ca_bundle(const std::string &data_root);

} // namespace ps5craft::console
