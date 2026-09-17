/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nouveau_horizon_private.h"

#include "util/os_misc.h"
#include "util/u_atomic.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include <assert.h>
#include <inttypes.h>
#include <string.h>

#define NOUVEAU_HORIZON_CLS_ENG3D 0xb197
#define NOUVEAU_HORIZON_CLS_COMPUTE 0xb1c0
#define NOUVEAU_HORIZON_CLS_COPY 0xb0b5
#define NOUVEAU_HORIZON_CLS_ENG2D 0x902d
#define NOUVEAU_HORIZON_CLS_M2MF 0xa140
#define NOUVEAU_HORIZON_CLS_GPFIFO 0xb06f

void
nouveau_horizon_get_gm20b_info(struct nv_device_info *info_out)
{
   if (info_out == NULL)
      return;

   memset(info_out, 0, sizeof(*info_out));
   info_out->type = NV_DEVICE_TYPE_SOC;
   info_out->device_id = 0x0fe0;
   info_out->chipset = 0x12b;
   strncpy(info_out->device_name, "NVIDIA Tegra X1 (GM20B)",
           sizeof(info_out->device_name) - 1);
   strncpy(info_out->chipset_name, "GM20B",
           sizeof(info_out->chipset_name) - 1);

   info_out->sm = 53;
   info_out->gpc_count = 1;
   info_out->tpc_count = 2;
   info_out->mp_per_tpc = 1;
   info_out->max_warps_per_mp = 64;

   info_out->cls_eng3d = NOUVEAU_HORIZON_CLS_ENG3D;
   info_out->cls_compute = NOUVEAU_HORIZON_CLS_COMPUTE;
   info_out->cls_copy = NOUVEAU_HORIZON_CLS_COPY;
   info_out->cls_eng2d = NOUVEAU_HORIZON_CLS_ENG2D;
   info_out->cls_m2mf = NOUVEAU_HORIZON_CLS_M2MF;
   info_out->cls_gpfifo = NOUVEAU_HORIZON_CLS_GPFIFO;

   /* GM20B is UMA.  Do not invent dedicated VRAM or a PCI BAR. */
   info_out->vram_size_B = 0;
   info_out->bar_size_B = 0;
   /* Use 64 KiB per SMM for carveout selection and the 48 KiB
    * per-workgroup limit for API/compiler validation.
    */
   info_out->sm_smem_sizes_kB[0] = 64;
   info_out->sm_smem_size_count = 1;
   info_out->max_smem_per_wg_kB = 48;
   info_out->nc_atom_size_B = 128;
}

static void
nouveau_horizon_add_runtime_device_info(struct nouveau_horizon_device *device)
{
   const nvioctl_zcull_info *zcull = nvGpuGetZcullInfo();
   const uint32_t ctxsw_size = nvGpuGetZcullCtxSize();
   if (zcull == NULL || ctxsw_size == 0 ||
       zcull->width_align_pixels == 0 ||
       zcull->height_align_pixels == 0 ||
       zcull->pixel_squares_by_aliquots == 0 ||
       zcull->aliquot_total == 0)
      return;

   device->info.zcull_info = (struct nv_zcull_device_info) {
      .width_align_pixels = zcull->width_align_pixels,
      .height_align_pixels = zcull->height_align_pixels,
      .pixel_squares_by_aliquots = zcull->pixel_squares_by_aliquots,
      .aliquot_total = zcull->aliquot_total,
      .zcull_region_byte_multiplier = zcull->region_byte_multiplier,
      .zcull_region_header_size = zcull->region_header_size,
      .zcull_subregion_header_size = zcull->subregion_header_size,
      .subregion_count = zcull->subregion_count,
      .subregion_width_align_pixels =
         zcull->subregion_width_align_pixels,
      .subregion_height_align_pixels =
         zcull->subregion_height_align_pixels,
      .ctxsw_size = ctxsw_size,
      /* nvhost allocates the GM20B Z-cull context at a 4 KiB boundary. */
      .ctxsw_align = 0x1000,
   };
   device->info.has_zcull_info = true;
}

