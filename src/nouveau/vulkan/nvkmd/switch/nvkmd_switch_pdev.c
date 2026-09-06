/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_switch.h"

#include "nvk_device.h"
#include "vk_log.h"
#include "util/cnd_monotonic.h"
#include "util/os_time.h"
#include "util/u_memory.h"
#include "util/macros.h"
#include "util/timespec.h"

#include <string.h>

/* Binary sync: imported native fences track GPU completion; a condition
 * variable handles CPU signals and pending fence imports. Signal wakes
 * waiters but cannot bypass an installed fence. Reset clears both states.
 * Native waits release the mutex and retain the Horizon device for their
 * duration.
 */
struct nvkmd_switch_sync {
   struct vk_sync sync;
   mtx_t mutex;
   struct u_cnd_monotonic cond;
   struct nouveau_horizon_device *horizon;
   struct nouveau_horizon_fence fences[4];
   uint32_t fence_count;
   bool signaled;
};

static struct nvkmd_switch_dev *
nvkmd_switch_dev_from_nvk(struct nvk_device *dev)
{
   return container_of(dev->nvkmd, struct nvkmd_switch_dev, base);
}

static void
nvkmd_switch_fences_reset(struct nvkmd_switch_sync *sync)
{
   sync->fence_count = 0;
   for (uint32_t i = 0; i < ARRAY_SIZE(sync->fences); i++) {
      sync->fences[i].id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
      sync->fences[i].value = 0;
   }
}

static uint32_t
nvkmd_switch_fences_import_native(struct nvkmd_switch_sync *dst,
                                  const NvMultiFence *src)
{
   nvkmd_switch_fences_reset(dst);

   if (src == NULL)
      return 0;

   const uint32_t src_count = MIN2(src->num_fences, ARRAY_SIZE(src->fences));

   for (uint32_t i = 0; i < src_count; i++) {
      if ((int32_t)src->fences[i].id < 0)
         continue;

      if (dst->fence_count >= ARRAY_SIZE(dst->fences))
         break;

      dst->fences[dst->fence_count++] =
         (struct nouveau_horizon_fence) {
            .id = src->fences[i].id,
            .value = src->fences[i].value,
         };
   }

   return dst->fence_count;
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

   ssync->horizon = nouveau_horizon_device_ref(sdev->horizon);
   nvkmd_switch_fences_reset(ssync);
   ssync->signaled = initial_value != 0;
   return VK_SUCCESS;
}

static void
nvkmd_switch_sync_finish(struct vk_device *device,
                         struct vk_sync *sync)
{
   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);

   nouveau_horizon_device_put(ssync->horizon);
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
   nvkmd_switch_fences_reset(ssync);
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
   struct nouveau_horizon_fence fences[4];
   uint32_t fence_count;
   bool signaled;

   mtx_lock(&src_sync->mutex);
   fence_count = src_sync->fence_count;
   memcpy(fences, src_sync->fences, sizeof(fences));
   signaled = src_sync->signaled;
   src_sync->signaled = false;
   nvkmd_switch_fences_reset(src_sync);
   mtx_unlock(&src_sync->mutex);

   mtx_lock(&dst_sync->mutex);
   dst_sync->fence_count = fence_count;
   memcpy(dst_sync->fences, fences, sizeof(fences));
   dst_sync->signaled = signaled;
   u_cnd_monotonic_broadcast(&dst_sync->cond);
   mtx_unlock(&dst_sync->mutex);

   return VK_SUCCESS;
}

static uint64_t
nvkmd_switch_remaining_timeout_ns(uint64_t abs_timeout_ns)
{
   if (abs_timeout_ns == UINT64_MAX)
      return UINT64_MAX;

   const uint64_t now_ns = os_time_get_nano();
   if (abs_timeout_ns <= now_ns)
      return 0;

   return abs_timeout_ns - now_ns;
}

