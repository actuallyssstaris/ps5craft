/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Minimal JSON value, parser and writer. Object keys keep insertion order. */
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ps5craft {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(double v) : type_(Type::Number), number_(v) {}
    Json(int v) : type_(Type::Number), number_(v) {}
    Json(std::int64_t v) : type_(Type::Number), number_(static_cast<double>(v)) {}
    Json(const char *v) : type_(Type::String), string_(v) {}
    Json(std::string v) : type_(Type::String), string_(std::move(v)) {}

    static Json array() { Json j; j.type_ = Type::Array; return j; }
    static Json object() { Json j; j.type_ = Type::Object; return j; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_string() const { return type_ == Type::String; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_bool() const { return type_ == Type::Bool; }

    bool as_bool(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double as_number(double fallback = 0) const { return type_ == Type::Number ? number_ : fallback; }
    std::int64_t as_int(std::int64_t fallback = 0) const {
        return type_ == Type::Number ? static_cast<std::int64_t>(number_) : fallback;
    }
    const std::string &as_string() const { return string_; }
    std::string as_string(const std::string &fallback) const {
        return type_ == Type::String ? string_ : fallback;
    }

    const std::vector<Json> &items() const { return items_; }
    std::vector<Json> &items() { return items_; }

    const std::vector<std::pair<std::string, Json>> &members() const { return members_; }
    std::vector<std::pair<std::string, Json>> &members() { return members_; }

    /* Object access. Missing keys return a shared null value. */
    const Json &operator[](const std::string &key) const;
    Json &operator[](const std::string &key);

    bool has(const std::string &key) const;

    Json &push(Json value) { items_.push_back(std::move(value)); return items_.back(); }
    Json &set(std::string key, Json value);

    std::string dump() const;
    static bool parse(const std::string &text, Json &out);

private:
    void dump_to(std::string &out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> members_;
};

/* Helpers used across the manager. */
std::string json_escape(const std::string &text);

} // namespace ps5craft
