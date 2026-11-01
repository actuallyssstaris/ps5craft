/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "server.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/wait.h>
#include <unistd.h>

#include "fs.hpp"
#include "instance.hpp"

namespace ps5craft {

namespace {

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

Server::Server(Sandbox &sandbox, Instance &instance, LogRing &log)
    : sandbox_(sandbox), instance_(instance), log_(log) {}

std::vector<std::string> Server::build_command() const {
    const auto &cfg = instance_.config();
    std::vector<std::string> cmd;
    cmd.push_back(cfg.java_path.empty() ? "java" : cfg.java_path);
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

    std::string cwd;
    if (!sandbox_.resolve(cfg.workdir, cwd)) {
        exit_reason_ = "invalid workdir";
        return -1;
    }

    ProcessResult result;
    int pid = process_spawn(argv, env, cwd, result);
    if (pid < 0) {
        exit_reason_ = result.reason;
        return -1;
    }
    pid_ = pid;
    exited_ = false;
    exit_code_ = -1;
    result_ = result;
    exit_reason_.clear();
    stop_deadline_ms_ = 0;
    term_sent_ = false;
    kill_sent_ = false;
    return pid;
}

bool Server::request_stop(std::int64_t timeout_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ <= 0 || exited_) return false;
    /* Minecraft reads commands on stdin; "stop" saves and exits cleanly. */
    const char *line = "stop\n";
    process_write(pid_, line, std::strlen(line));
    stop_deadline_ms_ = now_ms() + timeout_ms;
    return true;
}

int Server::send_command(const std::string &line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ <= 0 || exited_) return -1;
    std::string cmd = line;
    if (cmd.empty() || cmd.back() != '\n') cmd += '\n';
    return process_write(pid_, cmd.data(), cmd.size());
}

int Server::poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ <= 0 || exited_) return 0;

    /* Collect output first so the final lines are kept. */
    process_drain(pid_, &log_);

    int status = 0;
    int rc = process_waitpid(pid_, &status, WNOHANG, result_);
    if (rc == 1) {
        exited_ = true;
        exit_code_ = result_.exit_code;
        exit_reason_ = result_.signal ? "signal " + std::to_string(result_.signal)
                                      : "exit " + std::to_string(result_.exit_code);
        return 1;
    }

    if (stop_deadline_ms_ > 0 && now_ms() >= stop_deadline_ms_) {
        if (!term_sent_) {
            term_sent_ = true;
            process_signal(pid_, SIGTERM);
            stop_deadline_ms_ = now_ms() + 5000;
        } else if (!kill_sent_) {
            kill_sent_ = true;
            process_signal(pid_, SIGKILL);
        }
    }
    return 0;
}

int Server::wait() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ <= 0 || exited_) return exit_code_.load();
    process_drain(pid_, &log_);
    int status = 0;
    int rc = process_waitpid(pid_, &status, 0, result_);
    if (rc == 1) {
        exited_ = true;
        exit_code_ = result_.exit_code;
        exit_reason_ = result_.signal ? "signal " + std::to_string(result_.signal)
                                      : "exit " + std::to_string(result_.exit_code);
    }
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
    return sandbox_.write(cfg.workdir + "/server.properties", content);
}

bool Server::accept_eula() {
    const auto &cfg = instance_.config();
    eula_written_ = sandbox_.write(cfg.workdir + "/eula.txt", "eula=true\n");
    return eula_written_;
}

bool Server::eula_accepted() const {
    return eula_written_;
}

} // namespace ps5craft
