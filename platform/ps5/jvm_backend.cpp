/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Embedded HotSpot backend for the console title.
 *
 * A title cannot exec another program, so the PS5 OpenJDK build is linked
 * into eboot.bin. This file turns the host backend's "java -jar ..." command
 * line into JVM options, starts the VM on a worker thread, and runs the jar's
 * Main-Class in that VM. stdout/stderr and stdin are pipes so the existing
 * Server supervisor keeps working unchanged.
 */

#include "jvm_backend.hpp"

#include "include/jni.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

namespace ps5craft {

namespace {

using Initializer = void (*)();

extern "C" {
extern Initializer __jvm_init_array_start[];
extern Initializer __jvm_init_array_end[];
}

constexpr int kEmbeddedPid = 0x4A564D31;
constexpr std::size_t kPendingCap = 512 * 1024;

enum class State { kIdle, kRunning, kExited };

std::mutex g_mutex;
std::condition_variable g_cv;
State g_state = State::kIdle;
int g_exit_code = -1;
std::string g_exit_reason;
std::int64_t g_started_ms = 0;
std::int64_t g_finished_ms = 0;

int g_stdin_fd = -1;
int g_output_fd = -1;
int g_saved_output = -1;
std::string g_pending;

JavaVM *g_vm = nullptr;
bool g_constructors_run = false;

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

void queue_line(const std::string &line) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_pending.size() + line.size() > kPendingCap) {
        /* Keep the tail: the newest lines explain the current state. */
        std::size_t keep = kPendingCap / 2;
        if (keep < line.size()) keep = line.size();
        if (g_pending.size() > keep)
            g_pending.erase(0, g_pending.size() - keep);
    }
    g_pending += line;
    if (g_pending.empty() || g_pending.back() != '\n') g_pending += '\n';
}

void set_exited(int code, const std::string &reason) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_exit_code = code;
        g_exit_reason = reason;
        g_finished_ms = now_ms();
        g_state = State::kExited;
    }
    g_cv.notify_all();
}

/* The kit redirects process stdout to app.log before main(). Keep a copy of
 * that descriptor and tee the JVM output into it while the supervisor drains
 * the pipe. */
