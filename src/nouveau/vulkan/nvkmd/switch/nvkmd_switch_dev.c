/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nvkmd_switch.h"

#define MESA_LOG_TAG "nvkmd-switch"

#include "util/u_math.h"
#include "util/u_debug.h"
#include "util/log.h"
#include "util/os_time.h"
#include "vk_log.h"
#include "vk_sync.h"
#include "util/u_memory.h"
#include "util/stack_array.h"

#include <assert.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#define NVKMD_DBG(...) do { \
} while (0)

static bool
nvkmd_switch_file_toggle_enabled(const char *path)
{
   FILE *f = fopen(path, "r");
   if (f == NULL)
      return false;

   fclose(f);
   return true;
}

static bool
nvkmd_switch_prefetch_invalidate_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0) {
      enabled =
         debug_get_bool_option("NVK_SWITCH_PREFETCH_INVALIDATE", false) ? 1 : 0;
      if (!enabled &&
          nvkmd_switch_file_toggle_enabled(
             "sdmc:/nvk_switch_prefetch_invalidate.enable"))
         enabled = 1;
   }

   return enabled != 0;
}

static bool
nvkmd_switch_signal_payload_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0) {
      enabled =
         debug_get_bool_option("NVK_SWITCH_SIGNAL_PAYLOAD", false) ? 1 : 0;
      if (!enabled &&
          nvkmd_switch_file_toggle_enabled(
             "sdmc:/nvk_switch_signal_payload.enable"))
         enabled = 1;
   }

   return enabled != 0;
}

static bool
nvkmd_switch_skip_post_fence_flush(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled =
         debug_get_bool_option("NVK_SWITCH_SKIP_POST_FENCE_FLUSH", false) ? 1 : 0;

   return enabled != 0;
}

/* Small internal command ring for per-submit native wait packets. We keep
 * several fixed slices so back-to-back submissions can reuse the buffer
 * without overwriting commands the GPU hasn't consumed yet.
 */
#define NVKMD_SWITCH_WAIT_SLICE_COUNT 64
#define NVKMD_SWITCH_WAIT_SLICE_WORDS 128
#define NVKMD_SWITCH_WAIT_CMD_WORDS_PER_FENCE 3
#define NVKMD_SWITCH_SIGNAL_SLICE_COUNT 64
#define NVKMD_SWITCH_SIGNAL_SLICE_WORDS 128
#define NVKMD_SWITCH_SIGNAL_CMD_WORDS_PER_PAYLOAD 5
#define NVKMD_SWITCH_BUILTIN_CMDBUF_SIZE_B 0x20000
#define NVKMD_SWITCH_GPFIFO_FINAL_SKID_ENTRIES 4
#define NVKMD_SWITCH_MAX_MULTIFENCE_FENCES 4
#define NVKMD_SWITCH_SVC_INVALIDATE_PROCESS_DATA_CACHE 0x5d
#define NVKMD_SWITCH_MEM_OP_L2_FLUSH_DIRTY 0x80000000
#define NVKMD_SWITCH_MEM_OP_L2_SYSMEM_INVALIDATE 0x70000000
#define NVKMD_SWITCH_SEMAPHORE_RELEASE 0x01100002

struct nvkmd_switch_mem {
   struct nvkmd_mem base;

   NvMap map;
   void *cpu_addr;
   NvKind kind;
   bool gpu_cacheable;

   /* True if the CPU pages backing cpu_addr were switched to an uncached
    * mapping via svcSetMemoryAttribute(). Must be restored before free(). */
   bool cpu_uncached;
};

struct nvkmd_switch_va_mapping {
   struct list_head link;
   uint64_t addr;
   uint64_t range_B;
};

struct nvkmd_switch_va {
   struct nvkmd_va base;
   struct list_head mappings;
};

struct nvkmd_switch_ctx {
   struct nvkmd_ctx base;
   enum nvkmd_engines engines;

   bool uses_gpu_channel;
   bool pending_execs;
   bool has_last_fence;
   bool prefetch_invalidate_emitted;

   NvGpuChannel gpu_channel;
   NvFence last_fence;

   NvMap builtin_cmdbuf_map;
   void *builtin_cmdbuf_cpu;
   uint64_t builtin_cmdbuf_addr;
   uint32_t static_cmd_offset_words;
   uint32_t prefetch_invalidate_offset_words;
   uint32_t prefetch_invalidate_num_cmds;
   uint32_t fence_num_cmds;
   uint32_t flush_num_cmds;

   NvFence wait_slice_fences[NVKMD_SWITCH_WAIT_SLICE_COUNT];
   uint32_t next_wait_slice;
   int32_t pending_wait_slice;
   uint32_t pending_wait_words;
   bool pending_wait_emitted;

   NvFence signal_slice_fences[NVKMD_SWITCH_SIGNAL_SLICE_COUNT];
   uint32_t signal_slice_offset_words;
   uint32_t next_signal_slice;
   int32_t pending_signal_slice;

   NvMap zcull_ctx_map;
   void *zcull_ctx_cpu;
   uint64_t zcull_ctx_addr;
};

struct nvkmd_switch_signal_payload {
   struct vk_sync *sync;
   uint64_t addr;
   uint32_t value;
};

static struct nvkmd_switch_dev *
nvkmd_switch_dev(struct nvkmd_dev *dev)
{
   return container_of(dev, struct nvkmd_switch_dev, base);
}

static struct nvkmd_switch_mem *
nvkmd_switch_mem(struct nvkmd_mem *mem)
{
   return container_of(mem, struct nvkmd_switch_mem, base);
}

static struct nvkmd_switch_va *
nvkmd_switch_va(struct nvkmd_va *va)
{
   return container_of(va, struct nvkmd_switch_va, base);
}

static struct nvkmd_switch_ctx *
nvkmd_switch_ctx(struct nvkmd_ctx *ctx)
{
   return container_of(ctx, struct nvkmd_switch_ctx, base);
}

static struct vk_device *
nvkmd_switch_log_device(struct vk_object_base *log_obj)
{
   return log_obj ? log_obj->device : NULL;
}

static VkResult
nvkmd_switch_log_result(struct vk_object_base *log_obj, VkResult error)
{
   /* vk_error*() re-targets OODM/map failures to a device object. During
    * vkCreateDevice() bring-up we only have a physical-device log object, so
    * return an init failure instead of tripping runtime asserts in logging.
    */
   if (log_obj != NULL && log_obj->type == VK_OBJECT_TYPE_PHYSICAL_DEVICE &&
       (error == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
        error == VK_ERROR_MEMORY_MAP_FAILED))
      return VK_ERROR_INITIALIZATION_FAILED;

   return error;
}

static uint32_t
nvkmd_switch_bind_align_B(const struct nvkmd_switch_dev *dev)
{
   uint32_t bind_align_B =
      MAX2(dev->base.pdev->bind_align_B, NVKMD_SWITCH_BIND_ALIGN_B);
   const uint32_t page_size_B = dev->addr_space.page_size;

   if (util_is_power_of_two_nonzero64(page_size_B))
      bind_align_B = MAX2(bind_align_B, page_size_B);

   return bind_align_B;
}

static VkResult MUST_CHECK
nvkmd_switch_heap_alloc_locked(struct nvkmd_switch_dev *dev,
                               struct vk_object_base *log_obj,
                               enum nvkmd_va_flags flags,
                               uint64_t size_B, uint64_t align_B,
                               uint64_t fixed_addr, uint64_t *addr_out)
{
   if (flags & NVKMD_VA_ALLOC_FIXED) {
      if (!util_vma_heap_alloc_addr(&dev->heap, fixed_addr, size_B)) {
         return vk_errorf(log_obj, VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS,
                          "Switch VA collision at 0x%" PRIx64, fixed_addr);
      }

      *addr_out = fixed_addr;
   } else {
      *addr_out = util_vma_heap_alloc(&dev->heap, size_B, align_B);
      if (*addr_out == 0) {
         return vk_errorf(log_obj,
                          nvkmd_switch_log_result(log_obj,
                                                  VK_ERROR_OUT_OF_DEVICE_MEMORY),
                          "Failed to allocate Switch VA range");
      }
   }

   return VK_SUCCESS;
}

static VkResult MUST_CHECK
nvkmd_switch_heap_alloc(struct nvkmd_switch_dev *dev,
                        struct vk_object_base *log_obj,
                        enum nvkmd_va_flags flags,
                        uint64_t size_B, uint64_t align_B,
                        uint64_t fixed_addr, uint64_t *addr_out)
{
   simple_mtx_lock(&dev->heap_mutex);
   VkResult result = nvkmd_switch_heap_alloc_locked(dev, log_obj, flags,
                                                    size_B, align_B,
                                                    fixed_addr, addr_out);
   simple_mtx_unlock(&dev->heap_mutex);
   return result;
}

static void
nvkmd_switch_heap_free(struct nvkmd_switch_dev *dev,
                       uint64_t addr, uint64_t size_B)
{
   simple_mtx_lock(&dev->heap_mutex);
   util_vma_heap_free(&dev->heap, addr, size_B);
   simple_mtx_unlock(&dev->heap_mutex);
}

static uint64_t
nvkmd_switch_page_size_B(const struct nvkmd_switch_dev *dev)
{
   return dev->addr_space.page_size ? dev->addr_space.page_size : 0x1000u;
}

static uint32_t
nvkmd_switch_choose_as_page_size(const nvioctl_gpu_characteristics *gpu_info)
{
   const uint32_t available = gpu_info->available_big_page_sizes;

   if (available != 0) {
      /* libnx reports this as a bitmask of supported big-page sizes on GM20B.
       * Pick the smallest supported page so NVK's 64K arena chunks remain
       * bindable in a contiguous VA.
       */
      const uint32_t smallest = available & (~available + 1u);
      if (util_is_power_of_two_nonzero64(smallest))
         return smallest;
   }

   if (util_is_power_of_two_nonzero64(gpu_info->big_page_size))
      return gpu_info->big_page_size;

   return 0x10000u;
}

