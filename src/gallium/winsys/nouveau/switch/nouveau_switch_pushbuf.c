/*
 * Copyright 2012 Red Hat Inc.
 * Copyright 2026 Mesa Switch contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Based on devkitPro libdrm_nouveau 1.0.1's MIT-licensed pushbuf.c.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <switch.h>

#include "nouveau_switch_abi.h"
#include "nouveau_switch_public.h"

#define NOUVEAU_SWITCH_GPFIFO_SKID 8u
#define NOUVEAU_SWITCH_DEFAULT_BATCH_SUBMITS 128u
#define NOUVEAU_SWITCH_RESOURCE_RETRIES 3u
#define NOUVEAU_SWITCH_DRAIN_TIMEOUT_US 200000u

/* Packed push length: 23-bit byte count plus NO_PREFETCH in bit 23. */
#define NOUVEAU_SWITCH_PUSH_LENGTH_MASK UINT64_C(0x007fffff)
#define NOUVEAU_SWITCH_PUSH_NO_PREFETCH UINT64_C(0x00800000)
#define NOUVEAU_SWITCH_PUSH_KNOWN_MASK \
   (NOUVEAU_SWITCH_PUSH_LENGTH_MASK | NOUVEAU_SWITCH_PUSH_NO_PREFETCH)

struct nouveau_pushbuf_krec {
   struct nouveau_pushbuf_krec *next;
   struct drm_nouveau_gem_pushbuf_bo buffer[NOUVEAU_GEM_MAX_BUFFERS];
   struct drm_nouveau_gem_pushbuf_push push[NOUVEAU_GEM_MAX_PUSH];
   int nr_buffer;
   int nr_push;
};

struct nouveau_switch_pending_ref {
   struct nouveau_bo *bo;
   NvFence fence;
   uint32_t access;
};

struct nouveau_pushbuf_priv {
   struct nouveau_pushbuf base;
   /*
    * Mesa 26.2 makes kick notification fallible. Keep that callback in the
    * Switch winsys until devkitPro's public libdrm_nouveau ABI grows the bool
    * return type; the public struct layout remains compatible either way.
    */
   bool (*kick_notify)(struct nouveau_pushbuf *);
   struct nouveau_pushbuf_krec *list;
   struct nouveau_pushbuf_krec *krec;
   struct nouveau_list bctx_list;
   struct nouveau_bo *bo;
   struct nouveau_bo *bo_zcullctx;
   struct nouveau_bo *bo_builtin_cmdbuf;
   NvGpuChannel gpu_channel;
   uint32_t fence_num_cmds;
   uint32_t flush_num_cmds;
   uint32_t type;
   uint32_t *ptr;
   uint32_t *bgn;
   int bo_next;
   int bo_nr;

   bool channel_ready;
   bool batch_enabled;
   bool batch_fence_enabled;
   bool fast_refs_enabled;
   bool incremental_refs_enabled;
   bool tail_queued;
   int submission_error;
   uint32_t batch_submit_limit;
   uint32_t pending_submits;
   struct nouveau_switch_pending_ref *pending_refs;
   size_t pending_ref_count;
   size_t pending_ref_capacity;
   size_t *pending_ref_hash;
   size_t pending_ref_hash_capacity;

   uint64_t residency_generation;

   struct nouveau_bo *bos[];
};

static inline struct nouveau_pushbuf_priv *
nouveau_switch_pushbuf(struct nouveau_pushbuf *push)
{
   return (struct nouveau_pushbuf_priv *)push;
}

static int pushbuf_validate(struct nouveau_pushbuf *push, bool retry);
static int pushbuf_flush_logical(struct nouveau_pushbuf *push);
static int pushbuf_native_kick(struct nouveau_pushbuf *push);
static void pushbuf_refn_fail(struct nouveau_pushbuf *push, int start_ref);

static int
pushbuf_latch_error(struct nouveau_pushbuf_priv *nvpb, int error)
{
   if (error && !nvpb->submission_error)
      nvpb->submission_error = error;

   return nvpb->submission_error;
}

static size_t
pending_ref_hash_value(const struct nouveau_bo *bo)
{
   uintptr_t value = (uintptr_t)bo;

   value >>= 4;
#if UINTPTR_MAX > UINT32_MAX
   value ^= value >> 33;
   value *= UINT64_C(0xff51afd7ed558ccd);
   value ^= value >> 33;
#else
   value ^= value >> 16;
   value *= UINT32_C(0x7feb352d);
   value ^= value >> 15;
#endif
   return (size_t)value;
}

static size_t
pending_ref_lookup(const struct nouveau_pushbuf_priv *nvpb,
                   const struct nouveau_bo *bo)
{
   if (nvpb->fast_refs_enabled && nvpb->pending_ref_hash_capacity) {
      const size_t mask = nvpb->pending_ref_hash_capacity - 1;
      size_t slot = pending_ref_hash_value(bo) & mask;

      for (;;) {
         const size_t entry = nvpb->pending_ref_hash[slot];
         if (!entry)
            return SIZE_MAX;
         if (nvpb->pending_refs[entry - 1].bo == bo)
            return entry - 1;
         slot = (slot + 1) & mask;
      }
   }

   for (size_t i = 0; i < nvpb->pending_ref_count; i++) {
      if (nvpb->pending_refs[i].bo == bo)
         return i;
   }

   return SIZE_MAX;
}

static bool
bo_has_pending_ref(const struct nouveau_pushbuf_priv *nvpb,
                   const struct nouveau_bo *bo)
{
   return pending_ref_lookup(nvpb, bo) != SIZE_MAX;
}

