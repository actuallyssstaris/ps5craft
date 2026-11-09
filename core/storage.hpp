/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Where PS5Craft may keep its data.
 *
 * ShadowMountPlus maps the host /data into homebrew titles as a writable
 * nullfs, and gives mounted USB, extended and disc volumes an alias under
 * /mnt. Without it /data inside the title is a private sandbox directory that
 * is discarded with the title. The probe below tells the two apart by writing
 * a file and reading it back.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps5craft {

struct StorageVolume {
    std::string path;   /* mount point, e.g. /data or /mnt/usb0 */
    std::string label;  /* short name for the UI */
    bool present = false;
    bool writable = false;
    std::uint64_t total = 0;
    std::uint64_t free = 0;
};

/* Internal storage first, then USB 0-7, ext 0-1 and the disc. Probed once per
   process; later calls return the same list. */
const std::vector<StorageVolume> &storage_volumes();

/* /data/shadowmount exists, so ShadowMountPlus is installed. */
bool shadowmount_present();

/* mkdirs the directory, then writes and reads back a probe file. */
bool storage_prepare(const std::string &path);

/* Directory under a mounted volume. Empty for a path that is not in the
   storage_volumes() list. */
std::string storage_root_for(const std::string &volume);

/* Root the user picked last time, empty when none was saved or it is gone. */
std::string storage_saved_root();

/* Remembers the pick across launches. The pointer lives on the host /data
   partition, outside the chosen root, so it survives a root change. */
bool storage_save_root(const std::string &root);

} // namespace ps5craft
