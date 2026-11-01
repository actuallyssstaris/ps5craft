/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "server.hpp"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

namespace ps5craft {

Server::Server(Sandbox &sandbox, Instance &instance, LogRing &log)
    : sandbox_(sandbox), instance_(instance), log_(log) {}

std::vector<std::string> Server::build_command() const {
    const auto &cfg = instance_.config();
    std::vector<std::string> cmd;
    cmd.push_back("java"); /* resolved on host; on PS5 this is the bundled runtime entry */
    cmd.push_back("-Xmx" + std::to_string(cfg.memory.max_mib) + "m");
    cmd.push_back("-Xms" + std::to_string(cfg.memory.initial_mib) + "m");
    cmd.push_back("-XX:+UseSerialGC");
    cmd.push_back("-XX:ActiveProcessorCount=2");
    cmd.push_back("-Xshare:off");
    cmd.push_back("-Dlog4j2.disableJmx=true");
    cmd.push_back("-Djava.io.tmpdir=" + cfg.workdir + "/tmp");
    cmd.push_back("-Duser.home=" + cfg.workdir);
    cmd.push_back("-Duser.dir=" + cfg.workdir);
    cmd.push_back("-Dps5.mc.workdir=" + cfg.workdir);
    for (const auto &a : cfg.jvm_args) cmd.push_back(a);
    cmd.push_back("-jar");
    cmd.push_back(cfg.jar_path);
    for (const auto &a : cfg.server_args) cmd.push_back(a);
    return cmd;
}

int Server::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ > 0 && !exited_) return pid_;

    const auto &cfg = instance_.config();
    auto argv = build_command();

    std::vector<std::string> env = {
        "PATH=/usr/bin:/bin",
        "HOME=" + cfg.workdir,
        "USER=ps5craft",
        "LC_ALL=C",
    };

    ProcessResult result;
    int pid = process_spawn(argv, env, cfg.workdir, result);
    if (pid < 0) {
        exit_reason_ = result.reason;
        return -1;
    }
    pid_ = pid;
    exited_ = false;
    exit_code_ = -1;
    return pid;
}

bool Server::stop(std::int64_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ <= 0 || exited_) return true;

    /* Try SIGTERM first (clean JVM shutdown). */
    if (process_signal(pid_, SIGTERM) != 0) {
        process_signal(pid_, SIGKILL);
    }

    /* Poll until exit or timeout. */
    using namespace std::chrono;
    auto deadline = steady_clock::now() + milliseconds(timeout_ms);
    while (steady_clock::now() < deadline) {
        int status = 0;
        int rc = process_waitpid(pid_, &status, WNOHANG, /*result*/ *(new ProcessResult()));
        /* we only need reaped/not; ignore details here */
        if (rc == 1) break;
        usleep(50000); /* 50 ms */
    }

    if (pid_ > 0 && !exited_) {
        process_signal(pid_, SIGKILL);
    }
    bool ok = exited_;
    if (ok) {
        pid_ = 0;
    }
    return ok;
}

int Server::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ <= 0 || exited_) return 0;
    int status = 0;
    int rc = process_waitpid(pid_, &status, WNOHANG, /*result*/ *(new ProcessResult()));
    if (rc == 1) {
        exited_ = true;
        return 1;
    }
    return 0;
}

int Server::wait() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ <= 0 || exited_) return exit_code_.load();
    int status = 0;
    int rc = process_waitpid(pid_, &status, 0, /*result*/ *(new ProcessResult()));
    if (rc == 1) exited_ = true;
    return exit_code_.load();
}

bool Server::write_properties() const {
    const auto &cfg = instance_.config();
    std::string content;
    content += "# Minecraft server properties (managed by PS5Craft)\n";
    content += "generator-settings=\n";
    content += "level-name=world\n";
    content += "motd=" + cfg.name + "\n";
    content += "online-mode=" + std::string(cfg.online_mode ? "true" : "false") + "\n";
    content += "enable-query=false\n";
    content += "allow-flight=false\n";
    content += "level-type=default\n";
    content += "level-seed=\n";
    content += "server-port=25565\n";
    content += "max-players=20\n";
    content += "enable-rcon=false\n";
    content += "gamemode=survival\n";
    content += "difficulty=easy\n";
    content += "spawn-protection=16\n";
    content += "view-distance=10\n";
    content += "simulation-distance=10\n";
    content += "hardcore=false\n";
    content += "white-list=false\n";
    content += "broadcast-console-to-ops=true\n";
    content += "prevent-proxy-connections=false\n";
    content += "player-idle-timeout=0\n";
    content += "max-build-height=256\n";
    content += "network-compression-threshold=256\n";
    content += "max-world-size=29999984\n";
    return sandbox_.write("server.properties", content);
}

bool Server::accept_eula() {
    eula_written_ = sandbox_.write("eula.txt", "eula=true\n");
    return eula_written_;
}

bool Server::eula_accepted() const {
    return eula_written_;
}

} // namespace ps5craft
