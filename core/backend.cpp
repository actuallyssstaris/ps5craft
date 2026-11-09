/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <time.h>

#include "catalog.hpp"
#include "hash.hpp"
#include "http_client.hpp"
#include "monitor.hpp"

namespace ps5craft {

namespace {
/* UTC timestamp without gmtime()/strftime(): the console libc keeps the
   broken-down time in a shared static buffer, and a slow or contended call
   there would stall whichever request holds the backend lock. */
std::string now_iso() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return std::string();
    std::int64_t seconds = ts.tv_sec;
    std::int64_t days = seconds / 86400;
    std::int64_t rem = seconds % 86400;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    const std::int64_t z = days + 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const std::int64_t doe = z - era * 146097;
    const std::int64_t yoe =
        (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t year = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t month = mp + (mp < 10 ? 3 : -9);
    year += (month <= 2);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ",
                  static_cast<long long>(year), static_cast<long long>(month),
                  static_cast<long long>(day),
                  static_cast<long long>(rem / 3600),
                  static_cast<long long>((rem / 60) % 60),
                  static_cast<long long>(rem % 60));
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

Backend::~Backend() {
    std::vector<std::shared_ptr<InstallJob>> jobs;
    std::vector<std::shared_ptr<ModJob>> mod_jobs;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto &kv : installs_) jobs.push_back(kv.second);
        for (auto &kv : mod_installs_) mod_jobs.push_back(kv.second);
    }
    for (auto &job : jobs) {
        if (job->thread.joinable()) job->thread.join();
    }
    for (auto &job : mod_jobs) {
        if (job->thread.joinable()) job->thread.join();
    }
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
    auto job = installs_.find(id);
    if (job != installs_.end() && !job->second->done.load())
        return false; /* a download is in flight */
    instances_.erase(it);
    order_.erase(std::find(order_.begin(), order_.end(), id), order_.end());
    disk_cache_.erase(id);
    save_registry();
    return true;
}

std::string Backend::install_start(const std::string &id,
                                   const std::string &software,
                                   const std::string &version) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return "unknown instance";

    const InstanceState state = it->second->state();
    if (state == InstanceState::kRunning || state == InstanceState::kStarting ||
        state == InstanceState::kStopping) {
        return "stop the server before installing software";
    }

    bool known = false;
    for (const auto &s : catalog_software()) {
        if (s.id == software && s.supported) known = true;
    }
    if (!known) return "unsupported software: " + software;

    auto existing = installs_.find(id);
    if (existing != installs_.end() && !existing->second->done.load())
        return "an install is already running for this instance";

    auto job = std::make_shared<InstallJob>();
    job->software = software;
    job->version = version;
    installs_[id] = job;
    job->thread = std::thread([this, id, job, software, version]() {
        run_install(id, job, software, version);
    });
    return std::string();
}

void Backend::run_install(const std::string &id,
                          const std::shared_ptr<InstallJob> &job,
                          const std::string &software,
                          const std::string &version) {
    auto fail = [&job](const std::string &why) {
        job->error = why;
        job->failed = true;
        job->done = true;
    };

    SoftwareArtifact artifact;
    std::string error;
    if (!catalog_resolve(software, version, artifact, error)) {
        fail(error);
        return;
    }
    job->version = artifact.version;
    job->note = artifact.note;
    if (artifact.size > 0) job->total = static_cast<std::uint64_t>(artifact.size);

    std::string final_path;
    if (!sandbox_.resolve("instances/" + id + "/server.jar", final_path)) {
        fail("invalid instance directory");
        return;
    }
    const std::string part_path = final_path + ".part";

    const bool downloaded = http_download(
        artifact.url, part_path,
        [&job](std::uint64_t received, std::uint64_t total) {
            job->received = received;
            if (total > 0) job->total = total;
            return true;
        },
        error, 1800);
    if (!downloaded) {
        std::remove(part_path.c_str());
        fail(error);
        return;
    }

    /* Verify what we got against the publisher's checksum. A source without a
       published checksum (Fabric) is accepted, but the note says so. */
    std::string expected;
    std::string actual;
    bool have_expected = false;
    if (!artifact.sha1.empty()) {
        expected = artifact.sha1;
        have_expected = true;
        if (!sha1_file_hex(part_path, actual)) {
            std::remove(part_path.c_str());
            fail("cannot read the downloaded file");
            return;
        }
    } else if (!artifact.sha256.empty()) {
        expected = artifact.sha256;
        have_expected = true;
        if (!sha256_file_hex(part_path, actual)) {
            std::remove(part_path.c_str());
            fail("cannot read the downloaded file");
            return;
        }
    }
    for (char &c : expected) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char &c : actual) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (have_expected && expected != actual) {
        std::remove(part_path.c_str());
        fail("checksum mismatch: expected " + expected + ", got " + actual);
        return;
    }
    if (artifact.size > 0) {
        FileEntry fe;
        if (sandbox_.entry_stat("instances/" + id + "/server.jar.part", fe) &&
            static_cast<std::int64_t>(fe.size) != artifact.size) {
            std::remove(part_path.c_str());
            fail("size mismatch: expected " + std::to_string(artifact.size) +
                 " bytes, got " + std::to_string(fe.size));
            return;
        }
    }

    std::remove(final_path.c_str());
    if (std::rename(part_path.c_str(), final_path.c_str()) != 0) {
        std::remove(part_path.c_str());
        fail("cannot move the downloaded file into place");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(id);
        if (it == instances_.end()) {
            fail("instance was removed during the install");
            return;
        }
        it->second->apply_software(artifact.version, artifact.software,
                                   "server.jar");
        disk_cache_.erase(id);
        save_registry();
    }
    job->done = true;
}

