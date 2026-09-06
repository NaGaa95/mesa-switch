/*
 * Copyright 2012 Red Hat Inc.
 * Copyright 2026 Mesa Switch contributors
 * SPDX-License-Identifier: MIT
 *
 * Public nouveau.h compatibility adapter backed by Mesa's generic Horizon
 * backend.  The public structures are embedded as prefixes because Gallium
 * intentionally consumes that public API; no external private ABI is used.
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "c11/threads.h"
#include "nouveau/horizon/nouveau_horizon.h"
#include "nouveau_switch_libdrm.h"
#include "util/u_debug.h"
#include "util/os_time.h"
#include "util/u_math.h"
#include "util/vma.h"

#include "nvif/class.h"

struct nouveau_switch_object {
   struct nouveau_object base;
   struct nouveau_device *device;
};

#define NOUVEAU_SWITCH_BO_POOL_CHUNK_SIZE (16ull << 20)
#define NOUVEAU_SWITCH_BO_POOL_MAX_ALLOCATION (1ull << 20)

struct nouveau_switch_bo_pool {
   struct nouveau_switch_bo_pool *next;
   struct nouveau_horizon_memory *memory;
   struct nouveau_horizon_va *va;
   struct util_vma_heap heap;
   void *cpu_map;
   uint64_t size_B;
   bool cpu_uncached;
   bool gpu_cached;
   bool has_pte_kind;
   uint8_t pte_kind;
};

enum nouveau_switch_completion_source_state {
   NOUVEAU_SWITCH_COMPLETION_SOURCE_ACTIVE,
   NOUVEAU_SWITCH_COMPLETION_SOURCE_COMPLETE,
   NOUVEAU_SWITCH_COMPLETION_SOURCE_FAILED,
};

/* BOs retain this adapter, which borrows the producer channel. Its lock
 * protects CPU-fence upgrades on a separate completion channel.
 * total_order_channels must stay enabled so later rendering waits for
 * replacement completions. Teardown waits the producer idle, then retires
 * this source under its lock before releasing the channel.
 */
struct nouveau_switch_completion_source {
   int refcnt;
   simple_mtx_t lock;
   struct nouveau_horizon_channel *channel;
   struct nouveau_horizon_channel *sync_channel;
   struct nouveau_horizon_device *device;
   NvFence cached_producer_fence;
   NvFence cached_cpu_fence;
   bool cached_cpu_fence_valid;
   enum nouveau_switch_completion_source_state state;
};

static void bo_destroy(struct nouveau_switch_bo *bo);

struct nouveau_switch_completion_source *
nouveau_switch_completion_source_create(
   struct nouveau_horizon_channel *channel,
   struct nouveau_horizon_device *device)
{
   if (!channel || !device)
      return NULL;
   struct nouveau_horizon_device_properties properties = {0};
   nouveau_horizon_device_get_properties(device, &properties);
   if (!properties.total_order_channels) {
      _debug_printf("nouveau/switch: completion-only CPU fences require "
                    "device-wide channel ordering\n");
      return NULL;
   }

   struct nouveau_switch_completion_source *source =
      calloc(1, sizeof(*source));
   if (!source)
      return NULL;

   p_atomic_set(&source->refcnt, 1);
   simple_mtx_init(&source->lock, mtx_plain);
   source->channel = channel;
   source->device = device;
   source->state = NOUVEAU_SWITCH_COMPLETION_SOURCE_ACTIVE;
   return source;
}

struct nouveau_switch_completion_source *
nouveau_switch_completion_source_ref(
   struct nouveau_switch_completion_source *source)
{
   if (source)
      p_atomic_inc(&source->refcnt);
   return source;
}

void
nouveau_switch_completion_source_unref(
   struct nouveau_switch_completion_source *source)
{
   if (!source || !p_atomic_dec_zero(&source->refcnt))
      return;

   simple_mtx_destroy(&source->lock);
   free(source);
}

void
nouveau_switch_completion_source_retire(
   struct nouveau_switch_completion_source *source, bool complete)
{
   if (!source)
      return;

   simple_mtx_lock(&source->lock);
   if (source->sync_channel) {
      const enum nouveau_horizon_status idle_status =
         nouveau_horizon_channel_wait_idle(source->sync_channel, UINT64_MAX);
      const enum nouveau_horizon_channel_put_result put_result =
         nouveau_horizon_channel_put(source->sync_channel);
      if (idle_status != NOUVEAU_HORIZON_SUCCESS ||
          put_result != NOUVEAU_HORIZON_CHANNEL_PUT_COMPLETE) {
         complete = false;
         _debug_printf("nouveau/switch: completion-only channel teardown "
                       "failed; retaining its native ownership graph\n");
      }
      source->sync_channel = NULL;
   }
   source->state = complete ? NOUVEAU_SWITCH_COMPLETION_SOURCE_COMPLETE :
                              NOUVEAU_SWITCH_COMPLETION_SOURCE_FAILED;
   source->channel = NULL;
   source->device = NULL;
   simple_mtx_unlock(&source->lock);
}

int
nouveau_switch_completion_source_upgrade_cpu_fence(
   struct nouveau_switch_completion_source *source,
   const NvFence *physical_fence, NvFence *fence_out,
   bool *already_complete_out)
{
   *already_complete_out = false;
   fence_out->id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
   fence_out->value = 0;

   simple_mtx_lock(&source->lock);
   if (source->state == NOUVEAU_SWITCH_COMPLETION_SOURCE_COMPLETE) {
      *already_complete_out = true;
      simple_mtx_unlock(&source->lock);
      return 0;
   }
   if (source->state != NOUVEAU_SWITCH_COMPLETION_SOURCE_ACTIVE ||
       !source->channel || !source->device) {
      simple_mtx_unlock(&source->lock);
      return -EIO;
   }

   if (!source->sync_channel) {
      const struct nouveau_horizon_channel_create_info info = {
         .priority = NOUVEAU_HORIZON_CHANNEL_PRIORITY_MEDIUM,
      };
      const enum nouveau_horizon_status create_status =
         nouveau_horizon_channel_create(source->device, &info,
                                        &source->sync_channel);
      if (create_status != NOUVEAU_HORIZON_SUCCESS) {
         simple_mtx_unlock(&source->lock);
         return nouveau_switch_horizon_status_to_errno(create_status);
      }
   }

   /* Reuse completion upgrades only for equal producer fences. BO records
    * may outlive syncpoint wraparound, making threshold comparisons unsafe
    * here.
    */
   if (source->cached_cpu_fence_valid &&
       source->cached_producer_fence.id == physical_fence->id &&
       source->cached_producer_fence.value == physical_fence->value) {
      *fence_out = source->cached_cpu_fence;
      simple_mtx_unlock(&source->lock);
      return 0;
   }

   const struct nouveau_horizon_fence wait = {
      .id = physical_fence->id,
      .value = physical_fence->value,
   };
   enum nouveau_horizon_status status =
      nouveau_horizon_channel_enqueue_waits(source->sync_channel, 1, &wait);
   struct nouveau_horizon_fence hfence = {
      .id = NOUVEAU_HORIZON_INVALID_FENCE_ID,
   };
   if (status == NOUVEAU_HORIZON_SUCCESS) {
      status = nouveau_horizon_channel_submit(
         source->sync_channel, NOUVEAU_HORIZON_COMPLETION_CPU, &hfence);
   }
   if (status == NOUVEAU_HORIZON_SUCCESS &&
       (int32_t)hfence.id < 0) {
      source->state = NOUVEAU_SWITCH_COMPLETION_SOURCE_FAILED;
      status = NOUVEAU_HORIZON_ERROR_DEVICE_LOST;
   }
   if (status == NOUVEAU_HORIZON_SUCCESS) {
      fence_out->id = hfence.id;
      fence_out->value = hfence.value;
      source->cached_producer_fence = *physical_fence;
      source->cached_cpu_fence = *fence_out;
      source->cached_cpu_fence_valid = true;
   }
   simple_mtx_unlock(&source->lock);

   return nouveau_switch_horizon_status_to_errno(status);
}

