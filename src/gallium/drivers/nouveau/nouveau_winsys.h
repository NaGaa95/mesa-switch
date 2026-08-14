#ifndef NOUVEAU_WINSYS_H
#define NOUVEAU_WINSYS_H

#include <stdint.h>
#include <inttypes.h>

#include "pipe/p_defines.h"
#include "util/os_misc.h"

#include "drm-uapi/drm.h"
#include "drm-uapi/nouveau_drm.h"
#include "nouveau.h"
#include "nv_device_info.h"
#include "nouveau_screen.h"

#ifndef NV04_PFIFO_MAX_PACKET_LEN
#define NV04_PFIFO_MAX_PACKET_LEN 2047
#endif

#define NOUVEAU_MIN_BUFFER_MAP_ALIGN      64
#define NOUVEAU_MIN_BUFFER_MAP_ALIGN_MASK (NOUVEAU_MIN_BUFFER_MAP_ALIGN - 1)

#ifdef __SWITCH__
#include "nouveau_switch_libdrm.h"
#define NOUVEAU_BUFREF_LIST_TYPE struct nouveau_list

/* Horizon GPU allocations share the process memory entitlement with the
 * application.  Keep enough memory outside Nouveau's advertised allocation
 * budget for application/libnx allocations and allocation-time bookkeeping.
 */
#define NOUVEAU_SWITCH_MIN_MEMORY_HEADROOM (64ull * 1024ull * 1024ull)

static inline uint64_t
nouveau_switch_memory_headroom(uint64_t total)
{
   return MIN2(total, MAX2(NOUVEAU_SWITCH_MIN_MEMORY_HEADROOM, total / 20));
}

static inline uint64_t
nouveau_switch_memory_budget(uint64_t total, uint64_t available)
{
   available = MIN2(total, available);
   const uint64_t headroom = nouveau_switch_memory_headroom(total);

   return available > headroom ? available - headroom : 0;
}

int nouveau_switch_pushbuf_kick_deferred(struct nouveau_pushbuf *push,
                                         struct nouveau_object *chan);
int nouveau_switch_pushbuf_kick_full_barrier(
   struct nouveau_pushbuf *push, struct nouveau_object *chan);
int nouveau_switch_pushbuf_enqueue_nvmultifence(
   struct nouveau_pushbuf *push, const NvMultiFence *waits);
bool nouveau_switch_pushbuf_get_last_fence(
   struct nouveau_pushbuf *push, NvFence *out_fence,
   bool *cpu_visible_out);
int nouveau_switch_pushbuf_get_cpu_fence(
   struct nouveau_pushbuf *push, NvFence *out_fence);
int nouveau_switch_pushbuf_upgrade_physical_cpu_fence(
   struct nouveau_pushbuf *push, const NvFence *physical_fence,
   NvFence *out_fence);
int nouveau_switch_pushbuf_kick_cpu(struct nouveau_pushbuf *push,
                                    struct nouveau_object *chan);
int nouveau_switch_pushbuf_wait_fence(struct nouveau_pushbuf *push,
                                      const NvFence *fence,
                                      uint64_t timeout_ns);
int nouveau_switch_pushbuf_wait_fence_required(
   struct nouveau_pushbuf *push, const NvFence *fence, uint64_t timeout_ns,
   const char *reason);
int nouveau_switch_pushbuf_get_error(struct nouveau_pushbuf *push);
int nouveau_switch_pushbuf_enable_mapped_completion(
   struct nouveau_pushbuf *push);
bool nouveau_switch_pushbuf_channel_lost(struct nouveau_pushbuf *push);
bool nouveau_switch_pushbuf_device_lost(struct nouveau_pushbuf *push);
void nouveau_switch_pushbuf_set_kick_notify(
   struct nouveau_pushbuf *push,
   bool (*kick_notify)(struct nouveau_pushbuf *));
void nouveau_switch_pushbuf_set_native_kick_notify(
   struct nouveau_pushbuf *push,
   uint32_t (*native_kick_notify)(struct nouveau_pushbuf *,
                                  const NvFence *, bool cpu_visible));
void nouveau_switch_pushbuf_set_fence_batch_notify(
   struct nouveau_pushbuf *push,
   bool (*marker_notify)(struct nouveau_pushbuf *, uint64_t *cookie_out),
   uint32_t (*native_notify)(struct nouveau_pushbuf *, const NvFence *,
                             bool cpu_visible, uint64_t cookie));
uint64_t nouveau_switch_pushbuf_batch_generation(
   struct nouveau_pushbuf *push);
#else
#define NOUVEAU_BUFREF_LIST_TYPE struct list_head
#endif