void *output_reader(void *) {
    char buffer[4096];
    for (;;) {
        ssize_t n = read(g_output_fd, buffer, sizeof buffer);
        if (n > 0) {
            if (g_saved_output >= 0) {
                ssize_t offset = 0;
                while (offset < n) {
                    ssize_t written = write(g_saved_output, buffer + offset,
                                            static_cast<std::size_t>(n - offset));
                    if (written <= 0) break;
                    offset += written;
                }
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending.append(buffer, static_cast<std::size_t>(n));
            if (g_pending.size() > kPendingCap) {
                std::size_t cut = g_pending.size() - kPendingCap;
                std::size_t newline = g_pending.find('\n', cut);
                if (newline == std::string::npos) newline = cut;
                else ++newline;
                g_pending.erase(0, newline);
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    return nullptr;
}

bool java_exception(JNIEnv *env, const char *stage) {
    if (!env->ExceptionCheck()) return false;
    std::string line = "[PS5Craft] Java exception during ";
    line += stage;
    line += ":\n";
    queue_line(line);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

std::string jar_main_class(JNIEnv *env, const std::string &jar, bool &ok) {
    ok = false;
    jclass jar_class = env->FindClass("java/util/jar/JarFile");
    if (!jar_class || java_exception(env, "JarFile lookup")) return {};

    jmethodID ctor = env->GetMethodID(jar_class, "<init>", "(Ljava/lang/String;)V");
    jmethodID get_manifest = env->GetMethodID(jar_class, "getManifest",
                                              "()Ljava/util/jar/Manifest;");
    if (!ctor || !get_manifest || java_exception(env, "JarFile method lookup")) {
        env->DeleteLocalRef(jar_class);
        return {};
    }

    jstring path = env->NewStringUTF(jar.c_str());
    jobject file = env->NewObject(jar_class, ctor, path);
    env->DeleteLocalRef(path);
    if (!file || java_exception(env, "JarFile open")) {
        env->DeleteLocalRef(jar_class);
        return {};
    }

    jobject manifest = env->CallObjectMethod(file, get_manifest);
    if (!manifest || java_exception(env, "manifest read")) {
        env->DeleteLocalRef(file);
        env->DeleteLocalRef(jar_class);
        return {};
    }

    jclass manifest_class = env->FindClass("java/util/jar/Manifest");
    jclass attributes_class = env->FindClass("java/util/jar/Attributes");
    jmethodID main_attributes = manifest_class
        ? env->GetMethodID(manifest_class, "getMainAttributes",
                           "()Ljava/util/jar/Attributes;")
        : nullptr;
    jmethodID get_value = attributes_class
        ? env->GetMethodID(attributes_class, "getValue",
                           "(Ljava/lang/String;)Ljava/lang/String;")
        : nullptr;
    if (!main_attributes || !get_value || java_exception(env, "manifest method lookup")) {
        if (manifest_class) env->DeleteLocalRef(manifest_class);
        if (attributes_class) env->DeleteLocalRef(attributes_class);
        env->DeleteLocalRef(manifest);
        env->DeleteLocalRef(file);
        env->DeleteLocalRef(jar_class);
        return {};
    }

    jobject attributes = env->CallObjectMethod(manifest, main_attributes);
    jstring key = env->NewStringUTF("Main-Class");
    jstring value = attributes
        ? static_cast<jstring>(env->CallObjectMethod(attributes, get_value, key))
        : nullptr;
    env->DeleteLocalRef(key);
    if (!value || java_exception(env, "Main-Class read")) {
        if (attributes) env->DeleteLocalRef(attributes);
        env->DeleteLocalRef(manifest_class);
        env->DeleteLocalRef(attributes_class);
        env->DeleteLocalRef(manifest);
        env->DeleteLocalRef(file);
        env->DeleteLocalRef(jar_class);
        return {};
    }

    const char *chars = env->GetStringUTFChars(value, nullptr);
    std::string name = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    env->DeleteLocalRef(value);
    env->DeleteLocalRef(attributes);
    env->DeleteLocalRef(manifest_class);
    env->DeleteLocalRef(attributes_class);
    env->DeleteLocalRef(manifest);
    env->DeleteLocalRef(file);
    env->DeleteLocalRef(jar_class);

    while (!name.empty() && (name.back() == '\r' || name.back() == '\n' ||
                             name.back() == ' '))
        name.pop_back();
    ok = !name.empty();
    return name;
}

bool run_server(JNIEnv *env, const std::string &jar,
                const std::vector<std::string> &args) {
    bool ok = false;
    std::string main = jar_main_class(env, jar, ok);
    if (!ok) {
        queue_line("[PS5Craft] jar manifest has no Main-Class: " + jar);
        return false;
    }
    for (char &c : main)
        if (c == '.') c = '/';
    queue_line("[PS5Craft] main class: " + main);

    jclass klass = env->FindClass(main.c_str());
    if (!klass || java_exception(env, "main class lookup")) {
        queue_line("[PS5Craft] cannot load main class: " + main);
        return false;
    }
    jmethodID method = env->GetStaticMethodID(klass, "main", "([Ljava/lang/String;)V");
    if (!method || java_exception(env, "main method lookup")) {
        queue_line("[PS5Craft] main class has no main(String[]): " + main);
        env->DeleteLocalRef(klass);
        return false;
    }

    jclass string_class = env->FindClass("java/lang/String");
    jobjectArray arguments = string_class
        ? env->NewObjectArray(static_cast<jsize>(args.size()), string_class, nullptr)
        : nullptr;
    if (!arguments || java_exception(env, "argument array")) {
        if (string_class) env->DeleteLocalRef(string_class);
        env->DeleteLocalRef(klass);
        return false;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        jstring value = env->NewStringUTF(args[i].c_str());
        env->SetObjectArrayElement(arguments, static_cast<jsize>(i), value);
        env->DeleteLocalRef(value);
    }

    env->CallStaticVoidMethod(klass, method, arguments);
    bool failed = java_exception(env, "server main");
    env->DeleteLocalRef(arguments);
    env->DeleteLocalRef(string_class);
    env->DeleteLocalRef(klass);
    return !failed;
}

struct Launch {
    std::string jar;
    std::vector<std::string> vm_args;
    std::vector<std::string> server_args;
};

bool parse_launch(const std::vector<std::string> &argv, Launch &launch,
                  std::string &error) {
    if (argv.size() < 2) {
        error = "empty java command line";
        return false;
    }
    bool after_jar = false;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string &arg = argv[i];
        if (after_jar) {
            launch.server_args.push_back(arg);
            continue;
        }
        if (arg == "-jar") {
            if (i + 1 >= argv.size()) {
                error = "-jar without a file";
                return false;
            }
            launch.jar = argv[++i];
            after_jar = true;
            continue;
        }
        if (arg == "-cp" || arg == "-classpath") {
            if (i + 1 >= argv.size()) {
                error = arg + " without a value";
                return false;
            }
            launch.vm_args.push_back("-Djava.class.path=" + argv[++i]);
            continue;
        }
        if (arg.rfind("-X", 0) == 0 || arg.rfind("-D", 0) == 0 ||
            arg.rfind("-XX:", 0) == 0 || arg == "-ea" || arg == "-enableassertions") {
            launch.vm_args.push_back(arg);
            continue;
        }
        /* A bare class name or an option this backend does not model. */
        launch.server_args.push_back(arg);
    }
    if (launch.jar.empty()) {
        error = "no -jar in the java command line";
        return false;
    }
    return true;
}

void jvm_thread(Launch *launch) {
    JavaVM *vm = nullptr;
    JNIEnv *env = nullptr;
    std::vector<std::string> option_text;
    option_text.reserve(launch->vm_args.size() + 16);
    for (const auto &arg : launch->vm_args) option_text.push_back(arg);
    option_text.push_back("-Djava.class.path=" + launch->jar);
    option_text.push_back("-Djava.awt.headless=true");
    option_text.push_back("-Dfile.encoding=UTF-8");
    option_text.push_back("-Dsun.stdout.encoding=UTF-8");
    option_text.push_back("-Dsun.stderr.encoding=UTF-8");
    option_text.push_back("-XX:UseAVX=0");
    option_text.push_back("-XX:UseSSE=2");
    option_text.push_back("-XX:CompressedClassSpaceSize=32m");
    option_text.push_back("-XX:-UsePerfData");
    option_text.push_back("-XX:TieredStopAtLevel=1");
    option_text.push_back("-XX:CICompilerCount=1");
    option_text.push_back("-XX:CompileThresholdScaling=4.0");
    option_text.push_back("-Xshare:off");
    option_text.push_back("-XX:+UseSerialGC");

    std::vector<JavaVMOption> options;
    options.reserve(option_text.size());
    for (const auto &text : option_text) {
        JavaVMOption option{};
        option.optionString = const_cast<char *>(text.c_str());
        options.push_back(option);
    }

    JavaVMInitArgs vm_args{};
    vm_args.version = JNI_VERSION_1_8;
    vm_args.nOptions = static_cast<jint>(options.size());
    vm_args.options = options.data();
    vm_args.ignoreUnrecognized = JNI_FALSE;

    queue_line("[PS5Craft] starting embedded Java runtime");
    if (!g_constructors_run) {
        for (auto *fn = __jvm_init_array_start; fn != __jvm_init_array_end; ++fn)
            if (*fn) (*fn)();
        g_constructors_run = true;
    }

    jint rc = JNI_CreateJavaVM(&vm, reinterpret_cast<void **>(&env), &vm_args);
    if (rc != JNI_OK || !vm || !env) {
        char text[80];
        std::snprintf(text, sizeof text, "JVM creation failed (code %d)", rc);
        queue_line(std::string("[PS5Craft] ") + text);
        set_exited(1, text);
        delete launch;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_vm = vm;
    }
    char version[80];
    std::snprintf(version, sizeof version, "[PS5Craft] Java VM ready (JNI 0x%08x)",
                  static_cast<unsigned>(env->GetVersion()));
    queue_line(version);

    bool ok = run_server(env, launch->jar, launch->server_args);
    queue_line(ok ? "[PS5Craft] server main returned"
                  : "[PS5Craft] server main failed");

    rc = vm->DestroyJavaVM();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_vm = nullptr;
    }
    if (rc != JNI_OK) {
        char text[80];
        std::snprintf(text, sizeof text, "DestroyJavaVM returned %d", rc);
        queue_line(std::string("[PS5Craft] ") + text);
        set_exited(ok ? 0 : 1, text);
    } else {
        set_exited(ok ? 0 : 1, ok ? "exit 0" : "server main failed");
    }
    delete launch;
}

bool setup_pipes(std::string &error) {
    int input[2] = {-1, -1};
    int output[2] = {-1, -1};
    if (pipe(input) != 0) {
        error = "stdin pipe failed";
        return false;
    }
    if (pipe(output) != 0) {
        close(input[0]);
        close(input[1]);
        error = "stdout pipe failed";
        return false;
    }
    g_saved_output = dup(STDOUT_FILENO);
    if (g_saved_output < 0) {
        close(input[0]);
        close(input[1]);
        close(output[0]);
        close(output[1]);
        error = "stdout duplicate failed";
        return false;
    }
    std::fflush(stdout);
    std::fflush(stderr);
    if (dup2(input[0], STDIN_FILENO) < 0 ||
        dup2(output[1], STDOUT_FILENO) < 0 ||
        dup2(output[1], STDERR_FILENO) < 0) {
        close(input[0]);
        close(input[1]);
        close(output[0]);
        close(output[1]);
        close(g_saved_output);
        g_saved_output = -1;
        error = "descriptor redirect failed";
        return false;
    }
    close(input[0]);
    close(output[1]);
    g_stdin_fd = input[1];
    g_output_fd = output[0];
    int flags = fcntl(g_stdin_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(g_stdin_fd, F_SETFL, flags | O_NONBLOCK);
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    return true;
}

} // namespace

bool jvm_backend_owns(int pid) { return pid == kEmbeddedPid; }

int jvm_backend_spawn(const std::vector<std::string> &argv, const std::string &cwd,
                      ProcessResult &result) {
    (void)cwd;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state != State::kIdle) {
            result.reason = "embedded Java runtime already used; restart the title";
            return -1;
        }
    }

    auto *launch = new Launch();
    std::string error;
    if (!parse_launch(argv, *launch, error)) {
        result.reason = error;
        delete launch;
        return -1;
    }
    if (access(launch->jar.c_str(), R_OK) != 0) {
        result.reason = "server jar not readable: " + launch->jar;
        delete launch;
        return -1;
    }
    if (!setup_pipes(error)) {
        result.reason = error;
        delete launch;
        return -1;
    }

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_state = State::kRunning;
        g_exit_code = -1;
        g_exit_reason.clear();
        g_started_ms = now_ms();
        g_finished_ms = 0;
    }
    int rc = pthread_create(&thread, &attr, [](void *data) -> void * {
        jvm_thread(static_cast<Launch *>(data));
        return nullptr;
    }, launch);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_state = State::kIdle;
        }
        close(g_stdin_fd);
        close(g_output_fd);
        close(g_saved_output);
        g_stdin_fd = g_output_fd = g_saved_output = -1;
        result.reason = "JVM thread creation failed";
        delete launch;
        return -1;
    }

    pthread_t reader;
    pthread_attr_t reader_attr;
    pthread_attr_init(&reader_attr);
    pthread_attr_setdetachstate(&reader_attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&reader, &reader_attr, output_reader, nullptr) != 0)
        queue_line("[PS5Craft] output reader thread failed");
    pthread_attr_destroy(&reader_attr);

    result.started_at = 0;
    return kEmbeddedPid;
}