static int
completion_source_wait_fence(
   struct nouveau_switch_completion_source *source,
   const NvFence *fence, uint64_t timeout_ns)
{
   if (!source || !fence || (int32_t)fence->id < 0)
      return -EIO;

   simple_mtx_lock(&source->lock);
   if (source->state == NOUVEAU_SWITCH_COMPLETION_SOURCE_COMPLETE) {
      simple_mtx_unlock(&source->lock);
      return 0;
   }
   if (source->state != NOUVEAU_SWITCH_COMPLETION_SOURCE_ACTIVE ||
       !source->channel) {
      simple_mtx_unlock(&source->lock);
      return -EIO;
   }

   const struct nouveau_horizon_fence hfence = {
      .id = fence->id,
      .value = fence->value,
   };
   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_fence_wait(
         source->channel, &hfence, timeout_ns);
   simple_mtx_unlock(&source->lock);
   return nouveau_switch_horizon_status_to_errno(status);
}

static uint64_t
switch_fence_wait_timeout(uint64_t requested_timeout_ns)
{
   if (requested_timeout_ns != UINT64_MAX)
      return requested_timeout_ns;

   const int64_t watchdog_us =
      debug_get_num_option("NOUVEAU_SWITCH_FENCE_TIMEOUT_US", 0);
   if (watchdog_us <= 0)
      return UINT64_MAX;

   return (uint64_t)watchdog_us > UINT64_MAX / 1000 ?
             UINT64_MAX : (uint64_t)watchdog_us * 1000;
}

static struct nouveau_switch_device *
switch_device_ref(struct nouveau_switch_device *device)
{
   p_atomic_inc(&device->refcnt);
   return device;
}

static void
bo_pool_destroy(struct nouveau_switch_bo_pool *pool)
{
   if (!pool)
      return;

   util_vma_heap_finish(&pool->heap);
   nouveau_horizon_va_put(pool->va);
   nouveau_horizon_memory_put(pool->memory);
   free(pool);
}

static void
switch_device_unref(struct nouveau_switch_device *device)
{
   if (!device || !p_atomic_dec_zero(&device->refcnt))
      return;

   struct nouveau_switch_bo_pool *pool = device->bo_pools;
   while (pool) {
      struct nouveau_switch_bo_pool *next = pool->next;
      bo_pool_destroy(pool);
      pool = next;
   }

   nouveau_horizon_device_put(device->hdev);
   simple_mtx_destroy(&device->lock);
   free(device);
}

static bool
bo_try_ref(struct nouveau_switch_bo *bo)
{
   int refcnt = p_atomic_read(&bo->refcnt);
   while (refcnt > 0) {
      const int old = p_atomic_cmpxchg(&bo->refcnt, refcnt, refcnt + 1);
      if (old == refcnt)
         return true;
      refcnt = old;
   }

   return false;
}

/* Called with device->lock held.  Zero-ref wrappers remain linked until their
 * destructor acquires the lock, so never resurrect one after final unref has
 * already selected destruction.
 */
static struct nouveau_switch_bo *
device_lookup_named_bo_locked(struct nouveau_switch_device *device,
                              uint32_t nvmap_id, bool *tombstone_out,
                              bool *quarantined_out,
                              NvFence *tombstone_fence_out)
{
   *tombstone_out = false;
   *quarantined_out = false;
   tombstone_fence_out->id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
   tombstone_fence_out->value = 0;

   for (struct nouveau_switch_bo *bo = device->named_bos; bo;
        bo = bo->next_named) {
      if (nouveau_horizon_memory_get_nvmap_id(bo->memory) != nvmap_id)
         continue;

      if (bo_try_ref(bo))
         return bo;

      /* Final unref selected destruction but the wrapper remains the
       * canonical hazard tombstone until its last GPU use retires.  Copy the
       * dependency while the registry keeps the object storage stable.
       */
      simple_mtx_lock(&bo->lock);
      *tombstone_out = true;
      *quarantined_out = bo->quarantined;
      *tombstone_fence_out = bo->fence;
      simple_mtx_unlock(&bo->lock);
      return NULL;
   }

   return NULL;
}

static int
device_wait_named_tombstone(struct nouveau_switch_device *device,
                            bool quarantined, const NvFence *fence)
{
   if (quarantined)
      return -EIO;

   if ((int32_t)fence->id >= 0) {
      const struct nouveau_horizon_fence horizon_fence = {
         .id = fence->id,
         .value = fence->value,
      };
      const uint64_t timeout_ns = switch_fence_wait_timeout(UINT64_MAX);
      const enum nouveau_horizon_status status = nouveau_horizon_fence_wait(
         device->hdev, &horizon_fence, timeout_ns);
      if (status != NOUVEAU_HORIZON_SUCCESS) {
         _debug_printf(
            "nouveau/switch: named BO tombstone fence wait failed: %s "
            "(syncpoint=%u threshold=%u timeout_ns=%llu)\n",
            nouveau_horizon_status_string(status), fence->id, fence->value,
            (unsigned long long)timeout_ns);
         return nouveau_switch_horizon_status_to_errno(status);
      }
   }

   thrd_yield();
   return 0;
}

static void
device_register_bo(struct nouveau_switch_device *device,
                   struct nouveau_switch_bo *bo)
{
   simple_mtx_lock(&device->lock);
   bo->next_named = device->named_bos;
   device->named_bos = bo;
   bo->registered = true;
   simple_mtx_unlock(&device->lock);
}

int
nouveau_switch_horizon_status_to_errno(enum nouveau_horizon_status status)
{
   switch (status) {
   case NOUVEAU_HORIZON_SUCCESS:
      return 0;
   case NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT:
      return -EINVAL;
   case NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY:
      return -ENOMEM;
   case NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY:
   case NOUVEAU_HORIZON_ERROR_NO_SPACE:
      return -ENOSPC;
   case NOUVEAU_HORIZON_ERROR_NOT_SUPPORTED:
      return -ENOSYS;
   case NOUVEAU_HORIZON_ERROR_TIMEOUT:
   case NOUVEAU_HORIZON_ERROR_BUSY:
      return -EAGAIN;
   case NOUVEAU_HORIZON_ERROR_DEVICE_LOST:
      return -EIO;
   case NOUVEAU_HORIZON_ERROR_SYSTEM:
   default:
      return -EIO;
   }
}

