/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SHA-1 and SHA-256 implementations (FIPS 180-4). Small and dependency free
   so the host build, the PS5 build and the tests agree. */
#include "hash.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ps5craft {
namespace {

inline std::uint32_t rol(std::uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

inline std::uint32_t ror(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

void put_hex(const std::uint8_t *bytes, std::size_t n, std::string &out) {
    static const char *digits = "0123456789abcdef";
    out.resize(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
}

/* ---- SHA-1 ---- */

struct Sha1 {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                          0xC3D2E1F0u};
    std::uint64_t total = 0;
    std::uint8_t buf[64];
    std::size_t fill = 0;

    void block(const std::uint8_t *p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(p[i * 4]) << 24) |
                   (std::uint32_t(p[i * 4 + 1]) << 16) |
                   (std::uint32_t(p[i * 4 + 2]) << 8) |
                   std::uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void update(const void *data, std::size_t len) {
        const std::uint8_t *p = static_cast<const std::uint8_t *>(data);
        total += len;
        while (len > 0) {
            const std::size_t take = 64 - fill < len ? 64 - fill : len;
            std::memcpy(buf + fill, p, take);
            fill += take;
            p += take;
            len -= take;
            if (fill == 64) {
                block(buf);
                fill = 0;
            }
        }
    }

    void finish(std::uint8_t out[20]) {
        const std::uint64_t bits = total * 8;
        const std::uint8_t pad = 0x80;
        update(&pad, 1);
        const std::uint8_t zero = 0;
        while (fill != 56) update(&zero, 1);
        std::uint8_t lenbuf[8];
        for (int i = 0; i < 8; ++i)
            lenbuf[i] = std::uint8_t(bits >> (56 - i * 8));
        update(lenbuf, 8);
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = std::uint8_t(h[i] >> 24);
            out[i * 4 + 1] = std::uint8_t(h[i] >> 16);
            out[i * 4 + 2] = std::uint8_t(h[i] >> 8);
            out[i * 4 + 3] = std::uint8_t(h[i]);
        }
    }
};

/* ---- SHA-256 ---- */

const std::uint32_t kSha256[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::uint64_t total = 0;
    std::uint8_t buf[64];
    std::size_t fill = 0;

    void block(const std::uint8_t *p) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(p[i * 4]) << 24) |
                   (std::uint32_t(p[i * 4 + 1]) << 16) |
                   (std::uint32_t(p[i * 4 + 2]) << 8) |
                   std::uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^
                                     (w[i - 15] >> 3);
            const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^
                                     (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4],
                      f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + s1 + ch + kSha256[i] + w[i];
            const std::uint32_t s0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    void update(const void *data, std::size_t len) {
        const std::uint8_t *p = static_cast<const std::uint8_t *>(data);
        total += len;
        while (len > 0) {
            const std::size_t take = 64 - fill < len ? 64 - fill : len;
            std::memcpy(buf + fill, p, take);
            fill += take;
            p += take;
            len -= take;
            if (fill == 64) {
                block(buf);
                fill = 0;
            }
        }
    }

    void finish(std::uint8_t out[32]) {
        const std::uint64_t bits = total * 8;
        const std::uint8_t pad = 0x80;
        update(&pad, 1);
        const std::uint8_t zero = 0;
        while (fill != 56) update(&zero, 1);
        std::uint8_t lenbuf[8];
        for (int i = 0; i < 8; ++i)
            lenbuf[i] = std::uint8_t(bits >> (56 - i * 8));
        update(lenbuf, 8);
        for (int i = 0; i < 8; ++i) {
            out[i * 4] = std::uint8_t(h[i] >> 24);
            out[i * 4 + 1] = std::uint8_t(h[i] >> 16);
            out[i * 4 + 2] = std::uint8_t(h[i] >> 8);
            out[i * 4 + 3] = std::uint8_t(h[i]);
        }
    }
};

bool hash_file(const std::string &path, bool want_sha1, std::string &out) {
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    Sha1 s1;
    Sha256 s256;
    std::vector<std::uint8_t> buf(64 * 1024);
    bool ok = true;
    for (;;) {
        const std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n > 0) {
            if (want_sha1) s1.update(buf.data(), n);
            else s256.update(buf.data(), n);
        }
        if (n < buf.size()) {
            if (std::ferror(f)) ok = false;
            break;
        }
    }
    std::fclose(f);
    if (!ok) return false;
    if (want_sha1) {
        std::uint8_t digest[20];
        s1.finish(digest);
        put_hex(digest, sizeof digest, out);
    } else {
        std::uint8_t digest[32];
        s256.finish(digest);
        put_hex(digest, sizeof digest, out);
    }
    return true;
}

} // namespace

std::string sha1_hex(const void *data, std::size_t len) {
    Sha1 s;
    s.update(data, len);
    std::uint8_t digest[20];
    s.finish(digest);
    std::string out;
    put_hex(digest, sizeof digest, out);
    return out;
}

std::string sha256_hex(const void *data, std::size_t len) {
    Sha256 s;
    s.update(data, len);
    std::uint8_t digest[32];
    s.finish(digest);
    std::string out;
    put_hex(digest, sizeof digest, out);
    return out;
}

bool sha1_file_hex(const std::string &path, std::string &out) {
    return hash_file(path, true, out);
}

bool sha256_file_hex(const std::string &path, std::string &out) {
    return hash_file(path, false, out);
}

} // namespace ps5craft
