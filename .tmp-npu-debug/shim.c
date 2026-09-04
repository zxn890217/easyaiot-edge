#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/syscall.h>

/* aarch64 only has the *at syscalls; SYS_open does not exist there. */
#ifndef SYS_openat
#define SYS_openat 56
#endif
#ifndef SYS_readlinkat
#define SYS_readlinkat 78
#endif
#ifndef SYS_statx
#define SYS_statx 291
#endif

static FILE *F;
static long IN;

static int want(const char *p)
{
    return p && (strstr(p, "/dev/") || strstr(p, "/sys/"));
}

static void openlog(void)
{
    const char *t;
    char e[256];

    if (F || IN || !getenv("TRON"))
        return;
    IN = 1;
    t = getenv("TRTAG");
    if (!t)
        t = "x";
    snprintf(e, sizeof e, "/tmp/tr/%s.log", t);
    F = fopen(e, "w");
    IN = 0;
}

static void say(const char *s)
{
    if (F) {
        fputs(s, F);
        fputc('\n', F);
        fflush(F);
    }
}

static void head(const char *op, const char *p)
{
    char b[600];
    snprintf(b, sizeof b, "%s %s", op, p);
    say(b);
}

static void res(long r)
{
    char b[192];
    if (r < 0)
        snprintf(b, sizeof b, "      -> FAIL errno=%d(%s)", errno, strerror(errno));
    else
        snprintf(b, sizeof b, "      -> ok(%ld)", r);
    say(b);
}

static long do_open(const char *p, int flags, long m)
{
#ifdef SYS_open
    return syscall(SYS_open, p, flags, m, 0, 0, 0);
#else
    return syscall(SYS_openat, -100, p, flags, m, 0, 0);
#endif
}

static long do_openat(int d, const char *p, int flags, long m)
{
    return syscall(SYS_openat, d, p, flags, m, 0, 0);
}

int open(const char *p, int flags, ...)
{
    long r;
    unsigned m = 0;
    int mine;

    if (flags & 0100) {
        __builtin_va_list a;
        __builtin_va_start(a, flags);
        m = __builtin_va_arg(a, unsigned);
        __builtin_va_end(a);
    }
    mine = !IN;
    if (mine) {
        openlog();
        if (want(p))
            head("open", p);
    }
    IN = 1;
    errno = 0;
    r = do_open(p, flags, m);
    IN = 0;
    if (mine && want(p))
        res(r);
    return (int) r;
}

int open64(const char *p, int flags, ...)
{
    return open(p, flags, (flags & 0100) ? 0666 : 0);
}

int creat(const char *p, unsigned m)
{
    return open(p, 0101 | 0644, m);
}

int openat(int d, const char *p, int flags, ...)
{
    long r;
    unsigned m = 0;
    int mine;

    if (flags & 0100) {
        __builtin_va_list a;
        __builtin_va_start(a, flags);
        m = __builtin_va_arg(a, unsigned);
        __builtin_va_end(a);
    }
    mine = !IN;
    if (mine) {
        openlog();
        if (want(p))
            head("openat", p);
    }
    IN = 1;
    errno = 0;
    r = do_openat(d, p, flags, m);
    IN = 0;
    if (mine && want(p))
        res(r);
    return (int) r;
}

int openat64(int d, const char *p, int flags, ...)
{
    return openat(d, p, flags, (flags & 0100) ? 0666 : 0);
}

ssize_t readlink(const char *p, char *b, size_t s)
{
    long r;
    int mine;

    mine = !IN;
    if (mine) {
        openlog();
        if (want(p))
            head("readlink", p);
    }
    IN = 1;
    errno = 0;
    r = syscall(SYS_readlinkat, -100, p, b, s, 0, 0);
    IN = 0;
    if (mine && want(p)) {
        if (r > 0) {
            if ((size_t) r < s) {
                b[r] = 0;
                say(b);
            }
        } else {
            res(r);
        }
    }
    return r;
}

ssize_t readlinkat(int d, const char *p, char *b, size_t s)
{
    return readlink(p, b, s);
}

int statx(int d, const char *p, int f, unsigned m, void *o)
{
    long r;
    int mine;

    mine = !IN;
    if (mine) {
        openlog();
        if (want(p))
            head("statx", p);
    }
    IN = 1;
    errno = 0;
    r = syscall(SYS_statx, d, p, f, m, (long) o, 0);
    IN = 0;
    if (mine && want(p))
        res(r);
    return (int) r;
}

static int (*rioctl)(int, unsigned long, void *);

int ioctl(int fd, unsigned long req, void *arg)
{
    int r;
    char b[160];

    if (!rioctl)
        rioctl = (int (*)(int, unsigned long, void *)) dlsym(RTLD_NEXT, "ioctl");
    openlog();
    if (F) {
        snprintf(b, sizeof b, "ioctl fd=%d req=%#lx", fd, req);
        say(b);
    }
    IN = 1;
    errno = 0;
    r = rioctl(fd, req, arg);
    IN = 0;
    if (F)
        res(r);
    return r;
}
