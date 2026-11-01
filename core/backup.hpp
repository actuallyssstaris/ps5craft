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

namespace ps5craft {

class Instance;

struct BackupInfo {
    std::string id;            /* timestamp-based id, file name without .zip */
    std::string file_name;     /* e.g. "20261101-193000.zip" */
    std::string path;          /* relative to the data root */
    std::uint64_t size = 0;
    std::int64_t modified = 0;
    std::string instance_id;
    bool full = false;         /* true = whole server dir, false = world only */
};

class BackupManager {
public:
    explicit BackupManager(Sandbox &sandbox);

    /* Create a backup of inst's world (or whole server dir if full=true).
       If the server is running, auto-save is paused and flushed first.
       Returns the backup id on success, empty on failure. */
    std::string create(Instance &inst, bool full = false);

    /* List backups for an instance, newest first. */
    std::vector<BackupInfo> list(const std::string &instance_id) const;

    /* Restore a backup. The caller must ensure the instance is stopped.
       Destroys the current world (or whole dir for full backups) only after
       the archive has been validated. */
    bool restore(const std::string &instance_id, const std::string &workdir,
                 const std::string &backup_id);

    /* Delete a backup. */
    bool remove(const std::string &instance_id, const std::string &backup_id);

    std::string backups_root(const std::string &instance_id) const;

    static const char *manifest_name();

private:
    Sandbox &sandbox_;

    std::string now_id() const;
    void save_pause(const Instance &inst);
    void save_resume(const Instance &inst);
    bool zip_directory(const std::string &abs_dir, const std::string &rel,
                       std::vector<ZipEntry> &entries);
    bool read_manifest(const std::string &abs_zip, bool &full) const;
};

} // namespace ps5craft
