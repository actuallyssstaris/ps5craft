/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "fs.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace ps5craft {

namespace {

std::string join(const std::string &a, const std::string &b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

bool starts_with(const std::string &text, const std::string &prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

} // namespace

bool fs_mkdirs(const std::string &path) {
    if (path.empty()) return false;
    std::string partial;
    std::size_t index = 0;
    if (path[0] == '/') { partial = "/"; index = 1; }
    while (index <= path.size()) {
        std::size_t next = path.find('/', index);
        if (next == std::string::npos) next = path.size();
        if (next > index) {
            partial = join(partial, path.substr(index, next - index));
            if (mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST) return false;
        }
        index = next + 1;
    }
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool fs_read_file(const std::string &path, std::string &out) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    out.clear();
    char buffer[16384];
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof buffer);
        if (count < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (count == 0) break;
        out.append(buffer, static_cast<std::size_t>(count));
        if (out.size() > (64u << 20)) { close(fd); return false; }
    }
    close(fd);
    return true;
}

bool fs_write_file(const std::string &path, const std::string &data) {
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0) return false;
    std::size_t written = 0;
    while (written < data.size()) {
        ssize_t count = write(fd, data.data() + written, data.size() - written);
        if (count < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    close(fd);
    return true;
}

std::uint64_t fs_file_size(const std::string &path) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) return 0;
    return static_cast<std::uint64_t>(info.st_size);
}

std::int64_t fs_mtime(const std::string &path) {
    struct stat info {};
    if (stat(path.c_str(), &info) != 0) return 0;
    return static_cast<std::int64_t>(info.st_mtime);
}

bool fs_disk_space(const std::string &path, DiskSpace &out) {
#if defined(__PROSPERO__)
    /* statvfs faults in this title's runtime (it resolves to an unmapped
       address, backtrace frame 0x40b373). Capacity is therefore reported as
       unavailable on PS5 rather than guessed. */
    (void)path;
    (void)out;
    return false;
#else
    struct statvfs info {};
    if (statvfs(path.empty() ? "/" : path.c_str(), &info) != 0) return false;
    out.total = static_cast<std::uint64_t>(info.f_frsize) *
                static_cast<std::uint64_t>(info.f_blocks);
    out.free = static_cast<std::uint64_t>(info.f_frsize) *
               static_cast<std::uint64_t>(info.f_bavail);
    return true;
#endif
}

bool fs_remove_tree(const std::string &path) {
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) return true; /* already gone */
    if (!S_ISDIR(info.st_mode)) return unlink(path.c_str()) == 0;

    DIR *dir = opendir(path.c_str());
    if (!dir) return false;
    bool ok = true;
    while (struct dirent *entry = readdir(dir)) {
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
        if (!fs_remove_tree(join(path, entry->d_name))) ok = false;
    }
    closedir(dir);
    if (rmdir(path.c_str()) != 0 && ok) ok = false;
    return ok;
}

Sandbox::Sandbox(std::string root) : root_(std::move(root)) {
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
    fs_mkdirs(root_);
}

bool Sandbox::resolve(const std::string &relative, std::string &absolute) const {
    if (relative.size() > 4096) return false;
    if (relative.find('\0') != std::string::npos) return false;
    if (!relative.empty() && relative[0] == '/') return false;

    std::vector<std::string> parts;
    std::size_t index = 0;
    while (index <= relative.size()) {
        std::size_t next = relative.find('/', index);
        if (next == std::string::npos) next = relative.size();
        std::string part = relative.substr(index, next - index);
        if (part.empty() || part == ".") {
            /* skip */
        } else if (part == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
        } else {
            if (part.size() > 255) return false;
            if (part == ".ps5craft-tmp") return false;
            parts.push_back(part);
        }
        index = next + 1;
    }

    std::string path = root_;
    for (const auto &part : parts) path = join(path, part);
    absolute = path;
    return true;
}

