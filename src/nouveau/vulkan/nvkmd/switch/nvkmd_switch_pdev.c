/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_switch.h"

#include "nvk_device.h"
#include "vk_log.h"
#include "vk_sync_dummy.h"
#include "util/cnd_monotonic.h"
#include "util/os_time.h"
#include "util/u_memory.h"
#include "util/macros.h"
#include "util/timespec.h"

#include <limits.h>
#include <string.h>

/* Class IDs for GM20B (Tegra X1) — see src/nouveau/drm/nvif/class.h */
#define NVKMD_SWITCH_CLS_ENG3D    0xb197 /* MAXWELL_B */
#define NVKMD_SWITCH_CLS_COMPUTE  0xb1c0 /* MAXWELL_COMPUTE_B */
#define NVKMD_SWITCH_CLS_COPY     0xb0b5 /* MAXWELL_DMA_COPY_A */
#define NVKMD_SWITCH_CLS_ENG2D    0x902d /* FERMI_TWOD_A */
#define NVKMD_SWITCH_CLS_M2MF     0xa140 /* KEPLER_INLINE_TO_MEMORY_B */
#define NVKMD_SWITCH_CLS_GPFIFO   0xb06f /* MAXWELL_CHANNEL_GPFIFO_A */

/* Horizon maps every nvkmd memory allocation at the 64 KiB GPU bind
 * granularity.  A native-fence sync only needs one four-byte completion
 * payload, so allocating one BO per sync wastes almost the entire mapping and
 * repeats NvMap, VA-allocation and address-space-map work on every submit.
 *
 * Keep slots on separate 64-byte lines.  The memory is CPU-uncached and
 * GPU-uncached, so this is not required for cache coherence, but it prevents
 * independent GPU semaphore writes from sharing the same transaction-sized
 * region and leaves room to extend a payload without changing the allocator.
 */
#define NVKMD_SWITCH_SYNC_PAYLOAD_SLAB_SIZE_B NVKMD_SWITCH_BIND_ALIGN_B
#define NVKMD_SWITCH_SYNC_PAYLOAD_SLOT_SIZE_B 64u
#define NVKMD_SWITCH_SYNC_PAYLOAD_SLOT_COUNT \
   (NVKMD_SWITCH_SYNC_PAYLOAD_SLAB_SIZE_B / \
    NVKMD_SWITCH_SYNC_PAYLOAD_SLOT_SIZE_B)
#define NVKMD_SWITCH_SYNC_PAYLOAD_MASK_BITS 64u
#define NVKMD_SWITCH_SYNC_PAYLOAD_MASK_WORDS \
   (NVKMD_SWITCH_SYNC_PAYLOAD_SLOT_COUNT / \
    NVKMD_SWITCH_SYNC_PAYLOAD_MASK_BITS)

static_assert(NVKMD_SWITCH_SYNC_PAYLOAD_SLAB_SIZE_B %
              NVKMD_SWITCH_SYNC_PAYLOAD_SLOT_SIZE_B == 0,
              "payload slab must contain whole slots");
static_assert(NVKMD_SWITCH_SYNC_PAYLOAD_SLOT_COUNT %
              NVKMD_SWITCH_SYNC_PAYLOAD_MASK_BITS == 0,
              "payload slot bitmap must contain whole words");
static_assert(NVKMD_SWITCH_SYNC_PAYLOAD_SLOT_COUNT <= UINT16_MAX,
              "payload slot index must fit in uint16_t");

struct nvkmd_switch_sync_payload_slab {
   struct list_head link;
   struct nvkmd_mem *mem;
   uint64_t used[NVKMD_SWITCH_SYNC_PAYLOAD_MASK_WORDS];
   uint16_t used_count;
};

/* Binary vk_sync backed by a libnx native fence payload.
 *
 * Lifecycle:
 *  - init():   fence not present, not signaled.
 *  - signal(): CPU-side pre-signal (used by runtime for reset/initial
 *              value paths). Marks `signaled=true` without a native fence.
 *  - import(): called from the ctx submit path right after
 *              nvGpuChannelGetFence() or from WSI acquire import. Installs
 *              the native fence payload and wakes any CPU-side waiters that
 *              were blocking on the condvar.
 *  - reset():  clears both fence and signaled state.
 *  - wait():   if a native fence payload is installed, delegates to
 *              nvMultiFenceWait() (dropping the mutex first to avoid
 *              blocking import() broadcasts). Otherwise spins on the
 *              condvar waiting for either a CPU signal or a GPU-side
 *              fence import.
 */