int jvm_backend_drain(int pid, LogRing *log) {
    if (!jvm_backend_owns(pid) || !log) return -1;
    std::string pending;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        pending.swap(g_pending);
    }
    if (!pending.empty()) log->append(pending.data(), pending.size());
    return static_cast<int>(pending.size());
}

int jvm_backend_signal(int pid, int sig) {
    if (!jvm_backend_owns(pid)) return -1;
    if (sig == SIGTERM) {
        const char line[] = "stop\n";
        return jvm_backend_write(pid, line, sizeof line - 1) < 0 ? -1 : 0;
    }
    if (sig == SIGKILL) {
        JavaVM *vm = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            vm = g_vm;
        }
        if (!vm) return -1;
        queue_line("[PS5Craft] forcing Java VM halt");
        JNIEnv *env = nullptr;
        if (vm->AttachCurrentThread(reinterpret_cast<void **>(&env), nullptr) != JNI_OK ||
            !env)
            return -1;
        jclass runtime = env->FindClass("java/lang/Runtime");
        jmethodID get_runtime = runtime
            ? env->GetStaticMethodID(runtime, "getRuntime", "()Ljava/lang/Runtime;")
            : nullptr;
        jobject instance = get_runtime
            ? env->CallStaticObjectMethod(runtime, get_runtime)
            : nullptr;
        jmethodID halt = instance
            ? env->GetMethodID(env->GetObjectClass(instance), "halt", "(I)V")
            : nullptr;
        if (halt) env->CallVoidMethod(instance, halt, 137);
        env->ExceptionClear();
        return 0;
    }
    return -1;
}

