/*
 * Stubs for Rust std symbols that don't exist in Switch/newlib.
 * These are needed to link Rust static libraries compiled for
 * aarch64-unknown-linux-gnu against the devkitPro toolchain.
 *
 * NAK (the shader compiler) never calls these at runtime —
 * they exist only to satisfy the linker.
 */

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

/* --- regex stubs (needed for xmlconfig on bare metal) --- */
int regcomp(void *preg, const char *regex, int cflags) { return 0; }
int regexec(const void *preg, const char *string, size_t nmatch, void *pmatch, int eflags) { return 1; /* REG_NOMATCH */ }
void regfree(void *preg) {}

/* --- expat stubs (needed for xmlconfig on bare metal) --- */
static void *dummy_parser_ptr = NULL;
void *XML_ParserCreate(const char *encoding) { return &dummy_parser_ptr; }
void XML_SetElementHandler(void *parser, void *start, void *end) {}
void XML_SetUserData(void *parser, void *userData) { *(void **)parser = userData; }
void *XML_GetBuffer(void *parser, int len) {
    static char dummy_buf[4096];
    return dummy_buf;
}
int XML_ParseBuffer(void *parser, int len, int isFinal) { return 1; /* XML_STATUS_OK */ }
void XML_ParserFree(void *parser) {}
int XML_GetErrorCode(void *parser) { return 0; }
const char *XML_ErrorString(int code) { return "stub"; }