static void
pending_ref_hash_insert(struct nouveau_pushbuf_priv *nvpb,
                        struct nouveau_bo *bo, size_t index)
{
   const size_t mask = nvpb->pending_ref_hash_capacity - 1;
   size_t slot = pending_ref_hash_value(bo) & mask;

   while (nvpb->pending_ref_hash[slot])
      slot = (slot + 1) & mask;
   nvpb->pending_ref_hash[slot] = index + 1;
}

static void
pending_ref_hash_clear(struct nouveau_pushbuf_priv *nvpb)
{
   if (nvpb->pending_ref_hash)
      memset(nvpb->pending_ref_hash, 0,
             nvpb->pending_ref_hash_capacity *
                sizeof(*nvpb->pending_ref_hash));
}

static bool
env_bool(const char *name, bool default_value)
{
   const char *value = getenv(name);

   if (!value || !*value)
      return default_value;

   if (!strcasecmp(value, "0") || !strcasecmp(value, "false") ||
       !strcasecmp(value, "no") || !strcasecmp(value, "off"))
      return false;
   if (!strcasecmp(value, "1") || !strcasecmp(value, "true") ||
       !strcasecmp(value, "yes") || !strcasecmp(value, "on"))
      return true;

   return default_value;
}

static uint32_t
env_u32(const char *name, uint32_t default_value)
{
   const char *value = getenv(name);
   char *end = NULL;
   unsigned long parsed;

   if (!value || !*value)
      return default_value;

   errno = 0;
   parsed = strtoul(value, &end, 0);
   if (errno || end == value || *end || parsed > UINT32_MAX)
      return default_value;

   return (uint32_t)parsed;
}

static bool
pushbuf_kref_fits(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
                  uint32_t *domains)
{
   (void)push;
   (void)bo;
   (void)domains;
   return true;
}

static bool
kref_is_current(const struct nouveau_pushbuf_krec *krec,
                const struct drm_nouveau_gem_pushbuf_bo *kref,
                const struct nouveau_bo *bo)
{
   if (!krec || !kref || krec->nr_buffer <= 0 ||
       krec->nr_buffer > NOUVEAU_GEM_MAX_BUFFERS)
      return false;

   const uintptr_t begin = (uintptr_t)&krec->buffer[0];
   const uintptr_t end =
      begin + (size_t)krec->nr_buffer * sizeof(krec->buffer[0]);
   const uintptr_t address = (uintptr_t)kref;

   return address >= begin && address < end &&
          (address - begin) % sizeof(krec->buffer[0]) == 0 &&
          kref->bo == bo;
}

static struct drm_nouveau_gem_pushbuf_bo *
pushbuf_kref(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
             uint32_t flags)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec = nvpb->krec;
   struct nouveau_pushbuf *other_push;
   struct drm_nouveau_gem_pushbuf_bo *kref;
   uint32_t domains = NOUVEAU_GEM_DOMAIN_GART;
   const uint32_t domains_wr = domains * !!(flags & NOUVEAU_BO_WR);
   const uint32_t domains_rd = domains * !!(flags & NOUVEAU_BO_RD);

   /* Submit another channel before taking its BO. */
   other_push = cli_push_get(push->client, bo);
   if (other_push && other_push != push) {
      int ret = pushbuf_flush_logical(other_push);
      if (!ret)
         ret = pushbuf_native_kick(other_push);
      if (ret) {
         pushbuf_latch_error(nvpb, ret);
         return NULL;
      }
   }

   kref = cli_kref_get(push->client, bo);
   if (kref) {
      if (kref_is_current(krec, kref, bo)) {
         kref->write_domains |= domains_wr;
         kref->read_domains |= domains_rd;
         return kref;
      }

      cli_kref_set(push->client, bo, NULL, NULL);
      kref = NULL;
   }

   if (krec->nr_buffer == NOUVEAU_GEM_MAX_BUFFERS ||
       !pushbuf_kref_fits(push, bo, &domains))
      return NULL;

   kref = &krec->buffer[krec->nr_buffer++];
   kref->bo = bo;
   kref->handle = bo->handle;
   kref->write_domains = domains_wr;
   kref->read_domains = domains_rd;
   cli_kref_set(push->client, bo, kref, push);

   /* Transfer this hold to pending_refs when submission is deferred. */
   struct nouveau_bo *hold = NULL;
   nouveau_bo_ref(bo, &hold);

   return kref;
}

static int
report_invalid_push(struct nouveau_pushbuf_priv *nvpb)
{
   return pushbuf_latch_error(nvpb, -EINVAL);
}

static int
validate_push_record(struct nouveau_pushbuf_priv *nvpb,
                     struct nouveau_pushbuf_krec *krec,
                     struct drm_nouveau_gem_pushbuf_push *kpsh)
{
   const uint64_t packed_length = kpsh ? kpsh->length : 0;
   const uint64_t length = packed_length & NOUVEAU_SWITCH_PUSH_LENGTH_MASK;

   if (!krec || krec->nr_buffer < 0 ||
       krec->nr_buffer > NOUVEAU_GEM_MAX_BUFFERS ||
       krec->nr_push < 0 || krec->nr_push > NOUVEAU_GEM_MAX_PUSH)
      return report_invalid_push(nvpb);
   if (kpsh->bo_index >= (uint32_t)krec->nr_buffer)
      return report_invalid_push(nvpb);

   struct nouveau_bo *bo = krec->buffer[kpsh->bo_index].bo;
   if (!bo)
      return report_invalid_push(nvpb);
   if ((packed_length & ~NOUVEAU_SWITCH_PUSH_KNOWN_MASK) || !length ||
       ((kpsh->offset | length) & 3) || length / 4 > 0x1fffffu)
      return report_invalid_push(nvpb);
   if (kpsh->offset > bo->size ||
       length > bo->size - kpsh->offset)
      return report_invalid_push(nvpb);

