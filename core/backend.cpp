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

Backend::Backend(std::string data_root)
    : sandbox_(std::move(data_root)), backup_mgr_(sandbox_) {
    load_registry();
}

void Backend::load_registry() {
    std::string text;
    if (!sandbox_.read("instances.json", text, 1u << 20)) return;
    Json root;
    if (!Json::parse(text, root) || !root.is_object()) return;
    const Json &list = root["instances"];
    if (!list.is_array()) return;
    for (const Json &item : list.items()) {
        if (!item.is_object()) continue;
        InstanceConfig cfg;
        cfg.id = item["id"].as_string();
        cfg.name = item["name"].as_string();
        if (cfg.id.empty() || cfg.name.empty()) continue;
        cfg.version = item["version"].as_string("26.3");
        cfg.software = item["software"].as_string("vanilla");
        cfg.jar_path = item["jar_path"].as_string();
        cfg.java_path = item["java_path"].as_string("java");
        cfg.memory.initial_mib = item["memory_initial_mib"].as_int(16);
        cfg.memory.max_mib = item["memory_max_mib"].as_int(224);
        cfg.eula_accepted = item["eula_accepted"].as_bool(false);
        cfg.online_mode = item["online_mode"].as_bool(true);
        cfg.workdir = "instances/" + cfg.id;
        for (const Json &a : item["jvm_args"].items())
            cfg.jvm_args.push_back(a.as_string());
        for (const Json &a : item["server_args"].items())
            cfg.server_args.push_back(a.as_string());
        if (instances_.find(cfg.id) != instances_.end()) continue;
        auto inst = std::make_unique<Instance>(cfg);
        inst->attach(sandbox_);
        order_.push_back(cfg.id);
        instances_[cfg.id] = std::move(inst);
    }
}

void Backend::save_registry() const {
    Json root = Json::object();
    Json list = Json::array();
    for (const auto &id : order_) {
        auto it = instances_.find(id);
        if (it == instances_.end()) continue;
        const InstanceConfig &cfg = it->second->config();
        Json item = Json::object();
        item.set("id", cfg.id);
        item.set("name", cfg.name);
        item.set("version", cfg.version);
        item.set("software", cfg.software);
        item.set("jar_path", cfg.jar_path);
        item.set("java_path", cfg.java_path);
        item.set("memory_initial_mib", cfg.memory.initial_mib);
        item.set("memory_max_mib", cfg.memory.max_mib);
        item.set("eula_accepted", cfg.eula_accepted);
        item.set("online_mode", cfg.online_mode);
        Json jvm = Json::array();
        for (const auto &a : cfg.jvm_args) jvm.push(a);
        item.set("jvm_args", jvm);
        Json sargs = Json::array();
        for (const auto &a : cfg.server_args) sargs.push(a);
        item.set("server_args", sargs);
        list.push(item);
    }
    root.set("instances", list);
    sandbox_.write("instances.json", root.dump());
}

std::string Backend::create_instance(InstanceConfig config) {
    std::lock_guard<std::mutex> lock(mutex_);
    /* Only software verified on the console is accepted; see docs/research.md. */
    if (config.software != "vanilla" && config.software != "fabric" &&
        config.software != "paper") {
        return std::string();
    }
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
    inst->attach(sandbox_);
    instances_[id] = std::move(inst);
    order_.push_back(id);
    save_registry();
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
    save_registry();
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
        it->second->poll();
    }
}

std::string Backend::instance_logs(const std::string &id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return std::string();
    return it->second->logs();
}

bool Backend::accept_eula(const std::string &id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return false;
    if (!it->second->write_eula()) return false;
    save_registry();
    return true;
}

bool Backend::instance_command(const std::string &id,
                               const std::string &line) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return false;
    return it->second->send_command(line) >= 0;
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

bool Backend::fs_write(const std::string &relative, const std::string &data) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sandbox_.write(relative, data);
}

bool Backend::fs_mkdir(const std::string &relative) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sandbox_.mkdirs(relative);
}

bool Backend::fs_rename(const std::string &from, const std::string &to) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sandbox_.rename(from, to);
}

bool Backend::fs_remove(const std::string &relative) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sandbox_.remove(relative);
}

bool Backend::fs_copy(const std::string &from, const std::string &to) {
    std::lock_guard<std::mutex> lock(mutex_);
    return sandbox_.copy(from, to);
}

std::string Backend::backup_create(const std::string &instance_id,
                                   bool full) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(instance_id);
    if (it == instances_.end()) return std::string();
    return backup_mgr_.create(*it->second, full);
}

std::vector<BackupInfo> Backend::backup_list(
    const std::string &instance_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backup_mgr_.list(instance_id);
}

bool Backend::backup_restore(const std::string &instance_id,
                             const std::string &backup_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(instance_id);
    if (it == instances_.end()) return false;
    InstanceState st = it->second->state();
    if (st != InstanceState::kStopped && st != InstanceState::kCrashed &&
        st != InstanceState::kError) {
        return false; /* stop the server before restoring */
    }
    return backup_mgr_.restore(instance_id, it->second->config().workdir,
                               backup_id);
}

bool Backend::backup_remove(const std::string &instance_id,
                            const std::string &backup_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return backup_mgr_.remove(instance_id, backup_id);
}

} // namespace ps5craft
