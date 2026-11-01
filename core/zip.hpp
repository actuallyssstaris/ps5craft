/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal ZIP writer (STORE method, no compression) and reader.
   Produces standard .zip files readable by any tool. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps5craft {

struct ZipEntry {
    std::string name;       /* path within archive, forward slashes */
    std::string data;       /* file contents */
    bool directory = false;
};

/* Write a ZIP archive to out. Returns true on success. */
bool zip_write(const std::string &out_path,
               const std::vector<ZipEntry> &entries);

/* List entries in a ZIP archive (names only). */
std::vector<std::string> zip_list(const std::string &zip_path);

/* Extract a ZIP archive to dest_dir. Returns true on success. */
bool zip_extract(const std::string &zip_path, const std::string &dest_dir);

} // namespace ps5craft
