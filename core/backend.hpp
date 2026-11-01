/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Shared backend: instance registry, lifecycle, JSON snapshot. */
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "instance.hpp"
#include "json.hpp"
#include "fs.hpp"
#include "backup.hpp"
#include "../platform/process.hpp"

namespace ps5craft {

class Backend {
public:
    explicit Backend(std::string data_root);
    ~Backend();

    const std::string &data_root() const { return sandbox_.root(); }

    /* Create a new instance from config. Returns instance id. */
    std::string create_instance(InstanceConfig config);

    bool remove_instance(const std::string &id);

    Instance *instance(const std::string &id);
    const Instance *instance(const std::string &id) const;

    std::vector<Json> list_snapshots() const;

    bool start_instance(const std::string &id);
    bool stop_instance(const std::string &id);
    bool restart_instance(const std::string &id);

    /* Poll all instances for process exits; updates state to running/crashed. */
    void poll_all();

    /* Recent process output for one instance (empty when unknown). */
    std::string instance_logs(const std::string &id) const;

    /* Send one command line to the server stdin. */
    bool instance_command(const std::string &id, const std::string &line);

    /* Write eula=true for an instance and persist the flag. */
    bool accept_eula(const std::string &id);

    Json status_snapshot() const;

    /* File manager operations (all sandboxed). */
    bool fs_write(const std::string &relative, const std::string &data);
    bool fs_mkdir(const std::string &relative);
    bool fs_rename(const std::string &from, const std::string &to);
    bool fs_remove(const std::string &relative);
    bool fs_copy(const std::string &from, const std::string &to);

    /* Backup operations. */
    std::string backup_create(const std::string &instance_id, bool full = false);
    std::vector<BackupInfo> backup_list(const std::string &instance_id) const;
    bool backup_restore(const std::string &instance_id,
                        const std::string &backup_id);
    bool backup_remove(const std::string &instance_id,
                       const std::string &backup_id);

    std::uint64_t instance_disk_bytes(const std::string &id);

    /* Download + verify server software into an instance. Runs on a worker
       thread; progress is read with install_status(). Returns an error string
       (empty on success). */
    std::string install_start(const std::string &id, const std::string &software,
                              const std::string &version);
    Json install_status(const std::string &id) const;

    Sandbox &sandbox() { return sandbox_; }
    const Sandbox &sandbox() const { return sandbox_; }

private:
    struct InstallJob {
        std::thread thread;
        std::atomic<bool> done{false};
        std::atomic<bool> failed{false};
        std::atomic<std::uint64_t> received{0};
        std::atomic<std::uint64_t> total{0};
        std::string software;
        std::string version;
        std::string error;
        std::string note;
    };
    /* Worker thread: resolve, download, verify, then record the result. */
    void run_install(const std::string &id,
                     const std::shared_ptr<InstallJob> &job,
                     const std::string &software, const std::string &version);

    Sandbox sandbox_;
    BackupManager backup_mgr_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Instance>> instances_;
    std::unordered_map<std::string, std::shared_ptr<InstallJob>> installs_;
    std::vector<std::string> order_; /* creation order */
    /* Directory size is expensive on big worlds; cache it briefly. */
    std::unordered_map<std::string, std::pair<std::int64_t, std::uint64_t>> disk_cache_;

    /* Registry persistence: <root>/instances.json. Caller holds mutex_. */
    void load_registry();
    void save_registry() const;
};

} // namespace ps5craft
