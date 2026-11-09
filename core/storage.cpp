/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "storage.hpp"

#include "fs.hpp"

#include <mutex>

namespace ps5craft {

namespace {

const char *kProbe = ".ps5craft-write-probe";
const char *kRootPointer = "ps5craft-root";

/* The pointer cannot live inside the root it names, so it goes on the host
   /data partition first. */
std::string pointer_path() {
#if defined(__PROSPERO__)
    const char *dirs[] = {"/data", "/data/homebrew/PPSA90104", "/app_temp0"};
    for (const char *dir : dirs)
        if (fs_is_dir(dir)) return std::string(dir) + "/" + kRootPointer;
#else
    (void)kRootPointer;
#endif
    return std::string();
}

std::vector<StorageVolume> probe_volumes() {
    std::vector<StorageVolume> volumes;
#if defined(__PROSPERO__)
    auto add = [&volumes](const std::string &path, const std::string &label) {
        StorageVolume volume;
        volume.path = path;
        volume.label = label;
        volume.present = fs_is_dir(path);
        volume.writable = volume.present && storage_prepare(path);
        if (volume.present) {
            DiskSpace disk;
            if (fs_disk_space(path, disk)) {
                volume.total = disk.total;
                volume.free = disk.free;
            }
        }
        volumes.push_back(std::move(volume));
    };

    add("/data", "Internal");
    for (int i = 0; i < 8; ++i)
        add("/mnt/usb" + std::to_string(i), "USB " + std::to_string(i));
    add("/mnt/ext0", "Extended 0");
    add("/mnt/ext1", "Extended 1");
    add("/mnt/disc", "Disc");
#endif
    return volumes;
}

} // namespace

bool storage_prepare(const std::string &path) {
    if (path.empty() || !fs_mkdirs(path)) return false;
    const std::string probe = path + "/" + kProbe;
    const std::string token = "ps5craft";
    if (!fs_write_file(probe, token)) return false;
    std::string back;
    const bool ok = fs_read_file(probe, back) && back == token;
    fs_remove_tree(probe);
    return ok;
}

const std::vector<StorageVolume> &storage_volumes() {
    static std::once_flag once;
    static std::vector<StorageVolume> volumes;
    std::call_once(once, [] { volumes = probe_volumes(); });
    return volumes;
}

bool shadowmount_present() {
    return fs_is_dir("/data/shadowmount");
}

std::string storage_root_for(const std::string &volume) {
    for (const auto &entry : storage_volumes())
        if (entry.path == volume) return entry.path + "/ps5craft";
    return std::string();
}

std::string storage_saved_root() {
    const std::string path = pointer_path();
    std::string text;
    if (path.empty() || !fs_read_file(path, text)) return std::string();
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    if (text.empty() || text[0] != '/' || !fs_is_dir(text)) return std::string();
    return text;
}

bool storage_save_root(const std::string &root) {
    if (root.empty() || root[0] != '/') return false;
    if (!storage_prepare(root)) return false;
    const std::string path = pointer_path();
    if (path.empty()) return false;
    return fs_write_file(path, root + "\n");
}

} // namespace ps5craft
