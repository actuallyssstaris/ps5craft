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
    std::string data;       /* file contents (STORE) */
    bool directory = false;
};

/* Write a ZIP archive to out_path. Returns true on success. */
bool zip_write(const std::string &out_path,
               const std::vector<ZipEntry> &entries);

/* List file entries in a ZIP archive (names only, newest format STORE). */
std::vector<std::string> zip_list(const std::string &zip_path);

/* Read one entry's contents. Returns false when absent or unsupported. */
bool zip_read_entry(const std::string &zip_path, const std::string &name,
                    std::string &out, std::uint64_t max_bytes);

/* Extract a STORE-method ZIP archive to dest_dir. Rejects entry names that
   escape dest_dir. Returns true on success. */
bool zip_extract(const std::string &zip_path, const std::string &dest_dir);

} // namespace ps5craft