#define NOUVEAU_BUFREF_LIST_FOR_EACH(ref, list)                              \
   for (NOUVEAU_BUFREF_LIST_TYPE *__node = (list)->next;                     \
        __node != (list) && (((ref) = (struct nouveau_bufref *)__node), true); \
        __node = __node->next)

static inline const struct nv_device_info *
nouveau_device_get_info(const struct nouveau_device *dev,
                        struct nv_device_info *storage)
{
#ifdef __SWITCH__
   (void)storage;
   return nouveau_switch_device_get_info(dev);
#else
   (void)storage;
   return &dev->info;
#endif
}

static inline uint64_t
nouveau_device_get_memory_size(const struct nouveau_device *dev)
{
#ifdef __SWITCH__
   return dev->vram_size ? dev->vram_size : dev->gart_size;
#else
   return dev->vram_size;
#endif
}

static inline uint32_t
PUSH_AVAIL(struct nouveau_pushbuf *push)
{
   return push->end - push->cur;
}

static inline bool
PUSH_SPACE_EX(struct nouveau_pushbuf *push, uint32_t size, uint32_t relocs, uint32_t pushes)
{
   struct nouveau_pushbuf_priv *ppush = push->user_priv;
   simple_mtx_lock(&ppush->screen->fence.lock);
   bool res = nouveau_pushbuf_space(push, size, relocs, pushes) == 0;
   simple_mtx_unlock(&ppush->screen->fence.lock);
   return res;
}

static inline bool
PUSH_SPACE(struct nouveau_pushbuf *push, uint32_t size)
{
   /* Provide a buffer so that fences always have room to be emitted */
   size += 8;
   if (PUSH_AVAIL(push) < size)
      return PUSH_SPACE_EX(push, size, 0, 0);
   return true;
}

static inline void
PUSH_DATA(struct nouveau_pushbuf *push, uint32_t data)
{
   *push->cur++ = data;
}

static inline void
PUSH_DATAp(struct nouveau_pushbuf *push, const void *data, uint32_t size)
{
   memcpy(push->cur, data, size * 4);
   push->cur += size;
}

static inline void
PUSH_DATAb(struct nouveau_pushbuf *push, const void *data, uint32_t size)
{
   memcpy(push->cur, data, size);
   push->cur += DIV_ROUND_UP(size, 4);
}

static inline void
PUSH_DATAf(struct nouveau_pushbuf *push, float f)
{
   union { float f; uint32_t i; } u;
   u.f = f;
   PUSH_DATA(push, u.i);
}

static inline int
PUSH_REFN(struct nouveau_pushbuf *push, struct nouveau_pushbuf_refn *refs, int nr)
{
   struct nouveau_pushbuf_priv *ppush = push->user_priv;
   simple_mtx_lock(&ppush->screen->fence.lock);
   int ret = nouveau_pushbuf_refn(push, refs, nr);
   simple_mtx_unlock(&ppush->screen->fence.lock);
   return ret;
}

static inline int
PUSH_REF1(struct nouveau_pushbuf *push, struct nouveau_bo *bo, uint32_t flags)
{
   struct nouveau_pushbuf_refn ref = { bo, flags };
   return PUSH_REFN(push, &ref, 1);
}

static inline int
PUSH_KICK_RET(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *ppush = push->user_priv;
   simple_mtx_lock(&ppush->screen->fence.lock);
#ifdef __SWITCH__
   const int ret = nouveau_pushbuf_kick(push, push->channel);
#else
   const int ret = nouveau_pushbuf_kick(push);
#endif
   simple_mtx_unlock(&ppush->screen->fence.lock);
   return ret;
}

static inline void
PUSH_KICK(struct nouveau_pushbuf *push)
{
   const int ret = PUSH_KICK_RET(push);
#ifndef __SWITCH__
   assert(!ret);
#endif
   /* The Switch pushbuf latches and logs the first failure; reset-status
    * queries propagate it to GL even in NDEBUG builds.
    */
   (void)ret;
}

#ifdef __SWITCH__
static inline int
PUSH_KICK_FULL_BARRIER(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *ppush = push->user_priv;
   simple_mtx_lock(&ppush->screen->fence.lock);
   const int ret =
      nouveau_switch_pushbuf_kick_full_barrier(push, push->channel);
   simple_mtx_unlock(&ppush->screen->fence.lock);
   return ret;
}
#endif

/* Defer draw kicks only; other kick and wait paths still submit immediately. */
static inline void
PUSH_KICK_DEFER(struct nouveau_pushbuf *push)
{
#ifdef __SWITCH__
   struct nouveau_pushbuf_priv *ppush = push->user_priv;

   simple_mtx_lock(&ppush->screen->fence.lock);
   const int ret =
      nouveau_switch_pushbuf_kick_deferred(push, push->channel);
   (void)ret;
   simple_mtx_unlock(&ppush->screen->fence.lock);
#else
   PUSH_KICK(push);
#endif
}