static uint32_t
nouveau_horizon_choose_page_size(
   const nvioctl_gpu_characteristics *gpu_info)
{
   if (gpu_info != NULL) {
      const uint32_t available = gpu_info->available_big_page_sizes;
      if (available != 0) {
         const uint32_t smallest = available & (~available + 1u);
         if (util_is_power_of_two_nonzero64(smallest))
            return smallest;
      }

      if (util_is_power_of_two_nonzero64(gpu_info->big_page_size))
         return gpu_info->big_page_size;
   }

   return NOUVEAU_HORIZON_BIND_ALIGN_B;
}

static bool
nouveau_horizon_va_region_range(const nvioctl_va_region *region,
                                uint64_t *start_out,
                                uint64_t *end_out)
{
   if (region->page_size == 0 || region->pages == 0 ||
       region->pages > UINT64_MAX / region->page_size)
      return false;

   const uint64_t size_B = region->pages * (uint64_t)region->page_size;
   if (region->offset > UINT64_MAX - size_B)
      return false;

   *start_out = region->offset;
   *end_out = region->offset + size_B;
   return true;
}

static enum nouveau_horizon_status
nouveau_horizon_device_init_va_heap(struct nouveau_horizon_device *device)
{
   nvioctl_va_region regions[2] = {0};
   Result rc = nvioctlNvhostAsGpu_GetVARegions(device->addr_space.fd, regions);
   if (R_FAILED(rc)) {
      nouveau_horizon_log(device, NOUVEAU_HORIZON_LOG_ERROR,
                          "GetVARegions failed: 0x%x", R_VALUE(rc));
      return nouveau_horizon_status_from_result(
         rc, NOUVEAU_HORIZON_ERROR_SYSTEM);
   }

   uint64_t heap_start = 0;
   uint64_t heap_end = 0;
   bool found = false;

   for (uint32_t i = 0; i < 2; i++) {
      if (regions[i].page_size != device->page_size_B)
         continue;

      if (!nouveau_horizon_va_region_range(&regions[i],
                                            &heap_start, &heap_end))
         continue;

      found = true;
      break;
   }

   if (!found) {
      nouveau_horizon_log(
         device, NOUVEAU_HORIZON_LOG_ERROR,
         "no VA region for page size 0x%x "
         "(regions={0x%" PRIx64 ",0x%x,0x%" PRIx64 "},"
         "{0x%" PRIx64 ",0x%x,0x%" PRIx64 "})",
         device->page_size_B,
         regions[0].offset, regions[0].page_size, regions[0].pages,
         regions[1].offset, regions[1].page_size, regions[1].pages);
      return NOUVEAU_HORIZON_ERROR_SYSTEM;
   }

   if (heap_start == 0)
      heap_start = device->page_size_B;

   if (heap_start >= heap_end) {
      nouveau_horizon_log(device, NOUVEAU_HORIZON_LOG_ERROR,
                          "invalid VA range [0x%" PRIx64 ",0x%" PRIx64 ")",
                          heap_start, heap_end);
      return NOUVEAU_HORIZON_ERROR_SYSTEM;
   }

   device->va_start = heap_start;
   device->va_end = heap_end;
   util_vma_heap_init(&device->va_heap, heap_start, heap_end - heap_start);
   return NOUVEAU_HORIZON_SUCCESS;
}

