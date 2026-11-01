/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Backup manager: create, list, restore, delete world/server backups.
   Backups are standard ZIP archives stored under
   <data_root>/backups/<instance_id>/. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fs.hpp"
#include "zip.hpp"
#include "../platform/process.hpp"

namespace ps5craft {

class Backend;
class Instance;

struct BackupInfo {
    std::string id;            /* timestamp-based id, also the file name without .zip */
    std::string file_name;     /* e.g. "20261101-193000.zip" */
    std::string path;          /* relative to backups root */
    std::uint64_t size = 0;
    std::int64_t modified = 0;
    std::string instance_id;
    bool full = false;         /* true = whole server dir, false = world only */
};

class BackupManager {
public:
    BackupManager(Sandbox &sandbox, Backend &backend);

    /* Create a backup of instance's world (or whole server dir if full=true).
       If the server is running, attempts save-off/save-all via stdin first.
       Returns the backup id on success, empty on failure. */
    std::string create(const std::string &instance_id, bool full = false);

    /* List backups for an instance, newest first. */
    std::vector<BackupInfo> list(const std::string &instance_id) const;

    /* Restore a backup: stops the instance, extracts over the world dir. */
    bool restore(const std::string &instance_id, const std::string &backup_id);

    /* Delete a backup. */
    bool remove(const std::string &instance_id, const std::string &backup_id);

    std::string backups_root(const std::string &instance_id) const;

private:
    Sandbox &sandbox_;
    Backend &backend_;

    std::string now_id() const;
    bool save_pause(Instance *inst);
    bool save_resume(Instance *inst);
    bool zip_directory(const std::string &dir, std::vector<ZipEntry> &entries);
};

} // namespace ps5craft