static bool
nvkmd_switch_va_region_range(const nvioctl_va_region *region,
                             uint64_t *start_out, uint64_t *end_out)
{
   if (region->page_size == 0 || region->pages == 0)
      return false;

   if (region->pages > UINT64_MAX / region->page_size)
      return false;

   const uint64_t size_B = region->pages * (uint64_t)region->page_size;
   if (region->offset > UINT64_MAX - size_B)
      return false;

   *start_out = region->offset;
   *end_out = region->offset + size_B;
   return true;
}

static bool
nvkmd_switch_va_in_range(uint64_t start, uint64_t end,
                         uint64_t addr, uint64_t size_B)
{
   return addr >= start && addr <= end && size_B <= end - addr;
}

static VkResult
nvkmd_switch_init_va_heap(struct nvkmd_switch_dev *dev,
                          struct vk_object_base *log_obj)
{
   nvioctl_va_region regions[2] = {0};
   Result rc = nvioctlNvhostAsGpu_GetVARegions(dev->addr_space.fd, regions);
   if (R_FAILED(rc)) {
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "nvkmd-switch: GetVARegions failed: 0x%x", rc);
   }

   const uint32_t page_size_B = nvkmd_switch_page_size_B(dev);
   uint64_t heap_start = 0, heap_end = 0;
   bool found = false;

   for (uint32_t i = 0; i < ARRAY_SIZE(regions); i++) {
      if (regions[i].page_size != page_size_B)
         continue;

      if (!nvkmd_switch_va_region_range(&regions[i], &heap_start, &heap_end))
         continue;

      found = true;
      break;
   }

   if (!found) {
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "nvkmd-switch: no VA region for page size 0x%x "
                       "(regions: {offset=0x%" PRIx64 ", page=0x%x, "
                       "pages=0x%" PRIx64 "}, {offset=0x%" PRIx64
                       ", page=0x%x, pages=0x%" PRIx64 "})",
                       page_size_B,
                       regions[0].offset, regions[0].page_size, regions[0].pages,
                       regions[1].offset, regions[1].page_size, regions[1].pages);
   }

   if (heap_start == 0)
      heap_start = page_size_B;

   if (heap_start >= heap_end) {
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "nvkmd-switch: VA region for page size 0x%x is too small "
                       "(range=[0x%" PRIx64 ", 0x%" PRIx64 "))",
                       page_size_B, heap_start, heap_end);
   }

   dev->base.va_start = heap_start;
   dev->base.va_end = heap_end;
   util_vma_heap_init(&dev->heap, heap_start, heap_end - heap_start);

   return VK_SUCCESS;
}

static uint64_t
nvkmd_switch_align_up(uint64_t value, uint64_t align)
{
   if (align == 0)
      return value;

   const uint64_t rem = value % align;
   if (rem == 0)
      return value;

   return value + (align - rem);
}

static uint32_t
nvkmd_switch_generate_fence_cmdlist(uint32_t *buf_start, uint32_t syncpt_id)
{
   uint32_t *cmd = buf_start;
   const uint32_t syncpt_flush_incr = syncpt_id | (1 << 20) | (1 << 16);

   /* Match deko3d's flush fence used by dkQueuePresentImage(): the
    * compositor-facing fence is only complete after two flush+increment
    * SyncptAction packets, and libnx's channel fence value must advance
    * by the same amount.
    */
   *cmd++ = 0x451 | (0 << 13) | (0 << 16) | (4 << 29);
   *cmd++ = 0x0B2 | (0 << 13) | (1 << 16) | (1 << 29);
   *cmd++ = syncpt_flush_incr;
   *cmd++ = 0x0B2 | (0 << 13) | (1 << 16) | (1 << 29);
   *cmd++ = syncpt_flush_incr;
   return cmd - buf_start;
}

static uint32_t
nvkmd_switch_generate_prefetch_invalidate_cmdlist(uint32_t *buf_start)
{
   uint32_t *cmd = buf_start;

   /* Public deko3d uses a post-flush cache-invalidate command list followed
    * by a sync/no-prefetch GPFIFO split.  Do the same kind of ordering before
    * later command-buffer segments can be fetched from memory that may have
    * stale L2 lines from earlier reuse.
    */
   *cmd++ = 0x00B | (6 << 13) | (1 << 16) | (1 << 29);
   *cmd++ = NVKMD_SWITCH_MEM_OP_L2_SYSMEM_INVALIDATE;

   return cmd - buf_start;
}

static uint32_t
nvkmd_switch_generate_flush_cmdlist(uint32_t *buf_start)
{
   uint32_t *cmd = buf_start;
   *cmd++ = 0x00B | (6 << 13) | (1 << 16) | (1 << 29);
   *cmd++ = NVKMD_SWITCH_MEM_OP_L2_FLUSH_DIRTY;
   *cmd++ = 0x00B | (6 << 13) | (1 << 16) | (1 << 29);
   *cmd++ = NVKMD_SWITCH_MEM_OP_L2_SYSMEM_INVALIDATE;
   *cmd++ = 0x4A2 | (0 << 13) | (0 << 16) | (4 << 29);
   *cmd++ = 0x369 | (0 << 13) | (0x1011 << 16) | (4 << 29);
   *cmd++ = 0x50A | (0 << 13) | (0 << 16) | (4 << 29);
   *cmd++ = 0x509 | (0 << 13) | (0 << 16) | (4 << 29);
   *cmd = 0;
   return cmd - buf_start;
}

static void
nvkmd_switch_reset_nvfence(NvFence *fence)
{
   fence->id = UINT32_MAX;
   fence->value = 0;
}

static uint32_t
nvkmd_switch_generate_wait_cmdlist(uint32_t *buf_start,
                                   uint32_t wait_count,
                                   const NvFence *waits)
{
   uint32_t *cmd = buf_start;

   for (uint32_t i = 0; i < wait_count; i++) {
      assert((int32_t)waits[i].id >= 0);

      /* Matches deko3d's GPFIFO SyncpointPayload/Syncpoint wait packet:
       * payload=value, then wait-on-switch syncpoint metadata.
       */
      *cmd++ = 0x01C | (0 << 13) | (2 << 16) | (1 << 29);
      *cmd++ = waits[i].value;
      *cmd++ = (1 << 4) | ((uint32_t)waits[i].id << 8);
   }

   return cmd - buf_start;
}

static void
nvkmd_switch_ctx_reset_fence(struct nvkmd_switch_ctx *ctx)
{
   nvkmd_switch_reset_nvfence(&ctx->last_fence);
   ctx->has_last_fence = false;
}

static uint32_t *
nvkmd_switch_ctx_wait_slice_cpu(struct nvkmd_switch_ctx *ctx,
                                uint32_t slice)
{
   return (uint32_t *)ctx->builtin_cmdbuf_cpu +
          slice * NVKMD_SWITCH_WAIT_SLICE_WORDS;
}

static uint64_t
nvkmd_switch_ctx_wait_slice_addr(struct nvkmd_switch_ctx *ctx,
                                 uint32_t slice)
{
   return ctx->builtin_cmdbuf_addr +
          (uint64_t)slice * NVKMD_SWITCH_WAIT_SLICE_WORDS * sizeof(uint32_t);
}

static uint32_t *
nvkmd_switch_ctx_signal_slice_cpu(struct nvkmd_switch_ctx *ctx,
                                  uint32_t slice)
{
   return (uint32_t *)ctx->builtin_cmdbuf_cpu +
          ctx->signal_slice_offset_words +
          slice * NVKMD_SWITCH_SIGNAL_SLICE_WORDS;
}

static uint64_t
nvkmd_switch_ctx_signal_slice_addr(struct nvkmd_switch_ctx *ctx,
                                   uint32_t slice)
{
   return ctx->builtin_cmdbuf_addr +
          (uint64_t)(ctx->signal_slice_offset_words +
                     slice * NVKMD_SWITCH_SIGNAL_SLICE_WORDS) *
          sizeof(uint32_t);
}

static VkResult
nvkmd_switch_ctx_wait_slice_ready(struct nvkmd_switch_ctx *ctx,
                                  struct vk_object_base *log_obj,
                                  uint32_t slice)
{
   NvFence *fence = &ctx->wait_slice_fences[slice];

   if ((int32_t)fence->id < 0)
      return VK_SUCCESS;

   Result rc = nvFenceWait(fence, -1);
   if (R_FAILED(rc)) {
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: wait-slice fence wait failed: 0x%x",
                       rc);
   }

   nvkmd_switch_reset_nvfence(fence);
   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_ctx_signal_slice_ready(struct nvkmd_switch_ctx *ctx,
                                    struct vk_object_base *log_obj,
                                    uint32_t slice)
{
   NvFence *fence = &ctx->signal_slice_fences[slice];

   if ((int32_t)fence->id < 0)
      return VK_SUCCESS;

   Result rc = nvFenceWait(fence, -1);
   if (R_FAILED(rc)) {
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: signal-slice fence wait failed: 0x%x",
                       rc);
   }

   nvkmd_switch_reset_nvfence(fence);
   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_ctx_reserve_wait_slice(struct nvkmd_switch_ctx *ctx,
                                    struct vk_object_base *log_obj)
{
   if (ctx->pending_wait_slice >= 0)
      return VK_SUCCESS;

   const uint32_t slice = ctx->next_wait_slice;
   ctx->next_wait_slice = (ctx->next_wait_slice + 1) %
                          NVKMD_SWITCH_WAIT_SLICE_COUNT;

   VkResult result = nvkmd_switch_ctx_wait_slice_ready(ctx, log_obj, slice);
   if (result != VK_SUCCESS)
      return result;