struct nvkmd_switch_sync {
   struct vk_sync sync;
   mtx_t mutex;
   struct u_cnd_monotonic cond;
   NvMultiFence fence;
   struct nvkmd_switch_sync_payload_slab *payload_slab;
   struct nvkmd_mem *payload_mem;
   uint16_t payload_slot;
   volatile uint32_t *payload;
   uint64_t payload_addr;
   uint32_t next_payload_value;
   uint32_t payload_wait_value;
   bool has_payload_wait;
   bool signaled;
};

static struct nvkmd_switch_dev *
nvkmd_switch_dev_from_nvk(struct nvk_device *dev)
{
   return container_of(dev->nvkmd, struct nvkmd_switch_dev, base);
}

void
nvkmd_switch_sync_payload_pool_init(struct nvkmd_switch_dev *dev)
{
   simple_mtx_init(&dev->sync_payload_mutex, mtx_plain);
   list_inithead(&dev->sync_payload_slabs);
}

void
nvkmd_switch_sync_payload_pool_finish(struct nvkmd_switch_dev *dev)
{
   list_for_each_entry_safe(struct nvkmd_switch_sync_payload_slab, slab,
                            &dev->sync_payload_slabs, link) {
      assert(slab->used_count == 0);

      list_del(&slab->link);
      nvkmd_mem_unref(slab->mem);
      FREE(slab);
   }

   simple_mtx_destroy(&dev->sync_payload_mutex);
}

static bool
nvkmd_switch_sync_payload_slab_take(
   struct nvkmd_switch_sync_payload_slab *slab,
   uint16_t *slot_out)
{
   if (slab->used_count == NVKMD_SWITCH_SYNC_PAYLOAD_SLOT_COUNT)
      return false;

   for (uint32_t w = 0; w < ARRAY_SIZE(slab->used); w++) {
      const uint64_t free_mask = ~slab->used[w];
      if (free_mask == 0)
         continue;

      const uint32_t bit = __builtin_ctzll(free_mask);
      const uint32_t slot = w * NVKMD_SWITCH_SYNC_PAYLOAD_MASK_BITS + bit;
      assert(slot < NVKMD_SWITCH_SYNC_PAYLOAD_SLOT_COUNT);

      slab->used[w] |= UINT64_C(1) << bit;
      slab->used_count++;
      *slot_out = slot;
      return true;
   }

   UNREACHABLE("sync payload slab free-count mismatch");
}

static VkResult
nvkmd_switch_sync_payload_pool_acquire(
   struct nvkmd_switch_dev *dev,
   struct vk_object_base *log_obj,
   struct nvkmd_switch_sync_payload_slab **slab_out,
   uint16_t *slot_out,
   volatile uint32_t **payload_out,
   uint64_t *payload_addr_out)
{
   struct nvkmd_switch_sync_payload_slab *selected = NULL;
   uint16_t slot = 0;

   simple_mtx_lock(&dev->sync_payload_mutex);

   list_for_each_entry(struct nvkmd_switch_sync_payload_slab, slab,
                       &dev->sync_payload_slabs, link) {
      if (nvkmd_switch_sync_payload_slab_take(slab, &slot)) {
         selected = slab;
         break;
      }
   }

   VkResult result = VK_SUCCESS;
   if (selected == NULL) {
      selected = CALLOC_STRUCT(nvkmd_switch_sync_payload_slab);
      if (selected == NULL) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto out_unlock;
      }

      result =
         nvkmd_dev_alloc_mapped_mem(&dev->base, log_obj,
                                    NVKMD_SWITCH_SYNC_PAYLOAD_SLAB_SIZE_B,
                                    NVKMD_SWITCH_SYNC_PAYLOAD_SLAB_SIZE_B,
                                    NVKMD_MEM_GART | NVKMD_MEM_COHERENT |
                                    NVKMD_MEM_GPU_UNCACHED,
                                    NVKMD_MEM_MAP_RDWR,
                                    &selected->mem);
      if (result != VK_SUCCESS) {
         FREE(selected);
         selected = NULL;
         goto out_unlock;
      }

      list_addtail(&selected->link, &dev->sync_payload_slabs);
      const bool took_slot =
         nvkmd_switch_sync_payload_slab_take(selected, &slot);
      assert(took_slot);
   }

   const uint64_t offset_B =
      (uint64_t)slot * NVKMD_SWITCH_SYNC_PAYLOAD_SLOT_SIZE_B;
   *slab_out = selected;
   *slot_out = slot;
   *payload_out = (volatile uint32_t *)((char *)selected->mem->map + offset_B);
   *payload_addr_out = selected->mem->va->addr + offset_B;

