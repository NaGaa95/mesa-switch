/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef NVKMD_SWITCH_H
#define NVKMD_SWITCH_H 1

#include "nvkmd/nvkmd.h"
#include "util/vma.h"
#include "vk_sync.h"
#include "vk_sync_timeline.h"

#include <switch.h>

/* Stub Switch (Horizon / Tegra X1) backend for nvkmd.
 *
 * We currently synthesize a hard-coded GM20B physical device and provide a
 * logical-device object backed by libnx nv services for BO allocation and VA
 * management. Real GPU context submission still needs explicit nvServices /
 * nvgpu wiring.
 */

struct nvkmd_switch_pdev {
   struct nvkmd_pdev base;

   struct vk_sync_timeline_type timeline_sync_type;

   /* sync_types is a NULL-terminated array referenced by base.sync_types */
   const struct vk_sync_type *sync_types[3];
};

struct nvkmd_switch_dev {
   struct nvkmd_dev base;

   NvAddressSpace addr_space;

   simple_mtx_t heap_mutex;
   struct util_vma_heap heap;
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

/* Return the libnx NvMap backing this nvkmd_mem. The pointer is owned by
 * the nvkmd_mem and lives until it is freed. Used by the WSI to register
 * a swapchain image as a scanout buffer with the Horizon compositor.
 */
NvMap *nvkmd_switch_mem_get_nvmap(struct nvkmd_mem *mem);

#endif /* NVKMD_SWITCH_H */