   ctx->pending_wait_slice = (int32_t)slice;
   ctx->pending_wait_words = 0;
   ctx->pending_wait_emitted = false;
   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_ctx_reserve_signal_slice(struct nvkmd_switch_ctx *ctx,
                                      struct vk_object_base *log_obj,
                                      uint32_t *slice_out)
{
   assert(ctx->pending_signal_slice < 0);

   const uint32_t slice = ctx->next_signal_slice;
   ctx->next_signal_slice = (ctx->next_signal_slice + 1) %
                            NVKMD_SWITCH_SIGNAL_SLICE_COUNT;

   VkResult result = nvkmd_switch_ctx_signal_slice_ready(ctx, log_obj, slice);
   if (result != VK_SUCCESS)
      return result;

   ctx->pending_signal_slice = (int32_t)slice;
   *slice_out = slice;
   return VK_SUCCESS;
}

static uint32_t
nvkmd_switch_generate_semaphore_release_cmdlist(
   uint32_t *buf_start,
   uint32_t syncpt_id,
   const struct nvkmd_switch_signal_payload *payloads,
   uint32_t payload_count)
{
   uint32_t *cmd = buf_start;

   cmd += nvkmd_switch_generate_fence_cmdlist(cmd, syncpt_id);

   for (uint32_t i = 0; i < payload_count; i++) {
      const uint64_t addr = payloads[i].addr;

      *cmd++ = 0x004 | (6 << 13) | (4 << 16) | (1 << 29);
      *cmd++ = addr >> 32;
      *cmd++ = addr;
      *cmd++ = payloads[i].value;
      *cmd++ = NVKMD_SWITCH_SEMAPHORE_RELEASE;
   }

   return cmd - buf_start;
}

static VkResult
nvkmd_switch_ctx_intermediate_kickoff(struct nvkmd_switch_ctx *ctx,
                                      struct vk_object_base *log_obj,
                                      const char *reason)
{
   if (ctx->gpu_channel.num_entries == 0)
      return VK_SUCCESS;

   Result rc = nvGpuChannelKickoff(&ctx->gpu_channel);
   if (R_FAILED(rc)) {
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: intermediate kickoff failed while %s: "
                       "0x%x (entries=%u)", reason, rc,
                       ctx->gpu_channel.num_entries);
   }

   /* A kickoff ends the current prefetch window.  Emit the public deko3d-style
    * invalidate/no-prefetch split again before the next executable entry. */
   ctx->prefetch_invalidate_emitted = false;
   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_ctx_ensure_gpfifo_space(struct nvkmd_switch_ctx *ctx,
                                     struct vk_object_base *log_obj,
                                     uint32_t entries_needed,
                                     uint32_t skid_entries,
                                     bool allow_flush,
                                     const char *reason)
{
   assert(entries_needed > 0);

   if (entries_needed + skid_entries > GPFIFO_QUEUE_SIZE) {
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: GPFIFO reservation too large while %s "
                       "(need=%u, skid=%u, queue=%u)", reason,
                       entries_needed, skid_entries, GPFIFO_QUEUE_SIZE);
   }

   if (ctx->gpu_channel.num_entries + entries_needed + skid_entries <=
       GPFIFO_QUEUE_SIZE)
      return VK_SUCCESS;

   if (!allow_flush) {
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: GPFIFO skid exhausted while %s "
                       "(entries=%u, need=%u, skid=%u, queue=%u)", reason,
                       ctx->gpu_channel.num_entries, entries_needed,
                       skid_entries, GPFIFO_QUEUE_SIZE);
   }

   VkResult result =
      nvkmd_switch_ctx_intermediate_kickoff(ctx, log_obj, reason);
   if (result != VK_SUCCESS)
      return result;

   if (ctx->gpu_channel.num_entries + entries_needed + skid_entries >
       GPFIFO_QUEUE_SIZE) {
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: GPFIFO still full after kickoff while %s "
                       "(entries=%u, need=%u, skid=%u, queue=%u)", reason,
                       ctx->gpu_channel.num_entries, entries_needed,
                       skid_entries, GPFIFO_QUEUE_SIZE);
   }

   return VK_SUCCESS;
}

/* Append a GPFIFO entry into libnx's userspace queue-control memory.  The
 * caller tells us whether a pre-append kickoff is legal and how many entries
 * must remain after the append.  This mirrors the public deko3d queue shape:
 * accumulate entries in control memory, keep skid space for mandatory tail
 * commands, and only kick when queue pressure makes that safe and necessary.
 */
static VkResult
nvkmd_switch_ctx_append_entry(struct nvkmd_switch_ctx *ctx,
                              struct vk_object_base *log_obj,
                              uint64_t addr, uint32_t num_cmds,
                              uint32_t flags, bool allow_flush,
                              uint32_t skid_entries,
                              const char *reason)
{
   VkResult result =
      nvkmd_switch_ctx_ensure_gpfifo_space(ctx, log_obj, 1, skid_entries,
                                           allow_flush, reason);
   if (result != VK_SUCCESS)
      return result;

   Result rc = nvGpuChannelAppendEntry(&ctx->gpu_channel, addr, num_cmds,
                                       flags, 0);
   if (R_SUCCEEDED(rc))
      return VK_SUCCESS;

   if (!allow_flush) {
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: GPFIFO append failed while %s: 0x%x",
                       reason, rc);
   }

   result = nvkmd_switch_ctx_intermediate_kickoff(ctx, log_obj, reason);
   if (result != VK_SUCCESS)
      return result;

   result =
      nvkmd_switch_ctx_ensure_gpfifo_space(ctx, log_obj, 1, skid_entries,
                                           false, reason);
   if (result != VK_SUCCESS)
      return result;

   rc = nvGpuChannelAppendEntry(&ctx->gpu_channel, addr, num_cmds, flags, 0);
   if (R_FAILED(rc)) {
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: GPFIFO append failed after kickoff "
                       "while %s: 0x%x", reason, rc);
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_ctx_emit_prefetch_invalidate(struct nvkmd_switch_ctx *ctx,
                                          struct vk_object_base *log_obj,
                                          uint32_t skid_entries)
{
   if (!nvkmd_switch_prefetch_invalidate_enabled() ||
       ctx->prefetch_invalidate_emitted ||
       ctx->prefetch_invalidate_num_cmds == 0)
      return VK_SUCCESS;

   VkResult result =
      nvkmd_switch_ctx_append_entry(ctx, log_obj,
                                    ctx->builtin_cmdbuf_addr +
                                    4ull * ctx->prefetch_invalidate_offset_words,
                                    ctx->prefetch_invalidate_num_cmds,
                                    GPFIFO_ENTRY_NOT_MAIN |
                                    GPFIFO_ENTRY_NO_PREFETCH,
                                    false, skid_entries,
                                    "emitting prefetch invalidate");
   if (result != VK_SUCCESS)
      return result;

   ctx->prefetch_invalidate_emitted = true;
   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_ctx_append_exec_entry(struct nvkmd_switch_ctx *ctx,
                                   struct vk_object_base *log_obj,
                                   uint64_t addr, uint32_t num_cmds,
                                   uint32_t flags, bool allow_flush,
                                   uint32_t reserve_after_exec)
{
   const bool emit_prefetch =
      nvkmd_switch_prefetch_invalidate_enabled() &&
      !ctx->prefetch_invalidate_emitted &&
      ctx->prefetch_invalidate_num_cmds > 0;
   const uint32_t entries_needed = 1 + (emit_prefetch ? 1 : 0);
   const uint32_t tail_skid =
      NVKMD_SWITCH_GPFIFO_FINAL_SKID_ENTRIES + reserve_after_exec;

   VkResult result =
      nvkmd_switch_ctx_ensure_gpfifo_space(ctx, log_obj, entries_needed,
                                           tail_skid, allow_flush,
                                           "appending exec entry");
   if (result != VK_SUCCESS)
      return result;

   result =
      nvkmd_switch_ctx_emit_prefetch_invalidate(ctx, log_obj,
                                                tail_skid + 1);
   if (result != VK_SUCCESS)
      return result;

   return nvkmd_switch_ctx_append_entry(ctx, log_obj, addr, num_cmds, flags,
                                        false, tail_skid,
                                        "appending exec entry");
}

static VkResult
nvkmd_switch_ctx_emit_pending_waits(struct nvkmd_switch_ctx *ctx,
                                    struct vk_object_base *log_obj)
{
   if (ctx->pending_wait_slice < 0 ||
       ctx->pending_wait_words == 0 ||
       ctx->pending_wait_emitted)
      return VK_SUCCESS;

   uint32_t *slice_cpu =
      nvkmd_switch_ctx_wait_slice_cpu(ctx, ctx->pending_wait_slice);
   armDCacheFlush(slice_cpu, ctx->pending_wait_words * sizeof(uint32_t));

   VkResult result =
      nvkmd_switch_ctx_append_entry(ctx, log_obj,
                                    nvkmd_switch_ctx_wait_slice_addr(
                                       ctx, ctx->pending_wait_slice),
                                    ctx->pending_wait_words,
                                    GPFIFO_ENTRY_NOT_MAIN |
                                    GPFIFO_ENTRY_NO_PREFETCH,
                                    true,
                                    NVKMD_SWITCH_GPFIFO_FINAL_SKID_ENTRIES,
                                    "emitting native wait packet");
   if (result != VK_SUCCESS)
      return result;

   ctx->pending_wait_emitted = true;
   ctx->pending_execs = true;
   return VK_SUCCESS;
}

static void
nvkmd_switch_ctx_free_internal_map(struct nvkmd_switch_dev *dev,
                                   NvMap *map,
                                   void **cpu_addr,
                                   uint64_t *gpu_addr)
{
   if (*gpu_addr != 0)
      nvAddressSpaceUnmap(&dev->addr_space, *gpu_addr);
   if (nvMapGetHandle(map) != 0)
      nvMapClose(map);
   free(*cpu_addr);

   memset(map, 0, sizeof(*map));
   *cpu_addr = NULL;
   *gpu_addr = 0;
}

static VkResult
nvkmd_switch_ctx_alloc_internal_map(struct nvkmd_switch_dev *dev,
                                    struct vk_object_base *log_obj,
                                    uint64_t size_B, uint64_t align_B,
                                    NvKind kind,
                                    NvMap *map_out,
                                    void **cpu_addr_out,
                                    uint64_t *gpu_addr_out)
{
   void *cpu_addr = NULL;
   Result rc;

   if (align_B < 0x1000)
      align_B = 0x1000;
   size_B = nvkmd_switch_align_up(size_B, 0x1000);

   cpu_addr = memalign(0x1000, size_B);
   if (cpu_addr == NULL) {
      return vk_errorf(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY,
                       "nvkmd-switch: failed to allocate internal BO");
   }

   memset(cpu_addr, 0, size_B);

   rc = nvMapCreate(map_out, cpu_addr, size_B, align_B, kind, false);
   if (R_FAILED(rc)) {
      free(cpu_addr);
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "nvkmd-switch: nvMapCreate failed: 0x%x", rc);
   }

   rc = nvAddressSpaceMap(&dev->addr_space, nvMapGetHandle(map_out),
                          true /* not coherent */, kind, gpu_addr_out);
   if (R_FAILED(rc)) {
      nvMapClose(map_out);
      free(cpu_addr);
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "nvkmd-switch: nvAddressSpaceMap failed: 0x%x", rc);
   }

   *cpu_addr_out = cpu_addr;
   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_ctx_wait_last_fence(struct nvkmd_switch_ctx *ctx,
                                 struct vk_object_base *log_obj)
{
   if (!ctx->has_last_fence)
      return VK_SUCCESS;