static struct nouveau_device *
object_device(struct nouveau_object *object)
{
   if (!object)
      return NULL;

   if (object->oclass == NOUVEAU_DEVICE_CLASS)
      return &((struct nouveau_switch_device *)object)->base;

   return ((struct nouveau_switch_object *)object)->device;
}

static bool
device_supports_class(const struct nv_device_info *info, uint32_t oclass)
{
   return oclass == info->cls_eng3d || oclass == info->cls_compute ||
          oclass == info->cls_copy || oclass == info->cls_eng2d ||
          oclass == info->cls_m2mf || oclass == info->cls_gpfifo ||
          oclass == 0x906e; /* GF100 software object */
}

int
nouveau_object_mclass(struct nouveau_object *object,
                      const struct nouveau_mclass *classes)
{
   if (!object || !classes)
      return -EINVAL;

   struct nouveau_device *device = object_device(object);
   if (!device)
      return -EINVAL;

   const struct nv_device_info *info =
      nouveau_switch_device_get_info(device);
   for (int i = 0; classes[i].oclass; i++) {
      if (device_supports_class(info, classes[i].oclass))
         return i;
   }

   return -ENODEV;
}

int
nouveau_object_new(struct nouveau_object *parent, uint64_t handle,
                   uint32_t oclass, void *data, uint32_t length,
                   struct nouveau_object **out)
{
   if (!parent || !out)
      return -EINVAL;

   struct nouveau_switch_object *object = calloc(1, sizeof(*object));
   if (!object)
      return -ENOMEM;

   object->device = object_device(parent);
   object->base.parent = parent;
   object->base.handle = handle;
   object->base.oclass = oclass;

   if (oclass == NOUVEAU_FIFO_CHANNEL_CLASS) {
      struct nouveau_fifo *fifo = calloc(1, sizeof(*fifo));
      if (!fifo) {
         free(object);
         return -ENOMEM;
      }

      fifo->object = parent;
      object->base.data = fifo;
      object->base.length = sizeof(*fifo);
   } else if (data && length) {
      object->base.data = malloc(length);
      if (!object->base.data) {
         free(object);
         return -ENOMEM;
      }
      memcpy(object->base.data, data, length);
      object->base.length = length;
   }

   *out = &object->base;
   return 0;
}

void
nouveau_object_del(struct nouveau_object **pobject)
{
   if (!pobject || !*pobject)
      return;

   struct nouveau_switch_object *object =
      (struct nouveau_switch_object *)*pobject;
   free(object->base.data);
   free(object);
   *pobject = NULL;
}

int
nouveau_drm_new(int fd, struct nouveau_drm **out)
{
   if (!out)
      return -EINVAL;

   struct nouveau_drm *drm = calloc(1, sizeof(*drm));
   if (!drm)
      return -ENOMEM;

   drm->fd = fd;
   drm->version = 0x01000202;
   *out = drm;
   return 0;
}

void
nouveau_drm_del(struct nouveau_drm **pdrm)
{
   if (!pdrm)
      return;
   free(*pdrm);
   *pdrm = NULL;
}

int
nouveau_device_new(struct nouveau_object *parent, int32_t oclass,
                   void *data, uint32_t size, struct nouveau_device **out)
{
   (void)oclass;
   (void)data;
   (void)size;

   if (!parent || !out)
      return -EINVAL;

   struct nouveau_horizon_runtime *runtime = NULL;
   enum nouveau_horizon_status status =
      nouveau_horizon_runtime_get(&runtime);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return nouveau_switch_horizon_status_to_errno(status);

   const bool diagnostics_enabled =
      debug_get_bool_option("NOUVEAU_SWITCH_LOG", false) ||
      debug_get_bool_option("NOUVEAU_SWITCH_STATS", false);
   struct nouveau_horizon_device_create_info create_info = {
      .total_order_channels = true,
      .enable_timing = diagnostics_enabled,
   };
   struct nouveau_horizon_device *hdev = NULL;
   status = nouveau_horizon_device_create(runtime, &create_info, &hdev);
   nouveau_horizon_runtime_put(runtime);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return nouveau_switch_horizon_status_to_errno(status);

   struct nouveau_switch_device *device = calloc(1, sizeof(*device));
   if (!device) {
      nouveau_horizon_device_put(hdev);
      return -ENOMEM;
   }

   simple_mtx_init(&device->lock, mtx_plain);
   p_atomic_set(&device->refcnt, 1);
   device->hdev = hdev;
   device->diagnostics_enabled = diagnostics_enabled;
   device->perf_enabled = false;
   device->base.object.parent = parent;
   device->base.object.handle = UINT64_MAX;
   device->base.object.oclass = NOUVEAU_DEVICE_CLASS;
   device->base.object.length = UINT32_MAX;

   const struct nv_device_info *info =
      nouveau_horizon_device_get_info(hdev);
   struct nouveau_horizon_memory_info memory_info;
   nouveau_horizon_device_get_memory_info(hdev, &memory_info);
   device->base.chipset = info->chipset;
   device->base.vram_size = 0;
   device->base.vram_limit = 0;
   device->base.gart_size = memory_info.total_B;
   device->base.gart_limit = memory_info.available_B;

   *out = &device->base;
   return 0;
}

void
nouveau_device_del(struct nouveau_device **pdev)
{
   if (!pdev || !*pdev)
      return;

   struct nouveau_switch_device *device = nouveau_switch_device(*pdev);
   *pdev = NULL;
   switch_device_unref(device);
}

struct nouveau_horizon_device *
nouveau_switch_device_get_horizon(struct nouveau_device *device)
{
   return device ? nouveau_switch_device(device)->hdev : NULL;
}

const struct nv_device_info *
nouveau_switch_device_get_info(const struct nouveau_device *device)
{
   return device ? nouveau_horizon_device_get_info(
                      nouveau_switch_device_const(device)->hdev) : NULL;
}

void
nouveau_switch_device_get_memory_info(
   const struct nouveau_device *device, uint64_t *total_B,
   uint64_t *available_B, uint64_t *allocated_B)
{
   struct nouveau_horizon_memory_info info = {0};
   if (device != NULL) {
      nouveau_horizon_device_get_memory_info(
         nouveau_switch_device_const(device)->hdev, &info);
   }

   if (total_B)
      *total_B = info.total_B;
   if (available_B)
      *available_B = info.available_B;
   if (allocated_B)
      *allocated_B = info.allocated_B;
}

void
nouveau_switch_device_get_pool_stats(
   const struct nouveau_device *device,
   struct nouveau_switch_bo_pool_stats *stats)
{
   if (!stats)
      return;

   memset(stats, 0, sizeof(*stats));
   if (!device)
      return;

