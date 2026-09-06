/** @file switch_mman.h
 * Heap-backed anonymous allocation shim for Switch/libnx.
 */

#ifndef _SWITCH_MMAN_H_
#define _SWITCH_MMAN_H_

#include <sys/types.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protection flags (unused on Switch, but needed for API compat) */
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

/* Map flags */
#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_ANON            MAP_ANONYMOUS
#define MAP_FIXED_NOREPLACE 0x0   /* no-op on Switch */

#define MAP_FAILED ((void *)-1)

/** Heap-backed anonymous mapping. File descriptors, protection, and
 * fixed-address semantics are unsupported.
 */
static inline void *mmap(void *addr, size_t length, int prot, int flags,
                         int fd, off_t offset)
{
   (void)addr;
   (void)prot;
   (void)offset;

   /* We only support anonymous mappings */
   if (fd != -1 || !(flags & MAP_ANONYMOUS)) {
      return MAP_FAILED;
   }

   /* Page-align the size (4 KB pages) */
   size_t aligned = (length + 0xFFF) & ~(size_t)0xFFF;

   void *ptr = aligned_alloc(0x1000, aligned);
   if (!ptr)
      return MAP_FAILED;

   memset(ptr, 0, aligned);
   return ptr;
}

/**
 * mprotect stub — no-op on Switch.
 */
static inline int mprotect(void *addr, size_t len, int prot)
{
   (void)addr;
   (void)len;
   (void)prot;
   return 0;
}

/**
 * munmap substitute — frees memory allocated by our mmap shim.
 */
static inline int munmap(void *addr, size_t length)
{
   (void)length;
   free(addr);
   return 0;
}

/* shm_open / shm_unlink stubs */
static inline int shm_open(const char *name, int oflag, int mode)
{
   (void)name; (void)oflag; (void)mode;
   return -1;
}

static inline int shm_unlink(const char *name)
{
   (void)name;
   return -1;
}

#ifdef __cplusplus
}
#endif

#endif /* _SWITCH_MMAN_H_ */