static VkResult
nvkmd_switch_sync_wait_result(struct vk_device *device,
                              enum nouveau_horizon_status status)
{
   switch (status) {
   case NOUVEAU_HORIZON_SUCCESS:
      return VK_SUCCESS;
   case NOUVEAU_HORIZON_ERROR_TIMEOUT:
      return VK_TIMEOUT;
   case NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY:
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   case NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY:
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   default:
      return vk_errorf(device, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: native fence wait failed: %s",
                       nouveau_horizon_status_string(status));
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

   /* WAIT_PENDING needs a CPU signal or installed native fence, not
    * completed GPU execution.
    */
   while (!ssync->signaled && ssync->fence_count == 0) {
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
      ssync->fence_count > 0 && !(wait_flags & VK_SYNC_WAIT_PENDING);
   struct nouveau_horizon_fence fences[4];
   const uint32_t fence_count = ssync->fence_count;
   memcpy(fences, ssync->fences, sizeof(fences));
   struct nouveau_horizon_device *horizon =
      nouveau_horizon_device_ref(ssync->horizon);
   mtx_unlock(&ssync->mutex);

   if (!do_fence_wait) {
      nouveau_horizon_device_put(horizon);
      return VK_SUCCESS;
   }

   for (uint32_t i = 0; i < fence_count; i++) {
      const enum nouveau_horizon_status status = nouveau_horizon_fence_wait(
         horizon, &fences[i],
         nvkmd_switch_remaining_timeout_ns(abs_timeout_ns));
      if (status != NOUVEAU_HORIZON_SUCCESS) {
         nouveau_horizon_device_put(horizon);
         return nvkmd_switch_sync_wait_result(device, status);
      }
   }
   nouveau_horizon_device_put(horizon);

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
nvkmd_switch_sync_import_nvmultifence(struct vk_sync *sync,
                                      const NvMultiFence *fence)
{
   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);

   mtx_lock(&ssync->mutex);
   nvkmd_switch_fences_import_native(ssync, fence);
   u_cnd_monotonic_broadcast(&ssync->cond);
   mtx_unlock(&ssync->mutex);
}

void
nvkmd_switch_sync_import_horizon_fence(
   struct vk_sync *sync, const struct nouveau_horizon_fence *fence)
{
   if (sync == NULL || fence == NULL ||
       sync->type != &nvkmd_switch_point_sync_type)
      return;

   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);
   mtx_lock(&ssync->mutex);
   nvkmd_switch_fences_reset(ssync);
   if (nouveau_horizon_fence_is_valid(fence)) {
      ssync->fences[0] = *fence;
      ssync->fence_count = 1;
   }
   u_cnd_monotonic_broadcast(&ssync->cond);
   mtx_unlock(&ssync->mutex);
}

uint32_t
nvkmd_switch_sync_peek_horizon_fences(
   struct vk_sync *sync, uint32_t max_fences,
   struct nouveau_horizon_fence *fences_out)
{
   if (sync == NULL || sync->type != &nvkmd_switch_point_sync_type)
      return 0;

   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);
   mtx_lock(&ssync->mutex);
   const uint32_t count = MIN2(ssync->fence_count, max_fences);
   if (count > 0 && fences_out != NULL)
      memcpy(fences_out, ssync->fences, count * sizeof(*fences_out));
   mtx_unlock(&ssync->mutex);
   return count;
}

bool
nvkmd_switch_sync_peek_nvmultifence(struct vk_sync *sync, NvMultiFence *out)
{
   if (sync == NULL || sync->type != &nvkmd_switch_point_sync_type)
      return false;

   struct nvkmd_switch_sync *ssync = nvkmd_switch_sync_from_vk(sync);
   struct nouveau_horizon_fence fences[4];
   uint32_t fence_count;

   mtx_lock(&ssync->mutex);
   fence_count = ssync->fence_count;
   memcpy(fences, ssync->fences, sizeof(fences));
   mtx_unlock(&ssync->mutex);

   if (fence_count == 0)
      return false;

   if (out != NULL) {
      memset(out, 0, sizeof(*out));
      out->num_fences = fence_count;
      for (uint32_t i = 0; i < fence_count; i++) {
         out->fences[i] = (NvFence) {
            .id = fences[i].id,
            .value = fences[i].value,
         };
      }
      for (uint32_t i = fence_count; i < ARRAY_SIZE(out->fences); i++)
         out->fences[i].id = UINT32_MAX;
   }

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

   /* Use static GM20B properties without acquiring libnx services during
    * enumeration.
    */
   nouveau_horizon_get_gm20b_info(&pdev->base.dev_info);

   pdev->base.kmd_info = (struct nvkmd_info) {
      .has_dma_buf = false,
      /* nvMapCreate accepts a caller-supplied page-aligned pointer. */
      .has_host_ptr_import = true,
      .has_get_vram_used = false,
      /* This currently gates EXT_image_drm_format_modifier exposure, not
       * whether the KMD has an internal tiled NvMap allocation path.
       */
      .has_alloc_tiled = false,
      .has_map_fixed = false,
      .has_overmap = false,
      /* Horizon supports GM20B compression despite nouveau's Turing+
       * restriction; see the public nvgpu UAPI and deko3d compressed
       * kinds. Keep NvMap backing pitch-linear and apply NIL kinds at GPU
       * VA mapping.
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