   return 0;
}

static bool
pending_refs_reserve(struct nouveau_pushbuf_priv *nvpb, size_t additional)
{
   const size_t needed = nvpb->pending_ref_count + additional;
   size_t capacity = nvpb->pending_ref_capacity;
   size_t hash_capacity = nvpb->pending_ref_hash_capacity;
   struct nouveau_switch_pending_ref *new_refs;
   size_t *new_hash = NULL;

   if (needed <= capacity &&
       (!nvpb->fast_refs_enabled || hash_capacity >= capacity * 2))
      return true;

   if (!capacity)
      capacity = 1024;
   while (capacity < needed) {
      if (capacity > SIZE_MAX / 2)
         return false;
      capacity *= 2;
   }

   if (capacity > SIZE_MAX / sizeof(*new_refs))
      return false;

   new_refs = malloc(capacity * sizeof(*new_refs));
   if (!new_refs)
      return false;

   if (nvpb->pending_ref_count)
      memcpy(new_refs, nvpb->pending_refs,
             nvpb->pending_ref_count * sizeof(*new_refs));

   if (nvpb->fast_refs_enabled) {
      hash_capacity = 1;
      while (hash_capacity < capacity * 2) {
         if (hash_capacity > SIZE_MAX / 2) {
            free(new_refs);
            return false;
         }
         hash_capacity *= 2;
      }
      new_hash = calloc(hash_capacity, sizeof(*new_hash));
      if (!new_hash) {
         free(new_refs);
         return false;
      }
   }

   free(nvpb->pending_refs);
   free(nvpb->pending_ref_hash);
   nvpb->pending_refs = new_refs;
   nvpb->pending_ref_capacity = capacity;
   nvpb->pending_ref_hash = new_hash;
   nvpb->pending_ref_hash_capacity = hash_capacity;

   if (nvpb->fast_refs_enabled) {
      for (size_t i = 0; i < nvpb->pending_ref_count; i++)
         pending_ref_hash_insert(nvpb, nvpb->pending_refs[i].bo, i);
   }
   return true;
}

static int
append_entry(struct nouveau_pushbuf_priv *nvpb, uint64_t address,
             uint32_t num_cmds, uint32_t flags)
{
   Result rc = nvGpuChannelAppendEntry(&nvpb->gpu_channel, address,
                                       num_cmds, flags, 0);
   if (R_SUCCEEDED(rc))
      return 0;

   return pushbuf_latch_error(nvpb, -(int)rc);
}

static int
append_cache_tail(struct nouveau_pushbuf_priv *nvpb)
{
   const uint64_t base = nvpb->bo_builtin_cmdbuf->offset;
   int ret;

   ret = append_entry(nvpb,
                      base + 4ull * nvpb->fence_num_cmds,
                      nvpb->flush_num_cmds,
                      GPFIFO_ENTRY_NOT_MAIN);
   if (ret)
      return ret;

   ret = append_entry(nvpb,
                      base + 4ull * (nvpb->fence_num_cmds +
                                     nvpb->flush_num_cmds),
                      1,
                      GPFIFO_ENTRY_NOT_MAIN | GPFIFO_ENTRY_NO_PREFETCH);
   if (ret) {
      nvpb->gpu_channel.num_entries--;
      return ret;
   }

   nvpb->tail_queued = true;
   return 0;
}

static void
pending_ref_add(struct nouveau_pushbuf *push,
                struct drm_nouveau_gem_pushbuf_bo *kref,
                const NvFence *logical_fence)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   uint32_t access = 0;
   size_t index = SIZE_MAX;

   if (kref->write_domains)
      access |= NOUVEAU_BO_WR;
   if (kref->read_domains)
      access |= NOUVEAU_BO_RD;

   if (nvpb->fast_refs_enabled)
      index = pending_ref_lookup(nvpb, kref->bo);

   if (index != SIZE_MAX) {
      struct nouveau_switch_pending_ref *pending =
         &nvpb->pending_refs[index];
      struct nouveau_bo *duplicate = kref->bo;

      pending->access |= access;
      if (logical_fence)
         pending->fence = *logical_fence;

      /* The first occurrence owns the BO; drop the duplicate hold. */
      cli_kref_set(push->client, kref->bo, NULL, push);
      nouveau_bo_ref(NULL, &duplicate);
      return;
   }

   index = nvpb->pending_ref_count++;
   struct nouveau_switch_pending_ref *pending = &nvpb->pending_refs[index];
   pending->bo = kref->bo;
   pending->fence = logical_fence ? *logical_fence : (NvFence){0};
   pending->access = access;

   if (nvpb->fast_refs_enabled)
      pending_ref_hash_insert(nvpb, pending->bo, index);

   /* Make BO waits force submission while no active kref exists. */
   cli_kref_set(push->client, kref->bo, NULL, push);
}

static void
commit_pending_refs(struct nouveau_pushbuf *push,
                    const NvFence *batch_fence)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);

   /* Publish fences before dropping BO ownership. */
   for (size_t i = 0; i < nvpb->pending_ref_count; i++) {
      struct nouveau_switch_pending_ref *pending = &nvpb->pending_refs[i];
      struct nouveau_bo_priv *nvbo = nouveau_switch_bo(pending->bo);

      if (cli_push_get(push->client, pending->bo) == push &&
          cli_kref_get(push->client, pending->bo) == NULL)
         cli_kref_set(push->client, pending->bo, NULL, NULL);

      nvbo->fence = batch_fence ? *batch_fence : pending->fence;
      nvbo->access |= pending->access;
   }

   for (size_t i = 0; i < nvpb->pending_ref_count; i++) {
      struct nouveau_bo *bo = nvpb->pending_refs[i].bo;
      nouveau_bo_ref(NULL, &bo);
   }

   nvpb->pending_ref_count = 0;
   pending_ref_hash_clear(nvpb);
}