   struct nouveau_switch_device *switch_device =
      nouveau_switch_device((struct nouveau_device *)device);
   simple_mtx_lock(&switch_device->lock);
   *stats = switch_device->pool_stats;
   simple_mtx_unlock(&switch_device->lock);
}

int
nouveau_getparam(struct nouveau_device *device, uint64_t param,
                 uint64_t *value)
{
   if (!device || !value)
      return -EINVAL;

   const struct nv_device_info *info =
      nouveau_switch_device_get_info(device);
   switch (param) {
   case NOUVEAU_GETPARAM_GRAPH_UNITS:
      *value = ((uint64_t)info->tpc_count << 8) | info->gpc_count;
      return 0;
   case NOUVEAU_GETPARAM_PCI_DEVICE:
      *value = 0;
      return 0;
   case NOUVEAU_GETPARAM_PTIMER_TIME:
      *value = os_time_get_nano();
      return 0;
   default:
      return -EINVAL;
   }
}

int
nouveau_client_new(struct nouveau_device *device,
                   struct nouveau_client **out)
{
   if (!device || !out)
      return -EINVAL;

   struct nouveau_switch_client *client = calloc(1, sizeof(*client));
   if (!client)
      return -ENOMEM;

   struct nouveau_switch_device *switch_device =
      nouveau_switch_device(device);
   simple_mtx_init(&client->lock, mtx_plain);
   simple_mtx_lock(&switch_device->lock);
   client->base.id = switch_device->next_client_id++;
   simple_mtx_unlock(&switch_device->lock);
   client->base.device = device;
   *out = &client->base;
   return 0;
}

void
nouveau_client_del(struct nouveau_client **pclient)
{
   if (!pclient || !*pclient)
      return;

   struct nouveau_switch_client *client =
      nouveau_switch_client(*pclient);
   nouveau_switch_client_map_finish(&client->base);
   simple_mtx_destroy(&client->lock);
   free(client);
   *pclient = NULL;
}

static bool
bo_pool_matches(const struct nouveau_switch_bo_pool *pool,
                bool cpu_uncached, bool gpu_cached,
                bool has_pte_kind, uint8_t pte_kind)
{
   return pool->cpu_uncached == cpu_uncached &&
          pool->gpu_cached == gpu_cached &&
          pool->has_pte_kind == has_pte_kind &&
          (!has_pte_kind || pool->pte_kind == pte_kind);
}

static struct nouveau_switch_bo_pool *
bo_pool_find_and_allocate_locked(struct nouveau_switch_device *device,
                                 bool cpu_uncached, bool gpu_cached,
                                 bool has_pte_kind,
                                 uint8_t pte_kind, uint64_t size_B,
                                 uint64_t align_B, uint64_t *offset_B)
{
   for (struct nouveau_switch_bo_pool *pool = device->bo_pools; pool;
        pool = pool->next) {
      if (!bo_pool_matches(pool, cpu_uncached, gpu_cached,
                           has_pte_kind, pte_kind))
         continue;

      *offset_B = util_vma_heap_alloc(&pool->heap, size_B, align_B);
      if (*offset_B != 0)
         return pool;
   }

   return NULL;
}

static int
bo_pool_create(struct nouveau_switch_device *device,
               bool cpu_uncached, bool gpu_cached,
               bool has_pte_kind, uint8_t pte_kind,
               struct nouveau_switch_bo_pool **pool_out)
{
   struct nouveau_horizon_device_properties properties;
   nouveau_horizon_device_get_properties(device->hdev, &properties);
   const uint64_t bind_align_B = properties.bind_align_B;
   if (bind_align_B == 0 || bind_align_B >= NOUVEAU_SWITCH_BO_POOL_CHUNK_SIZE)
      return -EINVAL;

   struct nouveau_switch_bo_pool *pool = calloc(1, sizeof(*pool));
   if (!pool)
      return -ENOMEM;

   const struct nouveau_horizon_memory_create_info memory_info = {
      .size_B = NOUVEAU_SWITCH_BO_POOL_CHUNK_SIZE,
      .align_B = bind_align_B,
      .backing_kind = NvKind_Pitch,
       .flags = NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE |
                NOUVEAU_HORIZON_MEMORY_ZERO |
                (cpu_uncached ? 0 : NOUVEAU_HORIZON_MEMORY_CPU_CACHED) |
                (gpu_cached ? NOUVEAU_HORIZON_MEMORY_GPU_CACHED : 0),
      /* The pool is private and never exported.  Its VA fixes one PTE kind,
       * while individual texture descriptors retain their own tile modes.
       */
      .layout = {.valid = false},
   };
   enum nouveau_horizon_status status = nouveau_horizon_memory_create(
      device->hdev, &memory_info, &pool->memory);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      free(pool);
      return nouveau_switch_horizon_status_to_errno(status);
   }

   const struct nouveau_horizon_va_create_info va_info = {
      .size_B = NOUVEAU_SWITCH_BO_POOL_CHUNK_SIZE,
      .align_B = bind_align_B,
      .has_pte_kind = has_pte_kind,
      .pte_kind = pte_kind,
   };
   status = nouveau_horizon_va_create(device->hdev, &va_info, &pool->va);
   if (status == NOUVEAU_HORIZON_SUCCESS) {
      status = nouveau_horizon_va_bind(
         pool->va, 0, pool->memory, 0, NOUVEAU_SWITCH_BO_POOL_CHUNK_SIZE);
   }
   if (status == NOUVEAU_HORIZON_SUCCESS)
      status = nouveau_horizon_memory_map(pool->memory, &pool->cpu_map);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      nouveau_horizon_va_put(pool->va);
      nouveau_horizon_memory_put(pool->memory);
      free(pool);
      return nouveau_switch_horizon_status_to_errno(status);
   }

   pool->size_B = NOUVEAU_SWITCH_BO_POOL_CHUNK_SIZE;
   pool->cpu_uncached = cpu_uncached;
   pool->gpu_cached = gpu_cached;
   pool->has_pte_kind = has_pte_kind;
   pool->pte_kind = pte_kind;
   /* util_vma_heap uses zero as allocation failure, so reserve the first
    * bind-alignment unit as a sentinel.  It is less than 0.4% of a chunk.
    */
   util_vma_heap_init(&pool->heap, bind_align_B,
                      pool->size_B - bind_align_B);
   *pool_out = pool;
   return 0;
}

static bool
bo_pool_eligible(uint32_t flags, uint32_t align, uint64_t size,
                 const union nouveau_bo_config *config)
{
   return config != NULL && (flags & NOUVEAU_BO_NOSNOOP) &&
          !(flags & NOUVEAU_BO_CONTIG) &&
          size <= NOUVEAU_SWITCH_BO_POOL_MAX_ALLOCATION &&
          align <= NOUVEAU_SWITCH_BO_POOL_MAX_ALLOCATION;
}