bool Sandbox::list(const std::string &relative, std::vector<FileEntry> &out) const {
    std::string path;
    if (!resolve(relative, path)) return false;
    DIR *dir = opendir(path.c_str());
    if (!dir) return false;
    out.clear();
    while (struct dirent *entry = readdir(dir)) {
        if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
        FileEntry item;
        item.name = entry->d_name;
        item.path = relative.empty() ? item.name : relative + "/" + item.name;
        struct stat info {};
        if (stat(join(path, item.name).c_str(), &info) != 0) continue;
        item.directory = S_ISDIR(info.st_mode);
        item.size = item.directory ? 0 : static_cast<std::uint64_t>(info.st_size);
        item.modified = static_cast<std::int64_t>(info.st_mtime);
        out.push_back(std::move(item));
    }
    closedir(dir);
    std::sort(out.begin(), out.end(), [](const FileEntry &a, const FileEntry &b) {
        if (a.directory != b.directory) return a.directory;
        return a.name < b.name;
    });
    return true;
}

bool Sandbox::read(const std::string &relative, std::string &out, std::uint64_t max_bytes) const {
    std::string path;
    if (!resolve(relative, path)) return false;
    struct stat info {};
    if (stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return false;
    if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > max_bytes) return false;
    return fs_read_file(path, out);
}

bool Sandbox::write(const std::string &relative, const std::string &data) const {
    std::string path;
    if (!resolve(relative, path)) return false;
    std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !fs_mkdirs(path.substr(0, slash))) return false;
    return fs_write_file(path, data);
}

bool Sandbox::mkdirs(const std::string &relative) const {
    std::string path;
    if (!resolve(relative, path)) return false;
    return fs_mkdirs(path);
}

bool Sandbox::rename(const std::string &from, const std::string &to) const {
    std::string a, b;
    if (!resolve(from, a) || !resolve(to, b)) return false;
    std::size_t slash = b.find_last_of('/');
    if (slash != std::string::npos && !fs_mkdirs(b.substr(0, slash))) return false;
    return ::rename(a.c_str(), b.c_str()) == 0;
}

bool Sandbox::remove(const std::string &relative) const {
    std::string path;
    if (!resolve(relative, path)) return false;
    if (path == root_) return false;
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) return false;
    if (S_ISDIR(info.st_mode)) return rmdir(path.c_str()) == 0;
    return unlink(path.c_str()) == 0;
}

bool Sandbox::remove_tree(const std::string &relative) const {
    std::string path;
    if (!resolve(relative, path)) return false;
    if (path == root_) return false;
    return fs_remove_tree(path);
}

bool Sandbox::copy(const std::string &from, const std::string &to) const {
    std::string a, b;
    if (!resolve(from, a) || !resolve(to, b)) return false;
    if (starts_with(b, a + "/")) return false; /* copying a tree into itself */

    struct stat info {};
    if (lstat(a.c_str(), &info) != 0) return false;
    if (S_ISDIR(info.st_mode)) {
        if (!fs_mkdirs(b)) return false;
        DIR *dir = opendir(a.c_str());
        if (!dir) return false;
        bool ok = true;
        while (struct dirent *entry = readdir(dir)) {
            if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, "..")) continue;
            std::string child = entry->d_name;
            std::string source = from.empty() ? child : from + "/" + child;
            std::string target = to.empty() ? child : to + "/" + child;
            if (!copy(source, target)) ok = false;
        }
        closedir(dir);
        return ok;
    }

    std::string data;
    if (!fs_read_file(a, data)) return false;
    std::size_t slash = b.find_last_of('/');
    if (slash != std::string::npos && !fs_mkdirs(b.substr(0, slash))) return false;
    return fs_write_file(b, data);
}

bool Sandbox::exists(const std::string &relative) const {
    std::string path;
    if (!resolve(relative, path)) return false;
    struct stat info {};
    return lstat(path.c_str(), &info) == 0;
}

bool Sandbox::entry_stat(const std::string &relative, FileEntry &out) const {
    std::string path;
    if (!resolve(relative, path)) return false;
    struct stat info {};
    if (lstat(path.c_str(), &info) != 0) return false;
    out.name = relative;
    out.path = relative;
    out.directory = S_ISDIR(info.st_mode);
    out.size = out.directory ? 0 : static_cast<std::uint64_t>(info.st_size);
    out.modified = static_cast<std::int64_t>(info.st_mtime);
    return true;
}

std::uint64_t Sandbox::tree_size(const std::string &relative) const {
    std::vector<FileEntry> entries;
    if (!list(relative, entries)) return 0;
    std::uint64_t total = 0;
    for (const auto &entry : entries) {
        total += entry.directory ? tree_size(entry.path) : entry.size;
    }
    return total;
}

} // namespace ps5craft