static inline int
PUSH_VAL(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *ppush = push->user_priv;
   simple_mtx_lock(&ppush->screen->fence.lock);
   int res = nouveau_pushbuf_validate(push);
   simple_mtx_unlock(&ppush->screen->fence.lock);
   return res;
}

static inline uint32_t
NV04_FIFO_PKHDR(int subc, int mthd, unsigned size)
{
   return 0x00000000 | (size << 18) | (subc << 13) | mthd;
}

static inline uint32_t
NV04_FIFO_PKHDR_NI(int subc, int mthd, unsigned size)
{
   return 0x40000000 | (size << 18) | (subc << 13) | mthd;
}

static inline void
BEGIN_NV04(struct nouveau_pushbuf *push, int subc, int mthd, unsigned size)
{
#ifndef NV50_PUSH_EXPLICIT_SPACE_CHECKING
   PUSH_SPACE(push, size + 1);
#endif
   PUSH_DATA (push, NV04_FIFO_PKHDR(subc, mthd, size));
}

static inline void
BEGIN_NI04(struct nouveau_pushbuf *push, int subc, int mthd, unsigned size)
{
#ifndef NV50_PUSH_EXPLICIT_SPACE_CHECKING
   PUSH_SPACE(push, size + 1);
#endif
   PUSH_DATA (push, NV04_FIFO_PKHDR_NI(subc, mthd, size));
}

static inline int
BO_MAP(struct nouveau_screen *screen, struct nouveau_bo *bo, uint32_t access, struct nouveau_client *client)
{
   int res;
#ifdef __SWITCH__
   nouveau_screen_submission_lock(screen);
#endif
   simple_mtx_lock(&screen->fence.lock);
   res = nouveau_bo_map(bo, access, client);
   simple_mtx_unlock(&screen->fence.lock);
#ifdef __SWITCH__
   nouveau_screen_submission_unlock(screen);
#endif
   return res;
}

static inline int
BO_WAIT(struct nouveau_screen *screen, struct nouveau_bo *bo, uint32_t access, struct nouveau_client *client)
{
   int res;
#ifdef __SWITCH__
   nouveau_screen_submission_lock(screen);
#endif
   simple_mtx_lock(&screen->fence.lock);
   res = nouveau_bo_wait(bo, access, client);
   simple_mtx_unlock(&screen->fence.lock);
#ifdef __SWITCH__
   nouveau_screen_submission_unlock(screen);
#endif
   return res;
}

#define NOUVEAU_RESOURCE_FLAG_LINEAR   (PIPE_RESOURCE_FLAG_DRV_PRIV << 0)
#define NOUVEAU_RESOURCE_FLAG_DRV_PRIV (PIPE_RESOURCE_FLAG_DRV_PRIV << 1)

static inline uint32_t
nouveau_screen_transfer_flags(unsigned pipe)
{
   uint32_t flags = 0;

   if (!(pipe & PIPE_MAP_UNSYNCHRONIZED)) {
      if (pipe & PIPE_MAP_READ)
         flags |= NOUVEAU_BO_RD;
      if (pipe & PIPE_MAP_WRITE)
         flags |= NOUVEAU_BO_WR;
      if (pipe & PIPE_MAP_DONTBLOCK)
         flags |= NOUVEAU_BO_NOBLOCK;
   }

   return flags;
}

extern struct nouveau_screen *
nv30_screen_create(struct nouveau_device *);

extern struct nouveau_screen *
nv50_screen_create(struct nouveau_device *);

extern struct nouveau_screen *
nvc0_screen_create(struct nouveau_device *);

static inline uint64_t
nouveau_device_get_global_mem_size(struct nouveau_device *dev)
{
#ifdef __SWITCH__
   /* On Switch, local and staging allocations use the same UMA process heap.
    * The winsys limit already includes the process-memory safety headroom and
    * provides a stable maximum allocation capability for the screen lifetime.
    */
   uint64_t size = dev->vram_size ? dev->vram_limit : dev->gart_limit;
#else
   uint64_t size = dev->vram_size;

   if (!size) {
      if (!os_get_available_system_memory(&size))
         size = dev->gart_limit;
      size = MIN2(dev->gart_size, size);
   }
#endif

   /* cap to 32 bit on nv50 and older */
   if (dev->chipset < 0xc0)
      size = MIN2(size, 1ull << 32);
   else
      size = MIN2(size, 1ull << 40);

   return size;
}

#endif
