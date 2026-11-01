/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Sandboxed filesystem access for the PS5Craft data root. */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ps5craft {

struct FileEntry {
    std::string name;
    std::string path;      /* relative to the data root */
    bool directory = false;
    std::uint64_t size = 0;
    std::int64_t modified = 0;
};

class Sandbox {
public:
    /* root is created if missing. */
    explicit Sandbox(std::string root);

    const std::string &root() const { return root_; }

    /* Lexical normalisation: no "..", no absolute paths, no NUL bytes.
       Returns false when the path would escape the root. */
    bool resolve(const std::string &relative, std::string &absolute) const;

    bool list(const std::string &relative, std::vector<FileEntry> &out) const;
    bool read(const std::string &relative, std::string &out, std::uint64_t max_bytes) const;
    bool write(const std::string &relative, const std::string &data) const;
    bool mkdirs(const std::string &relative) const;
    bool rename(const std::string &from, const std::string &to) const;
    bool remove(const std::string &relative) const;          /* file or empty dir */
    bool remove_tree(const std::string &relative) const;     /* recursive */
    bool copy(const std::string &from, const std::string &to) const; /* file or tree */
    bool exists(const std::string &relative) const;
    bool entry_stat(const std::string &relative, FileEntry &out) const;
    std::uint64_t tree_size(const std::string &relative) const;

private:
    std::string root_;
};

struct DiskSpace {
    std::uint64_t total = 0;   /* bytes */
    std::uint64_t free = 0;    /* bytes available to this process */
};

/* Capacity of the filesystem holding path. False when statvfs fails. */
bool fs_disk_space(const std::string &path, DiskSpace &out);

bool fs_mkdirs(const std::string &path);
bool fs_remove_tree(const std::string &path);
bool fs_read_file(const std::string &path, std::string &out);
bool fs_write_file(const std::string &path, const std::string &data);
std::uint64_t fs_file_size(const std::string &path);
std::int64_t fs_mtime(const std::string &path);

} // namespace ps5craft