   Result rc = nvFenceWait(&ctx->last_fence, -1);
   if (R_FAILED(rc)) {
      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: nvFenceWait failed: 0x%x", rc);
   }

   nvkmd_switch_ctx_reset_fence(ctx);
   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_ctx_kickoff(struct nvkmd_switch_ctx *ctx,
                         struct vk_object_base *log_obj,
                         const struct nvkmd_switch_signal_payload *payloads,
                         uint32_t payload_count)
{
   VkResult result = nvkmd_switch_ctx_emit_pending_waits(ctx, log_obj);
   if (result != VK_SUCCESS)
      return result;

   if (!ctx->uses_gpu_channel || !ctx->pending_execs)
      return VK_SUCCESS;

   /* GPU L2 cache flush after rendering commands, before the fence: ensures
    * GPU writes are in main RAM when the syncpoint fires.  Push data entries
    * were already appended by nvkmd_switch_ctx_exec().
    * Order in GPFIFO: [push data...] [flush] [fence increment].
    *
    * We keep enough GPFIFO skid space for this tail while appending execs.
    * IncrFence is deliberately deferred until after every tail entry is
    * appended, so even a safe pre-tail kickoff can never land between fence
    * bookkeeping and the fence cmdlist.
    */
   const bool append_flush =
      ctx->flush_num_cmds > 0 && !nvkmd_switch_skip_post_fence_flush();
   if (append_flush) {
      result = nvkmd_switch_ctx_append_entry(ctx, log_obj,
                                             ctx->builtin_cmdbuf_addr +
                                             4ull * (ctx->static_cmd_offset_words +
                                                     ctx->fence_num_cmds),
                                             ctx->flush_num_cmds,
                                             GPFIFO_ENTRY_NOT_MAIN,
                                             true, 1,
                                             "appending post-submit flush");
      if (result != VK_SUCCESS)
         return result;
   }

   if (payload_count > 0) {
      uint32_t slice;
      result = nvkmd_switch_ctx_reserve_signal_slice(ctx, log_obj, &slice);
      if (result != VK_SUCCESS)
         return result;

      uint32_t *slice_cpu = nvkmd_switch_ctx_signal_slice_cpu(ctx, slice);
      const uint32_t signal_words =
         nvkmd_switch_generate_semaphore_release_cmdlist(
            slice_cpu,
            nvGpuChannelGetSyncpointId(&ctx->gpu_channel),
            payloads, payload_count);
      assert(signal_words <= NVKMD_SWITCH_SIGNAL_SLICE_WORDS);
      armDCacheFlush(slice_cpu, signal_words * sizeof(uint32_t));

      result = nvkmd_switch_ctx_append_entry(ctx, log_obj,
                                             nvkmd_switch_ctx_signal_slice_addr(
                                                ctx, slice),
                                             signal_words,
                                             GPFIFO_ENTRY_NOT_MAIN |
                                             GPFIFO_ENTRY_NO_PREFETCH,
                                             true, 0,
                                             "appending fence/payload signal");
      if (result != VK_SUCCESS)
         return result;
   } else {
      result = nvkmd_switch_ctx_append_entry(ctx, log_obj,
                                             ctx->builtin_cmdbuf_addr +
                                             4ull * ctx->static_cmd_offset_words,
                                             ctx->fence_num_cmds,
                                             GPFIFO_ENTRY_NOT_MAIN |
                                             GPFIFO_ENTRY_NO_PREFETCH,
                                             true, 0,
                                             "appending fence signal");
      if (result != VK_SUCCESS)
         return result;
   }

   nvGpuChannelIncrFence(&ctx->gpu_channel);
   nvGpuChannelIncrFence(&ctx->gpu_channel);

   NVKMD_DBG("[nvkmd-dbg] kickoff: entries=%u, "
           "builtin_cmdbuf_addr=0x%" PRIx64 ", "
           "fence_cmds=%u, flush_cmds=%u\n",
           ctx->gpu_channel.num_entries,
           (uint64_t)ctx->builtin_cmdbuf_addr,
           ctx->fence_num_cmds, ctx->flush_num_cmds);

   Result rc = nvGpuChannelKickoff(&ctx->gpu_channel);
   if (R_FAILED(rc)) {
      NvError error_info = {0};
      Result err_rc = nvGpuChannelGetErrorInfo(&ctx->gpu_channel, &error_info);

      if (R_SUCCEEDED(err_rc)) {
         return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                          "nvkmd-switch: nvGpuChannelKickoff failed: 0x%x "
                          "(entries=%u, error type=%u, info=[%u,%u,%u,%u])",
                          rc, ctx->gpu_channel.num_entries,
                          error_info.type, error_info.info[0],
                          error_info.info[1], error_info.info[2],
                          error_info.info[3]);
      }

      return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                       "nvkmd-switch: nvGpuChannelKickoff failed: 0x%x "
                       "(entries=%u, error-info rc=0x%x)",
                       rc, ctx->gpu_channel.num_entries, err_rc);
   }

   NVKMD_DBG("[nvkmd-dbg] kickoff OK\n");

   nvGpuChannelGetFence(&ctx->gpu_channel, &ctx->last_fence);
   ctx->has_last_fence = (int32_t)ctx->last_fence.id >= 0;
   ctx->prefetch_invalidate_emitted = false;

   if (ctx->pending_wait_slice >= 0) {
      if (ctx->has_last_fence)
         ctx->wait_slice_fences[ctx->pending_wait_slice] = ctx->last_fence;
      else
         nvkmd_switch_reset_nvfence(
            &ctx->wait_slice_fences[ctx->pending_wait_slice]);

      ctx->pending_wait_slice = -1;
      ctx->pending_wait_words = 0;
      ctx->pending_wait_emitted = false;
   }

   if (ctx->pending_signal_slice >= 0) {
      if (ctx->has_last_fence)
         ctx->signal_slice_fences[ctx->pending_signal_slice] = ctx->last_fence;
      else
         nvkmd_switch_reset_nvfence(
            &ctx->signal_slice_fences[ctx->pending_signal_slice]);

      ctx->pending_signal_slice = -1;
   }

   ctx->pending_execs = false;
   return VK_SUCCESS;
}

static void
nvkmd_switch_free_services(struct nvkmd_switch_dev *dev)
{
   nvAddressSpaceClose(&dev->addr_space);
   nvGpuExit();
   nvMapExit();
   nvFenceExit();
   nvExit();
}

static void
nvkmd_switch_ctx_destroy(struct nvkmd_ctx *_ctx)
{
   struct nvkmd_switch_ctx *ctx = nvkmd_switch_ctx(_ctx);
   struct nvkmd_switch_dev *dev = nvkmd_switch_dev(_ctx->dev);

   if (ctx->uses_gpu_channel) {
      if (ctx->has_last_fence)
         nvFenceWait(&ctx->last_fence, -1);
      nvGpuChannelClose(&ctx->gpu_channel);
      nvkmd_switch_ctx_free_internal_map(dev, &ctx->zcull_ctx_map,
                                         &ctx->zcull_ctx_cpu,
                                         &ctx->zcull_ctx_addr);
      nvkmd_switch_ctx_free_internal_map(dev, &ctx->builtin_cmdbuf_map,
                                         &ctx->builtin_cmdbuf_cpu,
                                         &ctx->builtin_cmdbuf_addr);
   }

   FREE(ctx);
}