out_unlock:
   simple_mtx_unlock(&dev->sync_payload_mutex);
   return result;
}

static void
nvkmd_switch_sync_payload_pool_release(
   struct nvkmd_switch_dev *dev,
   struct nvkmd_switch_sync_payload_slab *slab,
   uint16_t slot)
{
   assert(slot < NVKMD_SWITCH_SYNC_PAYLOAD_SLOT_COUNT);
   const uint32_t word = slot / NVKMD_SWITCH_SYNC_PAYLOAD_MASK_BITS;
   const uint32_t bit = slot % NVKMD_SWITCH_SYNC_PAYLOAD_MASK_BITS;
   const uint64_t mask = UINT64_C(1) << bit;

   simple_mtx_lock(&dev->sync_payload_mutex);
   assert(slab->used[word] & mask);
   assert(slab->used_count > 0);
   slab->used[word] &= ~mask;
   slab->used_count--;
   simple_mtx_unlock(&dev->sync_payload_mutex);
}

static bool
nvkmd_switch_payload_reached(uint32_t payload, uint32_t wait_value)
{
   return (int32_t)(payload - wait_value) >= 0;
}

static void
nvkmd_switch_multifence_reset(NvMultiFence *fence)
{
   memset(fence, 0, sizeof(*fence));
   for (uint32_t i = 0; i < ARRAY_SIZE(fence->fences); i++)
      fence->fences[i].id = UINT32_MAX;
}

static uint32_t
nvkmd_switch_multifence_copy_valid(NvMultiFence *dst, const NvMultiFence *src)
{
   nvkmd_switch_multifence_reset(dst);

   if (src == NULL)
      return 0;

   const uint32_t src_count = MIN2(src->num_fences, ARRAY_SIZE(src->fences));
   uint32_t dst_count = 0;

   for (uint32_t i = 0; i < src_count; i++) {
      if ((int32_t)src->fences[i].id < 0)
         continue;

      if (dst_count >= ARRAY_SIZE(dst->fences))
         break;

      dst->fences[dst_count++] = src->fences[i];
   }

   dst->num_fences = dst_count;
   return dst_count;
}

static struct nvkmd_switch_sync *
nvkmd_switch_sync_from_vk(struct vk_sync *sync)
{
   return container_of(sync, struct nvkmd_switch_sync, sync);
}

static VkResult
nvkmd_switch_sync_init(struct vk_device *device,
                       struct vk_sync *sync,
                       uint64_t initial_value)
{
   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);
   struct nvk_device *dev = container_of(device, struct nvk_device, vk);
   struct nvkmd_switch_dev *sdev = nvkmd_switch_dev_from_nvk(dev);
   int ret;

   ret = mtx_init(&ssync->mutex, mtx_plain);
   if (ret != thrd_success)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "nvkmd-switch: mtx_init failed");

   ret = u_cnd_monotonic_init(&ssync->cond);
   if (ret != thrd_success) {
      mtx_destroy(&ssync->mutex);
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "nvkmd-switch: cnd_init failed");
   }

   ssync->payload_slab = NULL;
   ssync->payload_mem = NULL;
   ssync->payload = NULL;
   ssync->payload_addr = 0;
   ssync->payload_slot = 0;

   VkResult result =
      nvkmd_switch_sync_payload_pool_acquire(sdev, &dev->vk.base,
                                             &ssync->payload_slab,
                                             &ssync->payload_slot,
                                             &ssync->payload,
                                             &ssync->payload_addr);
   if (result != VK_SUCCESS) {
      /* Fall back to a dedicated payload allocation. */
      result =
         nvkmd_dev_alloc_mapped_mem(dev->nvkmd, &dev->vk.base,
                                    sizeof(uint32_t), sizeof(uint32_t),
                                    NVKMD_MEM_GART | NVKMD_MEM_COHERENT |
                                    NVKMD_MEM_GPU_UNCACHED,
                                    NVKMD_MEM_MAP_RDWR,
                                    &ssync->payload_mem);
      if (result == VK_SUCCESS) {
         ssync->payload = ssync->payload_mem->map;
         ssync->payload_addr = ssync->payload_mem->va->addr;
      }
   }

   if (result != VK_SUCCESS) {
      u_cnd_monotonic_destroy(&ssync->cond);
      mtx_destroy(&ssync->mutex);
      return result;
   }

   ssync->next_payload_value = initial_value != 0 ? 1 : 0;
   ssync->payload_wait_value = ssync->next_payload_value;
   ssync->has_payload_wait = initial_value != 0;
   *ssync->payload = ssync->next_payload_value;

   nvkmd_switch_multifence_reset(&ssync->fence);
   ssync->signaled = initial_value != 0;
   return VK_SUCCESS;
}

