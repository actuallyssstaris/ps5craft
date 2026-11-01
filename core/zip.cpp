/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zip.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>

#include "fs.hpp"

namespace ps5craft {

namespace {

struct ZipRecord {
    std::string name;
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t csize = 0;
    std::uint32_t usize = 0;
    std::uint32_t local_offset = 0;
};

std::uint32_t dos_time_now() {
    time_t t = time(nullptr);
    struct tm tm {};
    localtime_r(&t, &tm);
    std::uint16_t date = static_cast<std::uint16_t>(
        ((tm.tm_mday) & 0x1F) | (((tm.tm_mon + 1) & 0x0F) << 5) |
        (((tm.tm_year - 80) & 0x7F) << 9));
    std::uint16_t time = static_cast<std::uint16_t>(
        (((tm.tm_sec / 2) & 0x1F)) | ((tm.tm_min & 0x3F) << 5) |
        ((tm.tm_hour & 0x1F) << 11));
    return (static_cast<std::uint32_t>(date) << 16) | time;
}

void put_le16(std::uint8_t *p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put_le32(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}
std::uint16_t get_le16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t get_le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint32_t crc32_of(const std::string &data) {
    std::uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : data) {
        c ^= b;
        for (int i = 0; i < 8; ++i)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* Read the central directory into records. */
bool read_records(FILE *f, std::vector<ZipRecord> &out) {
    if (fseek(f, 0, SEEK_END) != 0) return false;
    long size = ftell(f);
    if (size < 22) return false;

    /* Locate the end-of-central-directory signature within the last 64 KiB. */
    long window = size < 65557 ? size : 65557;
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(window));
    if (fseek(f, size - window, SEEK_SET) != 0) return false;
    if (fread(tail.data(), 1, tail.size(), f) != tail.size()) return false;

    long eocd = -1;
    for (long i = window - 22; i >= 0; --i) {
        if (get_le32(&tail[static_cast<std::size_t>(i)]) == 0x06054B50u) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return false;
    const std::uint8_t *e = &tail[static_cast<std::size_t>(eocd)];
    std::uint16_t count = get_le16(e + 10);
    std::uint32_t central_off = get_le32(e + 16);

    if (fseek(f, static_cast<long>(central_off), SEEK_SET) != 0) return false;
    for (std::uint16_t i = 0; i < count; ++i) {
        std::uint8_t cd[46];
        if (fread(cd, 1, 46, f) != 46) return false;
        if (get_le32(cd) != 0x02014B50u) return false;
        std::uint16_t name_len = get_le16(cd + 28);
        std::uint16_t extra_len = get_le16(cd + 30);
        std::uint16_t comment_len = get_le16(cd + 32);
        ZipRecord r;
        r.method = get_le16(cd + 10);
        r.crc = get_le32(cd + 16);
        r.csize = get_le32(cd + 20);
        r.usize = get_le32(cd + 24);
        r.local_offset = get_le32(cd + 42);
        r.name.resize(name_len);
        if (name_len && fread(&r.name[0], 1, name_len, f) != name_len)
            return false;
        if (extra_len && fseek(f, extra_len, SEEK_CUR) != 0) return false;
        if (comment_len && fseek(f, comment_len, SEEK_CUR) != 0) return false;
        out.push_back(std::move(r));
    }
    return true;
}

/* Seek to an entry's payload given its central-directory record. */
bool seek_payload(FILE *f, const ZipRecord &r) {
    if (fseek(f, static_cast<long>(r.local_offset), SEEK_SET) != 0)
        return false;
    std::uint8_t lh[30];
    if (fread(lh, 1, 30, f) != 30) return false;
    if (get_le32(lh) != 0x04034B50u) return false;
    std::uint16_t name_len = get_le16(lh + 26);
    std::uint16_t extra_len = get_le16(lh + 28);
    if (fseek(f, static_cast<long>(name_len) + extra_len, SEEK_CUR) != 0)
        return false;
    return true;
}

/* Reject names that would escape the destination directory. */
bool safe_entry_name(const std::string &name) {
    if (name.empty()) return false;
    if (name.front() == '/') return false;
    if (name.find('\0') != std::string::npos) return false;
    std::size_t start = 0;
    while (start <= name.size()) {
        std::size_t end = name.find('/', start);
        std::string part =
            name.substr(start, end == std::string::npos ? std::string::npos
                                                        : end - start);
        if (part == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

} // namespace

bool zip_write(const std::string &out_path,
               const std::vector<ZipEntry> &entries) {
    FILE *f = fopen(out_path.c_str(), "wb");
    if (!f) return false;

    std::uint32_t offset = 0;
    std::vector<std::uint8_t> central;

    for (const auto &e : entries) {
        std::string name = e.name;
        std::replace(name.begin(), name.end(), '\\', '/');
        while (!name.empty() && name.front() == '/') name.erase(name.begin());
        if (!safe_entry_name(name)) { fclose(f); return false; }

        std::uint16_t name_len = static_cast<std::uint16_t>(name.size());
        std::uint16_t method = 0; /* STORE */
        std::uint32_t dostime = dos_time_now();
        std::uint32_t crc = crc32_of(e.data);
        std::uint32_t size = static_cast<std::uint32_t>(e.data.size());

        std::uint8_t lh[30] = {};
        put_le32(lh + 0, 0x04034B50u);
        put_le16(lh + 4, 20);
        put_le16(lh + 6, 0);
        put_le16(lh + 8, method);
        put_le16(lh + 10, static_cast<std::uint16_t>(dostime & 0xFFFF));
        put_le16(lh + 12, static_cast<std::uint16_t>(dostime >> 16));
        put_le32(lh + 14, crc);
        put_le32(lh + 18, size);
        put_le32(lh + 22, size);
        put_le16(lh + 26, name_len);
        put_le16(lh + 28, 0);

        if (fwrite(lh, 1, 30, f) != 30) { fclose(f); return false; }
        if (name_len && fwrite(name.data(), 1, name_len, f) != name_len) {
            fclose(f);
            return false;
        }
        if (size && fwrite(e.data.data(), 1, size, f) != size) {
            fclose(f);
            return false;
        }

        std::uint8_t cd[46] = {};
        put_le32(cd + 0, 0x02014B50u);
        put_le16(cd + 4, 20);
        put_le16(cd + 6, 20);
        put_le16(cd + 8, 0);
        put_le16(cd + 10, method);
        put_le16(cd + 12, static_cast<std::uint16_t>(dostime & 0xFFFF));
        put_le16(cd + 14, static_cast<std::uint16_t>(dostime >> 16));
        put_le32(cd + 16, crc);
        put_le32(cd + 20, size);
        put_le32(cd + 24, size);
        put_le16(cd + 28, name_len);
        put_le16(cd + 32, 0);
        put_le16(cd + 34, 0);
        put_le16(cd + 36, 0);
        put_le32(cd + 38, static_cast<std::uint32_t>(e.directory ? 0x10 : 0));
        put_le32(cd + 42, offset);

        central.insert(central.end(), cd, cd + 46);
        central.insert(central.end(), name.begin(), name.end());
        offset += 30 + name_len + size;
    }

    std::uint32_t central_size = static_cast<std::uint32_t>(central.size());
    std::uint8_t eocd[22] = {};
    put_le32(eocd + 0, 0x06054B50u);
    put_le16(eocd + 8, static_cast<std::uint16_t>(entries.size()));
    put_le16(eocd + 10, static_cast<std::uint16_t>(entries.size()));
    put_le32(eocd + 12, central_size);
    put_le32(eocd + 16, offset);

    if (!central.empty() &&
        fwrite(central.data(), 1, central.size(), f) != central.size()) {
        fclose(f);
        return false;
    }
    if (fwrite(eocd, 1, 22, f) != 22) { fclose(f); return false; }
    fclose(f);
    return true;
}

std::vector<std::string> zip_list(const std::string &zip_path) {
    std::vector<std::string> out;
    FILE *f = fopen(zip_path.c_str(), "rb");
    if (!f) return out;
    std::vector<ZipRecord> records;
    if (read_records(f, records)) {
        for (const auto &r : records) {
            if (!r.name.empty() && r.name.back() != '/') out.push_back(r.name);
        }
    }
    fclose(f);
    return out;
}

bool zip_read_entry(const std::string &zip_path, const std::string &name,
                    std::string &out, std::uint64_t max_bytes) {
    FILE *f = fopen(zip_path.c_str(), "rb");
    if (!f) return false;
    std::vector<ZipRecord> records;
    if (!read_records(f, records)) { fclose(f); return false; }
    bool ok = false;
    for (const auto &r : records) {
        if (r.name != name) continue;
        if (r.method != 0 || r.usize > max_bytes) break;
        if (!seek_payload(f, r)) break;
        out.assign(r.usize, '\0');
        if (r.usize && fread(&out[0], 1, r.usize, f) != r.usize) break;
        ok = true;
        break;
    }
    fclose(f);
    return ok;
}

bool zip_extract(const std::string &zip_path, const std::string &dest_dir) {
    FILE *f = fopen(zip_path.c_str(), "rb");
    if (!f) return false;
    std::vector<ZipRecord> records;
    if (!read_records(f, records)) { fclose(f); return false; }

    bool ok = true;
    for (const auto &r : records) {
        /* Entry names are validated before touching the filesystem. */
        if (!safe_entry_name(r.name)) {
            if (!r.name.empty() && r.name.back() == '/') {
                std::string dir_name = r.name.substr(0, r.name.size() - 1);
                if (!safe_entry_name(dir_name)) { ok = false; break; }
            } else {
                ok = false;
                break;
            }
        }
        if (r.method != 0) { ok = false; break; }
        if (!seek_payload(f, r)) { ok = false; break; }

        std::string full = dest_dir + "/" + r.name;
        if (!r.name.empty() && r.name.back() == '/') {
            if (!fs_mkdirs(full)) { ok = false; break; }
            continue;
        }
        std::size_t slash = full.find_last_of('/');
        if (slash != std::string::npos && !fs_mkdirs(full.substr(0, slash))) {
            ok = false;
            break;
        }
        FILE *out = fopen(full.c_str(), "wb");
        if (!out) { ok = false; break; }
        std::uint32_t remaining = r.usize;
        std::uint8_t buf[16384];
        while (remaining) {
            std::size_t chunk = remaining > sizeof buf ? sizeof buf : remaining;
            std::size_t nr = fread(buf, 1, chunk, f);
            if (nr == 0) { ok = false; break; }
            if (fwrite(buf, 1, nr, out) != nr) { ok = false; break; }
            remaining -= static_cast<std::uint32_t>(nr);
        }
        fclose(out);
        if (!ok) break;
    }
    fclose(f);
    return ok;
}

} // namespace ps5craft
