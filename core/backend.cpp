/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.hpp"

#include <algorithm>
#include <ctime>
#include <sstream>

namespace ps5craft {

namespace {
std::string now_iso() {
    std::time_t t = std::time(nullptr);
    std::tm tm {};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string safe_id(const std::string &name) {
    std::string out;
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_') {
            out += c;
        } else if (c == ' ') {
            out += '-';
        }
    }
    if (out.empty()) out = "instance";
    return out;
}
} // namespace

Backend::Backend(std::string data_root) : sandbox_(std::move(data_root)) {}

std::string Backend::create_instance(InstanceConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (config.id.empty()) config.id = safe_id(config.name);
    std::string id = config.id;
    int suffix = 0;
    while (instances_.find(id) != instances_.end()) {
        id = config.id + "-" + std::to_string(++suffix);
    }
    config.id = id;

    /* Create instance directory: <root>/instances/<id> */
    std::string dir = "instances/" + id;
    sandbox_.mkdirs(dir);
    config.workdir = dir;

    auto inst = std::make_unique<Instance>(config);
    instances_[id] = std::move(inst);
    order_.push_back(id);
    return id;
}

bool Backend::remove_instance(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return false;
    if (it->second->state() == InstanceState::kRunning ||
        it->second->state() == InstanceState::kStarting ||
        it->second->state() == InstanceState::kStopping) {
        return false; /* refuse to remove a running instance */
    }
    instances_.erase(it);
    order_.erase(std::find(order_.begin(), order_.end(), id), order_.end());
    return true;
}

Instance *Backend::instance(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : it->second.get();
}

const Instance *Backend::instance(const std::string &id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : it->second.get();
}

std::vector<Json> Backend::list_snapshots() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Json> out;
    for (const auto &id : order_) {
        auto it = instances_.find(id);
        if (it != instances_.end()) out.push_back(it->second->snapshot());
    }
    return out;
}

bool Backend::start_instance(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return false;
    return it->second->request_start();
}

bool Backend::stop_instance(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return false;
    return it->second->request_stop();
}

bool Backend::restart_instance(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return false;
    return it->second->request_restart();
}

void Backend::poll_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &id : order_) {
        auto it = instances_.find(id);
        if (it == instances_.end()) continue;
        /* The supervisor drives actual process state; here we only observe
           the state machine. Spawn/kill happen through the instance object. */
    }
}

Json Backend::status_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json root = Json::object();
    Json instances = Json::array();
    for (const auto &id : order_) {
        auto it = instances_.find(id);
        if (it != instances_.end()) instances.push(it->second->snapshot());
    }
    root.set("instances", instances);
    root.set("count", static_cast<double>(instances.items().size()));
    root.set("time", now_iso());
    return root;
}

} // namespace ps5craft
