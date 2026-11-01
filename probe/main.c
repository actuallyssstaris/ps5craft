/* PS5Craft runtime probe: can a payload dlopen the Java 25 runtime and start a VM? */
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <jni.h>

static int logfd = -1;
static void note(const char *fmt, ...) {
  char buf[1024];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
  if (buf[n-1] != '\n') { if (n < (int)sizeof buf - 1) buf[n++] = '\n'; }
  if (logfd >= 0) { write(logfd, buf, n); }
  write(1, buf, n);
}

typedef jint (*create_fn)(JavaVM **, void **, void *);

int main(void) {
  logfd = open("/data/jvmprobe.log", O_CREAT|O_WRONLY|O_TRUNC, 0600);
  note("probe start");

  void *jvm = dlopen("/data/java25/lib/server/libjvm.so", RTLD_NOW);
  if (!jvm) { note("dlopen libjvm FAILED: %s", dlerror()); return 1; }
  note("dlopen libjvm OK");

  create_fn create = (create_fn)dlsym(jvm, "JNI_CreateJavaVM");
  if (!create) { note("dlsym JNI_CreateJavaVM FAILED: %s", dlerror()); return 2; }
  note("dlsym JNI_CreateJavaVM OK");

  if (access("/data/jvmprobe.createmv", F_OK) != 0) {
    note("stop before VM create (no /data/jvmprobe.createmv)");
    return 0;
  }

  static char opt_home[]   = "-Djava.home=/data/java25";
  static char opt_cp[]     = "-Djava.class.path=/data/java25/jars/probe.jar";
  static char opt_heap[]   = "-Xmx192m";
  static char opt_ms[]     = "-Xms16m";
  static char opt_gc[]     = "-XX:+UseSerialGC";
  static char opt_c1[]     = "-XX:TieredStopAtLevel=1";
  static char opt_cc[]     = "-XX:CICompilerCount=1";
  static char opt_meta[]   = "-XX:MetaspaceSize=64m";
  static char opt_ccs[]    = "-XX:CompressedClassSpaceSize=32m";
  static char opt_code[]   = "-XX:ReservedCodeCacheSize=16m";
  static char opt_proc[]   = "-XX:ActiveProcessorCount=2";
  static char opt_tmp[]    = "-Djava.io.tmpdir=/data";
  static char opt_dir[]    = "-Duser.dir=/data";
  static char opt_share[]  = "-Xshare:off";
  static char opt_unrec[]  = "-XX:+IgnoreUnrecognizedVMOptions";
  JavaVMOption opts[] = {
    {opt_home,0},{opt_cp,0},{opt_heap,0},{opt_ms,0},{opt_gc,0},{opt_c1,0},
    {opt_cc,0},{opt_meta,0},{opt_ccs,0},{opt_code,0},{opt_proc,0},
    {opt_tmp,0},{opt_dir,0},{opt_share,0},{opt_unrec,0},
  };
  JavaVMInitArgs args;
  args.version = JNI_VERSION_1_8;
  args.nOptions = sizeof opts / sizeof opts[0];
  args.options = opts;
  args.ignoreUnrecognized = JNI_FALSE;

  JavaVM *vm = 0; JNIEnv *env = 0;
  note("calling JNI_CreateJavaVM");
  jint rc = create(&vm, (void **)&env, &args);
  note("JNI_CreateJavaVM rc=%d vm=%p env=%p", (int)rc, (void*)vm, (void*)env);
  if (rc != JNI_OK || !env) return 3;

  jclass cls = (*env)->FindClass(env, "probe/Hello");
  if (!cls) { note("FindClass failed"); (*env)->ExceptionDescribe(env); return 4; }
  jmethodID mid = (*env)->GetStaticMethodID(env, cls, "main", "([Ljava/lang/String;)V");
  if (!mid) { note("GetStaticMethodID failed"); return 5; }
  (*env)->CallStaticVoidMethod(env, cls, mid, 0);
  if ((*env)->ExceptionCheck(env)) { note("java exception"); (*env)->ExceptionDescribe(env); return 6; }
  note("java main returned");
  (*vm)->DestroyJavaVM(vm);
  note("VM destroyed");
  return 0;
}
