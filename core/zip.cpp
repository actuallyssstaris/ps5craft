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

uint16_t dos_time_now() {
    time_t t = time(nullptr);
    struct tm tm {};
    localtime_r(&t, &tm);
    uint16_t date = ((tm.tm_mday & 0x1F)) |
                    (((tm.tm_mon + 1) & 0x0F) << 5) |
                    (((tm.tm_year - 80) & 0x7F) << 9);
    uint16_t time = ((tm.tm_sec / 2) & 0x1F) |
                    ((tm.tm_min & 0x3F) << 5) |
                    ((tm.tm_hour & 0x1F) << 11);
    return (date << 16) | time;
}

void put_le16(unsigned char *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put_le32(unsigned char *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = v >> 24;
}
uint16_t get_le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
uint32_t get_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

} // namespace

bool zip_write(const std::string &out_path,
               const std::vector<ZipEntry> &entries) {
    FILE *f = fopen(out_path.c_str(), "wb");
    if (!f) return false;

    uint32_t offset = 0;
    std::vector<unsigned char> central;

    for (const auto &e : entries) {
        std::string name = e.name;
        std::replace(name.begin(), name.end(), '\\', '/');
        while (!name.empty() && name.front() == '/') name.erase(name.begin());

        unsigned char local_header[30];
        uint16_t name_len = (uint16_t)name.size();
        uint16_t method = 0; /* STORE */
        uint16_t dostime = dos_time_now();
        uint32_t crc = 0xFFFFFFFF;
        for (unsigned char b : e.data) {
            crc ^= b;
            for (int i = 0; i < 8; i++)
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        crc = ~crc;
        uint32_t size = (uint32_t)e.data.size();

        put_le32(local_header + 0, 0x04034B50);
        put_le16(local_header + 4, 20);
        put_le16(local_header + 6, 0);
        put_le16(local_header + 8, method);
        put_le16(local_header + 10, (uint16_t)(dostime & 0xFFFF));
        put_le16(local_header + 12, (uint16_t)(dostime >> 16));
        put_le32(local_header + 14, crc);
        put_le32(local_header + 18, size);
        put_le32(local_header + 22, size);
        put_le16(local_header + 26, name_len);
        put_le16(local_header + 28, 0);

        if (fwrite(local_header, 1, 30, f) != 30) { fclose(f); return false; }
        if (name_len && fwrite(name.data(), 1, name_len, f) != name_len) {
            fclose(f); return false;
        }
        if (size && fwrite(e.data.data(), 1, size, f) != size) {
            fclose(f); return false;
        }

        unsigned char cd[46];
        put_le32(cd + 0, 0x02014B50);
        put_le16(cd + 4, 20);
        put_le16(cd + 6, 20);
        put_le16(cd + 8, 0);
        put_le16(cd + 10, method);
        put_le16(cd + 12, (uint16_t)(dostime & 0xFFFF));
        put_le16(cd + 14, (uint16_t)(dostime >> 16));
        put_le32(cd + 16, crc);
        put_le32(cd + 20, size);
        put_le32(cd + 24, size);
        put_le16(cd + 28, name_len);
        put_le16(cd + 32, 0);
        put_le16(cd + 34, 0);
        put_le16(cd + 36, 0);
        put_le16(cd + 38, 0);
        put_le32(cd + 40, offset);
        put_le16(cd + 44, (uint16_t)(e.directory ? 0x10 : 0));

        central.insert(central.end(), cd, cd + 46);
        central.insert(central.end(), name.begin(), name.end());
        offset += 30 + name_len + size;
    }

    uint32_t central_size = (uint32_t)central.size();
    unsigned char eocd[22];
    put_le32(eocd + 0, 0x06054B50);
    put_le16(eocd + 4, 0);
    put_le16(eocd + 6, 0);
    put_le16(eocd + 8, (uint16_t)entries.size());
    put_le16(eocd + 10, (uint16_t)entries.size());
    put_le32(eocd + 12, central_size);
    put_le32(eocd + 16, offset);
    put_le16(eocd + 20, 0);

    if (fwrite(central.data(), 1, central.size(), f) != central.size()) {
        fclose(f); return false;
    }
    if (fwrite(eocd, 1, 22, f) != 22) { fclose(f); return false; }
    fclose(f);
    return true;
}

std::vector<std::string> zip_list(const std::string &zip_path) {
    std::vector<std::string> out;
    FILE *f = fopen(zip_path.c_str(), "rb");
    if (!f) return out;
    unsigned char sig[4];
    while (fread(sig, 1, 4, f) == 4) {
        if (get_le32(sig) == 0x04034B50) {
            unsigned char hdr[26];
            if (fread(hdr, 1, 26, f) != 26) break;
            uint16_t name_len = get_le16(hdr + 26);
            uint16_t extra_len = get_le16(hdr + 28);
            uint32_t csize = get_le32(hdr + 22);
            std::string name(name_len, '\0');
            if (fread(&name[0], 1, name_len, f) != name_len) break;
            if (!name.empty() && name.back() != '/') out.push_back(name);
            fseek(f, extra_len, SEEK_CUR);
            fseek(f, csize, SEEK_CUR);
        } else if (get_le32(sig) == 0x02014B50) {
            break;
        } else {
            break;
        }
    }
    fclose(f);
    return out;
}

bool zip_extract(const std::string &zip_path, const std::string &dest_dir) {
    FILE *f = fopen(zip_path.c_str(), "rb");
    if (!f) return false;
    bool ok = true;
    unsigned char sig[4];
    while (fread(sig, 1, 4, f) == 4) {
        if (get_le32(sig) == 0x04034B50) {
            unsigned char hdr[26];
            if (fread(hdr, 1, 26, f) != 26) { ok = false; break; }
            uint16_t name_len = get_le16(hdr + 26);
            uint16_t extra_len = get_le16(hdr + 28);
            uint16_t method = get_le16(hdr + 10);
            uint32_t usize = get_le32(hdr + 18);
            uint32_t csize = get_le32(hdr + 22);
            std::string name(name_len, '\0');
            if (fread(&name[0], 1, name_len, f) != name_len) { ok = false; break; }
            fseek(f, extra_len, SEEK_CUR);

            std::string full = dest_dir + "/" + name;
            if (!name.empty() && name.back() == '/') {
                fs_mkdirs(full);
            } else {
                std::size_t slash = full.find_last_of('/');
                if (slash != std::string::npos)
                    fs_mkdirs(full.substr(0, slash));
                FILE *out = fopen(full.c_str(), "wb");
                if (!out) { ok = false; break; }
                unsigned char buf[8192];
                uint32_t remaining = usize;
                while (remaining) {
                    size_t chunk = remaining > sizeof buf ? sizeof buf : remaining;
                    size_t nr = fread(buf, 1, chunk, f);
                    if (nr == 0) { ok = false; break; }
                    fwrite(buf, 1, nr, out);
                    remaining -= (uint32_t)nr;
                }
                fclose(out);
            }
            if (method != 0) fseek(f, csize - usize, SEEK_CUR);
        } else if (get_le32(sig) == 0x02014B50) {
            break;
        } else {
            break;
        }
    }
    fclose(f);
    return ok;
}

} // namespace ps5craft