static void
nvkmd_switch_sync_finish(struct vk_device *device,
                         struct vk_sync *sync)
{
   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);
   struct nvk_device *dev = container_of(device, struct nvk_device, vk);

   if (ssync->payload_slab != NULL) {
      nvkmd_switch_sync_payload_pool_release(
         nvkmd_switch_dev_from_nvk(dev), ssync->payload_slab,
         ssync->payload_slot);
   } else if (ssync->payload_mem != NULL) {
      nvkmd_mem_unref(ssync->payload_mem);
   }

   u_cnd_monotonic_destroy(&ssync->cond);
   mtx_destroy(&ssync->mutex);
}

static VkResult
nvkmd_switch_sync_signal(struct vk_device *device,
                         struct vk_sync *sync,
                         uint64_t value)
{
   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);
   int ret;

   mtx_lock(&ssync->mutex);
   if (ssync->fence.num_fences == 0) {
      ssync->next_payload_value++;
      if (ssync->next_payload_value == 0)
         ssync->next_payload_value = 1;
      ssync->payload_wait_value = ssync->next_payload_value;
      ssync->has_payload_wait = true;
      *ssync->payload = ssync->payload_wait_value;
   }
   ssync->signaled = true;
   ret = u_cnd_monotonic_broadcast(&ssync->cond);
   mtx_unlock(&ssync->mutex);

   if (ret != thrd_success)
      return vk_errorf(device, VK_ERROR_UNKNOWN,
                       "nvkmd-switch: cnd_broadcast failed");

   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_sync_reset(struct vk_device *device,
                        struct vk_sync *sync)
{
   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);

   mtx_lock(&ssync->mutex);
   ssync->signaled = false;
   nvkmd_switch_multifence_reset(&ssync->fence);
   ssync->has_payload_wait = false;
   ssync->next_payload_value = 0;
   ssync->payload_wait_value = 0;
   *ssync->payload = 0;
   mtx_unlock(&ssync->mutex);

   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_sync_move(struct vk_device *device,
                       struct vk_sync *dst,
                       struct vk_sync *src)
{
   struct nvkmd_switch_sync *dst_sync = nvkmd_switch_sync_from_vk(dst);
   struct nvkmd_switch_sync *src_sync = nvkmd_switch_sync_from_vk(src);
   NvMultiFence fence;
   bool signaled;

   mtx_lock(&src_sync->mutex);
   fence = src_sync->fence;
   signaled = src_sync->signaled;
   src_sync->signaled = false;
   nvkmd_switch_multifence_reset(&src_sync->fence);
   src_sync->has_payload_wait = false;
   mtx_unlock(&src_sync->mutex);

   mtx_lock(&dst_sync->mutex);
   dst_sync->fence = fence;
   dst_sync->signaled = signaled;
   dst_sync->has_payload_wait = false;
   u_cnd_monotonic_broadcast(&dst_sync->cond);
   mtx_unlock(&dst_sync->mutex);

   return VK_SUCCESS;
}

