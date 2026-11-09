/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Compatibility shims for functions used by libmicrohttpd that the PS5
   C library does not export (tsearch family, pipe2). Signatures match
   <search.h> exactly so libmicrohttpd's calls bind here. posix_tnode is
   an opaque void * in the public header, so nodes are cast internally.
   The PS5 runs on a trusted LAN, so IP limiting is intentionally a
   no-op stub. */
#include <search.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

extern "C" {

struct shim_node {
    const void *key;
    struct shim_node *llink, *rlink;
};

static int node_cmp(const void *a, const void *b) {
    return (a == b) ? 0 : ((a < b) ? -1 : 1);
}

void *tsearch(const void *key, posix_tnode **rootp,
              int (*compar)(const void *, const void *)) {
    if (!rootp) return 0;
    struct shim_node **p = (struct shim_node **)rootp;
    if (!*p) {
        *p = (struct shim_node *)calloc(1, sizeof(struct shim_node));
        if (!*p) return 0;
        (*p)->key = key;
        return *p;
    }
    while (*p) {
        int c = compar ? compar(key, (*p)->key) : node_cmp(key, (*p)->key);
        if (c == 0) return *p;
        p = (c < 0) ? &(*p)->llink : &(*p)->rlink;
    }
    *p = (struct shim_node *)calloc(1, sizeof(struct shim_node));
    if (!*p) return 0;
    (*p)->key = key;
    return *p;
}

posix_tnode *tfind(const void *key, posix_tnode * const *rootp,
                   int (*compar)(const void *, const void *)) {
    if (!rootp) return 0;
    struct shim_node *p = (struct shim_node *)*rootp;
    while (p) {
        int c = compar ? compar(key, p->key) : node_cmp(key, p->key);
        if (c == 0) return (posix_tnode *)p;
        p = (c < 0) ? p->llink : p->rlink;
    }
    return 0;
}

void *tdelete(const void *key, posix_tnode **rootp,
              int (*compar)(const void *, const void *)) {
    (void)key; (void)rootp; (void)compar;
    return 0;
}

void twalk(const posix_tnode *root, void (*action)(const posix_tnode *, VISIT, int)) {
    (void)root; (void)action;
}

int pipe2(int fd[2], int flags) {
    if (pipe(fd) != 0) return -1;
    if (flags & O_CLOEXEC) {
        fcntl(fd[0], F_SETFD, FD_CLOEXEC);
        fcntl(fd[1], F_SETFD, FD_CLOEXEC);
    }
    return 0;
}

/* The PS5 libc rejects F_SETFD/F_GETFD with EINVAL. There is no exec on the
   console, so close-on-exec flags have no effect; libcurl treats the error as
   fatal. The console link wraps fcntl so only these two commands are
   emulated and everything else reaches the real function. */
#include <stdarg.h>
extern "C" int __real_fcntl(int fd, int cmd, ...);
extern "C" int __wrap_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    if (cmd == F_SETFD || cmd == F_GETFD) return 0;
    return __real_fcntl(fd, cmd, arg);
}

/* PS5 libc provides gmtime but not gmtime_r. Provide a minimal shim. */
#include <time.h>
struct tm *gmtime_r(const time_t *timep, struct tm *result) {
    struct tm *tmp = gmtime(timep);
    if (!tmp) return 0;
    *result = *tmp;
    return result;
}

/* PS5 libc exports localtime_s with the same shape as localtime_r, but the
   public SDK header does not declare it. */
extern "C" struct tm *localtime_s(const time_t *timep, struct tm *result);

struct tm *localtime_r(const time_t *timep, struct tm *result) {
    if (!timep || !result) return 0;
    return localtime_s(timep, result);
}

/* libcurl/OpenSSL use _longjmp, getpwuid_r and if_nametoindex; the PS5 libc
   exports longjmp but not these spellings. */
#include <setjmp.h>
void _longjmp(jmp_buf env, int val) { longjmp(env, val); }
int _setjmp(jmp_buf env) { return setjmp(env); }

#include <pwd.h>
int getpwuid_r(uid_t uid, struct passwd *pwd, char *buf, size_t buflen,
               struct passwd **result) {
    (void)uid; (void)pwd; (void)buf; (void)buflen;
    if (result) *result = 0;
    return 0;
}

#include <net/if.h>
unsigned int if_nametoindex(const char *name) {
    (void)name;
    return 0;
}

/* recvmmsg is optional for DTLS; OpenSSL only reaches it for UDP BIOs.
   sys/socket.h is deliberately not included here: it would redeclare
   recvmmsg and C++ forbids redefining it. Pointer params need only
   forward declarations. */
struct mmsghdr;
struct timespec;
extern "C" ssize_t recvmmsg(int fd, struct mmsghdr *vec, size_t vlen,
                            int flags, const struct timespec *timeout) {
    (void)fd; (void)vec; (void)vlen; (void)flags; (void)timeout;
    return -1;
}

/* sendmmsg: same family as recvmmsg, only used for datagram BIOs. */
extern "C" ssize_t sendmmsg(int fd, struct mmsghdr *vec, size_t vlen,
                            int flags) {
    (void)fd; (void)vec; (void)vlen; (void)flags;
    return -1;
}

/* dladdr: OpenSSL's DSO loader uses it to map addresses to shared object
   paths; the PS5 libc does not export it. Stub: never reached here. */
#include <dlfcn.h>
int dladdr(const void *addr, Dl_info *info) {
    (void)addr;
    if (info) {
        info->dli_fname = 0;
        info->dli_fbase = 0;
        info->dli_sname = 0;
        info->dli_saddr = 0;
    }
    return 0;
}

/* libcurl was built with zstd content-encoding, but the PS5 SDK stubs do
   not export ZSTD_trace_*. Provide no-op stubs; curl tolerates a null
   decompress stream and falls back cleanly without encoding. */
extern "C" {
void *ZSTD_createDStream(void) { return 0; }
size_t ZSTD_decompressStream(void *dstream, size_t *dst_pos, size_t *dst_size,
                             const void *src, size_t *src_pos,
                             size_t src_size) {
    (void)dstream; (void)dst_pos; (void)dst_size; (void)src; (void)src_pos;
    (void)src_size;
    return 0;
}
void ZSTD_freeDStream(void *dstream) { (void)dstream; }
unsigned int ZSTD_isError(size_t code) { return code != 0; }
unsigned int ZSTD_versionNumber(void) { return 0; }
}

/* Exception entry points referenced by libc++abi but never reached
   because the title is built with -fno-exceptions. Provided so the link
   completes; they abort if somehow invoked. Weak, because the native UI
   kit links libunwind, whose strong definitions must win. */
__attribute__((weak)) void _Unwind_RaiseException(void *) { abort(); }
__attribute__((weak)) void _Unwind_DeleteException(void *) { abort(); }

} // extern "C"