static VkResult
nvkmd_switch_ctx_wait(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj,
                      uint32_t wait_count,
                      const struct vk_sync_wait *waits)
{
   struct nvkmd_switch_ctx *ctx = nvkmd_switch_ctx(_ctx);
   struct vk_device *device = nvkmd_switch_log_device(log_obj);
   if (device == NULL) {
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "nvkmd-switch: missing vk_device for context wait");
   }

   if (!ctx->uses_gpu_channel || wait_count == 0) {
      return vk_sync_wait_many(device, wait_count, waits,
                               0 /* wait_flags */, UINT64_MAX);
   }

   STACK_ARRAY(struct vk_sync_wait, host_waits, wait_count);
   STACK_ARRAY(struct vk_sync_wait, native_waits, wait_count);
   STACK_ARRAY(NvFence, native_fences,
               wait_count * NVKMD_SWITCH_MAX_MULTIFENCE_FENCES);
   uint32_t host_wait_count = 0;
   uint32_t native_wait_count = 0;
   uint32_t native_fence_count = 0;

   for (uint32_t i = 0; i < wait_count; i++) {
      NvMultiFence fence;

      if (waits[i].sync != NULL &&
          nvkmd_switch_sync_peek_nvmultifence(waits[i].sync, &fence)) {
         native_waits[native_wait_count] = waits[i];
         native_wait_count++;

         const uint32_t fence_count =
            MIN2(fence.num_fences, NVKMD_SWITCH_MAX_MULTIFENCE_FENCES);
         for (uint32_t j = 0; j < fence_count; j++) {
            if ((int32_t)fence.fences[j].id < 0)
               continue;

            native_fences[native_fence_count++] = fence.fences[j];
         }
      } else {
         host_waits[host_wait_count++] = waits[i];
      }
   }

   VkResult result = VK_SUCCESS;

   if (host_wait_count > 0) {
      result = vk_sync_wait_many(device, host_wait_count, host_waits,
                                 0 /* wait_flags */, UINT64_MAX);
      if (result != VK_SUCCESS)
         goto done;
   }

   if (native_wait_count == 0)
      goto done;

   if (ctx->pending_wait_emitted) {
      result = vk_sync_wait_many(device, native_wait_count, native_waits,
                                 0 /* wait_flags */, UINT64_MAX);
      goto done;
   }

   const uint32_t native_wait_words =
      native_fence_count * NVKMD_SWITCH_WAIT_CMD_WORDS_PER_FENCE;
   const uint32_t remaining_words =
      NVKMD_SWITCH_WAIT_SLICE_WORDS - ctx->pending_wait_words;

   if (native_wait_words > remaining_words) {
      result = vk_sync_wait_many(device, native_wait_count, native_waits,
                                 0 /* wait_flags */, UINT64_MAX);
      goto done;
   }

   result = nvkmd_switch_ctx_reserve_wait_slice(ctx, log_obj);
   if (result != VK_SUCCESS)
      goto done;

   ctx->pending_wait_words +=
      nvkmd_switch_generate_wait_cmdlist(
         nvkmd_switch_ctx_wait_slice_cpu(ctx, ctx->pending_wait_slice) +
         ctx->pending_wait_words,
         native_fence_count, native_fences);

done:
   STACK_ARRAY_FINISH(native_fences);
   STACK_ARRAY_FINISH(native_waits);
   STACK_ARRAY_FINISH(host_waits);
   return result;
}

static VkResult
nvkmd_switch_ctx_exec(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj,
                      uint32_t exec_count,
                      const struct nvkmd_ctx_exec *execs)
{
   struct nvkmd_switch_ctx *ctx = nvkmd_switch_ctx(_ctx);
   VkResult result = nvkmd_switch_ctx_emit_pending_waits(ctx, log_obj);
   if (result != VK_SUCCESS)
      return result;

   bool continuation_pending = false;

   for (uint32_t i = 0; i < exec_count; i++) {
      if ((execs[i].addr & 3) != 0 || (execs[i].size_B & 3) != 0) {
         return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                          "nvkmd-switch: unaligned push buffer");
      }

      if (!ctx->uses_gpu_channel)
         continue;

      NVKMD_DBG("[nvkmd-dbg] exec[%u/%u]: addr=0x%" PRIx64 ", "
              "size=0x%" PRIx64 ", no_prefetch=%d\n",
              i, exec_count, (uint64_t)execs[i].addr,
              (uint64_t)execs[i].size_B, execs[i].no_prefetch);

      uint32_t flags = GPFIFO_ENTRY_NOT_MAIN;
      if (execs[i].no_prefetch)
         flags |= GPFIFO_ENTRY_NO_PREFETCH;

      if (execs[i].incomplete && i + 1 == exec_count) {
         return vk_errorf(log_obj, VK_ERROR_DEVICE_LOST,
                          "nvkmd-switch: incomplete push buffer without "
                          "continuation");
      }

      /* An incomplete method may span two adjacent GPFIFO entries.  Once the
       * first half is appended, we must not kickoff until at least the next
       * entry is also queued.  Reserve that continuation entry up front; if
       * the ring is tight, kick before appending the incomplete entry. */
      result = nvkmd_switch_ctx_append_exec_entry(ctx, log_obj,
                                                  execs[i].addr,
                                                  execs[i].size_B / 4,
                                                  flags,
                                                  !continuation_pending,
                                                  execs[i].incomplete ? 1 : 0);
      if (result != VK_SUCCESS)
         return result;

      continuation_pending = execs[i].incomplete;
      ctx->pending_execs = true;
   }

   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_ctx_bind(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj,
                      uint32_t bind_count,
                      const struct nvkmd_ctx_bind *binds)
{
   return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                    "nvkmd-switch: context bind not implemented yet");
}

static VkResult
nvkmd_switch_ctx_signal(struct nvkmd_ctx *_ctx,
                        struct vk_object_base *log_obj,
                        uint32_t signal_count,
                        const struct vk_sync_signal *signals)
{
   struct nvkmd_switch_ctx *ctx = nvkmd_switch_ctx(_ctx);
   struct vk_device *device = nvkmd_switch_log_device(log_obj);
   if (device == NULL) {
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "nvkmd-switch: missing vk_device for context signal");
   }

   const uint32_t max_payloads =
      ctx->fence_num_cmds < NVKMD_SWITCH_SIGNAL_SLICE_WORDS ?
      (NVKMD_SWITCH_SIGNAL_SLICE_WORDS - ctx->fence_num_cmds) /
         NVKMD_SWITCH_SIGNAL_CMD_WORDS_PER_PAYLOAD : 0;
   STACK_ARRAY(struct nvkmd_switch_signal_payload, payloads,
               MIN2(signal_count, max_payloads));
   uint32_t payload_count = 0;
   const bool use_payloads = nvkmd_switch_signal_payload_enabled();

   if (use_payloads &&
       ctx->uses_gpu_channel && ctx->pending_execs && max_payloads > 0) {
      for (uint32_t i = 0; i < signal_count && payload_count < max_payloads;
           i++) {
         struct vk_sync *sync = signals[i].sync;

         if (sync == NULL || !nvkmd_switch_sync_is_nvfence(sync->type))
            continue;

         struct nvkmd_switch_signal_payload *payload =
            &payloads[payload_count];
         if (nvkmd_switch_sync_prepare_payload_signal(sync, &payload->addr,
                                                      &payload->value)) {
            payload->sync = sync;
            payload_count++;
         }
      }
   }

   VkResult result =
      nvkmd_switch_ctx_kickoff(ctx, log_obj, payloads, payload_count);
   if (result != VK_SUCCESS)
      goto done;

   /* Attach the just-kicked-off fence to every native binary signal. That
    * makes downstream CPU waits (vkWaitForFences) and WSI present handoff
    * block on real GPU completion instead of a host condvar broadcast —
    * single-channel submits no longer need the glFinish that used to live
    * here. Non-native sync objects (timeline wrappers, dummy syncs) still
    * fall through to vk_sync_signal_many below so their runtime-specific
    * bookkeeping runs. */
   if (ctx->has_last_fence) {
      for (uint32_t i = 0; i < signal_count; i++) {
         struct vk_sync *sync = signals[i].sync;
         if (sync == NULL || !nvkmd_switch_sync_is_nvfence(sync->type))
            continue;

         bool imported_payload = false;
         for (uint32_t j = 0; j < payload_count; j++) {
            if (payloads[j].sync == sync) {
               nvkmd_switch_sync_import_nvfence_payload(sync,
                                                        &ctx->last_fence,
                                                        payloads[j].value);
               imported_payload = true;
               break;
            }
         }

         if (!imported_payload)
            nvkmd_switch_sync_import_nvfence(sync, &ctx->last_fence);
      }
   }

   result = vk_sync_signal_many(device, signal_count, signals);

done:
   STACK_ARRAY_FINISH(payloads);
   return result;
}

static VkResult
nvkmd_switch_ctx_flush(struct nvkmd_ctx *_ctx,
                       struct vk_object_base *log_obj)
{
   struct nvkmd_switch_ctx *ctx = nvkmd_switch_ctx(_ctx);

   /* Flush is asynchronous: kick the GPU off and return. Any caller that
    * needs to observe completion must either wait on a signaled sync or
    * call ctx_sync() explicitly. */
   return nvkmd_switch_ctx_kickoff(ctx, log_obj, NULL, 0);
}

static VkResult
nvkmd_switch_ctx_sync(struct nvkmd_ctx *_ctx,
                      struct vk_object_base *log_obj)
{
   struct nvkmd_switch_ctx *ctx = nvkmd_switch_ctx(_ctx);

   /* Sync is the vkDeviceWaitIdle primitive: kick off, then block on the
    * resulting fence. This is the only remaining intentional CPU stall in
    * the submit path. */
   VkResult result = nvkmd_switch_ctx_kickoff(ctx, log_obj, NULL, 0);
   if (result != VK_SUCCESS)
      return result;

   return nvkmd_switch_ctx_wait_last_fence(ctx, log_obj);
}

static const struct nvkmd_ctx_ops nvkmd_switch_ctx_ops = {
   .destroy = nvkmd_switch_ctx_destroy,
   .wait = nvkmd_switch_ctx_wait,
   .exec = nvkmd_switch_ctx_exec,
   .bind = nvkmd_switch_ctx_bind,
   .signal = nvkmd_switch_ctx_signal,
   .flush = nvkmd_switch_ctx_flush,
   .sync = nvkmd_switch_ctx_sync,
};

/* Horizon backs malloc()/memalign() with ordinary cacheable heap pages.  A
 * VkDeviceMemory advertised to the app as HOST_COHERENT must really be
 * coherent: the common nvkmd layer skips every CPU cache maintenance call
 * for NVKMD_MEM_COHERENT memory (see nvkmd_mem_sync_to_gpu/_from_gpu).  The
 * GPU side is mapped uncached for those BOs, so the only way to keep the two
 * views in sync without explicit flushes is to make the CPU mapping uncached
 * too.  This mirrors deko3d's DkMemBlockFlags_CpuUncached blocks.
 *
 * Both addr and size_B are page-aligned by the caller (memalign() alignment
 * is >= 0x1000 and size_B is rounded up to the 64 KiB bind alignment), which
 * svcSetMemoryAttribute() requires.
 */