static void
discard_pending_refs(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);

   for (size_t i = 0; i < nvpb->pending_ref_count; i++) {
      struct nouveau_bo *bo = nvpb->pending_refs[i].bo;

      if (cli_push_get(push->client, bo) == push &&
          cli_kref_get(push->client, bo) == NULL)
         cli_kref_set(push->client, bo, NULL, NULL);
      nouveau_bo_ref(NULL, &bo);
   }

   nvpb->pending_ref_count = 0;
   pending_ref_hash_clear(nvpb);
}

/* Revalidate the current BO set after each native submission. */
static void
pushbuf_invalidate_residency(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_bufctx *bctx, *temp;

   NS_LIST_FOR_EACH_ENTRY_SAFE(bctx, temp, &nvpb->bctx_list, head) {
      NS_LIST_JOIN(&bctx->current, &bctx->pending);
      NS_LIST_INIT(&bctx->current);
      NS_LIST_DEL_INIT(&bctx->head);
   }

   nvpb->residency_generation++;
}

static bool
gpu_channel_has_error(struct nouveau_pushbuf_priv *nvpb)
{
   NvNotification notification = {0};
   if (R_SUCCEEDED(nvGpuChannelGetErrorNotification(&nvpb->gpu_channel,
                                                     &notification)) &&
       notification.status)
      return true;

   NvError error = {0};
   return R_SUCCEEDED(nvGpuChannelGetErrorInfo(&nvpb->gpu_channel, &error)) &&
          error.type != 0;
}

static Result
kickoff_with_resource_recovery(struct nouveau_pushbuf_priv *nvpb)
{
   const Result resource_error =
      MAKERESULT(Module_LibnxNvidia,
                 LibnxNvidiaError_InsufficientMemory);
   Result rc = 0;

   for (uint32_t attempt = 0;
        attempt <= NOUVEAU_SWITCH_RESOURCE_RETRIES; attempt++) {
      rc = nvGpuChannelKickoff(&nvpb->gpu_channel);
      if (R_SUCCEEDED(rc))
         return rc;

      if (rc != resource_error || gpu_channel_has_error(nvpb) ||
          attempt == NOUVEAU_SWITCH_RESOURCE_RETRIES)
         break;

      NvFence drain = nvpb->gpu_channel.fence;
      (void)nvFenceWait(&drain, NOUVEAU_SWITCH_DRAIN_TIMEOUT_US);

      if (gpu_channel_has_error(nvpb))
         break;
   }

   return rc;
}

static int
pushbuf_native_kick_impl(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   NvFence batch_fence = {0};
   const NvFence *batch_fence_ptr = NULL;
   Result rc;
   int tail_ret;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   if (!nvpb->pending_submits)
      return 0;

   /* Detach the final cache-acquire pair before kickoff. */
   if (!nvpb->tail_queued || nvpb->gpu_channel.num_entries < 2) {
      return -EINVAL;
   }

   nvpb->gpu_channel.num_entries -= 2;
   nvpb->tail_queued = false;

   if (nvpb->batch_fence_enabled) {
      int fence_ret =
         append_entry(nvpb, nvpb->bo_builtin_cmdbuf->offset,
                      nvpb->fence_num_cmds,
                      GPFIFO_ENTRY_NOT_MAIN | GPFIFO_ENTRY_NO_PREFETCH);
      if (fence_ret)
         return fence_ret;
      nvGpuChannelIncrFence(&nvpb->gpu_channel);
   }

   rc = kickoff_with_resource_recovery(nvpb);
   if (R_FAILED(rc))
      return pushbuf_latch_error(nvpb, -(int)rc);

   if (nvpb->batch_fence_enabled) {
      nvGpuChannelGetFence(&nvpb->gpu_channel, &batch_fence);
      batch_fence_ptr = &batch_fence;
   }

   nvpb->pending_submits = 0;
   tail_ret = append_cache_tail(nvpb);

   commit_pending_refs(push, batch_fence_ptr);
   pushbuf_invalidate_residency(push);

   return tail_ret;
}

static int
pushbuf_native_kick(struct nouveau_pushbuf *push)
{
   return pushbuf_native_kick_impl(push);
}

/* Preserve complete residency when pressure splits an in-progress record. */
static int
pushbuf_seed_boundary_refs(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec = nvpb->krec;
   struct nouveau_bufctx *bctx;
   struct nouveau_bufref *bref;
   const int start_ref = krec->nr_buffer;

   if (!nvpb->incremental_refs_enabled)
      return 0;

   NS_LIST_FOR_EACH_ENTRY(bctx, &nvpb->bctx_list, head) {
      NS_LIST_FOR_EACH_ENTRY(bref, &bctx->current, thead) {
         if (!pushbuf_kref(push, bref->bo, bref->flags)) {
            pushbuf_refn_fail(push, start_ref);
            return -ENOSPC;
         }
      }
   }

   return 0;
}

static int
ensure_gpfifo_space(struct nouveau_pushbuf *push, uint32_t entries_needed)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);

   if (nvpb->submission_error)
      return nvpb->submission_error;

   if (entries_needed + NOUVEAU_SWITCH_GPFIFO_SKID > GPFIFO_QUEUE_SIZE)
      return -E2BIG;

   if (nvpb->gpu_channel.num_entries + entries_needed +
       NOUVEAU_SWITCH_GPFIFO_SKID <= GPFIFO_QUEUE_SIZE)
      return 0;

   int ret = pushbuf_seed_boundary_refs(push);
   if (ret)
      return ret;

   ret = pushbuf_native_kick(push);
   if (ret)
      return ret;

   if (nvpb->gpu_channel.num_entries + entries_needed +
       NOUVEAU_SWITCH_GPFIFO_SKID > GPFIFO_QUEUE_SIZE)
      return -ENOSPC;

   return 0;
}