int jvm_backend_write(int pid, const char *data, std::size_t len) {
    if (!jvm_backend_owns(pid) || g_stdin_fd < 0) return -1;
    std::size_t written = 0;
    while (written < len) {
        ssize_t n = write(g_stdin_fd, data + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        written += static_cast<std::size_t>(n);
    }
    return static_cast<int>(written);
}

int jvm_backend_waitpid(int pid, int *status, int options, ProcessResult &result) {
    if (!jvm_backend_owns(pid)) return -1;
    std::unique_lock<std::mutex> lock(g_mutex);
    if (options == 0)
        g_cv.wait(lock, [] { return g_state == State::kExited; });
    else if (g_state != State::kExited)
        return 0;
    result.exit_code = g_exit_code;
    result.signaled = false;
    result.signal = 0;
    result.reason = g_exit_reason;
    result.started_at = g_started_ms;
    result.finished_at = g_finished_ms;
    result.runtime_ms = static_cast<std::uint64_t>(
        g_finished_ms > g_started_ms ? g_finished_ms - g_started_ms : 0);
    if (status) *status = (g_exit_code & 0xff) << 8;
    return 1;
}

int jvm_backend_wait(int pid, ProcessResult &result) {
    if (!jvm_backend_owns(pid)) return -1;
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait(lock, [] { return g_state == State::kExited; });
    int status = 0;
    lock.unlock();
    return jvm_backend_waitpid(pid, &status, 0, result);
}

} // namespace ps5craft