static bool
nvkmd_switch_set_cpu_uncached(void *addr, uint64_t size_B, bool uncached)
{
   /* Drain any dirty lines to DRAM before dropping cacheability so the
    * uncached view observes the correct contents. */
   if (uncached)
      armDCacheFlush(addr, size_B);

   Result rc = svcSetMemoryAttribute(addr, size_B, MemAttr_IsUncached,
                                     uncached ? MemAttr_IsUncached : 0);
   return R_SUCCEEDED(rc);
}

static void
nvkmd_switch_mem_free(struct nvkmd_mem *_mem)
{
   struct nvkmd_switch_mem *mem = nvkmd_switch_mem(_mem);

   if (mem->base.va != NULL)
      nvkmd_va_free(mem->base.va);

   nvMapClose(&mem->map);

   /* Restore the default cacheable attribute before handing the pages back
    * to the heap, otherwise a later cached allocation reusing this range
    * would silently be uncached. */
   if (mem->cpu_uncached)
      nvkmd_switch_set_cpu_uncached(mem->cpu_addr, mem->base.size_B, false);

   free(mem->cpu_addr);
   FREE(mem);
}

static VkResult
nvkmd_switch_mem_map(struct nvkmd_mem *_mem,
                     struct vk_object_base *log_obj,
                     enum nvkmd_mem_map_flags flags,
                     void *fixed_addr,
                     void **map_out)
{
   struct nvkmd_switch_mem *mem = nvkmd_switch_mem(_mem);

   if ((flags & NVKMD_MEM_MAP_FIXED) && fixed_addr != mem->cpu_addr) {
      return vk_errorf(log_obj,
                       nvkmd_switch_log_result(log_obj,
                                               VK_ERROR_MEMORY_MAP_FAILED),
                       "nvkmd-switch: fixed CPU mappings are unsupported");
   }

   *map_out = mem->cpu_addr;
   return VK_SUCCESS;
}

static void
nvkmd_switch_mem_unmap(struct nvkmd_mem *_mem,
                       enum nvkmd_mem_map_flags flags,
                       void *map)
{
}

static VkResult
nvkmd_switch_mem_overmap(struct nvkmd_mem *_mem,
                         struct vk_object_base *log_obj,
                         enum nvkmd_mem_map_flags flags,
                         void *map)
{
   return vk_errorf(log_obj,
                    nvkmd_switch_log_result(log_obj,
                                            VK_ERROR_MEMORY_MAP_FAILED),
                    "nvkmd-switch: overmap is unsupported");
}

static void
nvkmd_switch_mem_sync_to_gpu(struct nvkmd_mem *_mem,
                             uint64_t offset_B, uint64_t range_B)
{
   struct nvkmd_switch_mem *mem = nvkmd_switch_mem(_mem);

   if (range_B == 0)
      return;

   armDCacheClean((char *)mem->cpu_addr + offset_B, range_B);
}

static bool
nvkmd_switch_try_invalidate_cpu_cache(void *addr, uint64_t range_B)
{
   if (!envIsSyscallHinted(NVKMD_SWITCH_SVC_INVALIDATE_PROCESS_DATA_CACHE))
      return false;

   const Handle process = envGetOwnProcessHandle();
   if (process == INVALID_HANDLE)
      return false;

   return R_SUCCEEDED(svcInvalidateProcessDataCache(process, (uintptr_t)addr,
                                                    (size_t)range_B));
}

static void
nvkmd_switch_mem_sync_from_gpu(struct nvkmd_mem *_mem,
                               uint64_t offset_B, uint64_t range_B)
{
   struct nvkmd_switch_mem *mem = nvkmd_switch_mem(_mem);
   void *addr = (char *)mem->cpu_addr + offset_B;

   if (range_B == 0)
      return;

   if (!nvkmd_switch_try_invalidate_cpu_cache(addr, range_B))
      armDCacheFlush(addr, range_B);
}

static VkResult
nvkmd_switch_mem_export_dma_buf(struct nvkmd_mem *_mem,
                                struct vk_object_base *log_obj,
                                int *fd_out)
{
   return vk_errorf(log_obj, VK_ERROR_INVALID_EXTERNAL_HANDLE,
                    "nvkmd-switch: dma-buf export is unsupported");
}

static uint32_t
nvkmd_switch_mem_log_handle(struct nvkmd_mem *_mem)
{
   return nvMapGetHandle(&nvkmd_switch_mem(_mem)->map);
}

static const struct nvkmd_mem_ops nvkmd_switch_mem_ops = {
   .free = nvkmd_switch_mem_free,
   .map = nvkmd_switch_mem_map,
   .unmap = nvkmd_switch_mem_unmap,
   .overmap = nvkmd_switch_mem_overmap,
   .sync_to_gpu = nvkmd_switch_mem_sync_to_gpu,
   .sync_from_gpu = nvkmd_switch_mem_sync_from_gpu,
   .export_dma_buf = nvkmd_switch_mem_export_dma_buf,
   .log_handle = nvkmd_switch_mem_log_handle,
};

static void
nvkmd_switch_va_free(struct nvkmd_va *_va)
{
   struct nvkmd_switch_va *va = nvkmd_switch_va(_va);
   struct nvkmd_switch_dev *dev = nvkmd_switch_dev(_va->dev);

   list_for_each_entry_safe(struct nvkmd_switch_va_mapping, mapping,
                            &va->mappings, link) {
      nvAddressSpaceUnmap(&dev->addr_space, mapping->addr);
      list_del(&mapping->link);
      FREE(mapping);
   }

   nvAddressSpaceFree(&dev->addr_space, va->base.addr, va->base.size_B);
   nvkmd_switch_heap_free(dev, va->base.addr, va->base.size_B);
   FREE(va);
}

static VkResult
nvkmd_switch_va_bind_mem(struct nvkmd_va *_va,
                         struct vk_object_base *log_obj,
                         uint64_t va_offset_B,
                         struct nvkmd_mem *_mem,
                         uint64_t mem_offset_B,
                         uint64_t range_B)
{
   struct nvkmd_switch_dev *dev = nvkmd_switch_dev(_va->dev);
   struct nvkmd_switch_va *va = nvkmd_switch_va(_va);
   struct nvkmd_switch_mem *mem = nvkmd_switch_mem(_mem);

   const uint64_t map_align_B = nvkmd_switch_bind_align_B(dev);
   const uint64_t target_addr = va->base.addr + va_offset_B;

   if (va_offset_B > _va->size_B || range_B > _va->size_B - va_offset_B ||
       mem_offset_B > _mem->size_B || range_B > _mem->size_B - mem_offset_B ||
       target_addr % map_align_B != 0 ||
       va_offset_B % map_align_B != 0 ||
       mem_offset_B % map_align_B != 0 ||
       range_B % map_align_B != 0) {
      return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                       "nvkmd-switch: invalid VA bind "
                       "(va=0x%" PRIx64 ", va_offset=0x%" PRIx64
                       ", mem_offset=0x%" PRIx64 ", range=0x%" PRIx64
                       ", align=0x%" PRIx64 ")",
                       (uint64_t)va->base.addr, va_offset_B, mem_offset_B,
                       range_B, map_align_B);
   }

   const NvKind kind =
      va->base.pte_kind != 0 ? (NvKind)va->base.pte_kind : mem->kind;

   struct nvkmd_switch_va_mapping *mapping =
      CALLOC_STRUCT(nvkmd_switch_va_mapping);
   if (mapping == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   const bool partial_bind = mem_offset_B != 0 || range_B != _mem->size_B;
   if (partial_bind) {
      if (range_B > UINT32_MAX || map_align_B > UINT32_MAX) {
         FREE(mapping);
         return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                          "nvkmd-switch: invalid partial VA bind "
                          "(mem_offset=0x%" PRIx64 ", range=0x%" PRIx64
                          ", align=0x%" PRIx64 ")",
                          mem_offset_B, range_B, map_align_B);
      }
   }

   /* Map the BO at the pre-allocated VA address.  alloc_va reserved this
    * range via nvAddressSpaceAllocFixed so the kernel already verified
    * alignment and page-region placement.  This matches the reference
    * nouveau backend where alloc_va picks the address once and bind_mem
    * maps into it.
    */
   const char *map_op = partial_bind ? "MapBufferEx" : "nvAddressSpaceMapFixed";
   u64 mapped_addr = target_addr;
   Result rc;
   if (partial_bind) {
      const u32 flags = NvMapBufferFlags_FixedOffset |
                        (mem->gpu_cacheable ? NvMapBufferFlags_IsCacheable : 0);
      rc = nvioctlNvhostAsGpu_MapBufferEx(dev->addr_space.fd,
                                          flags,
                                          kind,
                                          nvMapGetHandle(&mem->map),
                                          dev->addr_space.page_size,
                                          mem_offset_B,
                                          range_B,
                                          target_addr,
                                          &mapped_addr);
   } else {
      rc = nvAddressSpaceMapFixed(&dev->addr_space,
                                  nvMapGetHandle(&mem->map),
                                  mem->gpu_cacheable,
                                  kind,
                                  target_addr);
   }
   if (R_FAILED(rc)) {
      FREE(mapping);
      return vk_errorf(log_obj,
                       nvkmd_switch_log_result(log_obj,
                                               VK_ERROR_OUT_OF_DEVICE_MEMORY),
                       "nvkmd-switch: %s failed: 0x%x "
                       "(addr=0x%" PRIx64 ", kind=%u, size=0x%" PRIx64 ")",
                       map_op, rc, (uint64_t)target_addr, (unsigned)kind,
                       (uint64_t)range_B);
   }

   if (mapped_addr != target_addr) {
      nvAddressSpaceUnmap(&dev->addr_space, mapped_addr);
      FREE(mapping);
      return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                       "nvkmd-switch: MapBufferEx ignored fixed address "
                       "(requested=0x%" PRIx64 ", got=0x%" PRIx64 ")",
                       (uint64_t)target_addr, (uint64_t)mapped_addr);
   }

   NVKMD_DBG("[nvkmd-dbg] bind_mem: handle=%u, mem_offset=0x%" PRIx64
           ", size=0x%" PRIx64 ", kind=%u, gpu_cacheable=%d"
           " -> iova=0x%" PRIx64 "\n",
           nvMapGetHandle(&mem->map), mem_offset_B, (uint64_t)range_B,
           (unsigned)kind, mem->gpu_cacheable, (uint64_t)target_addr);

   mapping->addr = target_addr;
   mapping->range_B = range_B;
   list_addtail(&mapping->link, &va->mappings);

   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_va_unbind(struct nvkmd_va *_va,
                       struct vk_object_base *log_obj,
                       uint64_t va_offset_B,
                       uint64_t range_B)
{
   struct nvkmd_switch_dev *dev = nvkmd_switch_dev(_va->dev);
   struct nvkmd_switch_va *va = nvkmd_switch_va(_va);
   const uint64_t addr = va->base.addr + va_offset_B;

   list_for_each_entry_safe(struct nvkmd_switch_va_mapping, mapping,
                            &va->mappings, link) {
      if (mapping->addr == addr && mapping->range_B == range_B) {
         Result rc = nvAddressSpaceUnmap(&dev->addr_space, addr);
         if (R_FAILED(rc)) {
            return vk_errorf(log_obj, VK_ERROR_UNKNOWN,
                             "nvkmd-switch: nvAddressSpaceUnmap failed: 0x%x",
                             rc);
         }

         list_del(&mapping->link);
         FREE(mapping);
         return VK_SUCCESS;
      }
   }

   return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                    "nvkmd-switch: VA unbind range not found");
}