/* Clamp abs-timeout-ns to an s32 microsecond value for nvFenceWait.
 * UINT64_MAX maps to "wait forever" (-1). Past deadlines clamp to 0.
 */
static s32
nvkmd_switch_abs_ns_to_us(uint64_t abs_timeout_ns)
{
   if (abs_timeout_ns == UINT64_MAX)
      return -1;

   const uint64_t now_ns = os_time_get_nano();
   if (abs_timeout_ns <= now_ns)
      return 0;

   const uint64_t delta_us = (abs_timeout_ns - now_ns) / 1000u;
   if (delta_us > (uint64_t)INT32_MAX)
      return INT32_MAX;

   return (s32)delta_us;
}

static bool
nvkmd_switch_result_is_timeout(Result rc)
{
   if (R_VALUE(rc) == R_VALUE(KERNELRESULT(TimedOut)))
      return true;

   switch (R_MODULE(rc)) {
   case Module_Libnx:
      return R_DESCRIPTION(rc) == LibnxError_Timeout;
   case Module_LibnxNvidia:
      return R_DESCRIPTION(rc) == LibnxNvidiaError_Timeout;
   case Module_LibnxBinder:
      return R_DESCRIPTION(rc) == LibnxBinderError_TimedOut;
   default:
      return false;
   }
}

static VkResult
nvkmd_switch_sync_wait(struct vk_device *device,
                       struct vk_sync *sync,
                       uint64_t wait_value,
                       enum vk_sync_wait_flags wait_flags,
                       uint64_t abs_timeout_ns)
{
   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);
   struct timespec abs_timeout_ts;

   timespec_from_nsec(&abs_timeout_ts, abs_timeout_ns);

   mtx_lock(&ssync->mutex);

   /* VK_SYNC_WAIT_PENDING: return as soon as the sync has an operation
    * that will eventually signal it — for us, "installed native fence
    * payload" counts as a pending op, and "CPU pre-signaled" is
    * trivially satisfied.
    */
   while (!ssync->signaled && ssync->fence.num_fences == 0) {
      int ret = u_cnd_monotonic_timedwait(&ssync->cond, &ssync->mutex,
                                          &abs_timeout_ts);
      if (ret == thrd_timedout) {
         mtx_unlock(&ssync->mutex);
         return VK_TIMEOUT;
      }

      if (ret != thrd_success) {
         mtx_unlock(&ssync->mutex);
         return vk_errorf(device, VK_ERROR_UNKNOWN,
                          "nvkmd-switch: cnd_timedwait failed");
      }
   }

   const bool do_fence_wait =
      ssync->fence.num_fences > 0 && !(wait_flags & VK_SYNC_WAIT_PENDING);
   NvMultiFence fence_copy = ssync->fence;
   const bool has_payload_wait = ssync->has_payload_wait;
   const uint32_t payload_wait_value = ssync->payload_wait_value;
   volatile uint32_t *payload = ssync->payload;
   mtx_unlock(&ssync->mutex);

   if (!do_fence_wait)
      return VK_SUCCESS;

   if (has_payload_wait &&
       nvkmd_switch_payload_reached(*payload, payload_wait_value))
      return VK_SUCCESS;

   const s32 timeout_us = nvkmd_switch_abs_ns_to_us(abs_timeout_ns);
   Result rc = nvMultiFenceWait(&fence_copy, timeout_us);
   if (R_FAILED(rc)) {
      if (nvkmd_switch_result_is_timeout(rc))
         return VK_TIMEOUT;

      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: nvMultiFenceWait failed: 0x%x", rc);
   }

   while (has_payload_wait &&
          !nvkmd_switch_payload_reached(*payload, payload_wait_value)) {
      if (abs_timeout_ns != UINT64_MAX &&
          os_time_get_nano() >= abs_timeout_ns)
         return VK_TIMEOUT;

      svcSleepThread(0);
   }

   return VK_SUCCESS;
}