/* Release references from validation records that contain no commands. */
static void
release_reference_only_krec(struct nouveau_pushbuf *push,
                            struct nouveau_pushbuf_krec *krec)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   const int nr_buffer = krec->nr_buffer;

   for (int i = 0; i < nr_buffer; i++) {
      struct drm_nouveau_gem_pushbuf_bo *kref = &krec->buffer[i];
      struct nouveau_bo *bo = kref->bo;

      if (bo && cli_push_get(push->client, bo) == push &&
          cli_kref_get(push->client, bo) == kref) {
         cli_kref_set(push->client, bo, NULL,
                      bo_has_pending_ref(nvpb, bo) ? push : NULL);
      }
      nouveau_bo_ref(NULL, &bo);
   }

   krec->nr_buffer = 0;
}

static int
pushbuf_submit_logical(struct nouveau_pushbuf *push,
                       struct nouveau_object *chan)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec = nvpb->list;
   int ret;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   if (!chan || chan->oclass != NOUVEAU_FIFO_CHANNEL_CLASS)
      return -EINVAL;

   if (nvpb->kick_notify && !nvpb->kick_notify(push))
      return -EINVAL;

   nouveau_pushbuf_data(push, NULL, 0, 0);
   if (nvpb->submission_error)
      return nvpb->submission_error;

   while (krec) {
      if (!krec->nr_push) {
         krec = krec->next;
         continue;
      }

      if (krec->nr_push < 0 || krec->nr_push > NOUVEAU_GEM_MAX_PUSH ||
          krec->nr_buffer < 0 ||
          krec->nr_buffer > NOUVEAU_GEM_MAX_BUFFERS)
         return report_invalid_push(nvpb);

      const uint32_t entries_needed =
         (uint32_t)krec->nr_push +
         (nvpb->batch_fence_enabled ? 2u : 3u);

      ret = ensure_gpfifo_space(push, entries_needed);
      if (ret)
         return ret;

      if (!pending_refs_reserve(nvpb, (size_t)krec->nr_buffer)) {
         ret = pushbuf_seed_boundary_refs(push);
         if (!ret)
            ret = pushbuf_native_kick(push);
         if (ret)
            return ret;
         if (!pending_refs_reserve(nvpb, (size_t)krec->nr_buffer))
            return -ENOMEM;
      }

      struct drm_nouveau_gem_pushbuf_push *kpsh = krec->push;
      /* The queued cache pair becomes an intermediate prefix. */
      nvpb->tail_queued = false;
      for (int i = 0; i < krec->nr_push; i++, kpsh++) {
         ret = validate_push_record(nvpb, krec, kpsh);
         if (ret)
            return ret;

         struct drm_nouveau_gem_pushbuf_bo *kref =
            krec->buffer + kpsh->bo_index;
         struct nouveau_bo *bo = kref->bo;
         const uint64_t length =
            kpsh->length & NOUVEAU_SWITCH_PUSH_LENGTH_MASK;
         uint32_t entry_flags = GPFIFO_ENTRY_NOT_MAIN;
         if (kpsh->length & NOUVEAU_SWITCH_PUSH_NO_PREFETCH)
            entry_flags |= GPFIFO_ENTRY_NO_PREFETCH;

         ret = append_entry(nvpb, bo->offset + kpsh->offset,
                            (uint32_t)(length / 4), entry_flags);
         if (ret)
            return ret;
      }

      NvFence logical_fence = {0};
      const NvFence *logical_fence_ptr = NULL;
      if (!nvpb->batch_fence_enabled) {
         ret = append_entry(nvpb, nvpb->bo_builtin_cmdbuf->offset,
                            nvpb->fence_num_cmds,
                            GPFIFO_ENTRY_NOT_MAIN |
                               GPFIFO_ENTRY_NO_PREFETCH);
         if (ret)
            return ret;
         nvGpuChannelIncrFence(&nvpb->gpu_channel);
         nvGpuChannelGetFence(&nvpb->gpu_channel, &logical_fence);
         logical_fence_ptr = &logical_fence;
      }

      struct drm_nouveau_gem_pushbuf_bo *kref = krec->buffer;
      for (int i = 0; i < krec->nr_buffer; i++, kref++)
         pending_ref_add(push, kref, logical_fence_ptr);

      ret = append_cache_tail(nvpb);
      if (ret)
         return ret;

      nvpb->pending_submits++;
      krec = krec->next;
   }

   if (!nvpb->batch_enabled)
      return pushbuf_native_kick(push);

   if (nvpb->batch_submit_limit &&
       nvpb->pending_submits >= nvpb->batch_submit_limit)
      return pushbuf_native_kick(push);

   return 0;
}

static int
pushbuf_flush_logical(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec;
   struct nouveau_bufctx *bctx, *temp;
   bool reference_only = false;
   int ret;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   ret = pushbuf_submit_logical(push, push->channel);
   if (ret)
      return ret;

   for (krec = nvpb->list; krec; krec = krec->next) {
      if (!krec->nr_push) {
         reference_only |= krec->nr_buffer > 0;
         release_reference_only_krec(push, krec);
      } else {
         krec->nr_buffer = 0; /* ownership moved to pending_refs */
      }
      krec->nr_push = 0;
   }

   if (nvpb->incremental_refs_enabled) {
      if (reference_only) {
         /* Rebuild residency after a reference-only record. */
         pushbuf_invalidate_residency(push);
      }
   } else {
      NS_LIST_FOR_EACH_ENTRY_SAFE(bctx, temp, &nvpb->bctx_list, head) {
         NS_LIST_JOIN(&bctx->current, &bctx->pending);
         NS_LIST_INIT(&bctx->current);
         NS_LIST_DEL_INIT(&bctx->head);
      }
   }

   return 0;
}