static const struct nvkmd_va_ops nvkmd_switch_va_ops = {
   .free = nvkmd_switch_va_free,
   .bind_mem = nvkmd_switch_va_bind_mem,
   .unbind = nvkmd_switch_va_unbind,
};

static void
nvkmd_switch_dev_destroy(struct nvkmd_dev *_dev)
{
   struct nvkmd_switch_dev *dev = nvkmd_switch_dev(_dev);

   util_vma_heap_finish(&dev->heap);
   simple_mtx_destroy(&dev->heap_mutex);
   nvkmd_switch_free_services(dev);
   simple_mtx_destroy(&dev->base.mems_mutex);
   FREE(dev);
}

static uint64_t
nvkmd_switch_dev_get_gpu_timestamp(struct nvkmd_dev *_dev)
{
   /* The GPU PTIMER counts nanoseconds (NVK reports timestampPeriod == 1.0),
    * but libnx exposes no way to read it from userspace.  Fall back to the
    * monotonic system clock, which is also nanoseconds: the rate matches the
    * GPU's query-pool timestamps so host/device deltas come out correct.
    * Only a constant origin offset versus the real PTIMER remains, which
    * calibrated-timestamp consumers cancel out.  Returning a live clock is
    * in any case far better than a constant 0, which broke timestamps whole.
    */
   return os_time_get_nano();
}

static int
nvkmd_switch_dev_get_drm_fd(struct nvkmd_dev *_dev)
{
   return -1;
}

static VkResult
nvkmd_switch_dev_alloc_mem_impl(struct nvkmd_switch_dev *dev,
                                struct vk_object_base *log_obj,
                                uint64_t size_B, uint64_t align_B,
                                uint8_t pte_kind,
                                enum nvkmd_mem_flags flags,
                                struct nvkmd_mem **mem_out)
{
   const uint32_t bind_align_B = nvkmd_switch_bind_align_B(dev);
   const uint32_t min_align_B = MAX2(bind_align_B, 0x1000u);
   const uint64_t raw_align_B = MAX2(align_B, (uint64_t)min_align_B);

   if (size_B > UINT32_MAX || raw_align_B > UINT32_MAX) {
      return vk_errorf(log_obj,
                       nvkmd_switch_log_result(log_obj,
                                               VK_ERROR_OUT_OF_DEVICE_MEMORY),
                       "nvkmd-switch: BO allocation too large");
   }

   size_B = align64(size_B, bind_align_B);
   const uint32_t alloc_align_B = raw_align_B;
   const NvKind kind = pte_kind == 0 ? NvKind_Pitch : (NvKind)pte_kind;

   struct nvkmd_switch_mem *mem = CALLOC_STRUCT(nvkmd_switch_mem);
   if (mem == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   mem->cpu_addr = memalign(alloc_align_B, size_B);
   if (mem->cpu_addr == NULL) {
      FREE(mem);
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);
   }
   memset(mem->cpu_addr, 0, size_B);

   /* Coherent BOs must have a genuinely coherent CPU view; the common nvkmd
    * layer never issues cache maintenance for them.  Fail the allocation
    * rather than hand back a "coherent" BO that silently isn't. */
   if (flags & NVKMD_MEM_COHERENT) {
      if (!nvkmd_switch_set_cpu_uncached(mem->cpu_addr, size_B, true)) {
         free(mem->cpu_addr);
         FREE(mem);
         return vk_errorf(log_obj,
                          nvkmd_switch_log_result(log_obj,
                                                  VK_ERROR_OUT_OF_DEVICE_MEMORY),
                          "nvkmd-switch: failed to make coherent BO uncached");
      }
      mem->cpu_uncached = true;
   }

   Result rc = nvMapCreate(&mem->map, mem->cpu_addr, size_B, alloc_align_B,
                           kind, false);
   if (R_FAILED(rc)) {
      if (mem->cpu_uncached)
         nvkmd_switch_set_cpu_uncached(mem->cpu_addr, size_B, false);
      free(mem->cpu_addr);
      FREE(mem);
      return vk_errorf(log_obj,
                       nvkmd_switch_log_result(log_obj,
                                               VK_ERROR_OUT_OF_DEVICE_MEMORY),
                       "nvkmd-switch: nvMapCreate failed: 0x%x", rc);
   }

   nvkmd_mem_init(&dev->base, &mem->base, &nvkmd_switch_mem_ops,
                  flags, size_B, bind_align_B);
   mem->kind = kind;
   mem->gpu_cacheable = !(flags & NVKMD_MEM_COHERENT);

   VkResult result = nvkmd_dev_alloc_va(&dev->base, log_obj,
                                        0 /* va_flags */,
                                        pte_kind,
                                        size_B, alloc_align_B,
                                        0 /* fixed_addr */,
                                        &mem->base.va);
   if (result != VK_SUCCESS)
      goto fail_map;

   result = nvkmd_va_bind_mem(mem->base.va, log_obj,
                              0 /* va_offset_B */,
                              &mem->base,
                              0 /* mem_offset_B */,
                              size_B);
   if (result != VK_SUCCESS)
      goto fail_va;

   *mem_out = &mem->base;
   return VK_SUCCESS;

fail_va:
   nvkmd_va_free(mem->base.va);
fail_map:
   nvMapClose(&mem->map);
   if (mem->cpu_uncached)
      nvkmd_switch_set_cpu_uncached(mem->cpu_addr, size_B, false);
   free(mem->cpu_addr);
   FREE(mem);
   return result;
}

static VkResult
nvkmd_switch_dev_alloc_mem(struct nvkmd_dev *_dev,
                           struct vk_object_base *log_obj,
                           uint64_t size_B, uint64_t align_B,
                           enum nvkmd_mem_flags flags,
                           struct nvkmd_mem **mem_out)
{
   return nvkmd_switch_dev_alloc_mem_impl(nvkmd_switch_dev(_dev), log_obj,
                                          size_B, align_B,
                                          0 /* pte_kind */,
                                          flags, mem_out);
}

static VkResult
nvkmd_switch_dev_alloc_tiled_mem(struct nvkmd_dev *_dev,
                                 struct vk_object_base *log_obj,
                                 uint64_t size_B, uint64_t align_B,
                                 uint8_t pte_kind, uint16_t tile_mode,
                                 enum nvkmd_mem_flags flags,
                                 struct nvkmd_mem **mem_out)
{
   return nvkmd_switch_dev_alloc_mem_impl(nvkmd_switch_dev(_dev), log_obj,
                                          size_B, align_B,
                                          pte_kind,
                                          flags, mem_out);
}

static VkResult
nvkmd_switch_dev_import_dma_buf(struct nvkmd_dev *_dev,
                                struct vk_object_base *log_obj,
                                int fd, struct nvkmd_mem **mem_out)
{
   return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                    "nvkmd-switch: dma-buf import not implemented yet");
}

