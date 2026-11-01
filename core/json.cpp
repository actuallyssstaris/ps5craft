/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ps5craft {

namespace {
const Json kNull;
}

const Json &Json::operator[](const std::string &key) const {
    for (const auto &member : members_) {
        if (member.first == key) return member.second;
    }
    return kNull;
}

Json &Json::operator[](const std::string &key) {
    for (auto &member : members_) {
        if (member.first == key) return member.second;
    }
    members_.emplace_back(key, Json());
    return members_.back().second;
}

bool Json::has(const std::string &key) const {
    for (const auto &member : members_) {
        if (member.first == key) return true;
    }
    return false;
}

Json &Json::set(std::string key, Json value) {
    for (auto &member : members_) {
        if (member.first == key) {
            member.second = std::move(value);
            return member.second;
        }
    }
    members_.emplace_back(std::move(key), std::move(value));
    return members_.back().second;
}

std::string json_escape(const std::string &text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof buffer, "\\u%04x", c);
                out += buffer;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

void Json::dump_to(std::string &out) const {
    switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: {
        if (std::isfinite(number_) && number_ == std::floor(number_) &&
            std::fabs(number_) < 9.0e15) {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%lld", static_cast<long long>(number_));
            out += buffer;
        } else if (std::isfinite(number_)) {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%.17g", number_);
            out += buffer;
        } else {
            out += "null";
        }
        break;
    }
    case Type::String: out += '"'; out += json_escape(string_); out += '"'; break;
    case Type::Array: {
        out += '[';
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (i) out += ',';
            items_[i].dump_to(out);
        }
        out += ']';
        break;
    }
    case Type::Object: {
        out += '{';
        for (std::size_t i = 0; i < members_.size(); ++i) {
            if (i) out += ',';
            out += '"';
            out += json_escape(members_[i].first);
            out += "\":";
            members_[i].second.dump_to(out);
        }
        out += '}';
        break;
    }
    }
}

std::string Json::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

namespace {

struct Parser {
    const std::string &text;
    std::size_t pos = 0;
    int depth = 0;

    explicit Parser(const std::string &t) : text(t) {}

    void skip_space() {
        while (pos < text.size()) {
            char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
            else break;
        }
    }

    bool literal(const char *word) {
        std::size_t length = 0;
        while (word[length]) ++length;
        if (text.compare(pos, length, word) != 0) return false;
        pos += length;
        return true;
    }

    bool string(std::string &out) {
        if (pos >= text.size() || text[pos] != '"') return false;
        ++pos;
        out.clear();
        while (pos < text.size()) {
            char c = text[pos++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (pos >= text.size()) return false;
            char e = text[pos++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                if (pos + 4 > text.size()) return false;
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = text[pos++];
                    code <<= 4;
                    if (h >= '0' && h <= '9') code |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                if (code < 0x80) {
                    out += static_cast<char>(code);
                } else if (code < 0x800) {
                    out += static_cast<char>(0xC0 | (code >> 6));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | (code >> 12));
                    out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (code & 0x3F));
                }
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    bool value(Json &out) {
        if (++depth > 64) return false;
        skip_space();
        if (pos >= text.size()) return false;
        bool ok = false;
        char c = text[pos];
        if (c == '{') {
            ++pos;
            out = Json::object();
            skip_space();
            if (pos < text.size() && text[pos] == '}') { ++pos; ok = true; }
            else {
                for (;;) {
                    skip_space();
                    std::string key;
                    if (!string(key)) break;
                    skip_space();
                    if (pos >= text.size() || text[pos] != ':') break;
                    ++pos;
                    Json member;
                    if (!value(member)) break;
                    out.set(std::move(key), std::move(member));
                    skip_space();
                    if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
                    if (pos < text.size() && text[pos] == '}') { ++pos; ok = true; }
                    break;
                }
            }
        } else if (c == '[') {
            ++pos;
            out = Json::array();
            skip_space();
            if (pos < text.size() && text[pos] == ']') { ++pos; ok = true; }
            else {
                for (;;) {
                    Json item;
                    if (!value(item)) break;
                    out.push(std::move(item));
                    skip_space();
                    if (pos < text.size() && text[pos] == ',') { ++pos; continue; }
                    if (pos < text.size() && text[pos] == ']') { ++pos; ok = true; }
                    break;
                }
            }
        } else if (c == '"') {
            std::string s;
            if (string(s)) { out = Json(std::move(s)); ok = true; }
        } else if (c == 't') {
            if (literal("true")) { out = Json(true); ok = true; }
        } else if (c == 'f') {
            if (literal("false")) { out = Json(false); ok = true; }
        } else if (c == 'n') {
            if (literal("null")) { out = Json(); ok = true; }
        } else {
            const char *begin = text.c_str() + pos;
            char *end = nullptr;
            double number = std::strtod(begin, &end);
            if (end != begin) {
                pos += static_cast<std::size_t>(end - begin);
                out = Json(number);
                ok = true;
            }
        }
        --depth;
        return ok;
    }
};

} // namespace

bool Json::parse(const std::string &text, Json &out) {
    Parser parser(text);
    if (!parser.value(out)) return false;
    parser.skip_space();
    return parser.pos == text.size();
}

} // namespace ps5craft
