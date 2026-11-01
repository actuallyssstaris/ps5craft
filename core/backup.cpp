/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "backup.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "instance.hpp"
#include "../platform/process.hpp"

namespace ps5craft {

namespace {

constexpr const char *kManifestName = "__ps5craft_backup.json";

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

/* Extract a JSON string or boolean field without pulling in the parser. */
bool manifest_full(const std::string &text) {
    std::size_t pos = text.find("\"full\"");
    if (pos == std::string::npos) return false;
    pos = text.find(':', pos);
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n'))
        ++pos;
    return text.compare(pos, 4, "true") == 0;
}

} // namespace

BackupManager::BackupManager(Sandbox &sandbox) : sandbox_(sandbox) {}

const char *BackupManager::manifest_name() { return kManifestName; }

std::string BackupManager::now_id() const { return now_iso_file(); }

void BackupManager::save_pause(const Instance &inst) {
    if (inst.state() != InstanceState::kRunning) return;
    int pid = static_cast<int>(inst.pid());
    if (pid <= 0) return;
    const char *off = "save-off\n";
    process_write(pid, off, std::strlen(off));
    const char *flush = "save-all flush\n";
    process_write(pid, flush, std::strlen(flush));
    /* Give the server time to write chunks to disk. */
    usleep(750000);
}

void BackupManager::save_resume(const Instance &inst) {
    if (inst.state() != InstanceState::kRunning) return;
    int pid = static_cast<int>(inst.pid());
    if (pid <= 0) return;
    const char *on = "save-on\n";
    process_write(pid, on, std::strlen(on));
}

bool BackupManager::zip_directory(const std::string &abs_dir,
                                  const std::string &rel,
                                  std::vector<ZipEntry> &entries) {
    DIR *d = opendir(abs_dir.c_str());
    if (!d) return false;
    bool ok = true;
    while (struct dirent *e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
            continue;
        std::string full = abs_dir + "/" + e->d_name;
        std::string child_rel = rel.empty() ? e->d_name : rel + "/" + e->d_name;
        struct stat info {};
        if (lstat(full.c_str(), &info) != 0) continue;
        if (S_ISDIR(info.st_mode)) {
            ZipEntry de;
            de.name = child_rel + "/";
            de.directory = true;
            entries.push_back(de);
            if (!zip_directory(full, child_rel, entries)) ok = false;
        } else if (S_ISREG(info.st_mode)) {
            ZipEntry fe;
            fe.name = child_rel;
            if (!read_file(full, fe.data)) { ok = false; continue; }
            entries.push_back(std::move(fe));
        }
        /* Symlinks and specials are deliberately skipped. */
    }
    closedir(d);
    return ok;
}

std::string BackupManager::create(Instance &inst, bool full) {
    const std::string instance_id = inst.config().id;
    const std::string workdir = inst.config().workdir;
    if (instance_id.empty() || workdir.empty()) return std::string();

    std::string source =
        full ? workdir : (workdir + "/world");
    if (!sandbox_.exists(source)) return std::string();

    std::string abs_source;
    if (!sandbox_.resolve(source, abs_source)) return std::string();

    save_pause(inst);

    std::vector<ZipEntry> entries;
    bool ok = zip_directory(abs_source, "", entries);
    if (ok) {
        char created[40];
        std::time_t t = std::time(nullptr);
        struct tm tm {};
        gmtime_r(&t, &tm);
        std::strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%SZ", &tm);
        ZipEntry manifest;
        manifest.name = kManifestName;
        manifest.data = std::string("{\"full\":") +
                        (full ? "true" : "false") +
                        ",\"created\":\"" + created +
                        "\",\"instance\":\"" + instance_id + "\"}";
        entries.push_back(std::move(manifest));
    }

    save_resume(inst);

    if (!ok || entries.size() < 2) return std::string();

    std::string backup_dir = backups_root(instance_id);
    if (!sandbox_.mkdirs(backup_dir)) return std::string();

    std::string id = now_id();
    std::string zip_rel = backup_dir + "/" + id + ".zip";
    std::string abs_zip;
    if (!sandbox_.resolve(zip_rel, abs_zip)) return std::string();
    if (!zip_write(abs_zip, entries)) return std::string();

    /* Verify the archive before reporting success. */
    if (zip_list(abs_zip).empty()) {
        sandbox_.remove(zip_rel);
        return std::string();
    }
    return id;
}

std::vector<BackupInfo> BackupManager::list(
    const std::string &instance_id) const {
    std::vector<BackupInfo> out;
    std::string backup_dir = backups_root(instance_id);
    std::vector<FileEntry> files;
    if (!sandbox_.list(backup_dir, files)) return out;
    for (const auto &f : files) {
        if (f.directory || f.name.size() <= 4) continue;
        if (f.name.compare(f.name.size() - 4, 4, ".zip") != 0) continue;
        BackupInfo info;
        info.id = f.name.substr(0, f.name.size() - 4);
        info.file_name = f.name;
        info.path = backup_dir + "/" + f.name;
        info.size = f.size;
        info.modified = f.modified;
        info.instance_id = instance_id;
        std::string abs_zip;
        if (sandbox_.resolve(info.path, abs_zip))
            info.full = read_manifest(abs_zip, info.full) ? info.full : false;
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(),
              [](const BackupInfo &a, const BackupInfo &b) {
                  return a.modified > b.modified;
              });
    return out;
}

bool BackupManager::read_manifest(const std::string &abs_zip,
                                  bool &full) const {
    std::string text;
    if (!zip_read_entry(abs_zip, kManifestName, text, 4096)) return false;
    full = manifest_full(text);
    return true;
}

bool BackupManager::restore(const std::string &instance_id,
                            const std::string &workdir,
                            const std::string &backup_id) {
    /* The id becomes part of a path, so validate it strictly. */
    if (backup_id.empty()) return false;
    for (char c : backup_id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    if (workdir.empty()) return false;

    std::string zip_rel = backups_root(instance_id) + "/" + backup_id + ".zip";
    std::string abs_zip;
    if (!sandbox_.resolve(zip_rel, abs_zip)) return false;

    bool full = false;
    if (!read_manifest(abs_zip, full)) return false;

    /* Validate every member name before deleting the current data. */
    std::vector<std::string> members = zip_list(abs_zip);
    if (members.empty()) return false;

    std::string target_rel = full ? workdir : (workdir + "/world");
    std::string abs_target;
    if (!sandbox_.resolve(target_rel, abs_target)) return false;

    if (sandbox_.exists(target_rel) && !sandbox_.remove_tree(target_rel))
        return false;
    if (!sandbox_.mkdirs(target_rel)) return false;
    if (!zip_extract(abs_zip, abs_target)) return false;
    return true;
}

bool BackupManager::remove(const std::string &instance_id,
                           const std::string &backup_id) {
    if (backup_id.empty()) return false;
    for (char c : backup_id) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    std::string zip_rel = backups_root(instance_id) + "/" + backup_id + ".zip";
    std::string abs_zip;
    if (!sandbox_.resolve(zip_rel, abs_zip)) return false;
    return sandbox_.remove(zip_rel);
}

std::string BackupManager::backups_root(
    const std::string &instance_id) const {
    return "backups/" + instance_id;
}

} // namespace ps5craft
