/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SHA-1 and SHA-256, used to verify downloaded server artifacts. */
#pragma once

#include <cstddef>
#include <string>

namespace ps5craft {

std::string sha1_hex(const void *data, std::size_t len);
std::string sha256_hex(const void *data, std::size_t len);

/* Hash a file on disk. Returns false when the file cannot be read. */
bool sha1_file_hex(const std::string &path, std::string &out);
bool sha256_file_hex(const std::string &path, std::string &out);

} // namespace ps5craft
