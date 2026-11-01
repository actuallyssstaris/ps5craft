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
#include "../core/zip.hpp"
#include "../core/instance.hpp"
#include "../core/backend.hpp"
#include "../platform/process.hpp"

#include <cstdlib>
#include <limits.h>

using namespace ps5craft;

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } \
} while (0)

/* Absolute path of the fake Java launcher (Server chdirs into the instance). */
static std::string fake_java() {
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof buf)) return "tests/fake_java.sh";
    return std::string(buf) + "/tests/fake_java.sh";
}

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

    /* Without a sandbox the instance cannot spawn anything. */
    Instance inst(cfg);
    CHECK(inst.state() == InstanceState::kStopped);
    CHECK(inst.state_name() == "stopped");
    CHECK(inst.request_start() == false);
    CHECK(inst.state() == InstanceState::kError);
    CHECK(inst.request_stop() == false);

    Json snap = inst.snapshot();
    CHECK(snap["id"].as_string() == "t1");
    CHECK(snap["state"].as_string() == "error");
    std::printf("ok\n");
}

/* Start the fake server through the real supervisor, then stop it. */
static void test_instance_lifecycle() {
    std::printf("test_instance_lifecycle ... ");
    std::string root = "/tmp/ps5craft_lifecycle_test";
    std::system(("rm -rf " + root).c_str());
    Backend b(root);

    InstanceConfig cfg;
    cfg.name = "Lifecycle";
    cfg.java_path = fake_java();
    cfg.jar_path = "server.jar";
    std::string id = b.create_instance(cfg);
    CHECK(!id.empty());

    CHECK(b.start_instance(id) == true);
    Instance *inst = b.instance(id);
    CHECK(inst != nullptr);
    CHECK(inst->state() == InstanceState::kRunning);
    CHECK(inst->pid() > 0);
    CHECK(b.start_instance(id) == false);    /* already running */
    CHECK(b.stop_instance("missing") == false);

    usleep(300000);
    b.poll_all();
    CHECK(inst->logs().find("fake server starting") != std::string::npos);

    CHECK(b.instance_command(id, "save-all flush") == true);
    usleep(200000);
    b.poll_all();
    CHECK(inst->logs().find("saved the game") != std::string::npos);

    CHECK(b.stop_instance(id) == true);
    CHECK(inst->state() == InstanceState::kStopping);
    for (int i = 0; i < 100; ++i) {
        b.poll_all();
        if (inst->state() == InstanceState::kStopped) break;
        usleep(50000);
    }
    CHECK(inst->state() == InstanceState::kStopped);
    CHECK(inst->exit_code() == 0);
    CHECK(inst->pid() == 0);
    std::printf("ok\n");
}

static void test_zip() {
    std::printf("test_zip ... ");
    std::string root = "/tmp/ps5craft_zip_test";
    std::system(("rm -rf " + root).c_str());
    CHECK(fs_mkdirs(root));

    std::vector<ZipEntry> entries;
    entries.push_back({"a.txt", "alpha", false});
    entries.push_back({"dir/", "", true});
    entries.push_back({"dir/b.txt", "beta-content", false});
    std::string zip = root + "/test.zip";
    CHECK(zip_write(zip, entries));

    auto names = zip_list(zip);
    CHECK(names.size() == 2);

    std::string data;
    CHECK(zip_read_entry(zip, "dir/b.txt", data, 1024));
    CHECK(data == "beta-content");
    CHECK(!zip_read_entry(zip, "missing.txt", data, 1024));

    std::string out_dir = root + "/out";
    CHECK(fs_mkdirs(out_dir));
    CHECK(zip_extract(zip, out_dir));
    std::string read_back;
    CHECK(fs_read_file(out_dir + "/dir/b.txt", read_back));
    CHECK(read_back == "beta-content");
    std::printf("ok\n");
}

static void test_backup() {
    std::printf("test_backup ... ");
    std::string root = "/tmp/ps5craft_backup_test";
    std::system(("rm -rf " + root).c_str());
    Backend b(root);

    InstanceConfig cfg;
    cfg.name = "Backup Test";
    cfg.java_path = fake_java();
    cfg.jar_path = "server.jar";
    std::string id = b.create_instance(cfg);
    CHECK(!id.empty());

    CHECK(b.fs_mkdir("instances/" + id + "/world/region"));
    CHECK(b.fs_write("instances/" + id + "/world/level.dat", "original-level"));
    CHECK(b.fs_write("instances/" + id + "/world/region/r.0.0.mca", "region-data"));

    std::string backup_id = b.backup_create(id, false);
    CHECK(!backup_id.empty());
    CHECK(zip_list(root + "/backups/" + id + "/" + backup_id + ".zip").size() == 3);

    auto backups = b.backup_list(id);
    CHECK(backups.size() == 1);
    CHECK(backups[0].id == backup_id);
    CHECK(backups[0].full == false);

    /* Destroy the world, then restore it. */
    CHECK(b.fs_remove("instances/" + id + "/world/level.dat"));
    std::string content;
    CHECK(!b.sandbox().read("instances/" + id + "/world/level.dat", content, 4096));
    CHECK(b.backup_restore(id, backup_id));
    CHECK(b.sandbox().read("instances/" + id + "/world/level.dat", content, 4096));
    CHECK(content == "original-level");
    CHECK(b.sandbox().read("instances/" + id + "/world/region/r.0.0.mca",
                           content, 4096));
    CHECK(content == "region-data");

    /* Traversal-shaped ids must be rejected. */
    CHECK(!b.backup_restore(id, "../../etc/passwd"));
    CHECK(!b.backup_remove(id, "../x"));

    /* Restoring while running must be refused. */
    CHECK(b.start_instance(id));
    CHECK(!b.backup_restore(id, backup_id));
    CHECK(b.stop_instance(id));
    for (int i = 0; i < 100; ++i) {
        b.poll_all();
        if (b.instance(id)->state() == InstanceState::kStopped) break;
        usleep(50000);
    }

    CHECK(b.backup_remove(id, backup_id));
    CHECK(b.backup_list(id).empty());
    std::printf("ok\n");
}

static void test_backend() {
    std::printf("test_backend ... ");
    std::string root = "/tmp/ps5craft_backend_test";
    std::system(("rm -rf " + root).c_str());
    Backend b(root);

    InstanceConfig c1;
    c1.name = "Alpha";
    c1.software = "paper";
    c1.java_path = fake_java();
    c1.jar_path = "server.jar";
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
    CHECK(b.start_instance(id1) == false); /* already running */
    CHECK(b.stop_instance(id2) == false); /* not running */
    CHECK(b.stop_instance(id1) == true);
    for (int i = 0; i < 100; ++i) {
        b.poll_all();
        if (b.instance(id1)->state() == InstanceState::kStopped) break;
        usleep(50000);
    }

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
    test_zip();
    test_instance_state();
    test_instance_lifecycle();
    test_backup();
    test_backend();
    test_process_spawn();

    if (failures) {
        std::printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    std::printf("\nall tests passed\n");
    return 0;
}
