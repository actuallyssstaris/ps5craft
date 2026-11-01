/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "backup.hpp"
#include "backend.hpp"
#include "instance.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ps5craft {

namespace {

std::string now_iso_file() {
    time_t t = time(nullptr);
    struct tm tm {};
    localtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

bool read_file(const std::string &path, std::string &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[8192];
    for (;;) {
        size_t nr = fread(buf, 1, sizeof buf, f);
        if (nr == 0) break;
        out.append(buf, nr);
    }
    fclose(f);
    return true;
}

} // namespace

BackupManager::BackupManager(Sandbox &sandbox, Backend &backend)
    : sandbox_(sandbox), backend_(backend) {}

std::string BackupManager::now_id() const { return now_iso_file(); }

bool BackupManager::save_pause(Instance *inst) {
    if (!inst) return true;
    if (inst->state() != InstanceState::kRunning) return true;
    /* The server object owns the pid; expose send via backend is not available,
       so use process_write directly on the instance pid. We need the pid. */
    int pid = static_cast<int>(inst->pid());
    if (pid <= 0) return true;
    const char *cmd = "save-off\n";
    process_write(pid, cmd, std::strlen(cmd));
    /* Give the server a moment to process. */
    usleep(500000);
    return true;
}

bool BackupManager::save_resume(Instance *inst) {
    if (!inst) return true;
    if (inst->state() != InstanceState::kRunning) return true;
    int pid = static_cast<int>(inst->pid());
    if (pid <= 0) return true;
    const char *cmd = "save-on\n";
    process_write(pid, cmd, std::strlen(cmd));
    return true;
}

bool BackupManager::zip_directory(const std::string &dir,
                                  std::vector<ZipEntry> &entries) {
    DIR *d = opendir(dir.c_str());
    if (!d) return false;
    while (struct dirent *e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
            continue;
        std::string full = dir + "/" + e->d_name;
        struct stat info {};
        if (lstat(full.c_str(), &info) != 0) continue;
        std::string rel = e->d_name;
        if (S_ISDIR(info.st_mode)) {
            ZipEntry de;
            de.name = rel + "/";
            de.directory = true;
            entries.push_back(de);
            zip_directory(full, entries);
        } else if (S_ISREG(info.st_mode)) {
            ZipEntry fe;
            fe.name = rel;
            if (read_file(full, fe.data)) entries.push_back(fe);
        }
    }
    closedir(d);
    return true;
}

std::string BackupManager::create(const std::string &instance_id, bool full) {
    Instance *inst = backend_.instance(instance_id);
    if (!inst) return std::string();

    std::string workdir = inst->config().workdir;
    if (workdir.empty()) return std::string();

    /* Save-aware: pause auto-save and flush if running. */
    save_pause(inst);

    std::string source = full ? workdir : workdir + "/world";
    if (!sandbox_.exists(source)) {
        save_resume(inst);
        return std::string(); /* nothing to back up */
    }

    std::vector<ZipEntry> entries;
    if (!zip_directory(source, entries)) {
        save_resume(inst);
        return std::string();
    }

    std::string id = now_id();
    std::string backup_dir = "backups/" + instance_id;
    if (!sandbox_.mkdirs(backup_dir)) {
        save_resume(inst);
        return std::string();
    }
    std::string zip_path = backup_dir + "/" + id + ".zip";
    std::string abs_zip;
    if (!sandbox_.resolve(zip_path, abs_zip)) {
        save_resume(inst);
        return std::string();
    }

    if (!zip_write(abs_zip, entries)) {
        save_resume(inst);
        return std::string();
    }

    save_resume(inst);

    return id;
}

std::vector<BackupInfo> BackupManager::list(const std::string &instance_id) const {
    std::vector<BackupInfo> out;
    std::string backup_dir = "backups/" + instance_id;
    std::vector<FileEntry> files;
    if (!sandbox_.list(backup_dir, files)) return out;
    for (const auto &f : files) {
        if (!f.directory && f.name.size() > 4 &&
            f.name.compare(f.name.size() - 4, 4, ".zip") == 0) {
            BackupInfo info;
            info.id = f.name.substr(0, f.name.size() - 4);
            info.file_name = f.name;
            info.path = backup_dir + "/" + f.name;
            info.size = f.size;
            info.modified = f.modified;
            info.instance_id = instance_id;
            info.full = false;
            out.push_back(info);
        }
    }
    /* Newest first. */
    std::sort(out.begin(), out.end(),
              [](const BackupInfo &a, const BackupInfo &b) {
                  return a.modified > b.modified;
              });
    return out;
}

bool BackupManager::restore(const std::string &instance_id,
                            const std::string &backup_id) {
    Instance *inst = backend_.instance(instance_id);
    if (!inst) return false;

    /* Stop the instance before restoring. */
    if (inst->state() == InstanceState::kRunning ||
        inst->state() == InstanceState::kStarting) {
        backend_.stop_instance(instance_id);
        /* Wait briefly for stop. */
        for (int i = 0; i < 20; ++i) {
            if (inst->state() != InstanceState::kRunning &&
                inst->state() != InstanceState::kStarting &&
                inst->state() != InstanceState::kStopping)
                break;
            usleep(100000);
        }
    }

    std::string zip_path = "backups/" + instance_id + "/" + backup_id + ".zip";
    std::string abs_zip;
    if (!sandbox_.resolve(zip_path, abs_zip)) return false;

    std::string world_dir = inst->config().workdir + "/world";
    std::string abs_world;
    if (!sandbox_.resolve(world_dir, abs_world)) return false;

    /* Remove existing world dir, then extract. */
    sandbox_.remove_tree(world_dir);
    if (!sandbox_.mkdirs(world_dir)) return false;
    if (!zip_extract(abs_zip, abs_world)) return false;
    return true;
}

bool BackupManager::remove(const std::string &instance_id,
                           const std::string &backup_id) {
    std::string zip_path = "backups/" + instance_id + "/" + backup_id + ".zip";
    return sandbox_.remove(zip_path);
}

std::string BackupManager::backups_root(const std::string &instance_id) const {
    return "backups/" + instance_id;
}

} // namespace ps5craft