enum nouveau_horizon_status
nouveau_horizon_device_create(
   struct nouveau_horizon_runtime *runtime,
   const struct nouveau_horizon_device_create_info *create_info,
   struct nouveau_horizon_device **device_out)
{
   if (runtime == NULL || device_out == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   *device_out = NULL;
   struct nouveau_horizon_device *device =
      CALLOC_STRUCT(nouveau_horizon_device);
   if (device == NULL)
      return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;

   device->refcnt = 1;
   device->runtime = nouveau_horizon_runtime_ref(runtime);
   if (create_info != NULL) {
      device->logger = create_info->logger;
      device->total_order_channels = create_info->total_order_channels;
      device->enable_timing = create_info->enable_timing;
   }
   nouveau_horizon_get_gm20b_info(&device->info);
   nouveau_horizon_add_runtime_device_info(device);

   const nvioctl_gpu_characteristics *gpu_info = nvGpuGetCharacteristics();
   device->page_size_B = nouveau_horizon_choose_page_size(gpu_info);
   device->bind_align_B = MAX2(device->page_size_B,
                                NOUVEAU_HORIZON_BIND_ALIGN_B);

   Result rc = nvAddressSpaceCreate(&device->addr_space,
                                    device->page_size_B);
   if (R_FAILED(rc)) {
      nouveau_horizon_log(device, NOUVEAU_HORIZON_LOG_ERROR,
                          "nvAddressSpaceCreate failed: 0x%x", R_VALUE(rc));
      nouveau_horizon_runtime_put(device->runtime);
      FREE(device);
      return nouveau_horizon_status_from_result(
         rc, NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   simple_mtx_init(&device->va_mutex, mtx_plain);
   simple_mtx_init(&device->memory_mutex, mtx_plain);
   list_inithead(&device->memories);
   simple_mtx_init(&device->debug_stats_mutex, mtx_plain);
   simple_mtx_init(&device->submit_mutex, mtx_plain);
   simple_mtx_init(&device->channel_mutex, mtx_plain);
   list_inithead(&device->channels);
   nouveau_horizon_device_bo_cache_init(device);

   enum nouveau_horizon_status status =
      nouveau_horizon_device_init_va_heap(device);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      goto fail_heap;

   uint64_t total_B = 0;
   uint64_t available_B = 0;
   const bool have_total = os_get_total_physical_memory(&total_B);
   const bool have_available = os_get_available_system_memory(&available_B);
   if (!have_total && have_available)
      total_B = available_B;
   device->total_memory_B = total_B;

   device->global_fence.id = NOUVEAU_HORIZON_INVALID_FENCE_ID;
   device->next_channel_id = 1;

   nouveau_horizon_log(
      device, NOUVEAU_HORIZON_LOG_INFO,
      "GM20B device ready: page=0x%x bind=0x%x VA=[0x%" PRIx64
      ",0x%" PRIx64 ") total-order=%s timing=%s UMA=%" PRIu64
      " MiB available=%" PRIu64 " MiB bo-cache=%" PRIu64 " MiB",
      device->page_size_B, device->bind_align_B,
      device->va_start, device->va_end,
      device->total_order_channels ? "on" : "off",
      device->enable_timing ? "on" : "off",
      total_B >> 20,
      available_B >> 20,
      device->bo_cache_cap_B >> 20);

   list_addtail(&device->runtime_link, &device->runtime->devices);
   *device_out = device;
   return NOUVEAU_HORIZON_SUCCESS;

fail_heap:
   nouveau_horizon_device_bo_cache_finish(device);
   simple_mtx_destroy(&device->channel_mutex);
   simple_mtx_destroy(&device->submit_mutex);
   simple_mtx_destroy(&device->debug_stats_mutex);
   simple_mtx_destroy(&device->memory_mutex);
   simple_mtx_destroy(&device->va_mutex);
   nvAddressSpaceClose(&device->addr_space);
   nouveau_horizon_runtime_put(device->runtime);
   FREE(device);
   return status;
}

struct nouveau_horizon_device *
nouveau_horizon_device_ref(struct nouveau_horizon_device *device)
{
   if (device != NULL)
      p_atomic_inc(&device->refcnt);
   return device;
}

void
nouveau_horizon_device_put(struct nouveau_horizon_device *device)
{
   if (device == NULL || !p_atomic_dec_zero(&device->refcnt))
      return;

   assert(device->allocated_memory_B == 0);
   assert(list_is_empty(&device->memories));
   assert(list_is_empty(&device->channels));
   if (device->enable_timing) {
      struct nouveau_horizon_device_debug_stats stats = {0};
      nouveau_horizon_device_get_debug_stats(device, &stats);
      nouveau_horizon_log(
         device, NOUVEAU_HORIZON_LOG_INFO,
         "device perf: memory={create=%llu failures=%llu peak-native=%llu "
         "peak-wrappers=%llu} VA={create=%llu bind=%llu failures=%llu "
         "peak-live=%llu} fence-wait={calls=%llu timeouts=%llu failures=%llu "
         "total-us=%llu max-us=%llu} cache={to-gpu-calls=%llu bytes=%llu "
         "us=%llu max-us=%llu from-gpu-calls=%llu bytes=%llu us=%llu "
         "max-us=%llu}",
         (unsigned long long)stats.memory_create_calls,
         (unsigned long long)stats.memory_create_failures,
         (unsigned long long)stats.native_memories_peak,
         (unsigned long long)stats.memory_wrappers_peak,
         (unsigned long long)stats.va_create_calls,
         (unsigned long long)stats.va_bind_calls,
         (unsigned long long)stats.va_bind_failures,
         (unsigned long long)stats.mappings_peak,
         (unsigned long long)stats.fence_wait_calls,
         (unsigned long long)stats.fence_wait_timeouts,
         (unsigned long long)stats.fence_wait_failures,
         (unsigned long long)(stats.fence_wait_ns / 1000),
         (unsigned long long)(stats.fence_wait_max_ns / 1000),
         (unsigned long long)stats.cache_to_gpu_calls,
         (unsigned long long)stats.cache_to_gpu_bytes,
         (unsigned long long)(stats.cache_to_gpu_ns / 1000),
         (unsigned long long)(stats.cache_to_gpu_max_ns / 1000),
         (unsigned long long)stats.cache_from_gpu_calls,
         (unsigned long long)stats.cache_from_gpu_bytes,
         (unsigned long long)(stats.cache_from_gpu_ns / 1000),
         (unsigned long long)(stats.cache_from_gpu_max_ns / 1000));
   }
   simple_mtx_lock(&device->bo_cache_mutex);
   const uint64_t bo_hits = device->bo_cache_hits;
   const uint64_t bo_misses = device->bo_cache_misses;
   const uint64_t bo_evictions = device->bo_cache_evictions;
   simple_mtx_unlock(&device->bo_cache_mutex);
   if (bo_hits + bo_misses > 0) {
      nouveau_horizon_log(
         device, NOUVEAU_HORIZON_LOG_INFO,
         "bo-cache: %" PRIu64 " hits, %" PRIu64 " misses, %" PRIu64
         " evictions",
         bo_hits, bo_misses, bo_evictions);
   }
   list_del(&device->runtime_link);
   nouveau_horizon_device_bo_cache_finish(device);
   util_vma_heap_finish(&device->va_heap);
   simple_mtx_destroy(&device->channel_mutex);
   simple_mtx_destroy(&device->submit_mutex);
   simple_mtx_destroy(&device->debug_stats_mutex);
   simple_mtx_destroy(&device->memory_mutex);
   simple_mtx_destroy(&device->va_mutex);
   nvAddressSpaceClose(&device->addr_space);
   nouveau_horizon_runtime_put(device->runtime);
   FREE(device);
}

const struct nv_device_info *
nouveau_horizon_device_get_info(struct nouveau_horizon_device *device)
{
   return device != NULL ? &device->info : NULL;
}

void
nouveau_horizon_device_get_properties(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_device_properties *properties_out)
{
   if (device == NULL || properties_out == NULL)
      return;

   *properties_out = (struct nouveau_horizon_device_properties) {
      .page_size_B = device->page_size_B,
      .bind_align_B = device->bind_align_B,
      .va_start = device->va_start,
      .va_end = device->va_end,
      .has_compression = true,
      .total_order_channels = device->total_order_channels,
   };
}

void
nouveau_horizon_device_get_memory_info(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_memory_info *memory_info_out)
{
   if (device == NULL || memory_info_out == NULL)
      return;

   uint64_t available_B = 0;
   if (!os_get_available_system_memory(&available_B))
      available_B = device->total_memory_B;

   simple_mtx_lock(&device->memory_mutex);
   *memory_info_out = (struct nouveau_horizon_memory_info) {
      .total_B = device->total_memory_B,
      .available_B = available_B,
      .allocated_B = device->allocated_memory_B,
      .peak_allocated_B = device->peak_allocated_memory_B,
   };
   simple_mtx_unlock(&device->memory_mutex);
}

void
nouveau_horizon_device_get_debug_stats(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_device_debug_stats *stats_out)
{
   if (device == NULL || stats_out == NULL)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   *stats_out = device->debug_stats;
   simple_mtx_unlock(&device->debug_stats_mutex);

   simple_mtx_lock(&device->bo_cache_mutex);
   stats_out->bo_cache_hits = device->bo_cache_hits;
   stats_out->bo_cache_misses = device->bo_cache_misses;
   stats_out->bo_cache_evictions = device->bo_cache_evictions;
   stats_out->bo_cache_held_B = device->bo_cache_held_B;
   stats_out->bo_cache_entries = device->bo_cache_entry_count;
   simple_mtx_unlock(&device->bo_cache_mutex);

   struct nouveau_horizon_runtime *runtime = device->runtime;
   struct nouveau_horizon_zbc_state zbc;
   nouveau_horizon_runtime_get_zbc_state(runtime, &zbc);

   simple_mtx_lock(&runtime->zbc_mutex);
   stats_out->zbc_active_slot_mask = zbc.active_slot_mask;
   stats_out->zbc_slot_disable_mask = zbc.slot_disable_mask;
   stats_out->zbc_queried_slot = zbc.queried_slot;
   stats_out->zbc_enabled = zbc.enabled;
   stats_out->zbc_generation = zbc.generation;
   stats_out->zbc_query_calls = runtime->zbc_query_calls;
   stats_out->zbc_query_failures = runtime->zbc_query_failures;
   stats_out->zbc_state_changes = runtime->zbc_state_changes;
   stats_out->zbc_refresh_calls = runtime->zbc_refresh_calls;
   stats_out->zbc_add_failures = runtime->zbc_add_failures;
   simple_mtx_unlock(&runtime->zbc_mutex);
}

void
nouveau_horizon_device_get_zbc_state(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_zbc_state *state_out)
{
   if (device == NULL || state_out == NULL)
      return;

   nouveau_horizon_runtime_get_zbc_state(device->runtime, state_out);
}

uint64_t
nouveau_horizon_device_get_zbc_generation(
   struct nouveau_horizon_device *device)
{
   if (device == NULL)
      return 0;

   return p_atomic_read(&device->runtime->zbc_state.generation);
}

void
nouveau_horizon_device_refresh_zbc_state(
   struct nouveau_horizon_device *device)
{
   if (device == NULL)
      return;

   uint64_t failures_before;
   simple_mtx_lock(&device->runtime->zbc_mutex);
   failures_before = device->runtime->zbc_query_failures;
   simple_mtx_unlock(&device->runtime->zbc_mutex);

   simple_mtx_lock(&device->runtime->zbc_mutex);
   device->runtime->zbc_refresh_calls++;
   simple_mtx_unlock(&device->runtime->zbc_mutex);
   nouveau_horizon_runtime_refresh_zbc(device->runtime);

   simple_mtx_lock(&device->runtime->zbc_mutex);
   const bool failed =
      device->runtime->zbc_query_failures != failures_before;
   simple_mtx_unlock(&device->runtime->zbc_mutex);

   if (failed) {
      nouveau_horizon_log(
         device, NOUVEAU_HORIZON_LOG_WARNING,
         "ZBC active-slot query failed; all slots remain disabled");
   }
}

void
nouveau_horizon_device_record_zbc_program(
   struct nouveau_horizon_device *device)
{
   if (device == NULL || !device->enable_timing)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   device->debug_stats.zbc_programs++;
   simple_mtx_unlock(&device->debug_stats_mutex);
}

uint32_t
nouveau_horizon_device_bind_align(struct nouveau_horizon_device *device)
{
   return device->bind_align_B;
}

static unsigned
nouveau_horizon_memory_size_bucket(uint64_t size_B)
{
   if (size_B <= 64 * 1024)
      return 0;
   if (size_B <= 256 * 1024)
      return 1;
   if (size_B <= 1024 * 1024)
      return 2;
   if (size_B <= 4 * 1024 * 1024)
      return 3;
   return 4;
}

void
nouveau_horizon_device_account_alloc(
   struct nouveau_horizon_device *device, uint64_t size_B)
{
   simple_mtx_lock(&device->memory_mutex);
   device->allocated_memory_B += size_B;
   device->peak_allocated_memory_B =
      MAX2(device->peak_allocated_memory_B, device->allocated_memory_B);
   simple_mtx_unlock(&device->memory_mutex);

   if (device->enable_timing) {
      simple_mtx_lock(&device->debug_stats_mutex);
      const unsigned bucket = nouveau_horizon_memory_size_bucket(size_B);
      device->debug_stats.native_memories_live++;
      device->debug_stats.native_memories_peak =
         MAX2(device->debug_stats.native_memories_peak,
              device->debug_stats.native_memories_live);
      device->debug_stats.native_memory_live_by_size[bucket]++;
      device->debug_stats.native_memory_created_by_size[bucket]++;
      simple_mtx_unlock(&device->debug_stats_mutex);
   }
}

void
nouveau_horizon_device_account_free(
   struct nouveau_horizon_device *device, uint64_t size_B)
{
   simple_mtx_lock(&device->memory_mutex);
   assert(device->allocated_memory_B >= size_B);
   device->allocated_memory_B -= size_B;
   simple_mtx_unlock(&device->memory_mutex);

   if (device->enable_timing) {
      simple_mtx_lock(&device->debug_stats_mutex);
      const unsigned bucket = nouveau_horizon_memory_size_bucket(size_B);
      assert(device->debug_stats.native_memories_live > 0);
      assert(device->debug_stats.native_memory_live_by_size[bucket] > 0);
      device->debug_stats.native_memories_live--;
      device->debug_stats.native_memory_live_by_size[bucket]--;
      simple_mtx_unlock(&device->debug_stats_mutex);
   }
}

void
nouveau_horizon_device_record_fence_wait(
   struct nouveau_horizon_device *device,
   enum nouveau_horizon_status status, uint64_t elapsed_ns)
{
   if (device == NULL || !device->enable_timing)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   struct nouveau_horizon_device_debug_stats *stats = &device->debug_stats;
   stats->fence_wait_calls++;
   stats->fence_wait_ns += elapsed_ns;
   stats->fence_wait_max_ns = MAX2(stats->fence_wait_max_ns, elapsed_ns);
   if (status == NOUVEAU_HORIZON_ERROR_TIMEOUT)
      stats->fence_wait_timeouts++;
   else if (status != NOUVEAU_HORIZON_SUCCESS)
      stats->fence_wait_failures++;
   simple_mtx_unlock(&device->debug_stats_mutex);
}

void
nouveau_horizon_device_record_cache_sync(
   struct nouveau_horizon_device *device, bool to_gpu,
   uint64_t bytes, uint64_t elapsed_ns)
{
   if (device == NULL || !device->enable_timing)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   struct nouveau_horizon_device_debug_stats *stats = &device->debug_stats;
   if (to_gpu) {
      stats->cache_to_gpu_calls++;
      stats->cache_to_gpu_bytes += bytes;
      stats->cache_to_gpu_ns += elapsed_ns;
      stats->cache_to_gpu_max_ns =
         MAX2(stats->cache_to_gpu_max_ns, elapsed_ns);
   } else {
      stats->cache_from_gpu_calls++;
      stats->cache_from_gpu_bytes += bytes;
      stats->cache_from_gpu_ns += elapsed_ns;
      stats->cache_from_gpu_max_ns =
         MAX2(stats->cache_from_gpu_max_ns, elapsed_ns);
   }
   simple_mtx_unlock(&device->debug_stats_mutex);
}

void
nouveau_horizon_device_mark_lost(struct nouveau_horizon_device *device)
{
   if (device != NULL)
      p_atomic_set(&device->lost, 1);
}

bool
nouveau_horizon_device_is_lost(struct nouveau_horizon_device *device)
{
   return device != NULL && p_atomic_read(&device->lost) != 0;
}