static void
pushbuf_refn_fail(struct nouveau_pushbuf *push, int start_ref)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec = nvpb->krec;
   struct drm_nouveau_gem_pushbuf_bo *kref = krec->buffer + start_ref;

   while (krec->nr_buffer-- > start_ref) {
      struct nouveau_bo *bo = kref->bo;
      if (bo_has_pending_ref(nvpb, bo))
         cli_kref_set(push->client, bo, NULL, push);
      else
         cli_kref_set(push->client, bo, NULL, NULL);
      nouveau_bo_ref(NULL, &bo);
      kref++;
   }
   krec->nr_buffer = start_ref;
}

static int
pushbuf_refn(struct nouveau_pushbuf *push, bool retry,
             struct nouveau_pushbuf_refn *refs, int nr)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec = nvpb->krec;
   const int start_ref = krec->nr_buffer;
   int ret = 0;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   for (int i = 0; i < nr; i++) {
      if (!pushbuf_kref(push, refs[i].bo, refs[i].flags)) {
         ret = -ENOSPC;
         break;
      }
   }

   if (ret) {
      pushbuf_refn_fail(push, start_ref);
      if (retry) {
         ret = pushbuf_flush_logical(push);
         if (!ret)
            ret = nouveau_pushbuf_space(push, 0, 0, 0);
         if (!ret)
            return pushbuf_refn(push, false, refs, nr);
      }
   }

   return ret;
}

static int
pushbuf_validate(struct nouveau_pushbuf *push, bool retry)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec = nvpb->krec;
   struct nouveau_bufctx *bctx = push->bufctx;
   struct nouveau_bufref *bref;
   const int relocs = bctx ? bctx->relocs * 2 : 0;
   int start_ref, ret;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   ret = nouveau_pushbuf_space(push, relocs, relocs, 0);
   if (ret || !bctx)
      return ret;

   start_ref = krec->nr_buffer;
   NS_LIST_DEL(&bctx->head);
   NS_LIST_ADD(&bctx->head, &nvpb->bctx_list);

   NS_LIST_FOR_EACH_ENTRY(bref, &bctx->pending, thead) {
      if (!pushbuf_kref(push, bref->bo, bref->flags)) {
         ret = -ENOSPC;
         break;
      }
   }

   NS_LIST_JOIN(&bctx->pending, &bctx->current);
   NS_LIST_INIT(&bctx->pending);

   if (ret) {
      pushbuf_refn_fail(push, start_ref);
      if (retry) {
         ret = pushbuf_flush_logical(push);
         if (!ret)
            return pushbuf_validate(push, false);
         return ret;
      }
   }

   return ret;
}

static uint32_t
generate_fence_cmdlist(uint32_t *start, uint32_t syncpoint_id)
{
   uint32_t *cmd = start;

   *cmd++ = 0x451 | (0 << 13) | (0 << 16) | (4 << 29);
   *cmd++ = 0x0b2 | (0 << 13) | (1 << 16) | (1 << 29);
   *cmd++ = syncpoint_id | (1 << 20) | (1 << 16);
   return (uint32_t)(cmd - start);
}

static uint32_t
generate_flush_cmdlist(uint32_t *start)
{
   uint32_t *cmd = start;

   *cmd++ = 0x00b | (6 << 13) | (1 << 16) | (1 << 29);
   *cmd++ = 0x80000000;
   *cmd++ = 0x00b | (6 << 13) | (1 << 16) | (1 << 29);
   *cmd++ = 0x70000000;
   *cmd++ = 0x4a2 | (0 << 13) | (0 << 16) | (4 << 29);
   *cmd++ = 0x369 | (0 << 13) | (0x1011 << 16) | (4 << 29);
   *cmd++ = 0x50a | (0 << 13) | (0 << 16) | (4 << 29);
   *cmd++ = 0x509 | (0 << 13) | (0 << 16) | (4 << 29);
   *cmd = 0;
   return (uint32_t)(cmd - start);
}

int
nouveau_pushbuf_new(struct nouveau_client *client, struct nouveau_object *chan,
                    int nr, uint32_t size, bool immediate,
                    struct nouveau_pushbuf **out_push)
{
   struct nouveau_device_priv *nvdev =
      nouveau_switch_device(client->device);
   struct nouveau_pushbuf_priv *nvpb;
   struct nouveau_pushbuf *push;
   int ret;

   nvpb = calloc(1, sizeof(*nvpb) + nr * sizeof(*nvpb->bos));
   if (!nvpb)
      return -ENOMEM;

   nvpb->krec = calloc(1, sizeof(*nvpb->krec));
   nvpb->list = nvpb->krec;
   if (!nvpb->krec) {
      free(nvpb);
      return -ENOMEM;
   }

   push = &nvpb->base;
   push->client = client;
   push->channel = immediate ? chan : NULL;
   push->flags = NOUVEAU_BO_RD | NOUVEAU_BO_GART | NOUVEAU_BO_MAP;
   nvpb->type = NOUVEAU_BO_GART;
   nvpb->batch_enabled = env_bool("NOUVEAU_SWITCH_BATCH", true);
   nvpb->batch_fence_enabled =
      env_bool("NOUVEAU_SWITCH_BATCH_FENCE", true);
   nvpb->fast_refs_enabled =
      env_bool("NOUVEAU_SWITCH_FAST_REFS", true);
   nvpb->incremental_refs_enabled =
      env_bool("NOUVEAU_SWITCH_INCREMENTAL_REFS", true);
   nvpb->residency_generation = 1;
   nvpb->batch_submit_limit =
      env_u32("NOUVEAU_SWITCH_BATCH_SUBMITS",
              NOUVEAU_SWITCH_DEFAULT_BATCH_SUBMITS);