static int
bo_pool_allocate(struct nouveau_switch_device *device,
                 struct nouveau_switch_bo *bo,
                 bool cpu_uncached, bool gpu_cached,
                 bool has_pte_kind, uint8_t pte_kind, uint32_t align,
                 uint64_t requested_size_B)
{
   struct nouveau_horizon_device_properties properties;
   nouveau_horizon_device_get_properties(device->hdev, &properties);
   const uint64_t slice_align_B =
      MAX2((uint64_t)MAX2(align, 1u), (uint64_t)properties.bind_align_B);
   const uint64_t allocation_size_B =
      align64(requested_size_B, properties.bind_align_B);
   struct nouveau_switch_bo_pool *pool = NULL;
   struct nouveau_switch_bo_pool *candidate = NULL;
   uint64_t offset_B = 0;

   simple_mtx_lock(&device->lock);
   if (device->diagnostics_enabled)
      device->pool_stats.allocation_calls++;
   pool = bo_pool_find_and_allocate_locked(
      device, cpu_uncached, gpu_cached, has_pte_kind, pte_kind,
      allocation_size_B, slice_align_B, &offset_B);
   simple_mtx_unlock(&device->lock);

   if (!pool) {
      int ret = bo_pool_create(device, cpu_uncached, gpu_cached,
                               has_pte_kind, pte_kind, &candidate);
      if (ret) {
         simple_mtx_lock(&device->lock);
         if (device->diagnostics_enabled)
            device->pool_stats.allocation_fallbacks++;
         simple_mtx_unlock(&device->lock);
         return ret;
      }

      simple_mtx_lock(&device->lock);
      pool = bo_pool_find_and_allocate_locked(
         device, cpu_uncached, gpu_cached, has_pte_kind, pte_kind,
         allocation_size_B, slice_align_B, &offset_B);
      if (!pool) {
         candidate->next = device->bo_pools;
         device->bo_pools = candidate;
         if (device->diagnostics_enabled) {
            device->pool_stats.chunks++;
            device->pool_stats.reserved_B += candidate->size_B;
         }
         pool = candidate;
         candidate = NULL;
         offset_B = util_vma_heap_alloc(&pool->heap, allocation_size_B,
                                        slice_align_B);
      }
      simple_mtx_unlock(&device->lock);
      bo_pool_destroy(candidate);
   }

   if (!pool || offset_B == 0) {
      simple_mtx_lock(&device->lock);
      if (device->diagnostics_enabled)
         device->pool_stats.allocation_fallbacks++;
      simple_mtx_unlock(&device->lock);
      return -ENOSPC;
   }

   memset((char *)pool->cpu_map + offset_B, 0, allocation_size_B);
   bo->pool = pool;
   bo->memory = nouveau_horizon_memory_ref(pool->memory);
   bo->va = nouveau_horizon_va_ref(pool->va);
   bo->memory_offset_B = offset_B;
   bo->allocation_size_B = allocation_size_B;
   bo->base.offset = nouveau_horizon_va_get_addr(pool->va) + offset_B;
   p_atomic_set(&bo->cpu_dirty, true);

   simple_mtx_lock(&device->lock);
   if (device->diagnostics_enabled) {
      device->pool_stats.live_slices++;
      device->pool_stats.peak_slices =
         MAX2(device->pool_stats.peak_slices,
              device->pool_stats.live_slices);
      device->pool_stats.live_requested_B += requested_size_B;
      device->pool_stats.peak_requested_B =
         MAX2(device->pool_stats.peak_requested_B,
              device->pool_stats.live_requested_B);
   }
   simple_mtx_unlock(&device->lock);
   return 0;
}

static void
bo_pool_release(struct nouveau_switch_device *device,
                struct nouveau_switch_bo *bo)
{
   if (!bo->pool)
      return;

   simple_mtx_lock(&device->lock);
   util_vma_heap_free(&bo->pool->heap, bo->memory_offset_B,
                      bo->allocation_size_B);
   if (device->diagnostics_enabled) {
      assert(device->pool_stats.live_slices > 0);
      assert(device->pool_stats.live_requested_B >= bo->base.size);
      device->pool_stats.live_slices--;
      device->pool_stats.live_requested_B -= bo->base.size;
   }
   simple_mtx_unlock(&device->lock);
   bo->pool = NULL;
}

static int
bo_allocate_va(struct nouveau_switch_device *device,
               struct nouveau_switch_bo *bo,
               bool has_pte_kind, uint8_t pte_kind, uint64_t align_B)
{
   struct nouveau_horizon_device_properties properties;
   nouveau_horizon_device_get_properties(device->hdev, &properties);

   struct nouveau_horizon_va_create_info va_info = {
      .size_B = nouveau_horizon_memory_get_size(bo->memory),
      .align_B = MAX2(align_B, (uint64_t)properties.bind_align_B),
      .has_pte_kind = has_pte_kind,
      .pte_kind = pte_kind,
   };
   enum nouveau_horizon_status status =
      nouveau_horizon_va_create(device->hdev, &va_info, &bo->va);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return nouveau_switch_horizon_status_to_errno(status);

   status = nouveau_horizon_va_bind(
      bo->va, 0, bo->memory, 0,
      nouveau_horizon_memory_get_size(bo->memory));
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      nouveau_horizon_va_put(bo->va);
      bo->va = NULL;
      return nouveau_switch_horizon_status_to_errno(status);
   }

   bo->base.offset = nouveau_horizon_va_get_addr(bo->va);
   return 0;
}

static void
bo_init_public(struct nouveau_switch_bo *bo,
               struct nouveau_device *device, uint32_t flags,
               uint64_t logical_size_B)
{
   p_atomic_set(&bo->refcnt, 1);
   simple_mtx_init(&bo->lock, mtx_plain);
   bo->base.device = device;
   switch_device_ref(nouveau_switch_device(device));
   bo->base.handle =
      nouveau_horizon_memory_get_nvmap_handle(bo->memory);
   bo->base.size = logical_size_B;
   bo->base.flags = flags;
   if (bo->allocation_size_B == 0)
      bo->allocation_size_B = nouveau_horizon_memory_get_size(bo->memory);
   bo->fence.id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
}

int
nouveau_bo_new(struct nouveau_device *device, uint32_t flags,
               uint32_t align, uint64_t size,
               union nouveau_bo_config *config,
               struct nouveau_bo **out)
{
   if (!device || !out || size == 0)
      return -EINVAL;

   struct nouveau_switch_device *switch_device =
      nouveau_switch_device(device);
   struct nouveau_switch_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;

   const bool cpu_uncached = flags & NOUVEAU_BO_COHERENT;
   const bool gpu_cached = !cpu_uncached ||
                           (flags & NOUVEAU_BO_SWITCH_GPU_CACHED);
   const bool has_pte_kind = config && config->nvc0.memtype != 0;
   const uint8_t pte_kind = has_pte_kind ? config->nvc0.memtype :
                                           NvKind_Pitch;
   bool pooled = false;
   if (bo_pool_eligible(flags, align, size, config)) {
      pooled = bo_pool_allocate(switch_device, bo,
                                cpu_uncached, gpu_cached,
                                has_pte_kind, pte_kind, align, size) == 0;
   }

