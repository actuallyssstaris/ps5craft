/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Compatibility shims for functions used by libmicrohttpd that the PS5
   C library does not export (tsearch family, pipe2). Signatures match
   <search.h> exactly so libmicrohttpd's calls bind here. posix_tnode is
   an opaque void * in the public header, so nodes are cast internally.
   The PS5 runs on a trusted LAN, so IP limiting is intentionally a
   no-op stub. */
#include <search.h>
#include <stdlib.h>
#include <sys/socket.h>
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

/* Exception entry points referenced by libc++abi but never reached
   because the title is built with -fno-exceptions. Provided so the link
   completes; they abort if somehow invoked. */
void _Unwind_RaiseException(void *) { abort(); }
void _Unwind_DeleteException(void *) { abort(); }

} // extern "C"
