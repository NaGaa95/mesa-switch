/*
 * Stubs for Rust std symbols that don't exist in Switch/newlib.
 * These are needed to link Rust static libraries compiled for
 * aarch64-unknown-linux-gnu against the devkitPro toolchain.
 *
 * NAK (the shader compiler) never calls these at runtime —
 * they exist only to satisfy the linker.
 */

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Winvalid-memory-model"
#endif

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>

/* --- errno --- */
/* newlib uses a different errno mechanism; bridge it */
int *__errno_location(void) {
    return __errno();
}

/* libgcc provides the actual unwinder on Switch; only stub backtrace helpers. */

/* --- dl_iterate_phdr (used for backtrace, never needed) --- */
int dl_iterate_phdr(void *callback, void *data) { return 0; }

/* --- getauxval (used for CPU feature detection) --- */
unsigned long getauxval(unsigned long type) { return 0; }

/* --- posix_memalign (not in newlib, implement via memalign) --- */
int posix_memalign(void **memptr, size_t alignment, size_t size) {
    void *p = memalign(alignment, size);
    if (!p) return ENOMEM;
    *memptr = p;
    return 0;
}

/* --- mmap/munmap (used by std allocator fallback, not reached) --- */
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, long long off) {
    return (void *)-1; /* MAP_FAILED */
}
int munmap(void *addr, size_t len) { return -1; }

/* --- syscall (used for getrandom, etc.) --- */
long syscall(long num, ...) { return -1; }

/* --- strerror_r --- */
int __xpg_strerror_r(int errnum, char *buf, size_t buflen) {
    if (buf && buflen > 0) {
        strncpy(buf, "error", buflen);
        buf[buflen - 1] = '\0';
    }
    return 0;
}

/* --- open64 (newlib has open but not open64) --- */
#include <fcntl.h>
int open64(const char *path, int flags, ...) {
    return open(path, flags);
}

/* --- fstat64 / stat64 / lseek64 --- */
#include <sys/stat.h>
#include <unistd.h>
int fstat64(int fd, void *buf) { return fstat(fd, (struct stat *)buf); }
int stat64(const char *path, void *buf) { return stat(path, (struct stat *)buf); }
long long lseek64(int fd, long long off, int whence) { return lseek(fd, (off_t)off, whence); }

/* --- writev stub (needed by Rust std over devkitA64) --- */
struct iovec {
    void *iov_base;
    size_t iov_len;
};

ssize_t writev(int fd, const struct iovec *iov, int iovcnt) {
    ssize_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        ssize_t ret = write(fd, iov[i].iov_base, iov[i].iov_len);
        if (ret < 0) return -1;
        total += ret;
    }
    return total;
}

/* --- NVK DRM syncobj stub (when DRM is disabled) --- */
int vk_drm_syncobj_copy_payloads(void *device, unsigned int wait_count, const void *waits, unsigned int signal_count, const void *signals) {
    return 0; /* VK_SUCCESS */
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