   if (!pooled) {
      struct nouveau_horizon_memory_create_info memory_info = {
         .size_B = size,
         .align_B = MAX2(align, 0x1000u),
         .backing_kind = NvKind_Pitch,
         .flags = NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE |
                  NOUVEAU_HORIZON_MEMORY_ZERO,
         .layout = {
            .valid = true,
            .pte_kind = pte_kind,
            .tile_mode = config ? config->nvc0.tile_mode : 0,
         },
      };
      if (!cpu_uncached)
         memory_info.flags |= NOUVEAU_HORIZON_MEMORY_CPU_CACHED;
      if (gpu_cached)
         memory_info.flags |= NOUVEAU_HORIZON_MEMORY_GPU_CACHED;

      enum nouveau_horizon_status status = nouveau_horizon_memory_create(
         switch_device->hdev, &memory_info, &bo->memory);
      if (status != NOUVEAU_HORIZON_SUCCESS) {
         free(bo);
         return nouveau_switch_horizon_status_to_errno(status);
      }

      int ret = bo_allocate_va(switch_device, bo, has_pte_kind, pte_kind,
                               memory_info.align_B);
      if (ret) {
         nouveau_horizon_memory_put(bo->memory);
         free(bo);
         return ret;
      }
   }

   bo_init_public(bo, device, flags, size);
   if (config)
      bo->base.config = *config;
   if (!pooled)
      device_register_bo(switch_device, bo);
   *out = &bo->base;
   return 0;
}

static int
nouveau_switch_bo_validate_import(
   struct nouveau_switch_device *device,
   const struct nouveau_horizon_memory_import_info *import_info)
{
   struct nouveau_horizon_memory *memory = NULL;
   const enum nouveau_horizon_status status = nouveau_horizon_memory_import(
      device->hdev, import_info, &memory);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return nouveau_switch_horizon_status_to_errno(status);

   nouveau_horizon_memory_put(memory);
   return 0;
}

static int
nouveau_switch_bo_name_ref_import(
   struct nouveau_device *device, uint32_t name,
   const struct nouveau_horizon_memory_import_info *import_info,
   struct nouveau_bo **out)
{
   if (!device || !import_info || !out || import_info->nvmap_id != name)
      return -EINVAL;

   struct nouveau_switch_device *switch_device =
      nouveau_switch_device(device);

retry_lookup:
   bool tombstone = false;
   bool quarantined = false;
   NvFence tombstone_fence;
   simple_mtx_lock(&switch_device->lock);
   struct nouveau_switch_bo *existing =
      device_lookup_named_bo_locked(switch_device, name, &tombstone,
                                    &quarantined, &tombstone_fence);
   simple_mtx_unlock(&switch_device->lock);
   if (existing) {
      const int ret =
         nouveau_switch_bo_validate_import(switch_device, import_info);
      if (ret) {
         struct nouveau_bo *existing_bo = &existing->base;
         nouveau_bo_ref(NULL, &existing_bo);
         return ret;
      }
      *out = &existing->base;
      return 0;
   }
   if (tombstone) {
      const int ret = device_wait_named_tombstone(
         switch_device, quarantined, &tombstone_fence);
      if (ret)
         return ret;
      goto retry_lookup;
   }

   struct nouveau_switch_bo *bo = calloc(1, sizeof(*bo));
   if (!bo)
      return -ENOMEM;

   enum nouveau_horizon_status status = nouveau_horizon_memory_import(
      switch_device->hdev, import_info, &bo->memory);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      free(bo);
      return nouveau_switch_horizon_status_to_errno(status);
   }

   struct nouveau_horizon_memory_layout layout;
   nouveau_horizon_memory_get_layout(bo->memory, &layout);
   if (!layout.valid) {
      nouveau_horizon_memory_put(bo->memory);
      free(bo);
      return -ENOTSUP;
   }

   int ret = bo_allocate_va(switch_device, bo,
                            layout.pte_kind != NvKind_Pitch,
                            layout.pte_kind, 0x10000);
   if (ret) {
      nouveau_horizon_memory_put(bo->memory);
      free(bo);
      return ret;
   }

   bo_init_public(bo, device, NOUVEAU_BO_GART,
                  nouveau_horizon_memory_get_size(bo->memory));
   bo->base.config.nvc0.memtype = layout.pte_kind;
   bo->base.config.nvc0.tile_mode = layout.tile_mode;

   simple_mtx_lock(&switch_device->lock);
   /* Recheck after the potentially blocking import to deduplicate a racing
    * wrapper for the same NvMap ID.
    */
   existing = device_lookup_named_bo_locked(
      switch_device, name, &tombstone, &quarantined, &tombstone_fence);
   if (!existing && !tombstone) {
      bo->next_named = switch_device->named_bos;
      switch_device->named_bos = bo;
      bo->registered = true;
   }
   simple_mtx_unlock(&switch_device->lock);

   if (existing) {
      bo_destroy(bo);
      *out = &existing->base;
      return 0;
   }
   if (tombstone) {
      bo_destroy(bo);
      const int wait_ret = device_wait_named_tombstone(
         switch_device, quarantined, &tombstone_fence);
      if (wait_ret)
         return wait_ret;
      goto retry_lookup;
   }

   *out = &bo->base;
   return 0;
}

int
nouveau_bo_name_ref(struct nouveau_device *device, uint32_t name,
                    struct nouveau_bo **out)
{
   const struct nouveau_horizon_memory_import_info import_info = {
      .nvmap_id = name,
      /* Legacy imports require metadata registered in this process.
       * Unknown external NvMap IDs need the explicit import API.
       */
      .require_existing = true,
   };
   return nouveau_switch_bo_name_ref_import(device, name, &import_info, out);
}

int
nouveau_switch_bo_name_ref_explicit(
   struct nouveau_device *device, uint32_t name, uint64_t expected_size_B,
   uint8_t backing_kind, bool gpu_cacheable, uint8_t pte_kind,
   uint16_t tile_mode, struct nouveau_bo **out)
{
   const struct nouveau_horizon_memory_import_info import_info = {
      .nvmap_id = name,
      .has_metadata = true,
      .expected_size_B = expected_size_B,
      .backing_kind = backing_kind,
      .flags = gpu_cacheable ? NOUVEAU_HORIZON_MEMORY_GPU_CACHED : 0,
      .layout = {
         .valid = true,
         .pte_kind = pte_kind,
         .tile_mode = tile_mode,
      },
   };
   return nouveau_switch_bo_name_ref_import(device, name, &import_info, out);
}

int
nouveau_bo_name_get(struct nouveau_bo *bo, uint32_t *name)
{
   if (!bo || !name)
      return -EINVAL;

   struct nouveau_switch_bo *switch_bo = nouveau_switch_bo(bo);
   /* A pooled NvMap contains unrelated private BOs and cannot be represented
    * by the legacy handle-only ABI.  PIPE_BIND_SHARED resources are forced
    * dedicated by NVC0 before reaching this path.
    */
   if (switch_bo->pool)
      return -ENOTSUP;

   *name = nouveau_horizon_memory_export_nvmap_id(switch_bo->memory);
   return *name != 0 ? 0 : -ENOMEM;
}