static VkResult
nvkmd_switch_dev_alloc_va(struct nvkmd_dev *_dev,
                          struct vk_object_base *log_obj,
                          enum nvkmd_va_flags flags, uint8_t pte_kind,
                          uint64_t size_B, uint64_t align_B,
                          uint64_t fixed_addr, struct nvkmd_va **va_out)
{
   struct nvkmd_switch_dev *dev = nvkmd_switch_dev(_dev);
   struct nvkmd_switch_va *va = CALLOC_STRUCT(nvkmd_switch_va);
   if (va == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   const uint32_t bind_align_B = nvkmd_switch_bind_align_B(dev);
   const uint64_t page_size_B = nvkmd_switch_page_size_B(dev);
   size_B = nvkmd_switch_align_up(size_B, MAX2((uint64_t)bind_align_B,
                                               page_size_B));
   align_B = MAX2(align_B, (uint64_t)bind_align_B);

   /* The util_vma_heap is the single source of truth for VA placement.  We
    * never let nvhost-as-gpu pick addresses itself (nvAddressSpaceAlloc):
    * the heap would not know about kernel-chosen ranges and could later hand
    * the same address to a fixed or sparse allocation, producing a spurious
    * collision.  Every range — regular, fixed and sparse alike — is reserved
    * in the heap here and then pinned in the kernel with
    * nvAddressSpaceAllocFixed.
    */
   VkResult result = nvkmd_switch_heap_alloc(dev, log_obj, flags,
                                             size_B, align_B,
                                             fixed_addr, &va->base.addr);
   if (result != VK_SUCCESS) {
      FREE(va);
      return result;
   }

   Result rc = nvAddressSpaceAllocFixed(&dev->addr_space,
                                        flags & NVKMD_VA_SPARSE,
                                        size_B, va->base.addr);
   if (R_FAILED(rc)) {
      const uint64_t failed_addr = va->base.addr;
      nvkmd_switch_heap_free(dev, va->base.addr, size_B);
      FREE(va);
      return vk_errorf(log_obj,
                       nvkmd_switch_log_result(log_obj,
                                               VK_ERROR_OUT_OF_DEVICE_MEMORY),
                       "nvkmd-switch: nvAddressSpaceAllocFixed failed: "
                       "0x%x (addr=0x%" PRIx64 ", size=0x%" PRIx64
                       ", align=0x%" PRIx64 ", page=0x%" PRIx64 ")",
                       rc, failed_addr, size_B, align_B, page_size_B);
   }

   va->base.ops = &nvkmd_switch_va_ops;
   va->base.dev = _dev;
   va->base.flags = flags;
   va->base.pte_kind = pte_kind;
   va->base.size_B = size_B;
   list_inithead(&va->mappings);

   *va_out = &va->base;
   return VK_SUCCESS;
}

static VkResult
nvkmd_switch_dev_create_ctx(struct nvkmd_dev *_dev,
                            struct vk_object_base *log_obj,
                            enum nvkmd_engines engines,
                            struct nvkmd_ctx **ctx_out)
{
   struct nvkmd_switch_dev *dev = nvkmd_switch_dev(_dev);
   struct nvkmd_switch_ctx *ctx = CALLOC_STRUCT(nvkmd_switch_ctx);
   VkResult result;
   Result rc;

   if (ctx == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   ctx->base.ops = &nvkmd_switch_ctx_ops;
   ctx->base.dev = _dev;
   ctx->engines = engines;
   nvkmd_switch_ctx_reset_fence(ctx);

   if (engines != NVKMD_ENGINE_BIND) {
      ctx->uses_gpu_channel = true;

      result = nvkmd_switch_ctx_alloc_internal_map(dev, log_obj,
                                                   NVKMD_SWITCH_BUILTIN_CMDBUF_SIZE_B,
                                                   0x20000,
                                                   NvKind_Pitch,
                                                   &ctx->builtin_cmdbuf_map,
                                                   &ctx->builtin_cmdbuf_cpu,
                                                   &ctx->builtin_cmdbuf_addr);
      if (result != VK_SUCCESS)
         goto fail_ctx;

      result = nvkmd_switch_ctx_alloc_internal_map(dev, log_obj,
                                                   nvGpuGetZcullCtxSize(),
                                                   0x20000,
                                                   NvKind_Pitch,
                                                   &ctx->zcull_ctx_map,
                                                   &ctx->zcull_ctx_cpu,
                                                   &ctx->zcull_ctx_addr);
      if (result != VK_SUCCESS)
         goto fail_ctx;

      rc = nvGpuChannelCreate(&ctx->gpu_channel, &dev->addr_space,
                              NvChannelPriority_Medium);
      if (R_FAILED(rc)) {
         result = vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                            "nvkmd-switch: nvGpuChannelCreate failed: 0x%x",
                            rc);
         goto fail_ctx;
      }

      rc = nvGpuChannelZcullBind(&ctx->gpu_channel, ctx->zcull_ctx_addr);
      if (R_FAILED(rc)) {
         result = vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                            "nvkmd-switch: nvGpuChannelZcullBind failed: 0x%x",
                            rc);
         goto fail_channel;
      }

      uint32_t *cmds = (uint32_t *)ctx->builtin_cmdbuf_cpu;
      const uint32_t wait_slice_words =
         NVKMD_SWITCH_WAIT_SLICE_COUNT * NVKMD_SWITCH_WAIT_SLICE_WORDS;
      const uint32_t signal_slice_words =
         NVKMD_SWITCH_SIGNAL_SLICE_COUNT * NVKMD_SWITCH_SIGNAL_SLICE_WORDS;
      ctx->signal_slice_offset_words = wait_slice_words;
      cmds += wait_slice_words + signal_slice_words;
      ctx->prefetch_invalidate_offset_words =
         wait_slice_words + signal_slice_words;
      ctx->prefetch_invalidate_num_cmds =
         nvkmd_switch_generate_prefetch_invalidate_cmdlist(cmds);
      cmds += ctx->prefetch_invalidate_num_cmds;
      ctx->static_cmd_offset_words =
         wait_slice_words + signal_slice_words +
         ctx->prefetch_invalidate_num_cmds;
      ctx->fence_num_cmds =
         nvkmd_switch_generate_fence_cmdlist(cmds,
                                             nvGpuChannelGetSyncpointId(
                                                &ctx->gpu_channel));
      cmds += ctx->fence_num_cmds;
      ctx->flush_num_cmds = nvkmd_switch_generate_flush_cmdlist(cmds);

      /* The Switch has an incoherent CPU/GPU memory model.  The builtin
       * command buffer is mapped as GPU-cacheable, so we must explicitly
       * flush the CPU D-cache after writing the fence / flush command
       * lists, otherwise the GPU will read stale (zeroed) data and the
       * resulting malformed push buffer will trigger a PBDMA fault.
       */
      armDCacheFlush(ctx->builtin_cmdbuf_cpu, NVKMD_SWITCH_BUILTIN_CMDBUF_SIZE_B);
   }

   ctx->pending_wait_slice = -1;
   ctx->pending_wait_words = 0;
   ctx->pending_wait_emitted = false;
   ctx->next_wait_slice = 0;
   for (uint32_t i = 0; i < NVKMD_SWITCH_WAIT_SLICE_COUNT; i++)
      nvkmd_switch_reset_nvfence(&ctx->wait_slice_fences[i]);
   ctx->pending_signal_slice = -1;
   ctx->next_signal_slice = 0;
   for (uint32_t i = 0; i < NVKMD_SWITCH_SIGNAL_SLICE_COUNT; i++)
      nvkmd_switch_reset_nvfence(&ctx->signal_slice_fences[i]);

   *ctx_out = &ctx->base;
   return VK_SUCCESS;

fail_channel:
   nvGpuChannelClose(&ctx->gpu_channel);
fail_ctx:
   if (ctx->zcull_ctx_addr != 0 || ctx->zcull_ctx_cpu != NULL) {
      nvkmd_switch_ctx_free_internal_map(dev, &ctx->zcull_ctx_map,
                                         &ctx->zcull_ctx_cpu,
                                         &ctx->zcull_ctx_addr);
   }
   if (ctx->builtin_cmdbuf_addr != 0 || ctx->builtin_cmdbuf_cpu != NULL) {
      nvkmd_switch_ctx_free_internal_map(dev, &ctx->builtin_cmdbuf_map,
                                         &ctx->builtin_cmdbuf_cpu,
                                         &ctx->builtin_cmdbuf_addr);
   }
   FREE(ctx);
   return result;
}

static const struct nvkmd_dev_ops nvkmd_switch_dev_ops = {
   .destroy = nvkmd_switch_dev_destroy,
   .get_gpu_timestamp = nvkmd_switch_dev_get_gpu_timestamp,
   .get_drm_fd = nvkmd_switch_dev_get_drm_fd,
   .alloc_mem = nvkmd_switch_dev_alloc_mem,
   .alloc_tiled_mem = nvkmd_switch_dev_alloc_tiled_mem,
   .import_dma_buf = nvkmd_switch_dev_import_dma_buf,
   .alloc_va = nvkmd_switch_dev_alloc_va,
   .create_ctx = nvkmd_switch_dev_create_ctx,
};

VkResult
nvkmd_switch_create_dev(struct nvkmd_pdev *pdev,
                        struct vk_object_base *log_obj,
                        struct nvkmd_dev **dev_out)
{
   struct nvkmd_switch_dev *dev = CALLOC_STRUCT(nvkmd_switch_dev);
   const nvioctl_gpu_characteristics *gpu_info;
   uint32_t page_size_B;
   VkResult result;
   Result rc;
   if (dev == NULL)
      return vk_error(log_obj, VK_ERROR_OUT_OF_HOST_MEMORY);

   rc = nvInitialize();
   if (R_FAILED(rc))
      goto fail_dev;

   rc = nvFenceInit();
   if (R_FAILED(rc))
      goto fail_nv;

   rc = nvMapInit();
   if (R_FAILED(rc))
      goto fail_fence;

   rc = nvGpuInit();
   if (R_FAILED(rc))
      goto fail_map;

   gpu_info = nvGpuGetCharacteristics();
   page_size_B = nvkmd_switch_choose_as_page_size(gpu_info);
   rc = nvAddressSpaceCreate(&dev->addr_space, page_size_B);
   if (R_FAILED(rc))
      goto fail_gpu;

   dev->base.ops = &nvkmd_switch_dev_ops;
   dev->base.pdev = pdev;

   list_inithead(&dev->base.mems);
   simple_mtx_init(&dev->base.mems_mutex, mtx_plain);
   simple_mtx_init(&dev->heap_mutex, mtx_plain);

   result = nvkmd_switch_init_va_heap(dev, log_obj);
   if (result != VK_SUCCESS)
      goto fail_base;

   *dev_out = &dev->base;
   return VK_SUCCESS;

fail_base:
   simple_mtx_destroy(&dev->heap_mutex);
   simple_mtx_destroy(&dev->base.mems_mutex);
   nvAddressSpaceClose(&dev->addr_space);
   nvGpuExit();
   nvMapExit();
   nvFenceExit();
   nvExit();
   FREE(dev);
   return result;
fail_gpu:
   nvGpuExit();
fail_map:
   nvMapExit();
fail_fence:
   nvFenceExit();
fail_nv:
   nvExit();
fail_dev:
   FREE(dev);
   return vk_errorf(log_obj, VK_ERROR_INITIALIZATION_FAILED,
                    "nvkmd-switch: device init failed: 0x%x", rc);
}

NvMap *
nvkmd_switch_mem_get_nvmap(struct nvkmd_mem *mem)
{
   return &nvkmd_switch_mem(mem)->map;
}