Json Backend::install_status(const std::string &id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json out = Json::object();
    auto it = installs_.find(id);
    if (it == installs_.end()) {
        out.set("active", false);
        out.set("done", false);
        out.set("failed", false);
        return out;
    }
    const auto &job = it->second;
    const bool done = job->done.load();
    out.set("active", !done);
    out.set("done", done);
    out.set("failed", job->failed.load());
    out.set("software", job->software);
    out.set("version", job->version);
    out.set("received", static_cast<std::int64_t>(job->received.load()));
    out.set("total", static_cast<std::int64_t>(job->total.load()));
    out.set("error", job->error);
    out.set("note", job->note);
    return out;
}

std::string Backend::mod_install_start(const std::string &id, ModSource source,
                                       const std::string &project,
                                       const std::string &version_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return "unknown instance";

    const InstanceState state = it->second->state();
    if (state == InstanceState::kRunning || state == InstanceState::kStarting ||
        state == InstanceState::kStopping) {
        return "stop the server before adding content";
    }
    if (std::string(mod_content_dir(it->second->config().software)).empty())
        return "this server software takes no content";

    auto existing = mod_installs_.find(id);
    if (existing != mod_installs_.end() && !existing->second->done.load())
        return "a content download is already running for this instance";

    auto job = std::make_shared<ModJob>();
    job->project = project;
    job->version = version_id;
    mod_installs_[id] = job;
    job->thread = std::thread([this, id, job, source, project, version_id]() {
        run_mod_install(id, job, source, project, version_id);
    });
    return std::string();
}

void Backend::run_mod_install(const std::string &id,
                              const std::shared_ptr<ModJob> &job, ModSource source,
                              const std::string &project,
                              const std::string &version_id) {
    auto fail = [&job](const std::string &why) {
        job->error = why;
        job->failed = true;
        job->done = true;
    };

    std::string software;
    std::string game_version;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = instances_.find(id);
        if (it == instances_.end()) {
            fail("instance was removed during the download");
            return;
        }
        software = it->second->config().software;
        game_version = it->second->config().version;
    }

    const std::string dir = mod_content_dir(software);
    if (dir.empty()) {
        fail("this server software takes no content");
        return;
    }

    ModFile file;
    std::string error;
    if (!mod_resolve(source, project, version_id, software, game_version, file,
                     error)) {
        fail(error);
        return;
    }

    const std::string name = file.file_name;
    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        fail("the store returned an unsafe file name");
        return;
    }

    job->version = file.version_id;
    job->file_name = name;
    if (file.size > 0) job->total = file.size;

    const std::string relative_dir = "instances/" + id + "/" + dir;
    if (!sandbox_.mkdirs(relative_dir)) {
        fail("cannot create the content folder");
        return;
    }
    std::string final_path;
    if (!sandbox_.resolve(relative_dir + "/" + name, final_path)) {
        fail("invalid content path");
        return;
    }
    const std::string part_path = final_path + ".part";

    const bool downloaded = http_download(
        file.url, part_path,
        [&job](std::uint64_t received, std::uint64_t total) {
            job->received = received;
            if (total > 0) job->total = total;
            return true;
        },
        error, 1800);
    if (!downloaded) {
        std::remove(part_path.c_str());
        fail(error);
        return;
    }

    /* Verify against the publisher's hash. Modrinth publishes SHA-1 and
       SHA-512; Hangar publishes SHA-256. A source without a hash is refused
       rather than installed unchecked. */
    std::string expected;
    std::string actual;
    bool have_expected = false;
    if (!file.sha1.empty()) {
        expected = file.sha1;
        have_expected = sha1_file_hex(part_path, actual);
    } else if (!file.sha256.empty()) {
        expected = file.sha256;
        have_expected = sha256_file_hex(part_path, actual);
    }
    for (char &c : expected)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char &c : actual)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!have_expected) {
        std::remove(part_path.c_str());
        fail("the store published no checksum for this file");
        return;
    }
    if (expected != actual) {
        std::remove(part_path.c_str());
        fail("checksum mismatch: expected " + expected + ", got " + actual);
        return;
    }

    std::remove(final_path.c_str());
    if (std::rename(part_path.c_str(), final_path.c_str()) != 0) {
        std::remove(part_path.c_str());
        fail("cannot move the downloaded file into place");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        disk_cache_.erase(id);
    }
    job->done = true;
}