static int
bo_wait_fence(struct nouveau_switch_bo *bo, uint32_t access)
{
   const bool cpu_writes = access & NOUVEAU_BO_WR;
   const bool cpu_reads = access & NOUVEAU_BO_RD;

   for (;;) {
      NvFence fence;
      uint32_t previous_access;
      bool gpu_dirty;
      bool cpu_visible;
      struct nouveau_switch_completion_source *source;

      simple_mtx_lock(&bo->lock);
      fence = bo->fence;
      previous_access = bo->access;
      gpu_dirty = bo->gpu_dirty;
      cpu_visible = bo->fence_cpu_visible;
      source = nouveau_switch_completion_source_ref(
         bo->completion_source);
      simple_mtx_unlock(&bo->lock);

      const bool needs_wait = cpu_writes ? previous_access != 0 :
                               cpu_reads &&
                                  ((previous_access & NOUVEAU_BO_WR) ||
                                   gpu_dirty);
      if (!needs_wait) {
         nouveau_switch_completion_source_unref(source);
         return 0;
      }

      bool source_complete = false;
      if (!cpu_visible) {
         if (!source) {
            _debug_printf("nouveau/switch: BO %u has a GPU-only fence "
                          "without a completion source\n", bo->base.handle);
            return -EIO;
         }

         /* For NOUVEAU_BO_NOBLOCK, poll the producer before upgrading to
          * CPU-visible completion. Submitting the upgrade first could
          * block behind unfinished GPU work or native resource pressure.
          */
         if (access & NOUVEAU_BO_NOBLOCK) {
            if ((int32_t)fence.id < 0) {
               nouveau_switch_completion_source_unref(source);
               return -EIO;
            }

            const int producer_status =
               completion_source_wait_fence(source, &fence, 0);
            if (producer_status != 0) {
               nouveau_switch_completion_source_unref(source);
               return producer_status;
            }
         }

         NvFence cpu_fence;
         const int upgrade_ret =
            nouveau_switch_completion_source_upgrade_cpu_fence(
            source, &fence, &cpu_fence, &source_complete);
         if (upgrade_ret) {
            nouveau_switch_completion_source_unref(source);
            return upgrade_ret;
         }

         if (!source_complete) {
            if ((int32_t)cpu_fence.id < 0) {
               nouveau_switch_completion_source_unref(source);
               return -EIO;
            }

            /* Publish the upgraded fence only if this is still the exact BO
             * use we observed.  A concurrent newer submission wins and makes
             * us restart from its completion source instead.
             */
            simple_mtx_lock(&bo->lock);
            const bool unchanged =
               bo->completion_source == source &&
               bo->fence.id == fence.id && bo->fence.value == fence.value &&
               !bo->fence_cpu_visible;
            if (unchanged) {
               bo->fence = cpu_fence;
               bo->fence_cpu_visible = true;
               fence = cpu_fence;
               cpu_visible = true;
            }
            simple_mtx_unlock(&bo->lock);
            if (!unchanged) {
               nouveau_switch_completion_source_unref(source);
               continue;
            }
         }
      }

      if (!source_complete) {
         if (!cpu_visible || (int32_t)fence.id < 0) {
            nouveau_switch_completion_source_unref(source);
            return -EIO;
         }

         const uint64_t timeout = switch_fence_wait_timeout(
            access & NOUVEAU_BO_NOBLOCK ? 0 : UINT64_MAX);
         int wait_ret;
         if (source) {
            wait_ret =
               completion_source_wait_fence(source, &fence, timeout);
         } else {
            const struct nouveau_horizon_fence hfence = {
               .id = fence.id,
               .value = fence.value,
            };
            const enum nouveau_horizon_status status =
               nouveau_horizon_fence_wait(
                  nouveau_switch_device(bo->base.device)->hdev,
                  &hfence, timeout);
            wait_ret = nouveau_switch_horizon_status_to_errno(status);
         }
         if (wait_ret != 0) {
            _debug_printf(
               "nouveau/switch: BO %u fence wait failed: %d "
               "(access=0x%x previous=0x%x syncpoint=%u threshold=%u "
               "timeout_ns=%llu)\n",
               bo->base.handle, wait_ret,
               access, previous_access, fence.id, fence.value,
               (unsigned long long)timeout);
            nouveau_switch_completion_source_unref(source);
            return wait_ret;
         }
      }

      if ((cpu_reads || cpu_writes) && gpu_dirty) {
         nouveau_horizon_memory_sync_from_gpu(
            bo->memory, bo->memory_offset_B, bo->base.size);
      }

      struct nouveau_switch_completion_source *record_source = NULL;
      simple_mtx_lock(&bo->lock);
      const bool unchanged =
         bo->completion_source == source &&
         bo->fence.id == fence.id && bo->fence.value == fence.value &&
         (source_complete || bo->fence_cpu_visible);
      if (unchanged) {
         bo->fence.id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
         bo->fence.value = 0;
         bo->fence_cpu_visible = false;
         bo->access = 0;
         record_source = bo->completion_source;
         bo->completion_source = NULL;
         if (cpu_reads || cpu_writes)
            bo->gpu_dirty = false;
      }
      simple_mtx_unlock(&bo->lock);

      nouveau_switch_completion_source_unref(record_source);
      nouveau_switch_completion_source_unref(source);
      if (unchanged)
         return 0;
      /* A newer use raced the completed one.  It must also be made
       * CPU-visible before this map/read/write may proceed.
       */
   }
}

static void
bo_destroy(struct nouveau_switch_bo *bo)
{
   const int wait_ret = bo_wait_fence(bo, NOUVEAU_BO_RDWR);
   if (wait_ret) {
      simple_mtx_lock(&bo->lock);
      bo->quarantined = true;
      simple_mtx_unlock(&bo->lock);
      _debug_printf(
         "nouveau/switch: BO %u teardown wait failed (%d); "
         "quarantining NvMap, VA and backing storage\n",
         bo->base.handle, wait_ret);
      return;
   }

   struct nouveau_switch_completion_source *source = NULL;
   simple_mtx_lock(&bo->lock);
   source = bo->completion_source;
   bo->completion_source = NULL;
   simple_mtx_unlock(&bo->lock);
   nouveau_switch_completion_source_unref(source);

   if (bo->registered) {
      struct nouveau_switch_device *device =
         nouveau_switch_device(bo->base.device);
      simple_mtx_lock(&device->lock);
      struct nouveau_switch_bo **link = &device->named_bos;
      while (*link && *link != bo)
         link = &(*link)->next_named;
      if (*link == bo)
         *link = bo->next_named;
      bo->registered = false;
      simple_mtx_unlock(&device->lock);
   }

   struct nouveau_switch_device *device =
      nouveau_switch_device(bo->base.device);
   bo_pool_release(device, bo);
   nouveau_horizon_va_put(bo->va);
   nouveau_horizon_memory_put(bo->memory);
   simple_mtx_destroy(&bo->lock);
   free(bo);
   switch_device_unref(device);
}

