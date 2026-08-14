/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_SWITCH_H
#define NVKMD_SWITCH_H 1

#include "nouveau/horizon/nouveau_horizon.h"
#include "nvkmd/nvkmd.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"

#include <switch.h>

/* Horizon / Tegra X1 nvkmd backend using libnx nv services. */

struct nvkmd_switch_pdev {
   struct nvkmd_pdev base;

   struct vk_sync_timeline_type timeline_sync_type;

   /* sync_types is a NULL-terminated array referenced by base.sync_types */
   const struct vk_sync_type *sync_types[3];
};

struct nvkmd_switch_dev {
   struct nvkmd_dev base;

   struct nouveau_horizon_runtime *runtime;
   struct nouveau_horizon_device *horizon;

   /* Persistent four-byte GPU completion payload slabs. */
   simple_mtx_t sync_payload_mutex;
   struct list_head sync_payload_slabs;
};

/* Horizon's GPU address space is initialized with the smallest GM20B big-page
 * size (64 KiB).  Expose that as the public bind alignment so Vulkan memory
 * requirements and VMA suballocations never hand the KMD a 4 KiB-offset range
 * that nvhost-as-gpu cannot bind or alias safely.
 */
#define NVKMD_SWITCH_BIND_ALIGN_B ((uint32_t)0x10000)

#define NVKMD_SWITCH_HEAP_START ((uint64_t)4096)
#define NVKMD_SWITCH_HEAP_END ((uint64_t)(1ull << 38))

VkResult nvkmd_switch_try_create_pdev(struct vk_object_base *log_obj,
                                      enum nvk_debug debug_flags,
                                      struct nvkmd_pdev **pdev_out);

VkResult nvkmd_switch_create_dev(struct nvkmd_pdev *pdev,
                                 struct vk_object_base *log_obj,
                                 struct nvkmd_dev **dev_out);

void nvkmd_switch_sync_payload_pool_init(struct nvkmd_switch_dev *dev);
void nvkmd_switch_sync_payload_pool_finish(struct nvkmd_switch_dev *dev);

/* Attach a just-kicked-off native fence payload to a binary sync object so
 * that a subsequent vk_sync_wait (host or queue) waits on real GPU
 * completion instead of a CPU condvar broadcast.
 *
 * `sync` must belong to the point sync type registered by the Switch
 * backend; callers should only invoke this on syncs reachable through a
 * struct vk_sync_signal chain.
 */
void nvkmd_switch_sync_import_nvfence(struct vk_sync *sync,
                                      const NvFence *fence);
void nvkmd_switch_sync_import_nvmultifence(struct vk_sync *sync,
                                           const NvMultiFence *fence);

/* Whether the given sync type is the Switch nvfence binary sync. Lets
 * the ctx layer skip non-native sync objects (e.g. timeline wrappers,
 * dummy syncs) when importing fences. */
bool nvkmd_switch_sync_is_nvfence(const struct vk_sync_type *type);

/* Adapter-neutral forms used by the nvkmd context path.  The NvFence entry
 * points above remain the WSI boundary only.
 */
void nvkmd_switch_sync_import_horizon_fence(
   struct vk_sync *sync, const struct nouveau_horizon_fence *fence);
uint32_t nvkmd_switch_sync_peek_horizon_fences(
   struct vk_sync *sync, uint32_t max_fences,
   struct nouveau_horizon_fence *fences_out);

/* Copy out the native fence payload currently installed on a vk_sync,
 * without waiting. Returns true iff the sync is a native nvfence-backed
 * binary sync AND a GPU-side fence payload has been imported into it (i.e.
 * the producing submission has already handed over its kickoff fence via
 * ctx_signal or WSI acquire imported a release fence). Pure CPU pre-signals
 * (ssync->signaled without a real fence) return false, so callers can fall
 * back to a host-side wait.
 */
bool nvkmd_switch_sync_peek_nvmultifence(struct vk_sync *sync,
                                         NvMultiFence *out);
bool nvkmd_switch_sync_peek_nvfence(struct vk_sync *sync, NvFence *out);

VkResult nvkmd_switch_sync_copy_payloads(struct vk_device *device,
                                         uint32_t wait_count,
                                         const struct vk_sync_wait *waits,
                                         uint32_t signal_count,
                                         const struct vk_sync_signal *signals);

/* Return the public NvMap ID backing this nvkmd_mem.  WSI passes the numeric
 * ID to the compositor without exposing Horizon backend internals.
 */
uint32_t nvkmd_switch_mem_get_nvmap_id(struct nvkmd_mem *mem);

/* Thin queue adapters for the process-wide Horizon ZBC snapshot. */
void nvkmd_switch_dev_get_zbc_state(
   struct nvkmd_dev *dev, struct nouveau_horizon_zbc_state *state_out);
uint64_t nvkmd_switch_dev_get_zbc_generation(struct nvkmd_dev *dev);
void nvkmd_switch_dev_record_zbc_program(struct nvkmd_dev *dev);

/* Destroy a Switch context only when its Horizon channel can prove final
 * completion.  A false return consumes the channel reference but retains the
 * adapter context as a quarantine anchor; callers must retain every resource
 * which may still be referenced by that channel.
 */
bool nvkmd_switch_ctx_try_destroy(struct nvkmd_ctx *ctx,
                                  struct vk_object_base *log_obj);

#endif /* NVKMD_SWITCH_H */
