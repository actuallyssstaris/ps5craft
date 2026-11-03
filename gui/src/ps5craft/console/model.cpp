// PS5Craft - backend snapshots to server-list rows.
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ps5craft/console/model.hpp"

namespace ps5craft::console
{

std::vector<screen::ServerRow> rows_from_backend(const Backend &backend)
{
    std::vector<screen::ServerRow> rows;
    for (const Json &snapshot : backend.list_snapshots())
    {
        screen::ServerRow row;
        row.id = snapshot["id"].as_string();
        row.name = snapshot["name"].as_string();
        row.software = snapshot["software"].as_string();
        row.version = snapshot["version"].as_string();
        row.state = snapshot["state"].as_string();
        row.last_log = snapshot["last_log"].as_string();
        row.memory_max_mib = snapshot["memory_max_mib"].as_int();
        row.uptime_ms = static_cast<std::uint64_t>(snapshot["uptime_ms"].as_number());
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace ps5craft::console