static const struct vk_sync_type nvkmd_switch_point_sync_type = {
   .size = sizeof(struct nvkmd_switch_sync),
   .features = VK_SYNC_FEATURE_BINARY |
               VK_SYNC_FEATURE_GPU_WAIT |
               VK_SYNC_FEATURE_GPU_MULTI_WAIT |
               VK_SYNC_FEATURE_CPU_WAIT |
               VK_SYNC_FEATURE_CPU_RESET |
               VK_SYNC_FEATURE_CPU_SIGNAL |
               VK_SYNC_FEATURE_WAIT_PENDING,
   .init = nvkmd_switch_sync_init,
   .finish = nvkmd_switch_sync_finish,
   .signal = nvkmd_switch_sync_signal,
   .reset = nvkmd_switch_sync_reset,
   .move = nvkmd_switch_sync_move,
   .wait = nvkmd_switch_sync_wait,
};

bool
nvkmd_switch_sync_is_nvfence(const struct vk_sync_type *type)
{
   return type == &nvkmd_switch_point_sync_type;
}

void
nvkmd_switch_sync_import_nvfence(struct vk_sync *sync, const NvFence *fence)
{
   NvMultiFence mf;
   nvMultiFenceCreate(&mf, fence);
   nvkmd_switch_sync_import_nvmultifence(sync, &mf);
}

void
nvkmd_switch_sync_import_nvmultifence(struct vk_sync *sync,
                                      const NvMultiFence *fence)
{
   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);

   mtx_lock(&ssync->mutex);
   nvkmd_switch_multifence_copy_valid(&ssync->fence, fence);
   ssync->has_payload_wait = false;
   u_cnd_monotonic_broadcast(&ssync->cond);
   mtx_unlock(&ssync->mutex);
}

bool
nvkmd_switch_sync_peek_nvmultifence(struct vk_sync *sync, NvMultiFence *out)
{
   if (sync == NULL || sync->type != &nvkmd_switch_point_sync_type)
      return false;

   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);
   NvMultiFence local;

   mtx_lock(&ssync->mutex);
   const bool have_fence = ssync->fence.num_fences > 0;
   if (have_fence)
      local = ssync->fence;
   mtx_unlock(&ssync->mutex);

   if (have_fence && out != NULL)
      *out = local;

   return have_fence;
}

bool
nvkmd_switch_sync_peek_nvfence(struct vk_sync *sync, NvFence *out)
{
   NvMultiFence mf;
   if (!nvkmd_switch_sync_peek_nvmultifence(sync, &mf))
      return false;

   if (mf.num_fences != 1)
      return false;

   if (out != NULL)
      *out = mf.fences[0];

   return true;
}

VkResult
nvkmd_switch_sync_copy_payloads(struct vk_device *device,
                                uint32_t wait_count,
                                const struct vk_sync_wait *waits,
                                uint32_t signal_count,
                                const struct vk_sync_signal *signals)
{
   if (signal_count == 0)
      return VK_SUCCESS;

   if (wait_count == 1) {
      NvMultiFence fence;

      if (nvkmd_switch_sync_peek_nvmultifence(waits[0].sync, &fence)) {
         for (uint32_t i = 0; i < signal_count; i++) {
            struct vk_sync *sync = signals[i].sync;

            if (sync != NULL && nvkmd_switch_sync_is_nvfence(sync->type))
               nvkmd_switch_sync_import_nvmultifence(sync, &fence);
         }

         return vk_sync_signal_many(device, signal_count, signals);
      }
   }

   VkResult result = vk_sync_wait_many(device, wait_count, waits, 0,
                                       UINT64_MAX);
   if (result != VK_SUCCESS)
      return result;

   return vk_sync_signal_many(device, signal_count, signals);
}

static void
nvkmd_switch_pdev_destroy(struct nvkmd_pdev *pdev)
{
   FREE(pdev);
}

static uint64_t
nvkmd_switch_pdev_get_vram_used(struct nvkmd_pdev *pdev)
{
   return 0;
}

static int
nvkmd_switch_pdev_get_drm_primary_fd(struct nvkmd_pdev *pdev)
{
   return -1;
}

static VkResult
nvkmd_switch_pdev_create_dev(struct nvkmd_pdev *pdev,
                             struct vk_object_base *log_obj,
                             struct nvkmd_dev **dev_out)
{
   return nvkmd_switch_create_dev(pdev, log_obj, dev_out);
}

static const struct nvkmd_pdev_ops nvkmd_switch_pdev_ops = {
   .destroy = nvkmd_switch_pdev_destroy,
   .get_vram_used = nvkmd_switch_pdev_get_vram_used,
   .get_drm_primary_fd = nvkmd_switch_pdev_get_drm_primary_fd,
   .create_dev = nvkmd_switch_pdev_create_dev,
};