   for (nvpb->bo_nr = 0; nvpb->bo_nr < nr; nvpb->bo_nr++) {
      ret = nouveau_bo_new(client->device, nvpb->type, 0, size, NULL,
                           &nvpb->bos[nvpb->bo_nr]);
      if (ret) {
         nouveau_pushbuf_del(&push);
         return ret;
      }
   }

   ret = nouveau_bo_new(client->device, NOUVEAU_BO_GART, 0x20000, 0x1000,
                        NULL, &nvpb->bo_builtin_cmdbuf);
   if (ret) {
      nouveau_pushbuf_del(&push);
      return ret;
   }

   ret = nouveau_bo_new(client->device, NOUVEAU_BO_GART, 0x20000,
                        nvGpuGetZcullCtxSize(), NULL, &nvpb->bo_zcullctx);
   if (ret) {
      nouveau_pushbuf_del(&push);
      return ret;
   }

   Result rc = nvGpuChannelCreate(&nvpb->gpu_channel, &nvdev->addr_space,
                                  NvChannelPriority_Medium);
   if (R_FAILED(rc)) {
      nouveau_pushbuf_del(&push);
      return -(int)rc;
   }
   nvpb->channel_ready = true;

   rc = nvGpuChannelZcullBind(&nvpb->gpu_channel,
                              nvpb->bo_zcullctx->offset);
   if (R_FAILED(rc)) {
      nouveau_pushbuf_del(&push);
      return -(int)rc;
   }

   ret = nouveau_bo_map(nvpb->bo_builtin_cmdbuf, NOUVEAU_BO_WR, client);
   if (ret) {
      nouveau_pushbuf_del(&push);
      return ret;
   }

   uint32_t *cmds = nvpb->bo_builtin_cmdbuf->map;
   nvpb->fence_num_cmds =
      generate_fence_cmdlist(cmds,
                             nvGpuChannelGetSyncpointId(&nvpb->gpu_channel));
   cmds += nvpb->fence_num_cmds;
   nvpb->flush_num_cmds = generate_flush_cmdlist(cmds);

   NS_LIST_INIT(&nvpb->bctx_list);
   *out_push = push;

   return 0;
}

void
nouveau_pushbuf_del(struct nouveau_pushbuf **out_push)
{
   struct nouveau_pushbuf_priv *nvpb;

   if (!out_push || !*out_push)
      return;

   nvpb = nouveau_switch_pushbuf(*out_push);

   if (nvpb->channel_ready && nvpb->pending_submits &&
       !nvpb->submission_error) {
      int ret = pushbuf_native_kick(&nvpb->base);
      if (ret)
         discard_pending_refs(&nvpb->base);
   }

   if (nvpb->pending_ref_count)
      discard_pending_refs(&nvpb->base);

   if (nvpb->channel_ready)
      nvGpuChannelClose(&nvpb->gpu_channel);
   nouveau_bo_ref(NULL, &nvpb->bo_zcullctx);
   nouveau_bo_ref(NULL, &nvpb->bo_builtin_cmdbuf);

   while (nvpb->list) {
      struct nouveau_pushbuf_krec *krec = nvpb->list;
      struct drm_nouveau_gem_pushbuf_bo *kref = krec->buffer;

      while (krec->nr_buffer--) {
         struct nouveau_bo *bo = kref++->bo;
         cli_kref_set(nvpb->base.client, bo, NULL, NULL);
         nouveau_bo_ref(NULL, &bo);
      }
      nvpb->list = krec->next;
      free(krec);
   }

   while (nvpb->bo_nr--)
      nouveau_bo_ref(NULL, &nvpb->bos[nvpb->bo_nr]);
   nouveau_bo_ref(NULL, &nvpb->bo);
   free(nvpb->pending_refs);
   free(nvpb->pending_ref_hash);
   free(nvpb);
   *out_push = NULL;
}

struct nouveau_bufctx *
nouveau_pushbuf_bufctx(struct nouveau_pushbuf *push,
                       struct nouveau_bufctx *ctx)
{
   struct nouveau_bufctx *previous = push->bufctx;
   push->bufctx = ctx;
   return previous;
}

int
nouveau_pushbuf_space(struct nouveau_pushbuf *push, uint32_t dwords,
                      uint32_t relocs, uint32_t pushes)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec = nvpb->krec;
   struct nouveau_client *client = push->client;
   struct nouveau_bo *bo = NULL;
   bool flushed = false;
   int ret = 0;

   (void)relocs;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   if (push->cur + dwords >= push->end) {
      if (nvpb->bo_next < nvpb->bo_nr) {
         nouveau_bo_ref(nvpb->bos[nvpb->bo_next++], &bo);
         if (nvpb->bo_next == nvpb->bo_nr && push->channel)
            nvpb->bo_next = 0;
      } else {
         ret = nouveau_bo_new(client->device, nvpb->type, 0,
                              nvpb->bos[0]->size, NULL, &bo);
         if (ret)
            return ret;
      }
   }

   pushes++;

   if ((bo && (push->channel || !pushbuf_kref(push, bo, push->flags))) ||
       krec->nr_push + pushes >= NOUVEAU_GEM_MAX_PUSH) {
      if (nvpb->bo && krec->nr_buffer) {
         ret = pushbuf_flush_logical(push);
         if (ret) {
            nouveau_bo_ref(NULL, &bo);
            return ret;
         }
      }
      flushed = true;
   }

   if (bo) {
      ret = nouveau_bo_map(bo, NOUVEAU_BO_WR, push->client);
      if (ret) {
         nouveau_bo_ref(NULL, &bo);
         return ret;
      }

      nouveau_pushbuf_data(push, NULL, 0, 0);
      nouveau_bo_ref(bo, &nvpb->bo);
      nouveau_bo_ref(NULL, &bo);

      nvpb->bgn = nvpb->bo->map;
      nvpb->ptr = nvpb->bgn;
      push->cur = nvpb->bgn;
      push->end = push->cur + nvpb->bo->size / 4;
      push->end -= 2 + push->rsvd_kick;
   }

   if (!pushbuf_kref(push, nvpb->bo, push->flags))
      return -ENOSPC;
   return flushed ? pushbuf_validate(push, false) : 0;
}