void
nouveau_bo_ref(struct nouveau_bo *bo, struct nouveau_bo **pref)
{
   if (!pref)
      return;

   struct nouveau_bo *old = *pref;
   if (bo)
      p_atomic_inc(&nouveau_switch_bo(bo)->refcnt);
   if (old && p_atomic_dec_zero(&nouveau_switch_bo(old)->refcnt))
      bo_destroy(nouveau_switch_bo(old));
   *pref = bo;
}

int
nouveau_bo_wait(struct nouveau_bo *bo, uint32_t access,
                struct nouveau_client *client)
{
   if (!bo || !(access & NOUVEAU_BO_RDWR))
      return 0;

   if (client) {
      struct nouveau_pushbuf *push =
         nouveau_switch_client_push_get(client, bo);
      if (push && push->channel) {
         int ret = nouveau_pushbuf_kick(push, push->channel);
         if (ret)
            return ret;
      }
   }

   return bo_wait_fence(nouveau_switch_bo(bo), access);
}

int
nouveau_bo_map(struct nouveau_bo *bo, uint32_t access,
               struct nouveau_client *client)
{
   if (!bo)
      return -EINVAL;

   int ret = nouveau_bo_wait(bo, access, client);
   if (ret)
      return ret;

   struct nouveau_switch_bo *switch_bo = nouveau_switch_bo(bo);
   void *memory_map = NULL;
   enum nouveau_horizon_status status =
      nouveau_horizon_memory_map(switch_bo->memory, &memory_map);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return nouveau_switch_horizon_status_to_errno(status);
   bo->map = (char *)memory_map + switch_bo->memory_offset_B;

   if (access & NOUVEAU_BO_WR) {
      p_atomic_set(&switch_bo->cpu_dirty, true);
   }
   return 0;
}

void
nouveau_bo_unmap(struct nouveau_bo *bo)
{
   if (bo)
      bo->map = NULL;
}

int
nouveau_switch_bo_sync_to_gpu(struct nouveau_bo *bo)
{
   if (!bo)
      return -EINVAL;

   struct nouveau_switch_bo *switch_bo = nouveau_switch_bo(bo);
   const bool dirty = p_atomic_xchg(&switch_bo->cpu_dirty, false);

   if (dirty) {
      const bool cached = !(bo->flags & NOUVEAU_BO_COHERENT);
      const struct nouveau_switch_device *device =
         nouveau_switch_device_const(bo->device);
      const unsigned calls = cached && device->perf_enabled ?
         (unsigned)p_atomic_inc_return(&switch_bo->cache_to_gpu_calls) : 0;

      /* Dirtying a suballocation flushes its cached parent BO. Sample
       * exponentially to bound diagnostic output.
       */
      if (cached && device->perf_enabled && bo->size >= (1u << 20) &&
          calls >= 8 &&
          util_is_power_of_two_nonzero(calls)) {
         _debug_printf(
            "nouveau/switch: cache publish hot BO handle=%u size=%lluKiB "
            "memory_offset=%lluKiB allocation=%lluKiB flags=%08x "
            "pooled=%u calls=%u total=%lluMiB\n",
            bo->handle, (unsigned long long)(bo->size >> 10),
            (unsigned long long)(switch_bo->memory_offset_B >> 10),
            (unsigned long long)(switch_bo->allocation_size_B >> 10),
            bo->flags, switch_bo->pool != NULL, calls,
            (unsigned long long)(((uint64_t)calls * bo->size) >> 20));
      }

      nouveau_horizon_memory_sync_to_gpu(
         switch_bo->memory, switch_bo->memory_offset_B, bo->size);
   }
   return 0;
}

void
nouveau_switch_bo_mark_cpu_dirty(struct nouveau_bo *bo)
{
   if (!bo)
      return;

   struct nouveau_switch_bo *switch_bo = nouveau_switch_bo(bo);
   p_atomic_set(&switch_bo->cpu_dirty, true);
}

void
nouveau_switch_bo_record_submission(
   struct nouveau_bo *bo, const NvFence *fence, uint32_t access,
   struct nouveau_switch_completion_source *source, bool cpu_visible)
{
   if (!bo || !fence || !source)
      return;

   struct nouveau_switch_bo *switch_bo = nouveau_switch_bo(bo);
   nouveau_switch_completion_source_ref(source);
   simple_mtx_lock(&switch_bo->lock);
   struct nouveau_switch_completion_source *old_source =
      switch_bo->completion_source;
   switch_bo->fence = *fence;
   switch_bo->completion_source = source;
   switch_bo->fence_cpu_visible = cpu_visible;
   switch_bo->access = access;
   if (access & NOUVEAU_BO_WR)
      switch_bo->gpu_dirty = true;
   simple_mtx_unlock(&switch_bo->lock);
   nouveau_switch_completion_source_unref(old_source);
}

void
nouveau_switch_bo_clear_submission(struct nouveau_bo *bo)
{
   if (!bo)
      return;

   struct nouveau_switch_bo *switch_bo = nouveau_switch_bo(bo);
   simple_mtx_lock(&switch_bo->lock);
   struct nouveau_switch_completion_source *source =
      switch_bo->completion_source;
   switch_bo->completion_source = NULL;
   switch_bo->fence.id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
   switch_bo->fence.value = 0;
   switch_bo->fence_cpu_visible = false;
   switch_bo->access = 0;
   simple_mtx_unlock(&switch_bo->lock);
   nouveau_switch_completion_source_unref(source);
}

int
nouveau_bo_get_syncpoint(struct nouveau_bo *bo, unsigned int *threshold)
{
   if (!bo)
      return -1;

   struct nouveau_switch_bo *switch_bo = nouveau_switch_bo(bo);
   simple_mtx_lock(&switch_bo->lock);
   const NvFence fence = switch_bo->fence;
   const bool cpu_visible = switch_bo->fence_cpu_visible;
   simple_mtx_unlock(&switch_bo->lock);

   /* Native syncpoints exported to the CPU/compositor must carry the GM20B
    * cache-clean completion sequence.  GPU-only BO lifetime fences stay
    * private and are upgraded through their completion source when needed.
    */
   if (!cpu_visible)
      return -1;

   if (threshold)
      *threshold = fence.value;
   return (int)fence.id;
}

int
nouveau_bo_wrap(struct nouveau_device *device, uint32_t handle,
                struct nouveau_bo **out)
{
   (void)device;
   (void)handle;
   (void)out;
   return -ENOSYS;
}

int
nouveau_bo_prime_handle_ref(struct nouveau_device *device, int prime_fd,
                            struct nouveau_bo **out)
{
   (void)device;
   (void)prime_fd;
   (void)out;
   return -ENOSYS;
}

int
nouveau_bo_set_prime(struct nouveau_bo *bo, int *prime_fd)
{
   (void)bo;
   (void)prime_fd;
   return -ENOSYS;
}