VkResult
nvkmd_switch_try_create_pdev(struct vk_object_base *log_obj,
                             enum nvk_debug debug_flags,
                             struct nvkmd_pdev **pdev_out)
{
   struct nvkmd_switch_pdev *pdev = CALLOC_STRUCT(nvkmd_switch_pdev);
   if (pdev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   pdev->base.ops = &nvkmd_switch_pdev_ops;
   pdev->base.debug_flags = debug_flags;

   /* Hard-coded Tegra X1 (GM20B) device info. The Tegra X1+ in the Switch OLED
    * uses the same GM20B die at higher clocks, so this is a reasonable default
    * for both. We synthesize the values nouveau would normally read from the
    * NVIF device-info ioctl. */
   struct nv_device_info *info = &pdev->base.dev_info;
   info->type = NV_DEVICE_TYPE_SOC;
   info->device_id = 0x0fe0; /* placeholder; not exposed via PCI on SoC */
   info->chipset = 0x12b;    /* GM20B */
   strncpy(info->device_name, "NVIDIA Tegra X1 (GM20B)",
           sizeof(info->device_name) - 1);
   strncpy(info->chipset_name, "GM20B", sizeof(info->chipset_name) - 1);

   info->sm = 53;
   /* Tegra X1: 1 GPC, 2 SMs (TPCs); each TPC has 1 MP. */
   info->gpc_count = 1;
   info->tpc_count = 2;
   info->mp_per_tpc = 1;
   info->max_warps_per_mp = 64; /* sm 53 → 64 per max_warps_per_mp_for_sm */

   info->cls_eng3d   = NVKMD_SWITCH_CLS_ENG3D;
   info->cls_compute = NVKMD_SWITCH_CLS_COMPUTE;
   info->cls_copy    = NVKMD_SWITCH_CLS_COPY;
   info->cls_eng2d   = NVKMD_SWITCH_CLS_ENG2D;
   info->cls_m2mf    = NVKMD_SWITCH_CLS_M2MF;
   info->cls_gpfifo  = NVKMD_SWITCH_CLS_GPFIFO;

   /* SoC: no dedicated VRAM, no PCI BAR. The runtime will fall back to
    * sysmem-backed heaps, which is what we want on Switch.
    */
   info->vram_size_B = 0;
   info->bar_size_B = 0;

   /* GM20B / SM53 has a single shared-memory configuration of 64 KiB. */
   info->sm_smem_sizes_kB[0] = 64;
   info->sm_smem_size_count = 1;

   /* Non-coherent atom size: GM20B uses 128 B cache lines on the GPU side. */
   info->nc_atom_size_B = 128;

   pdev->base.kmd_info = (struct nvkmd_info) {
      .has_dma_buf = false,
      .has_get_vram_used = false,
      /* This currently gates EXT_image_drm_format_modifier exposure, not
       * whether the KMD has an internal tiled NvMap allocation path.
       */
      .has_alloc_tiled = false,
      .has_map_fixed = false,
      .has_overmap = false,
      /* GM20B supports color/depth compression.  The public L4T nvgpu UAPI
       * exposes compression page size and compbit metadata, and public
       * deko3d maps image memory with compressed kinds.  NVK's Turing+
       * restriction is a nouveau-kernel limitation, not a hardware one, so
       * it does not apply to this backend.
       *
       * The NvMap backing stays pitch and the selected NIL kind is applied at
       * GPU VA mapping time.
       */
      .has_compression = true,
   };

   pdev->base.bind_align_B = NVKMD_SWITCH_BIND_ALIGN_B;
   pdev->base.drm.render_dev = 0;
   pdev->base.drm.primary_dev = 0;

   pdev->timeline_sync_type =
      vk_sync_timeline_get_type(&nvkmd_switch_point_sync_type);
   pdev->sync_types[0] = &nvkmd_switch_point_sync_type;
   pdev->sync_types[1] = &pdev->timeline_sync_type.sync;
   pdev->sync_types[2] = NULL;
   pdev->base.sync_types = pdev->sync_types;

   *pdev_out = &pdev->base;
   return VK_SUCCESS;
}
