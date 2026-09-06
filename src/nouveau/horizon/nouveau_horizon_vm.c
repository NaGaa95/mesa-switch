/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nouveau_horizon_private.h"

#include "util/u_atomic.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include <assert.h>
#include <inttypes.h>

static void
nouveau_horizon_va_record_create_call(
   struct nouveau_horizon_device *device)
{
   if (!device->enable_timing)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   device->debug_stats.va_create_calls++;
   simple_mtx_unlock(&device->debug_stats_mutex);
}

static void
nouveau_horizon_va_record_create_failure(
   struct nouveau_horizon_device *device)
{
   simple_mtx_lock(&device->debug_stats_mutex);
   device->debug_stats.va_create_failures++;
   simple_mtx_unlock(&device->debug_stats_mutex);
}

static void
nouveau_horizon_va_record_created(
   struct nouveau_horizon_device *device)
{
   if (!device->enable_timing)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   device->debug_stats.vas_live++;
   device->debug_stats.vas_peak =
      MAX2(device->debug_stats.vas_peak, device->debug_stats.vas_live);
   simple_mtx_unlock(&device->debug_stats_mutex);
}

static void
nouveau_horizon_va_record_freed(
   struct nouveau_horizon_device *device)
{
   if (!device->enable_timing)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   assert(device->debug_stats.vas_live > 0);
   device->debug_stats.vas_live--;
   simple_mtx_unlock(&device->debug_stats_mutex);
}

static void
nouveau_horizon_va_record_bind_call(
   struct nouveau_horizon_device *device)
{
   if (!device->enable_timing)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   device->debug_stats.va_bind_calls++;
   simple_mtx_unlock(&device->debug_stats_mutex);
}

static uint64_t
nouveau_horizon_va_record_bind_failure(
   struct nouveau_horizon_device *device)
{
   simple_mtx_lock(&device->debug_stats_mutex);
   const uint64_t failures = ++device->debug_stats.va_bind_failures;
   simple_mtx_unlock(&device->debug_stats_mutex);
   return failures;
}

static void
nouveau_horizon_va_record_mapping_added(
   struct nouveau_horizon_device *device)
{
   if (!device->enable_timing)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   device->debug_stats.mappings_live++;
   device->debug_stats.mappings_peak =
      MAX2(device->debug_stats.mappings_peak,
           device->debug_stats.mappings_live);
   simple_mtx_unlock(&device->debug_stats_mutex);
}

static void
nouveau_horizon_va_record_mapping_removed(
   struct nouveau_horizon_device *device)
{
   if (!device->enable_timing)
      return;

   simple_mtx_lock(&device->debug_stats_mutex);
   assert(device->debug_stats.mappings_live > 0);
   device->debug_stats.mappings_live--;
   simple_mtx_unlock(&device->debug_stats_mutex);
}

static bool
nouveau_horizon_va_log_failure(uint64_t failures)
{
   return failures <= 16 || (failures & (failures - 1)) == 0 ||
          failures % 256 == 0;
}

static bool
nouveau_horizon_range_valid(uint64_t offset_B, uint64_t range_B,
                            uint64_t size_B)
{
   return range_B > 0 && offset_B <= size_B &&
          range_B <= size_B - offset_B;
}

enum nouveau_horizon_status
nouveau_horizon_va_create(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_va_create_info *create_info,
   struct nouveau_horizon_va **va_out)
{
   const uint32_t valid_flags = NOUVEAU_HORIZON_VA_FIXED |
                                NOUVEAU_HORIZON_VA_SPARSE;
   if (device == NULL || create_info == NULL || va_out == NULL ||
       create_info->size_B == 0 || (create_info->flags & ~valid_flags))
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   nouveau_horizon_va_record_create_call(device);

   *va_out = NULL;
   const uint64_t bind_align_B = nouveau_horizon_device_bind_align(device);
   uint64_t align_B = MAX2(create_info->align_B, bind_align_B);
   if (!util_is_power_of_two_nonzero64(align_B)) {
      nouveau_horizon_va_record_create_failure(device);
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
   }