Json Backend::mod_install_status(const std::string &id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json out = Json::object();
    auto it = mod_installs_.find(id);
    if (it == mod_installs_.end()) {
        out.set("active", false);
        out.set("done", false);
        out.set("failed", false);
        return out;
    }
    const auto &job = it->second;
    const bool done = job->done.load();
    out.set("active", !done);
    out.set("done", done);
    out.set("failed", job->failed.load());
    out.set("project", job->project);
    out.set("version", job->version);
    out.set("file_name", job->file_name);
    out.set("received", static_cast<std::int64_t>(job->received.load()));
    out.set("total", static_cast<std::int64_t>(job->total.load()));
    out.set("error", job->error);
    return out;
}

Json Backend::mods_list(const std::string &id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json out = Json::object();
    auto it = instances_.find(id);
    if (it == instances_.end()) {
        out.set("error", "not found");
        return out;
    }
    const std::string dir = mod_content_dir(it->second->config().software);
    out.set("dir", dir);
    Json files = Json::array();
    std::vector<FileEntry> entries;
    if (!dir.empty() && sandbox_.list("instances/" + id + "/" + dir, entries)) {
        for (const FileEntry &entry : entries) {
            if (entry.directory) continue;
            Json item = Json::object();
            item.set("name", entry.name);
            item.set("size", static_cast<double>(entry.size));
            item.set("modified", static_cast<double>(entry.modified));
            files.push(item);
        }
    }
    out.set("files", files);
    return out;
}

bool Backend::mod_remove(const std::string &id, const std::string &name) {
    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos)
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = instances_.find(id);
    if (it == instances_.end()) return false;
    const std::string dir = mod_content_dir(it->second->config().software);
    if (dir.empty()) return false;
    return sandbox_.remove("instances/" + id + "/" + dir + "/" + name);
}

std::uint64_t Backend::instance_disk_bytes(const std::string &id) {
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!instances_.count(id)) return 0;
        auto cached = disk_cache_.find(id);
        if (cached != disk_cache_.end() && now - cached->second.first < 30)
            return cached->second.second;
    }
    /* Walking a large world must not hold the registry lock: the dashboard
       would freeze for every other request while it runs. */
    const std::uint64_t bytes = sandbox_.tree_size("instances/" + id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        disk_cache_[id] = {now, bytes};
    }
    return bytes;
}

Json Backend::monitor_snapshot() {
    /* Sampling reads /proc and opens a UDP socket. Keep it outside the
       registry lock so a slow probe cannot stall status or lifecycle calls. */
    MonitorSnapshot m;
    {
        std::lock_guard<std::mutex> lock(monitor_mutex_);
        m = ps5craft::monitor_sample(&prev_cpu_);
    }
    Json out = Json::object();

    /* Process (this title) — measured from /proc, absent if unreadable. */
    if (m.proc_mem_present) {
        Json pm = Json::object();
        if (m.proc_mem.vms_kb >= 0)
            pm.set("vms_kb", static_cast<double>(m.proc_mem.vms_kb));
        if (m.proc_mem.rss_kb >= 0)
            pm.set("rss_kb", static_cast<double>(m.proc_mem.rss_kb));
        out.set("process_memory", pm);
    }

    /* System memory — measured from /proc/meminfo. */
    if (m.sys_mem_present) {
        Json sm = Json::object();
        if (m.sys_mem.total_kb >= 0)
            sm.set("total_kb", static_cast<double>(m.sys_mem.total_kb));
        if (m.sys_mem.available_kb >= 0)
            sm.set("available_kb", static_cast<double>(m.sys_mem.available_kb));
        out.set("system_memory", sm);
    }

    /* Process CPU percent — needs two samples; first call yields nothing. */
    if (m.proc_cpu_present)
        out.set("process_cpu_percent", m.proc_cpu_percent);

    /* Local IPs — measured via getifaddrs, loopback excluded. */
    if (m.net_present) {
        Json arr = Json::array();
        for (const auto &ip : m.ips) arr.push(ip);
        out.set("ips", arr);
    }

    return out;
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
    const std::string stamp = now_iso();
    std::lock_guard<std::mutex> lock(mutex_);
    Json root = Json::object();
    Json instances = Json::array();
    for (const auto &id : order_) {
        auto it = instances_.find(id);
        if (it != instances_.end()) instances.push(it->second->snapshot());
    }
    root.set("instances", instances);
    root.set("count", static_cast<double>(instances.items().size()));
    root.set("time", stamp);
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
