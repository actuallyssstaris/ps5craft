/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host unit tests for the shared backend. Pure C++, no PS5 SDK. */
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "../core/json.hpp"
#include "../core/fs.hpp"
#include "../core/instance.hpp"
#include "../core/backend.hpp"
#include "../platform/process.hpp"


using namespace ps5craft;

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

static void test_json() {
    std::printf("test_json ... ");
    Json j = Json::object();
    j.set("name", "test");
    j.set("count", 42);
    j.set("ratio", 3.14);
    j.set("ok", true);
    Json arr = Json::array();
    arr.push(Json("a"));
    arr.push(Json("b"));
    j.set("items", arr);
    std::string out = j.dump();
    CHECK(out.find("\"name\":\"test\"") != std::string::npos);
    CHECK(out.find("\"count\":42") != std::string::npos);

    Json parsed;
    CHECK(Json::parse(out, parsed));
    CHECK(parsed.is_object());
    CHECK(parsed["name"].as_string() == "test");
    CHECK(parsed["count"].as_int() == 42);
    CHECK(parsed["ok"].as_bool() == true);
    CHECK(parsed["items"].is_array());
    CHECK(parsed["items"].items().size() == 2);
    CHECK(parsed["missing"].is_null());
    std::printf("ok\n");
}

static void test_fs_sandbox() {
    std::printf("test_fs_sandbox ... ");
    std::string root = "/tmp/ps5craft_fs_test";
    Sandbox sb(root);

    CHECK(sb.mkdirs("a/b/c"));
    CHECK(sb.write("a/hello.txt", "world"));
    std::string out;
    CHECK(sb.read("a/hello.txt", out, 1024));
    CHECK(out == "world");

    /* directory traversal must be rejected */
    std::string abs;
    CHECK(!sb.resolve("../etc/passwd", abs));
    CHECK(!sb.resolve("/etc/passwd", abs));
    CHECK(!sb.resolve("a/../../etc/passwd", abs));

    /* list */
    std::vector<FileEntry> entries;
    CHECK(sb.list("a", entries));
    bool found = false;
    for (const auto &e : entries) { if (e.name == "hello.txt" && !e.directory) found = true; }
    CHECK(found);

    /* rename */
    CHECK(sb.rename("a/hello.txt", "a/renamed.txt"));
    CHECK(sb.exists("a/renamed.txt"));
    CHECK(!sb.exists("a/hello.txt"));

    /* tree size */
    auto sz = sb.tree_size("a");
    CHECK(sz > 0);

    /* remove tree */
    CHECK(sb.remove_tree("a"));
    CHECK(!sb.exists("a"));

    std::printf("ok\n");
}

static void test_instance_state() {
    std::printf("test_instance_state ... ");
    InstanceConfig cfg;
    cfg.id = "t1";
    cfg.name = "Test One";
    cfg.version = "26.3";
    cfg.software = "vanilla";
    cfg.memory.initial_mib = 16;
    cfg.memory.max_mib = 224;

    Instance inst(cfg);
    CHECK(inst.state() == InstanceState::kStopped);
    CHECK(inst.state_name() == "stopped");
    CHECK(inst.request_start() == true);
    CHECK(inst.state() == InstanceState::kStarting);
    /* cannot start again while starting */
    CHECK(inst.request_start() == false);
    CHECK(inst.request_stop() == true);
    CHECK(inst.state() == InstanceState::kStopping);

    Json snap = inst.snapshot();
    CHECK(snap["id"].as_string() == "t1");
    CHECK(snap["state"].as_string() == "stopping");
    std::printf("ok\n");
}

static void test_backend() {
    std::printf("test_backend ... ");
    Backend b("/tmp/ps5craft_backend_test");

    InstanceConfig c1;
    c1.name = "Alpha";
    c1.software = "paper";
    std::string id1 = b.create_instance(c1);
    CHECK(!id1.empty());

    InstanceConfig c2;
    c2.name = "Alpha"; /* duplicate name gets a suffix */
    std::string id2 = b.create_instance(c2);
    CHECK(id2 != id1);

    CHECK(b.instance(id1) != nullptr);
    CHECK(b.instance(id2) != nullptr);
    CHECK(b.instance("nope") == nullptr);

    auto snaps = b.list_snapshots();
    CHECK(snaps.size() == 2);

    CHECK(b.start_instance(id1) == true);
    CHECK(b.start_instance(id1) == false); /* already starting */
    CHECK(b.stop_instance(id2) == false); /* not running */

    CHECK(b.remove_instance(id2) == true);
    auto snaps2 = b.list_snapshots();
    CHECK(snaps2.size() == 1);

    Json status = b.status_snapshot();
    CHECK(status["count"].as_int() == 1);
    std::printf("ok\n");
}

static void test_process_spawn() {
    std::printf("test_process_spawn ... ");
    LogRing log(4096);
    ProcessResult result;
    std::vector<std::string> argv = {"/bin/sh", "-c", "echo hello-world-test; exit 3"};
    std::vector<std::string> env = {"PATH=/usr/bin:/bin", "HOME=/tmp"};
    int pid = process_spawn(argv, env, "/tmp", result);
    CHECK(pid > 0);
    /* drain output while the child runs, then wait */
    int rc = 0;
    int status = 0;
    for (int i = 0; i < 50; ++i) {
        process_drain(pid, &log);
        rc = process_waitpid(pid, &status, WNOHANG, result);
        if (rc == 1) break;
        usleep(20000);
    }
    if (rc != 1) rc = process_waitpid(pid, &status, 0, result);
    CHECK(rc == 1);
    CHECK(result.exit_code == 3);
    std::string snap = log.snapshot();
    CHECK(snap.find("hello-world-test") != std::string::npos);
    std::printf("ok\n");
}

int main() {
    test_json();
    test_fs_sandbox();
    test_instance_state();
    test_backend();
    test_process_spawn();

    if (failures) {
        std::printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall tests passed\n");
    return 0;
}