void
nouveau_pushbuf_data(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
                     uint64_t offset, uint64_t length)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec = nvpb->krec;

   if (nvpb->submission_error)
      return;

   if (bo != nvpb->bo && nvpb->bgn != push->cur) {
      nouveau_pushbuf_data(push, nvpb->bo,
                           (uint64_t)(nvpb->bgn - nvpb->ptr) * 4,
                           (uint64_t)(push->cur - nvpb->bgn) * 4);
      nvpb->bgn = push->cur;
   }

   if (bo) {
      struct drm_nouveau_gem_pushbuf_bo *kref =
         cli_kref_get(push->client, bo);
      if (!kref_is_current(krec, kref, bo)) {
         pushbuf_latch_error(nvpb, -EINVAL);
         return;
      }
      if (krec->nr_push < 0 || krec->nr_push >= NOUVEAU_GEM_MAX_PUSH) {
         report_invalid_push(nvpb);
         return;
      }

      struct drm_nouveau_gem_pushbuf_push candidate = {
         .bo_index =
            (uint32_t)(((uintptr_t)kref - (uintptr_t)&krec->buffer[0]) /
                       sizeof(krec->buffer[0])),
         .offset = offset,
         .length = length,
      };
      if (validate_push_record(nvpb, krec, &candidate))
         return;

      struct drm_nouveau_gem_pushbuf_push *kpsh =
         &krec->push[krec->nr_push++];
      *kpsh = candidate;
   }
}

int
nouveau_pushbuf_refn(struct nouveau_pushbuf *push,
                     struct nouveau_pushbuf_refn *refs, int nr)
{
   return pushbuf_refn(push, true, refs, nr);
}

void
nouveau_pushbuf_reloc(struct nouveau_pushbuf *push, struct nouveau_bo *bo,
                      uint32_t data, uint32_t flags, uint32_t vor,
                      uint32_t tor)
{
   (void)push;
   (void)bo;
   (void)data;
   (void)flags;
   (void)vor;
   (void)tor;
}

int
nouveau_pushbuf_validate(struct nouveau_pushbuf *push)
{
   return pushbuf_validate(push, true);
}

uint32_t
nouveau_pushbuf_refd(struct nouveau_pushbuf *push, struct nouveau_bo *bo)
{
   uint32_t flags = 0;

   if (cli_push_get(push->client, bo) == push) {
      struct drm_nouveau_gem_pushbuf_bo *kref =
         cli_kref_get(push->client, bo);
      if (!kref)
         return 0;
      if (kref->read_domains)
         flags |= NOUVEAU_BO_RD;
      if (kref->write_domains)
         flags |= NOUVEAU_BO_WR;
   }

   return flags;
}

int
nouveau_switch_pushbuf_kick_deferred(struct nouveau_pushbuf *push,
                                     struct nouveau_object *chan)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   int ret;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   if (!push->channel || !nvpb->batch_enabled)
      return nouveau_pushbuf_kick(push, chan);

   ret = pushbuf_flush_logical(push);
   if (ret)
      return ret;
   if (nvpb->incremental_refs_enabled) {
      /* Prepare the next command BO without rebuilding full residency. */
      ret = nouveau_pushbuf_space(push, 0, 0, 0);
      struct drm_nouveau_gem_pushbuf_bo *command_kref =
         nvpb->bo ? cli_kref_get(push->client, nvpb->bo) : NULL;
      if (ret || !nvpb->bo ||
          !kref_is_current(nvpb->krec, command_kref, nvpb->bo)) {
         return ret ? ret : pushbuf_latch_error(nvpb, -EINVAL);
      }
      return 0;
   }
   return pushbuf_validate(push, false);
}

uint64_t
nouveau_switch_pushbuf_batch_generation(struct nouveau_pushbuf *push)
{
   return push ? nouveau_switch_pushbuf(push)->residency_generation : 0;
}

void
nouveau_switch_pushbuf_set_kick_notify(
   struct nouveau_pushbuf *push,
   bool (*kick_notify)(struct nouveau_pushbuf *))
{
   nouveau_switch_pushbuf(push)->kick_notify = kick_notify;
}

int
nouveau_pushbuf_kick(struct nouveau_pushbuf *push,
                     struct nouveau_object *chan)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   int ret;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   if (!push->channel) {
      push->channel = chan;
      ret = pushbuf_flush_logical(push);
      push->channel = NULL;
      if (!ret)
         ret = pushbuf_native_kick(push);
      return ret;
   }

   ret = pushbuf_flush_logical(push);
   if (!ret)
      ret = pushbuf_native_kick(push);
   if (!ret)
      ret = pushbuf_validate(push, false);
   return ret;
}