   const uint64_t size_B =
      nouveau_horizon_align_u64(create_info->size_B, bind_align_B);
   if (size_B == 0) {
      nouveau_horizon_va_record_create_failure(device);
      return NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   if ((create_info->flags & NOUVEAU_HORIZON_VA_FIXED) &&
       (create_info->fixed_addr % align_B != 0 ||
        create_info->fixed_addr < device->va_start ||
        create_info->fixed_addr > device->va_end ||
        size_B > device->va_end - create_info->fixed_addr))
   {
      nouveau_horizon_va_record_create_failure(device);
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
   }

   struct nouveau_horizon_va *va = CALLOC_STRUCT(nouveau_horizon_va);
   if (va == NULL) {
      nouveau_horizon_va_record_create_failure(device);
      return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;
   }

   simple_mtx_lock(&device->va_mutex);
   if (create_info->flags & NOUVEAU_HORIZON_VA_FIXED) {
      if (!util_vma_heap_alloc_addr(&device->va_heap,
                                    create_info->fixed_addr, size_B)) {
         simple_mtx_unlock(&device->va_mutex);
         nouveau_horizon_va_record_create_failure(device);
         FREE(va);
         return NOUVEAU_HORIZON_ERROR_BUSY;
      }
      va->addr = create_info->fixed_addr;
   } else {
      va->addr = util_vma_heap_alloc(&device->va_heap, size_B, align_B);
      if (va->addr == 0) {
         simple_mtx_unlock(&device->va_mutex);
         nouveau_horizon_va_record_create_failure(device);
         FREE(va);
         return NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY;
      }
   }

   Result rc = nvAddressSpaceAllocFixed(
      &device->addr_space,
      (create_info->flags & NOUVEAU_HORIZON_VA_SPARSE) != 0,
      size_B, va->addr);
   if (R_FAILED(rc)) {
      nouveau_horizon_va_record_create_failure(device);
      util_vma_heap_free(&device->va_heap, va->addr, size_B);
      simple_mtx_unlock(&device->va_mutex);
      nouveau_horizon_log(
         device, NOUVEAU_HORIZON_LOG_ERROR,
         "nvAddressSpaceAllocFixed(addr=0x%" PRIx64
         ", size=0x%" PRIx64 ") failed: 0x%x",
         va->addr, size_B, R_VALUE(rc));
      FREE(va);
      return nouveau_horizon_status_from_result(
         rc, NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY);
   }
   simple_mtx_unlock(&device->va_mutex);

   va->refcnt = 1;
   va->device = nouveau_horizon_device_ref(device);
   va->size_B = size_B;
   va->flags = create_info->flags;
   va->has_pte_kind = create_info->has_pte_kind;
   va->pte_kind = create_info->pte_kind;
   simple_mtx_init(&va->mutex, mtx_plain);
   list_inithead(&va->mappings);
   nouveau_horizon_va_record_created(device);

   *va_out = va;
   return NOUVEAU_HORIZON_SUCCESS;
}

struct nouveau_horizon_va *
nouveau_horizon_va_ref(struct nouveau_horizon_va *va)
{
   if (va != NULL)
      p_atomic_inc(&va->refcnt);
   return va;
}

void
nouveau_horizon_va_put(struct nouveau_horizon_va *va)
{
   if (va == NULL || !p_atomic_dec_zero(&va->refcnt))
      return;

   struct nouveau_horizon_device *device = va->device;
   if (nouveau_horizon_device_is_lost(device)) {
      va->quarantined = true;
      nouveau_horizon_log(
         device, NOUVEAU_HORIZON_LOG_ERROR,
         "device completion is unknown; quarantining VA [0x%" PRIx64
         ",0x%" PRIx64 ") and all mapped backing storage",
         va->addr, va->addr + va->size_B);
      return;
   }

   simple_mtx_lock(&va->mutex);
   simple_mtx_lock(&device->va_mutex);
   list_for_each_entry_safe(struct nouveau_horizon_va_mapping, mapping,
                            &va->mappings, link) {
      const Result rc = nvAddressSpaceUnmap(&device->addr_space,
                                            mapping->addr);
      if (R_FAILED(rc)) {
         nouveau_horizon_log(device, NOUVEAU_HORIZON_LOG_ERROR,
                             "VA teardown unmap 0x%" PRIx64
                             " failed: 0x%x; quarantining VA range",
                             mapping->addr, R_VALUE(rc));
         va->quarantined = true;
         simple_mtx_unlock(&device->va_mutex);
         simple_mtx_unlock(&va->mutex);
         return;
      }
      list_del(&mapping->link);
      nouveau_horizon_va_record_mapping_removed(device);
      nouveau_horizon_memory_release_mapping(
         mapping->memory, mapping->pte_kind);
      nouveau_horizon_memory_put(mapping->memory);
      FREE(mapping);
   }

   const Result free_rc = nvAddressSpaceFree(&device->addr_space,
                                              va->addr, va->size_B);
   if (R_FAILED(free_rc)) {
      nouveau_horizon_log(device, NOUVEAU_HORIZON_LOG_ERROR,
                          "VA teardown free 0x%" PRIx64
                          " failed: 0x%x; quarantining VA range",
                          va->addr, R_VALUE(free_rc));
      va->quarantined = true;
      simple_mtx_unlock(&device->va_mutex);
      simple_mtx_unlock(&va->mutex);
      return;
   }
   util_vma_heap_free(&device->va_heap, va->addr, va->size_B);
   simple_mtx_unlock(&device->va_mutex);
   simple_mtx_unlock(&va->mutex);

   simple_mtx_destroy(&va->mutex);
   nouveau_horizon_va_record_freed(device);
   nouveau_horizon_device_put(device);
   FREE(va);
}

uint64_t
nouveau_horizon_va_get_addr(struct nouveau_horizon_va *va)
{
   return va != NULL ? va->addr : 0;
}

uint64_t
nouveau_horizon_va_get_size(struct nouveau_horizon_va *va)
{
   return va != NULL ? va->size_B : 0;
}

static bool
nouveau_horizon_va_overlaps(struct nouveau_horizon_va *va,
                            uint64_t addr, uint64_t range_B)
{
   const uint64_t end = addr + range_B;
   list_for_each_entry(struct nouveau_horizon_va_mapping, mapping,
                       &va->mappings, link) {
      const uint64_t mapping_end = mapping->addr + mapping->range_B;
      if (addr < mapping_end && mapping->addr < end)
         return true;
   }
   return false;
}

enum nouveau_horizon_status
nouveau_horizon_va_bind(struct nouveau_horizon_va *va,
                        uint64_t va_offset_B,
                        struct nouveau_horizon_memory *memory,
                        uint64_t memory_offset_B,
                        uint64_t range_B)
{
   if (va == NULL || memory == NULL || va->device != memory->device)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   struct nouveau_horizon_device *device = va->device;
   const uint64_t align_B = nouveau_horizon_device_bind_align(device);
   const uint64_t memory_size_B =
      nouveau_horizon_memory_get_size(memory);
   if (!nouveau_horizon_range_valid(va_offset_B, range_B, va->size_B) ||
       !nouveau_horizon_range_valid(memory_offset_B, range_B,
                                    memory_size_B) ||
       va_offset_B % align_B != 0 || memory_offset_B % align_B != 0 ||
       range_B % align_B != 0)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   nouveau_horizon_va_record_bind_call(device);

   const uint64_t target_addr = va->addr + va_offset_B;
   struct nouveau_horizon_va_mapping *mapping =
      CALLOC_STRUCT(nouveau_horizon_va_mapping);
   if (mapping == NULL) {
      nouveau_horizon_va_record_bind_failure(device);
      return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;
   }

   simple_mtx_lock(&va->mutex);
   if (nouveau_horizon_va_overlaps(va, target_addr, range_B)) {
      simple_mtx_unlock(&va->mutex);
      nouveau_horizon_va_record_bind_failure(device);
      FREE(mapping);
      return NOUVEAU_HORIZON_ERROR_BUSY;
   }

   const NvKind kind = va->has_pte_kind ?
      (NvKind)va->pte_kind :
      (NvKind)nouveau_horizon_memory_get_backing_kind(memory);
   enum nouveau_horizon_status mapping_status =
      nouveau_horizon_memory_acquire_mapping(memory, (uint8_t)kind);
   if (mapping_status != NOUVEAU_HORIZON_SUCCESS) {
      simple_mtx_unlock(&va->mutex);
      nouveau_horizon_va_record_bind_failure(device);
      FREE(mapping);
      return mapping_status;
   }

   const bool partial = memory_offset_B != 0 || range_B != memory_size_B;
   uint64_t mapped_addr = target_addr;

   simple_mtx_lock(&device->va_mutex);
   Result rc;
   if (partial) {
      const uint32_t flags = NvMapBufferFlags_FixedOffset |
         (nouveau_horizon_memory_is_gpu_cacheable(memory) ?
             NvMapBufferFlags_IsCacheable : 0);
      rc = nvioctlNvhostAsGpu_MapBufferEx(
         device->addr_space.fd, flags, kind,
         nouveau_horizon_memory_get_nvmap_handle(memory),
         device->page_size_B, memory_offset_B, range_B,
         target_addr, &mapped_addr);
   } else {
      rc = nvAddressSpaceMapFixed(
         &device->addr_space,
         nouveau_horizon_memory_get_nvmap_handle(memory),
         nouveau_horizon_memory_is_gpu_cacheable(memory),
         kind, target_addr);
   }

   if (R_SUCCEEDED(rc) && mapped_addr != target_addr) {
      const Result cleanup_rc =
         nvAddressSpaceUnmap(&device->addr_space, mapped_addr);
      if (R_FAILED(cleanup_rc)) {
         /* Failed unmapping leaves a live GPU mapping. Retain its backing
          * and PTE-kind reservation to prevent aliasing recycled storage.
          */
         mapping->addr = mapped_addr;
         mapping->range_B = range_B;
         mapping->memory = nouveau_horizon_memory_ref(memory);
         mapping->pte_kind = (uint8_t)kind;
         list_addtail(&mapping->link, &va->mappings);
         nouveau_horizon_va_record_mapping_added(device);
         nouveau_horizon_va_record_bind_failure(device);
         va->quarantined = true;
         nouveau_horizon_device_mark_lost(device);
         simple_mtx_unlock(&device->va_mutex);
         simple_mtx_unlock(&va->mutex);
         nouveau_horizon_log(
            device, NOUVEAU_HORIZON_LOG_ERROR,
            "unexpected VA mapping 0x%" PRIx64
            " cleanup failed: 0x%x; quarantining backing and address space",
            mapped_addr, R_VALUE(cleanup_rc));
         return NOUVEAU_HORIZON_ERROR_DEVICE_LOST;
      }
      rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
   }
   simple_mtx_unlock(&device->va_mutex);

   if (R_FAILED(rc)) {
      const uint64_t failures =
         nouveau_horizon_va_record_bind_failure(device);
      nouveau_horizon_memory_release_mapping(memory, (uint8_t)kind);
      simple_mtx_unlock(&va->mutex);
      if (nouveau_horizon_va_log_failure(failures)) {
         struct nouveau_horizon_device_debug_stats stats = {0};
         nouveau_horizon_device_get_debug_stats(device, &stats);
         nouveau_horizon_log(
            device, NOUVEAU_HORIZON_LOG_ERROR,
            "VA bind addr=0x%" PRIx64 ", range=0x%" PRIx64
            ", mem-offset=0x%" PRIx64 ", kind=%u failed: 0x%x "
            "(failure=%llu mem=%llu/%llu wrappers=%llu/%llu "
            "VA=%llu/%llu mappings=%llu/%llu)",
            target_addr, range_B, memory_offset_B, (unsigned)kind,
            R_VALUE(rc), (unsigned long long)failures,
            (unsigned long long)stats.native_memories_live,
            (unsigned long long)stats.native_memories_peak,
            (unsigned long long)stats.memory_wrappers_live,
            (unsigned long long)stats.memory_wrappers_peak,
            (unsigned long long)stats.vas_live,
            (unsigned long long)stats.vas_peak,
            (unsigned long long)stats.mappings_live,
            (unsigned long long)stats.mappings_peak);
      }
      FREE(mapping);
      return nouveau_horizon_status_from_result(
         rc, NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY);
   }

   mapping->addr = target_addr;
   mapping->range_B = range_B;
   mapping->memory = nouveau_horizon_memory_ref(memory);
   mapping->pte_kind = (uint8_t)kind;
   list_addtail(&mapping->link, &va->mappings);
   nouveau_horizon_va_record_mapping_added(device);
   simple_mtx_unlock(&va->mutex);
   return NOUVEAU_HORIZON_SUCCESS;
}

enum nouveau_horizon_status
nouveau_horizon_va_unbind(struct nouveau_horizon_va *va,
                          uint64_t va_offset_B,
                          uint64_t range_B)
{
   if (va == NULL ||
       !nouveau_horizon_range_valid(va_offset_B, range_B, va->size_B))
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   struct nouveau_horizon_device *device = va->device;
   const uint64_t addr = va->addr + va_offset_B;
   simple_mtx_lock(&va->mutex);

   list_for_each_entry_safe(struct nouveau_horizon_va_mapping, mapping,
                            &va->mappings, link) {
      if (mapping->addr != addr || mapping->range_B != range_B)
         continue;

      simple_mtx_lock(&device->va_mutex);
      const Result rc = nvAddressSpaceUnmap(&device->addr_space, addr);
      simple_mtx_unlock(&device->va_mutex);
      if (R_FAILED(rc)) {
         simple_mtx_unlock(&va->mutex);
         return nouveau_horizon_status_from_result(
            rc, NOUVEAU_HORIZON_ERROR_SYSTEM);
      }

      list_del(&mapping->link);
      nouveau_horizon_va_record_mapping_removed(device);
      struct nouveau_horizon_memory *memory = mapping->memory;
      const uint8_t pte_kind = mapping->pte_kind;
      FREE(mapping);
      simple_mtx_unlock(&va->mutex);
      nouveau_horizon_memory_release_mapping(memory, pte_kind);
      nouveau_horizon_memory_put(memory);
      return NOUVEAU_HORIZON_SUCCESS;
   }

   simple_mtx_unlock(&va->mutex);
   return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;
}
