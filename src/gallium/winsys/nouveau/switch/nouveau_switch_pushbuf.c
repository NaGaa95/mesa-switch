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

#include "util/macros.h"
#include "util/os_time.h"
#include "util/u_debug.h"

#include "nouveau/horizon/nouveau_horizon.h"
#include "nouveau_switch_libdrm.h"
#include "nouveau_switch_public.h"

/* Maximum backend-only overhead beyond Gallium's local exec vector: initial
 * cache acquire (2), full barrier (3), mapped completion tail (2), Horizon's
 * final native skid (4), and a possible total-order wait (1).  Client acquire
 * waits already queued directly in Horizon are tracked separately below.
 */
#define NOUVEAU_SWITCH_GPFIFO_SKID 12u
#define NOUVEAU_SWITCH_DEFAULT_BATCH_SUBMITS 128u
#define NOUVEAU_SWITCH_WAIT_FENCES 4u
#define NOUVEAU_SWITCH_DEFAULT_INFLIGHT_HIGH 256u
#define NOUVEAU_SWITCH_DEFAULT_INFLIGHT_LOW 128u
#define NOUVEAU_SWITCH_MAX_INFLIGHT_SUBMITS 512u
#define NOUVEAU_SWITCH_DEFAULT_RESOURCE_WAIT_US UINT64_C(5000)
#define NOUVEAU_SWITCH_MAX_RESOURCE_WAIT_US UINT64_C(50000)
#define NOUVEAU_SWITCH_GPFIFO_MAX_COMMANDS 0x1fffffu
#define NOUVEAU_SWITCH_GPFIFO_MAX_SIZE_B \
   (NOUVEAU_SWITCH_GPFIFO_MAX_COMMANDS * sizeof(uint32_t))

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
    * Mesa 26.2 makes kick notification fallible, while the public Nouveau
    * compatibility prefix still exposes the historical void callback.  Keep
    * the fallible callback in our Mesa-owned private tail; no external object
    * layout is involved.
   */
   bool (*kick_notify)(struct nouveau_pushbuf *);
   uint32_t (*native_kick_notify)(struct nouveau_pushbuf *,
                                  const NvFence *, bool cpu_visible);
   bool (*fence_marker_notify)(struct nouveau_pushbuf *,
                               uint64_t *cookie_out);
   uint32_t (*native_fence_batch_notify)(struct nouveau_pushbuf *,
                                          const NvFence *, bool cpu_visible,
                                          uint64_t cookie);
   struct nouveau_pushbuf_krec *list;
   struct nouveau_pushbuf_krec *krec;
   struct nouveau_list bctx_list;
   struct nouveau_bo *bo;
   struct nouveau_bo *bo_zcullctx;
   struct nouveau_horizon_device *hdevice;
   struct nouveau_horizon_channel *hchannel;
   struct nouveau_switch_completion_source *completion_source;
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
   bool log_enabled;
   bool stats_enabled;
   bool perf_enabled;
   bool diagnostics_enabled;
   bool trace_validate;
   bool channel_lost;
   bool fatal_native;
   bool last_native_fence_valid;
   bool last_native_fence_cpu_visible;
   bool cpu_completion_requested;
   bool command_record_armed;
   bool tail_queued;
   bool full_barrier_pending;
   bool pending_fence_marker_valid;
   int submission_error;
   Result submission_result;
   uint32_t batch_submit_limit;
   uint32_t log_interval;
   uint32_t fence_watchdog_us;
   uint32_t pending_submits;
   uint32_t pending_exec_count;
   uint32_t pending_native_prefix_entries;
   uint64_t pending_fence_cookie;
   uint32_t queued_entries;
   uint64_t queued_dwords;
   struct nouveau_switch_pending_ref *pending_refs;
   size_t pending_ref_count;
   size_t pending_ref_capacity;
   size_t *pending_ref_hash;
   size_t pending_ref_hash_capacity;

   uint64_t residency_generation;

   NvFence last_native_fence;
   struct nouveau_horizon_exec pending_execs[GPFIFO_QUEUE_SIZE];
   NvNotification last_notification;
   NvError last_channel_error;

   struct {
      uint64_t logical_submits;
      uint64_t native_kickoffs;
      uint64_t submitted_entries;
      uint64_t logical_entries;
      uint64_t merged_entries;
      uint64_t submitted_dwords;
      uint64_t gpu_completions;
      uint64_t cpu_completions;
      uint64_t cpu_completion_upgrades;
      uint64_t native_kickoff_failures;
      uint64_t resource_retries;
      uint64_t inflight_throttle_waits;
      uint64_t peak_inflight_submissions;
      uint64_t gallium_fences_assigned;
      uint64_t native_waits;
      uint64_t native_wait_timeouts;
      uint64_t native_wait_failures;
      uint64_t channel_errors;
      uint64_t fatal_native_errors;
      uint64_t external_wait_groups;
      uint64_t external_wait_fences;
      uint64_t command_bo_switches;
      uint64_t full_barriers;
      uint64_t logical_flush_calls;
      uint64_t logical_cpu_ns;
      uint64_t logical_max_cpu_ns;
      uint64_t native_kick_calls;
      uint64_t native_cpu_ns;
      uint64_t native_max_cpu_ns;
      uint64_t validate_calls;
      uint64_t validate_cpu_ns;
      uint64_t validate_max_cpu_ns;
      uint64_t commit_calls;
      uint64_t commit_refs;
      uint64_t commit_cpu_ns;
      uint64_t commit_max_cpu_ns;
      uint64_t peak_pending_refs;
   } stats;

   struct nouveau_bo *bos[];
};

static inline struct nouveau_pushbuf_priv *
nouveau_switch_pushbuf(struct nouveau_pushbuf *push)
{
   return (struct nouveau_pushbuf_priv *)push;
}

static int pushbuf_validate(struct nouveau_pushbuf *push, bool retry);
static int pushbuf_validate_impl(struct nouveau_pushbuf *push, bool retry);
static int pushbuf_flush_logical(struct nouveau_pushbuf *push);
static int pushbuf_native_kick(struct nouveau_pushbuf *push);
static void pushbuf_refn_fail(struct nouveau_pushbuf *push, int start_ref);

static int
pushbuf_latch_error_at(struct nouveau_pushbuf_priv *nvpb, int error,
                       const char *where)
{
   if (error && !nvpb->submission_error) {
      nvpb->submission_error = error;
      _debug_printf(
         "nouveau/switch: durable GPU submission error latched: %d at %s%s "
         "(pending=%u tail=%u entries=%u queued=%u dwords=%llu "
         "krec_buffers=%d krec_pushes=%d command_bo=%d/%d)\n",
         error, where ? where : "unknown",
         (nvpb->channel_lost || nvpb->fatal_native) ?
            " (device lost)" : "",
         nvpb->pending_submits, nvpb->tail_queued,
         nvpb->queued_entries, nvpb->queued_entries,
         (unsigned long long)nvpb->queued_dwords,
         nvpb->krec ? nvpb->krec->nr_buffer : -1,
         nvpb->krec ? nvpb->krec->nr_push : -1,
         nvpb->bo_next, nvpb->bo_nr);
   }

   return nvpb->submission_error;
}

#define pushbuf_latch_error(nvpb, error) \
   pushbuf_latch_error_at((nvpb), (error), __func__)

static int
pushbuf_invalid_state(struct nouveau_pushbuf_priv *nvpb, const char *reason)
{
   return pushbuf_latch_error_at(nvpb, -EINVAL, reason);
}

static void
pushbuf_mark_fatal_native(struct nouveau_pushbuf_priv *nvpb,
                          const char *where, Result rc)
{
   if (nvpb->fatal_native)
      return;

   nvpb->fatal_native = true;
   if (nvpb->diagnostics_enabled)
      nvpb->stats.fatal_native_errors++;
   _debug_printf("nouveau/switch: terminal native GPU failure at %s: "
                 "0x%x\n", where ? where : "unknown", rc);
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

static uint64_t
env_u64(const char *name, uint64_t default_value)
{
   const char *value = getenv(name);
   char *end = NULL;
   unsigned long long parsed;

   if (!value || !*value)
      return default_value;

   errno = 0;
   parsed = strtoull(value, &end, 0);
   if (errno || end == value || *end)
      return default_value;

   return (uint64_t)parsed;
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
   other_push = nouveau_switch_client_push_get(push->client, bo);
   if (other_push && other_push != push) {
      int ret = pushbuf_flush_logical(other_push);
      if (!ret)
         ret = pushbuf_native_kick(other_push);
      if (ret) {
         pushbuf_latch_error(nvpb, ret);
         return NULL;
      }
   }

   kref = nouveau_switch_client_kref_get(push->client, bo);
   if (kref) {
      if (kref_is_current(krec, kref, bo)) {
         kref->write_domains |= domains_wr;
         kref->read_domains |= domains_rd;
         return kref;
      }

      nouveau_switch_client_kref_set(push->client, bo, NULL, NULL);
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
   const int map_ret =
      nouveau_switch_client_kref_set(push->client, bo, kref, push);
   if (map_ret) {
      memset(kref, 0, sizeof(*kref));
      krec->nr_buffer--;
      pushbuf_latch_error(nvpb, map_ret);
      return NULL;
   }

   /* Transfer this hold to pending_refs when submission is deferred. */
   struct nouveau_bo *hold = NULL;
   nouveau_bo_ref(bo, &hold);

   return kref;
}

/* Closing a logical record transfers its command-BO hold to pending_refs, but
 * the CPU allocator intentionally keeps the unused tail of that BO current.
 * Arm a new, non-owning recording epoch after the old krec is completely
 * reset.  The first range (or space validation) in that epoch materializes
 * exactly one kref/hold.  This makes a missing kref an intentional state, not
 * something inferred and repaired after commands have already been written.
 */
static void
pushbuf_arm_current_command_record(struct nouveau_pushbuf_priv *nvpb)
{
   nvpb->command_record_armed = nvpb->bo != NULL;
}

static struct drm_nouveau_gem_pushbuf_bo *
pushbuf_materialize_current_command_bo(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec = nvpb->krec;

   if (!nvpb->bo)
      return NULL;

   struct drm_nouveau_gem_pushbuf_bo *kref =
      nouveau_switch_client_kref_get(push->client, nvpb->bo);
   if (kref) {
      if (!kref_is_current(krec, kref, nvpb->bo)) {
         pushbuf_invalid_state(
            nvpb, "current command BO retained a stale recording epoch");
         return NULL;
      }
      nvpb->command_record_armed = false;
      return kref;
   }

   if (!nvpb->command_record_armed) {
      pushbuf_invalid_state(
         nvpb, "current command BO has no armed recording epoch");
      return NULL;
   }

   kref = pushbuf_kref(push, nvpb->bo, push->flags);
   if (kref_is_current(krec, kref, nvpb->bo)) {
      nvpb->command_record_armed = false;
      return kref;
   }

   if (!nvpb->submission_error) {
      pushbuf_invalid_state(
         nvpb, "failed to materialize current command BO recording epoch");
   }

   return NULL;
}

static int
report_invalid_push(struct nouveau_pushbuf_priv *nvpb, const char *reason)
{
   return pushbuf_invalid_state(nvpb, reason);
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
      return report_invalid_push(nvpb, "invalid push record counts");
   if (kpsh->bo_index >= (uint32_t)krec->nr_buffer)
      return report_invalid_push(nvpb, "push BO index outside record");

   struct nouveau_bo *bo = krec->buffer[kpsh->bo_index].bo;
   if (!bo)
      return report_invalid_push(nvpb, "push record has no BO");
   if ((packed_length & ~NOUVEAU_SWITCH_PUSH_KNOWN_MASK) || !length ||
       ((kpsh->offset | length) & 3) || length / 4 > 0x1fffffu)
      return report_invalid_push(nvpb, "malformed push length or alignment");
   if (kpsh->offset > bo->size ||
       length > bo->size - kpsh->offset)
      return report_invalid_push(nvpb, "push range outside BO");

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

static bool
pushbuf_try_merge_exec(struct nouveau_horizon_exec *last,
                       uint64_t address, uint32_t size_B,
                       bool no_prefetch)
{
   /* Follow NVK's proven merge constraints, with the additional explicit
    * rule that a NO_PREFETCH sign-off remains an entry boundary on both
    * sides.  Gallium does not currently create incomplete chains, but keep
    * that boundary immutable if it gains such a producer later.
    */
   if (last->no_prefetch || no_prefetch || last->incomplete ||
       last->addr > UINT64_MAX - last->size_B ||
       last->addr + last->size_B != address ||
       last->size_B > NOUVEAU_SWITCH_GPFIFO_MAX_SIZE_B ||
       size_B > NOUVEAU_SWITCH_GPFIFO_MAX_SIZE_B - last->size_B)
      return false;

   last->size_B += size_B;
   return true;
}

static int
append_entry(struct nouveau_pushbuf_priv *nvpb, uint64_t address,
             uint32_t num_cmds, uint32_t flags)
{
   const uint32_t size_B = num_cmds * sizeof(uint32_t);
   const bool no_prefetch = flags & GPFIFO_ENTRY_NO_PREFETCH;

   if (!num_cmds || num_cmds > NOUVEAU_SWITCH_GPFIFO_MAX_COMMANDS)
      return pushbuf_invalid_state(nvpb, "invalid GPFIFO command count");

   if (nvpb->diagnostics_enabled)
      nvpb->stats.logical_entries++;
   nvpb->queued_dwords += num_cmds;

   if (nvpb->pending_exec_count > 0 &&
       pushbuf_try_merge_exec(
          &nvpb->pending_execs[nvpb->pending_exec_count - 1],
          address, size_B, no_prefetch)) {
      if (nvpb->diagnostics_enabled)
         nvpb->stats.merged_entries++;
      return 0;
   }

   if (nvpb->pending_exec_count >= ARRAY_SIZE(nvpb->pending_execs))
      return pushbuf_invalid_state(nvpb,
                                   "pending GPFIFO vector exhausted");

   nvpb->pending_execs[nvpb->pending_exec_count++] =
      (struct nouveau_horizon_exec) {
      .addr = address,
      .size_B = size_B,
      .incomplete = false,
      .no_prefetch = no_prefetch,
   };
   nvpb->queued_entries++;
   return 0;
}

static int
append_cache_tail(struct nouveau_pushbuf_priv *nvpb)
{
   /* Cache acquire and completion packets are owned by the shared Horizon
    * channel.  Keep this logical marker so the batching invariants below do
    * not depend on the backend's private GPFIFO representation.
    */
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
      nouveau_switch_client_kref_set(push->client, kref->bo, NULL, push);
      nouveau_bo_ref(NULL, &duplicate);
      return;
   }

   index = nvpb->pending_ref_count++;
   if (nvpb->diagnostics_enabled) {
      nvpb->stats.peak_pending_refs =
         MAX2(nvpb->stats.peak_pending_refs, nvpb->pending_ref_count);
   }
   struct nouveau_switch_pending_ref *pending = &nvpb->pending_refs[index];
   pending->bo = kref->bo;
   pending->fence = logical_fence ? *logical_fence : (NvFence){0};
   pending->access = access;

   if (nvpb->fast_refs_enabled)
      pending_ref_hash_insert(nvpb, pending->bo, index);

   /* Make BO waits force submission while no active kref exists. */
   nouveau_switch_client_kref_set(push->client, kref->bo, NULL, push);
}

static void
commit_pending_refs(struct nouveau_pushbuf *push,
                    const NvFence *batch_fence,
                    bool cpu_visible)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   const size_t committed_refs = nvpb->pending_ref_count;
   const uint64_t start_ns = nvpb->perf_enabled ? os_time_get_nano() : 0;

   /* Publish fences before dropping BO ownership. */
   for (size_t i = 0; i < nvpb->pending_ref_count; i++) {
      struct nouveau_switch_pending_ref *pending = &nvpb->pending_refs[i];
      if (nouveau_switch_client_push_get(push->client, pending->bo) == push &&
          nouveau_switch_client_kref_get(push->client, pending->bo) == NULL)
         nouveau_switch_client_kref_set(push->client, pending->bo,
                                         NULL, NULL);

      const NvFence *fence = batch_fence ? batch_fence : &pending->fence;
      nouveau_switch_bo_record_submission(
         pending->bo, fence, pending->access, nvpb->completion_source,
         cpu_visible);
   }

   for (size_t i = 0; i < nvpb->pending_ref_count; i++) {
      struct nouveau_bo *bo = nvpb->pending_refs[i].bo;
      nouveau_bo_ref(NULL, &bo);
   }

   nvpb->pending_ref_count = 0;
   pending_ref_hash_clear(nvpb);

   if (nvpb->perf_enabled) {
      const uint64_t elapsed_ns = os_time_get_nano() - start_ns;
      nvpb->stats.commit_calls++;
      nvpb->stats.commit_refs += committed_refs;
      nvpb->stats.commit_cpu_ns += elapsed_ns;
      nvpb->stats.commit_max_cpu_ns =
         MAX2(nvpb->stats.commit_max_cpu_ns, elapsed_ns);
   }
}

static void
discard_pending_refs(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);

   for (size_t i = 0; i < nvpb->pending_ref_count; i++) {
      struct nouveau_bo *bo = nvpb->pending_refs[i].bo;

      if (nouveau_switch_client_push_get(push->client, bo) == push &&
          nouveau_switch_client_kref_get(push->client, bo) == NULL)
         nouveau_switch_client_kref_set(push->client, bo, NULL, NULL);
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
gpu_channel_check_error(struct nouveau_pushbuf_priv *nvpb,
                        const char *where)
{
   struct nouveau_horizon_error error;
   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_get_error(nvpb->hchannel, &error);
   if (status == NOUVEAU_HORIZON_SUCCESS)
      return false;

   const bool first_error = !nvpb->channel_lost;
   nvpb->submission_result = error.native_result;
   nvpb->last_notification.timestamp = error.notification_timestamp;
   nvpb->last_notification.info32 = error.notification_type;
   nvpb->last_notification.info16 = error.notification_info;
   nvpb->last_notification.status = error.notification_status;
   nvpb->last_channel_error.type = error.channel_error_type;
   memcpy(nvpb->last_channel_error.info, error.channel_error_info,
          sizeof(nvpb->last_channel_error.info));
   if (first_error) {
      _debug_printf(
         "nouveau/switch: Horizon channel error at %s: %s "
         "notification={status=%u type=%u info16=%u timestamp=%llu} "
         "error={type=%u info=[%u,%u,%u,%u]} native=0x%x\n",
         where ? where : "unknown",
         nouveau_horizon_status_string(status),
         error.notification_status, error.notification_type,
         error.notification_info,
         (unsigned long long)error.notification_timestamp,
         error.channel_error_type, error.channel_error_info[0],
         error.channel_error_info[1], error.channel_error_info[2],
         error.channel_error_info[3], error.native_result);
   }

   nvpb->channel_lost = status == NOUVEAU_HORIZON_ERROR_DEVICE_LOST;
   nvpb->fatal_native |= nvpb->channel_lost;
   if (first_error && nvpb->diagnostics_enabled)
      nvpb->stats.channel_errors++;
   pushbuf_latch_error(
      nvpb, nouveau_switch_horizon_status_to_errno(status));
   return true;
}

static int
pushbuf_wait_native_fence(struct nouveau_pushbuf_priv *nvpb,
                          const NvFence *fence, uint64_t timeout_ns,
                          const char *reason)
{
   if (!fence || (int32_t)fence->id < 0)
      return -EINVAL;
   if (nvpb->submission_error)
      return nvpb->submission_error;
   if (gpu_channel_check_error(nvpb, reason))
      return nvpb->submission_error;

   uint64_t effective_timeout_ns = timeout_ns;
   if (nvpb->fence_watchdog_us) {
      const uint64_t watchdog_ns =
         (uint64_t)nvpb->fence_watchdog_us * 1000;
      effective_timeout_ns = timeout_ns == UINT64_MAX ? watchdog_ns :
                             MIN2(timeout_ns, watchdog_ns);
   }

   const struct nouveau_horizon_fence hfence = {
      .id = fence->id,
      .value = fence->value,
   };
   if (nvpb->diagnostics_enabled)
      nvpb->stats.native_waits++;
   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_fence_wait(
         nvpb->hchannel, &hfence, effective_timeout_ns);
   if (status == NOUVEAU_HORIZON_SUCCESS)
      return gpu_channel_check_error(nvpb, reason) ?
             nvpb->submission_error : 0;

   if (status == NOUVEAU_HORIZON_ERROR_TIMEOUT) {
      if (nvpb->diagnostics_enabled)
         nvpb->stats.native_wait_timeouts++;
      if (nvpb->log_enabled) {
         _debug_printf(
            "nouveau/switch: native fence timeout at %s "
            "(syncpoint=%u threshold=%u timeout_ns=%llu)\n",
            reason ? reason : "unknown", fence->id, fence->value,
            (unsigned long long)effective_timeout_ns);
      }
      if (gpu_channel_check_error(nvpb, reason))
         return nvpb->submission_error;
      return -ETIMEDOUT;
   }

   if (nvpb->diagnostics_enabled)
      nvpb->stats.native_wait_failures++;
   (void)gpu_channel_check_error(nvpb, reason);
   pushbuf_mark_fatal_native(nvpb, reason, 0);
   _debug_printf("nouveau/switch: Horizon fence wait failed at %s: %s "
                 "(syncpoint=%u threshold=%u)\n",
                 reason ? reason : "unknown",
                 nouveau_horizon_status_string(status),
                 fence->id, fence->value);
   return pushbuf_latch_error(
      nvpb, nouveau_switch_horizon_status_to_errno(status));
}

static enum nouveau_horizon_status
kickoff_with_resource_recovery(struct nouveau_pushbuf_priv *nvpb,
                               enum nouveau_horizon_completion_mode mode,
                               struct nouveau_horizon_fence *fence_out)
{
   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_exec_submit(
         nvpb->hchannel, nvpb->pending_exec_count,
         nvpb->pending_exec_count > 0 ? nvpb->pending_execs : NULL,
         nvpb->full_barrier_pending,
         mode, fence_out);

   if (nvpb->diagnostics_enabled) {
      struct nouveau_horizon_channel_stats stats;
      nouveau_horizon_channel_get_stats(nvpb->hchannel, &stats);
      nvpb->stats.resource_retries = stats.resource_retries;
      nvpb->stats.native_kickoff_failures = stats.submit_failures;
      nvpb->stats.inflight_throttle_waits = stats.inflight_throttle_waits;
      nvpb->stats.peak_inflight_submissions =
         stats.peak_inflight_submissions;
   }
   return status;
}

static void
pushbuf_log_credits(struct nouveau_pushbuf_priv *nvpb, const char *reason)
{
   if (!nvpb->hchannel)
      return;

   struct nouveau_horizon_channel_stats channel = {0};
   nouveau_horizon_channel_get_stats(nvpb->hchannel, &channel);
   _debug_printf(
      "nouveau/switch: credits %s current=%llu/%llu/%llu "
      "peak=%llu/%llu/%llu retired=%llu proactive=%llu/%llu "
      "waits=%llu(%llu/%llu/%llu) time=%llums(%llu/%llu/%llu) "
       "recovery=%llu/%llu mapped=%llu/%llu/%llu fallback=%llu lag=%llu\n",
      reason,
      (unsigned long long)channel.current_inflight_submissions,
      (unsigned long long)channel.current_inflight_entries,
      (unsigned long long)channel.current_inflight_command_bytes,
      (unsigned long long)channel.peak_inflight_submissions,
      (unsigned long long)channel.peak_inflight_entries,
      (unsigned long long)channel.peak_inflight_command_bytes,
      (unsigned long long)channel.inflight_retired_submissions,
      (unsigned long long)channel.inflight_proactive_polls,
      (unsigned long long)channel.inflight_proactive_retired_submissions,
      (unsigned long long)channel.inflight_credit_waits,
      (unsigned long long)channel.inflight_submission_watermark_waits,
      (unsigned long long)channel.inflight_entry_watermark_waits,
      (unsigned long long)channel.inflight_command_byte_watermark_waits,
      (unsigned long long)(channel.inflight_credit_wait_ns / 1000000),
      (unsigned long long)
         (channel.inflight_submission_watermark_wait_ns / 1000000),
      (unsigned long long)
         (channel.inflight_entry_watermark_wait_ns / 1000000),
      (unsigned long long)
         (channel.inflight_command_byte_watermark_wait_ns / 1000000),
      (unsigned long long)channel.resource_recovery_waits,
      (unsigned long long)channel.resource_retries,
      (unsigned long long)channel.mapped_completion_polls,
      (unsigned long long)channel.mapped_completion_hits,
      (unsigned long long)channel.mapped_completion_retired_submissions,
      (unsigned long long)channel.mapped_completion_native_fallbacks,
      (unsigned long long)channel.mapped_completion_report_lag_events);
}

static void
pushbuf_log_perf(struct nouveau_pushbuf_priv *nvpb, const char *reason)
{
   if (!nvpb->perf_enabled || !nvpb->hchannel || !nvpb->hdevice)
      return;

   struct nouveau_horizon_channel_stats channel = {0};
   struct nouveau_horizon_device_debug_stats device = {0};
   struct nouveau_horizon_memory_info memory = {0};
   struct nouveau_switch_bo_pool_stats pools = {0};
   nouveau_horizon_channel_get_stats(nvpb->hchannel, &channel);
   nouveau_horizon_device_get_debug_stats(nvpb->hdevice, &device);
   nouveau_horizon_device_get_memory_info(nvpb->hdevice, &memory);
   if (nvpb->base.client)
      nouveau_switch_device_get_pool_stats(nvpb->base.client->device,
                                           &pools);

   const uint64_t logical_avg_us = nvpb->stats.logical_flush_calls ?
      nvpb->stats.logical_cpu_ns / nvpb->stats.logical_flush_calls / 1000 : 0;
   const uint64_t native_avg_us = nvpb->stats.native_kick_calls ?
      nvpb->stats.native_cpu_ns / nvpb->stats.native_kick_calls / 1000 : 0;
   const uint64_t validate_avg_us = nvpb->stats.validate_calls ?
      nvpb->stats.validate_cpu_ns / nvpb->stats.validate_calls / 1000 : 0;
   const uint64_t commit_avg_us = nvpb->stats.commit_calls ?
      nvpb->stats.commit_cpu_ns / nvpb->stats.commit_calls / 1000 : 0;

   _debug_printf(
      "nouveau/switch: perf %s adapter logical=%llu/%llums avg/max=%llu/%lluus "
      "native=%llu/%llums avg/max=%llu/%lluus "
      "validate=%llu/%llums avg/max=%llu/%lluus "
      "commit=%llu refs=%llu peak_refs=%llu cpu=%llums avg/max=%llu/%lluus\n",
      reason, (unsigned long long)nvpb->stats.logical_flush_calls,
      (unsigned long long)(nvpb->stats.logical_cpu_ns / 1000000),
      (unsigned long long)logical_avg_us,
      (unsigned long long)(nvpb->stats.logical_max_cpu_ns / 1000),
      (unsigned long long)nvpb->stats.native_kick_calls,
      (unsigned long long)(nvpb->stats.native_cpu_ns / 1000000),
      (unsigned long long)native_avg_us,
      (unsigned long long)(nvpb->stats.native_max_cpu_ns / 1000),
      (unsigned long long)nvpb->stats.validate_calls,
      (unsigned long long)(nvpb->stats.validate_cpu_ns / 1000000),
      (unsigned long long)validate_avg_us,
      (unsigned long long)(nvpb->stats.validate_max_cpu_ns / 1000),
      (unsigned long long)nvpb->stats.commit_calls,
      (unsigned long long)nvpb->stats.commit_refs,
      (unsigned long long)nvpb->stats.peak_pending_refs,
      (unsigned long long)(nvpb->stats.commit_cpu_ns / 1000000),
      (unsigned long long)commit_avg_us,
      (unsigned long long)(nvpb->stats.commit_max_cpu_ns / 1000));

   _debug_printf(
      "nouveau/switch: perf %s horizon exec=%llu/%llums max=%lluus "
      "submit=%llu/%llums max=%lluus channel_lock=%llums/%lluus "
      "submit_lock=%llums/%lluus kickoff=%llu/%llums/%lluus "
      "throttle=%llu/%llums/%lluus recovery=%llu/%llums/%lluus\n",
      reason, (unsigned long long)channel.exec_calls,
      (unsigned long long)(channel.exec_cpu_ns / 1000000),
      (unsigned long long)(channel.exec_max_cpu_ns / 1000),
      (unsigned long long)channel.submit_calls,
      (unsigned long long)(channel.submit_cpu_ns / 1000000),
      (unsigned long long)(channel.submit_max_cpu_ns / 1000),
      (unsigned long long)(channel.channel_lock_wait_ns / 1000000),
      (unsigned long long)(channel.channel_lock_max_wait_ns / 1000),
      (unsigned long long)(channel.submit_lock_wait_ns / 1000000),
      (unsigned long long)(channel.submit_lock_max_wait_ns / 1000),
      (unsigned long long)channel.kickoff_calls,
      (unsigned long long)(channel.kickoff_cpu_ns / 1000000),
      (unsigned long long)(channel.kickoff_max_cpu_ns / 1000),
      (unsigned long long)channel.inflight_throttle_waits,
      (unsigned long long)(channel.inflight_throttle_wait_ns / 1000000),
      (unsigned long long)(channel.inflight_throttle_max_wait_ns / 1000),
      (unsigned long long)channel.resource_retries,
      (unsigned long long)(channel.resource_recovery_wait_ns / 1000000),
      (unsigned long long)(channel.resource_recovery_max_wait_ns / 1000));

   _debug_printf(
      "nouveau/switch: perf %s mapped polls/hits/retired=%llu/%llu/%llu "
      "fallback/lag=%llu/%llu poll=%lluns/%lluns lag=%lluus/%lluus\n",
      reason,
      (unsigned long long)channel.mapped_completion_polls,
      (unsigned long long)channel.mapped_completion_hits,
      (unsigned long long)channel.mapped_completion_retired_submissions,
      (unsigned long long)channel.mapped_completion_native_fallbacks,
      (unsigned long long)channel.mapped_completion_report_lag_events,
      (unsigned long long)channel.mapped_completion_poll_ns,
      (unsigned long long)channel.mapped_completion_poll_max_ns,
      (unsigned long long)
         (channel.mapped_completion_report_lag_wait_ns / 1000),
      (unsigned long long)
         (channel.mapped_completion_report_lag_max_wait_ns / 1000));

   _debug_printf(
      "nouveau/switch: perf %s objects mem=%llu/%llu wrappers=%llu/%llu "
      "VA=%llu/%llu mappings=%llu/%llu allocated=%llu/%lluMiB "
      "mem_bins=%llu,%llu,%llu,%llu,%llu "
      "create_fail=%llu bind_fail=%llu waits=%llu/%llums max=%lluus "
      "timeouts=%llu failures=%llu cache_to=%llu/%lluMiB/%llums/%lluus "
      "cache_from=%llu/%lluMiB/%llums/%lluus\n",
      reason, (unsigned long long)device.native_memories_live,
      (unsigned long long)device.native_memories_peak,
      (unsigned long long)device.memory_wrappers_live,
      (unsigned long long)device.memory_wrappers_peak,
      (unsigned long long)device.vas_live,
      (unsigned long long)device.vas_peak,
      (unsigned long long)device.mappings_live,
      (unsigned long long)device.mappings_peak,
      (unsigned long long)(memory.allocated_B >> 20),
      (unsigned long long)(memory.peak_allocated_B >> 20),
      (unsigned long long)device.native_memory_live_by_size[0],
      (unsigned long long)device.native_memory_live_by_size[1],
      (unsigned long long)device.native_memory_live_by_size[2],
      (unsigned long long)device.native_memory_live_by_size[3],
      (unsigned long long)device.native_memory_live_by_size[4],
      (unsigned long long)device.memory_create_failures,
      (unsigned long long)device.va_bind_failures,
      (unsigned long long)device.fence_wait_calls,
      (unsigned long long)(device.fence_wait_ns / 1000000),
      (unsigned long long)(device.fence_wait_max_ns / 1000),
      (unsigned long long)device.fence_wait_timeouts,
      (unsigned long long)device.fence_wait_failures,
      (unsigned long long)device.cache_to_gpu_calls,
      (unsigned long long)(device.cache_to_gpu_bytes >> 20),
      (unsigned long long)(device.cache_to_gpu_ns / 1000000),
      (unsigned long long)(device.cache_to_gpu_max_ns / 1000),
      (unsigned long long)device.cache_from_gpu_calls,
      (unsigned long long)(device.cache_from_gpu_bytes >> 20),
      (unsigned long long)(device.cache_from_gpu_ns / 1000000),
      (unsigned long long)(device.cache_from_gpu_max_ns / 1000));

   _debug_printf(
      "nouveau/switch: perf %s pool alloc=%llu fallback=%llu chunks=%llu "
      "slices=%llu/%llu requested=%llu/%lluMiB reserved=%lluMiB\n",
      reason, (unsigned long long)pools.allocation_calls,
      (unsigned long long)pools.allocation_fallbacks,
      (unsigned long long)pools.chunks,
      (unsigned long long)pools.live_slices,
      (unsigned long long)pools.peak_slices,
      (unsigned long long)(pools.live_requested_B >> 20),
      (unsigned long long)(pools.peak_requested_B >> 20),
      (unsigned long long)(pools.reserved_B >> 20));
}

static int
pushbuf_native_kick_impl(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   NvFence batch_fence = {0};
   NvFence completion_fence = {0};
   const NvFence *batch_fence_ptr = NULL;
   int tail_ret;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   if (!nvpb->pending_submits && !nvpb->full_barrier_pending &&
       !nvpb->cpu_completion_requested)
      return 0;

   if ((nvpb->pending_submits || nvpb->full_barrier_pending) &&
       !nvpb->tail_queued) {
      return pushbuf_invalid_state(nvpb,
                                   "native kickoff missing cache tail");
   }
   if (nvpb->queued_entries != nvpb->pending_exec_count)
      return pushbuf_invalid_state(
         nvpb, "local GPFIFO vector/accounting mismatch");

   const enum nouveau_horizon_completion_mode completion_mode =
      nvpb->cpu_completion_requested ? NOUVEAU_HORIZON_COMPLETION_CPU :
                                       NOUVEAU_HORIZON_COMPLETION_GPU;
   const bool cpu_upgrade =
      completion_mode == NOUVEAU_HORIZON_COMPLETION_CPU &&
      nvpb->last_native_fence_valid &&
      !nvpb->last_native_fence_cpu_visible &&
      !nvpb->pending_submits && !nvpb->full_barrier_pending;
   /* This marker was published only after its complete logical command record
    * joined the pending physical batch.  Capture it before submission so a
    * callback can never associate a fence emitted by a record which is still
    * being converted (for example, across an ensure_gpfifo_space() kickoff).
    */
   const bool submitted_fence_marker_valid =
      nvpb->pending_fence_marker_valid;
   const uint64_t submitted_fence_cookie =
      nvpb->pending_fence_cookie;
   nvpb->tail_queued = false;

   struct nouveau_horizon_fence hfence = {
      .id = NOUVEAU_HORIZON_INVALID_FENCE_ID,
   };
   const enum nouveau_horizon_status status =
      kickoff_with_resource_recovery(nvpb, completion_mode, &hfence);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      (void)gpu_channel_check_error(nvpb, "Horizon submit");
      pushbuf_mark_fatal_native(nvpb, "Horizon submit", 0);
      _debug_printf("nouveau/switch: Horizon channel submit failed: %s "
                    "(logical_submits=%u entries=%u)\n",
                    nouveau_horizon_status_string(status),
                    nvpb->pending_submits, nvpb->queued_entries);
      return pushbuf_latch_error(
         nvpb, nouveau_switch_horizon_status_to_errno(status));
   }

   completion_fence.id = hfence.id;
   completion_fence.value = hfence.value;

   if (nvpb->diagnostics_enabled) {
      nvpb->stats.native_kickoffs++;
      nvpb->stats.submitted_entries += nvpb->queued_entries;
      nvpb->stats.submitted_dwords += nvpb->queued_dwords;
   }
   nvpb->pending_exec_count = 0;
   nvpb->pending_native_prefix_entries = 0;
   nvpb->queued_entries = 0;
   nvpb->queued_dwords = 0;
   nvpb->last_native_fence = completion_fence;
   nvpb->last_native_fence_valid = (int32_t)completion_fence.id >= 0;
   nvpb->last_native_fence_cpu_visible =
      nvpb->last_native_fence_valid &&
      completion_mode == NOUVEAU_HORIZON_COMPLETION_CPU;
   nvpb->cpu_completion_requested = false;
   if (nvpb->diagnostics_enabled) {
      if (nvpb->last_native_fence_cpu_visible) {
         nvpb->stats.cpu_completions++;
         if (cpu_upgrade)
            nvpb->stats.cpu_completion_upgrades++;
      } else {
         nvpb->stats.gpu_completions++;
      }
   }
   if (nvpb->native_fence_batch_notify &&
       submitted_fence_marker_valid &&
       nvpb->last_native_fence_valid) {
      const uint32_t assigned =
         nvpb->native_fence_batch_notify(
            push, &completion_fence,
            nvpb->last_native_fence_cpu_visible,
            submitted_fence_cookie);
      if (nvpb->diagnostics_enabled)
         nvpb->stats.gallium_fences_assigned += assigned;
      if (assigned == 0) {
         /* The batch has been accepted, but without its opaque Gallium
          * boundary no logical fence may claim that completion.  Retain all
          * pending BO/command ownership and fail the pushbuf permanently;
          * teardown may release it only after proving the channel idle.
          */
         pushbuf_mark_fatal_native(
            nvpb, "Gallium physical-batch association", 0);
         return pushbuf_latch_error_at(
            nvpb, -EIO, "Gallium physical-batch association");
      }
   } else if (!nvpb->native_fence_batch_notify &&
              nvpb->native_kick_notify &&
              nvpb->last_native_fence_valid) {
      /* Preserve the legacy callback ABI for non-Gallium users.  Gallium uses
       * the batch-bound callback above and therefore never falls back to a
       * blanket fence-list scan.
       */
      const uint32_t assigned = nvpb->native_kick_notify(
         push, &completion_fence,
         nvpb->last_native_fence_cpu_visible);
      if (nvpb->diagnostics_enabled)
         nvpb->stats.gallium_fences_assigned += assigned;
   }
   nvpb->pending_fence_marker_valid = false;
   nvpb->pending_fence_cookie = 0;
   nvpb->full_barrier_pending = false;

   /* This adapter is now the sole submit owner of the render channel.  The
    * shared backend transaction has consumed exactly the local exec vector;
    * any residual local work here would make BO/fence publication ambiguous.
    */
   assert(nvpb->pending_exec_count == 0);
   assert(nvpb->queued_entries == 0);

   if (nvpb->log_enabled && nvpb->log_interval &&
       (nvpb->stats.native_kickoffs == 1 ||
        nvpb->stats.native_kickoffs % nvpb->log_interval == 0)) {
      _debug_printf("nouveau/switch: pushbuf periodic logical=%llu "
                    "native=%llu entries=%llu/%llu merged=%llu "
                    "dwords=%llu completion=%llu/%llu upgrades=%llu "
                    "kick_fail=%llu "
                    "retries=%llu throttle=%llu/%llu fences=%llu waits=%llu "
                    "acquire_groups=%llu full_barriers=%llu bo_switches=%llu\n",
                    (unsigned long long)nvpb->stats.logical_submits,
                    (unsigned long long)nvpb->stats.native_kickoffs,
                    (unsigned long long)nvpb->stats.submitted_entries,
                    (unsigned long long)nvpb->stats.logical_entries,
                    (unsigned long long)nvpb->stats.merged_entries,
                    (unsigned long long)nvpb->stats.submitted_dwords,
                    (unsigned long long)nvpb->stats.gpu_completions,
                    (unsigned long long)nvpb->stats.cpu_completions,
                    (unsigned long long)
                       nvpb->stats.cpu_completion_upgrades,
                    (unsigned long long)
                       nvpb->stats.native_kickoff_failures,
                    (unsigned long long)nvpb->stats.resource_retries,
                    (unsigned long long)
                       nvpb->stats.inflight_throttle_waits,
                    (unsigned long long)
                       nvpb->stats.peak_inflight_submissions,
                    (unsigned long long)
                       nvpb->stats.gallium_fences_assigned,
                    (unsigned long long)nvpb->stats.native_waits,
                    (unsigned long long)nvpb->stats.external_wait_groups,
                    (unsigned long long)nvpb->stats.full_barriers,
                    (unsigned long long)nvpb->stats.command_bo_switches);
      pushbuf_log_perf(nvpb, "periodic");
      pushbuf_log_credits(nvpb, "periodic");
   }

   batch_fence = completion_fence;
   batch_fence_ptr = &batch_fence;

   nvpb->pending_submits = 0;
   tail_ret = append_cache_tail(nvpb);

   commit_pending_refs(push, batch_fence_ptr,
                       nvpb->last_native_fence_cpu_visible);
   pushbuf_invalidate_residency(push);

   return tail_ret;
}

static int
pushbuf_native_kick(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (!nvpb->perf_enabled)
      return pushbuf_native_kick_impl(push);

   const uint64_t start_ns = os_time_get_nano();
   const int ret = pushbuf_native_kick_impl(push);
   const uint64_t elapsed_ns = os_time_get_nano() - start_ns;
   nvpb->stats.native_kick_calls++;
   nvpb->stats.native_cpu_ns += elapsed_ns;
   nvpb->stats.native_max_cpu_ns =
      MAX2(nvpb->stats.native_max_cpu_ns, elapsed_ns);
   return ret;
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

   if ((uint64_t)nvpb->pending_native_prefix_entries +
          nvpb->queued_entries + entries_needed +
          NOUVEAU_SWITCH_GPFIFO_SKID <= GPFIFO_QUEUE_SIZE)
      return 0;

   int ret = pushbuf_seed_boundary_refs(push);
   if (ret)
      return ret;

   ret = pushbuf_native_kick(push);
   if (ret)
      return ret;

   if ((uint64_t)nvpb->pending_native_prefix_entries +
          nvpb->queued_entries + entries_needed +
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

      if (bo && nouveau_switch_client_push_get(push->client, bo) == push &&
          nouveau_switch_client_kref_get(push->client, bo) == kref) {
         nouveau_switch_client_kref_set(
            push->client, bo, NULL,
            bo_has_pending_ref(nvpb, bo) ? push : NULL);
      }
      nouveau_bo_ref(NULL, &bo);
   }

   krec->nr_buffer = 0;
}

static int
pushbuf_submit_logical_impl(struct nouveau_pushbuf *push,
                            struct nouveau_object *chan)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec = nvpb->list;
   uint64_t logical_fence_cookie = 0;
   bool logical_fence_valid = false;
   bool appended_logical_work = false;
   int ret;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   if (!chan || chan->oclass != NOUVEAU_FIFO_CHANNEL_CLASS)
      return pushbuf_invalid_state(nvpb, "invalid logical-submit channel");

   if (nvpb->kick_notify && !nvpb->kick_notify(push))
      return pushbuf_invalid_state(nvpb, "kick notification rejected submit");

   if (nvpb->fence_marker_notify) {
      logical_fence_valid =
         nvpb->fence_marker_notify(push, &logical_fence_cookie);
   }

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
         return report_invalid_push(nvpb, "logical submit record counts");

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

      /* Publish every CPU-cached command/resource write before the first
       * GPFIFO entry which can consume it.  The adapter tracks dirtiness, so
       * this is a cheap no-op for untouched and CPU-uncached BOs.
       */
      for (int i = 0; i < krec->nr_push; i++) {
         const uint32_t bo_index = krec->push[i].bo_index;
         if (bo_index < (uint32_t)krec->nr_buffer)
            nouveau_switch_bo_mark_cpu_dirty(krec->buffer[bo_index].bo);
      }
      for (int i = 0; i < krec->nr_buffer; i++) {
         ret = nouveau_switch_bo_sync_to_gpu(krec->buffer[i].bo);
         if (ret)
            return ret;
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
      (void)logical_fence;

      struct drm_nouveau_gem_pushbuf_bo *kref = krec->buffer;
      for (int i = 0; i < krec->nr_buffer; i++, kref++)
         pending_ref_add(push, kref, logical_fence_ptr);

      /* pending_ref_add transfers every kref hold and client-map ownership to
       * pending_refs.  Publish that ownership change immediately, before a
       * fallible cache-tail append or physical/batch-limit kick can return;
       * error teardown must never see the same hold in both containers.
       */
      krec->nr_buffer = 0;

      ret = append_cache_tail(nvpb);
      if (ret)
         return ret;

      nvpb->pending_submits++;
      appended_logical_work = true;
      if (nvpb->diagnostics_enabled)
         nvpb->stats.logical_submits++;
      krec = krec->next;
   }

   /* Publish only after every command entry in this logical record has joined
    * the pending physical batch.  Any space/residency kickoff taken inside the
    * loop above therefore sees only the previous, already-published boundary.
    */
   if (logical_fence_valid && appended_logical_work) {
      nvpb->pending_fence_cookie = logical_fence_cookie;
      nvpb->pending_fence_marker_valid = true;
   }

   if (!nvpb->batch_enabled)
      return pushbuf_native_kick(push);

   if (nvpb->batch_submit_limit &&
       nvpb->pending_submits >= nvpb->batch_submit_limit)
      return pushbuf_native_kick(push);

   return 0;
}

static int
pushbuf_submit_logical(struct nouveau_pushbuf *push,
                       struct nouveau_object *chan)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (!nvpb->perf_enabled)
      return pushbuf_submit_logical_impl(push, chan);

   const uint64_t start_ns = os_time_get_nano();
   const int ret = pushbuf_submit_logical_impl(push, chan);
   const uint64_t elapsed_ns = os_time_get_nano() - start_ns;
   nvpb->stats.logical_flush_calls++;
   nvpb->stats.logical_cpu_ns += elapsed_ns;
   nvpb->stats.logical_max_cpu_ns =
      MAX2(nvpb->stats.logical_max_cpu_ns, elapsed_ns);
   return ret;
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

   pushbuf_arm_current_command_record(nvpb);
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
         nouveau_switch_client_kref_set(push->client, bo, NULL, push);
      else
         nouveau_switch_client_kref_set(push->client, bo, NULL, NULL);
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
pushbuf_validate_impl(struct nouveau_pushbuf *push, bool retry)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   struct nouveau_pushbuf_krec *krec = nvpb->krec;
   struct nouveau_bufctx *bctx = push->bufctx;
   struct nouveau_bufref *bref;
   const int relocs = bctx ? bctx->relocs * 2 : 0;
   int start_ref, ret;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   if (nvpb->trace_validate)
      _debug_printf("nouveau/switch: validate: begin "
                    "(krefs=%d relocs=%d retry=%u)\n",
                    krec->nr_buffer, relocs, retry);
   ret = nouveau_pushbuf_space(push, relocs, relocs, 0);
   if (ret || !bctx)
      return ret;
   if (nvpb->trace_validate)
      _debug_printf("nouveau/switch: validate: command space ready "
                    "(krefs=%d)\n", krec->nr_buffer);

   start_ref = krec->nr_buffer;
   NS_LIST_DEL(&bctx->head);
   NS_LIST_ADD(&bctx->head, &nvpb->bctx_list);

   struct nouveau_list *node = bctx->pending.next;
   unsigned pending_count = 0;
   bool list_valid = true;
   while (node != &bctx->pending) {
      if (!node || !node->next || !node->prev ||
          node->next->prev != node || node->prev->next != node ||
          pending_count >= NOUVEAU_GEM_MAX_BUFFERS) {
         ret = pushbuf_invalid_state(nvpb,
                                     "corrupt or oversized bufctx list");
         list_valid = false;
         break;
      }

      bref = NS_LIST_ENTRY(struct nouveau_bufref, node, thead);
      if (!bref->bo) {
         ret = pushbuf_invalid_state(nvpb, "bufctx reference has no BO");
         list_valid = false;
         break;
      }
      if (nvpb->trace_validate)
         _debug_printf("nouveau/switch: validate: reference %u "
                       "(BO=%u flags=0x%x)\n",
                       pending_count, bref->bo ? bref->bo->handle : 0,
                       bref->flags);
      if (!pushbuf_kref(push, bref->bo, bref->flags)) {
         ret = -ENOSPC;
         break;
      }
      if (nvpb->trace_validate)
         _debug_printf("nouveau/switch: validate: reference %u ready\n",
                       pending_count);
      pending_count++;
      node = node->next;
   }

   if (!list_valid) {
      pushbuf_refn_fail(push, start_ref);
      return ret;
   }

   NS_LIST_JOIN(&bctx->pending, &bctx->current);
   NS_LIST_INIT(&bctx->pending);
   if (nvpb->trace_validate)
      _debug_printf("nouveau/switch: validate: list committed "
                    "(pending=%u krefs=%d ret=%d)\n",
                    pending_count, krec->nr_buffer, ret);

   if (ret) {
      pushbuf_refn_fail(push, start_ref);
      if (retry) {
         ret = pushbuf_flush_logical(push);
         if (!ret)
            return pushbuf_validate(push, false);
         return ret;
      }
   }

   if (nvpb->trace_validate)
      _debug_printf("nouveau/switch: validate: complete (ret=%d)\n", ret);
   return ret;
}

static int
pushbuf_validate(struct nouveau_pushbuf *push, bool retry)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (!nvpb->perf_enabled)
      return pushbuf_validate_impl(push, retry);

   const uint64_t start_ns = os_time_get_nano();
   const int ret = pushbuf_validate_impl(push, retry);
   const uint64_t elapsed_ns = os_time_get_nano() - start_ns;
   nvpb->stats.validate_calls++;
   nvpb->stats.validate_cpu_ns += elapsed_ns;
   nvpb->stats.validate_max_cpu_ns =
      MAX2(nvpb->stats.validate_max_cpu_ns, elapsed_ns);
   return ret;
}

int
nouveau_pushbuf_new(struct nouveau_client *client, struct nouveau_object *chan,
                    int nr, uint32_t size, bool immediate,
                    struct nouveau_pushbuf **out_push)
{
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
   /* Command streams are written continuously and consumed once.  Keep the
    * command ring CPU/GPU uncached, matching the original Switch nouveau
    * implementation and deko3d.  Cached 512 KiB command BOs otherwise force
    * a whole-BO cache clean at every logical submission, which is especially
    * expensive in CPU-bound GL workloads.
    */
   nvpb->type = NOUVEAU_BO_GART | NOUVEAU_BO_COHERENT;
   nvpb->batch_enabled = env_bool("NOUVEAU_SWITCH_BATCH", true);
   nvpb->batch_fence_enabled =
      env_bool("NOUVEAU_SWITCH_BATCH_FENCE", true);
   nvpb->fast_refs_enabled =
      env_bool("NOUVEAU_SWITCH_FAST_REFS", true);
   nvpb->incremental_refs_enabled =
      env_bool("NOUVEAU_SWITCH_INCREMENTAL_REFS", true);
   nvpb->log_enabled = env_bool("NOUVEAU_SWITCH_LOG", false);
   nvpb->stats_enabled = env_bool("NOUVEAU_SWITCH_STATS", false);
   nvpb->perf_enabled = false;
   nvpb->diagnostics_enabled = nvpb->log_enabled || nvpb->stats_enabled ||
                               nvpb->perf_enabled;
   nvpb->trace_validate =
      env_bool("NOUVEAU_SWITCH_TRACE_VALIDATE", false);
   nvpb->residency_generation = 1;
   nvpb->batch_submit_limit =
      env_u32("NOUVEAU_SWITCH_BATCH_SUBMITS",
              NOUVEAU_SWITCH_DEFAULT_BATCH_SUBMITS);
   nvpb->log_interval = env_u32("NOUVEAU_SWITCH_LOG_INTERVAL", 120);
   nvpb->fence_watchdog_us =
      env_u32("NOUVEAU_SWITCH_FENCE_TIMEOUT_US", 0);

   for (nvpb->bo_nr = 0; nvpb->bo_nr < nr; nvpb->bo_nr++) {
      ret = nouveau_bo_new(client->device, nvpb->type, 0, size, NULL,
                           &nvpb->bos[nvpb->bo_nr]);
      if (ret) {
         nouveau_pushbuf_del(&push);
         return ret;
      }
   }

   ret = nouveau_bo_new(client->device, NOUVEAU_BO_GART, 0x20000,
                        nvGpuGetZcullCtxSize(), NULL, &nvpb->bo_zcullctx);
   if (ret) {
      nouveau_pushbuf_del(&push);
      return ret;
   }

   nvpb->hdevice =
      nouveau_switch_device_get_horizon(client->device);

   uint32_t inflight_high = env_u32(
      "NOUVEAU_SWITCH_INFLIGHT_SUBMIT_HIGH",
      NOUVEAU_SWITCH_DEFAULT_INFLIGHT_HIGH);
   inflight_high = CLAMP(inflight_high, 2u,
                         NOUVEAU_SWITCH_MAX_INFLIGHT_SUBMITS);
   uint32_t inflight_low = env_u32(
      "NOUVEAU_SWITCH_INFLIGHT_SUBMIT_LOW",
      NOUVEAU_SWITCH_DEFAULT_INFLIGHT_LOW);
   inflight_low = CLAMP(inflight_low, 1u, inflight_high - 1);

   const uint64_t entry_high = env_u64(
      "NOUVEAU_SWITCH_INFLIGHT_ENTRY_HIGH", 0);
   uint64_t entry_low = env_u64(
      "NOUVEAU_SWITCH_INFLIGHT_ENTRY_LOW", 0);
   if (!entry_high)
      entry_low = 0;
   else if (entry_low >= entry_high)
      entry_low = entry_high / 2;

   const uint64_t byte_high = env_u64(
      "NOUVEAU_SWITCH_INFLIGHT_BYTE_HIGH", 0);
   uint64_t byte_low = env_u64(
      "NOUVEAU_SWITCH_INFLIGHT_BYTE_LOW", 0);
   if (!byte_high)
      byte_low = 0;
   else if (byte_low >= byte_high)
      byte_low = byte_high / 2;

   const uint64_t resource_wait_us = MIN2(
      env_u64("NOUVEAU_SWITCH_RESOURCE_WAIT_US",
              NOUVEAU_SWITCH_DEFAULT_RESOURCE_WAIT_US),
      NOUVEAU_SWITCH_MAX_RESOURCE_WAIT_US);
   const struct nouveau_horizon_channel_create_info channel_info = {
      .priority = NOUVEAU_HORIZON_CHANNEL_PRIORITY_MEDIUM,
      .inflight_submit_high_watermark = inflight_high,
      .inflight_submit_low_watermark = inflight_low,
      .inflight_entry_high_watermark = entry_high,
      .inflight_entry_low_watermark = entry_low,
      .inflight_command_byte_high_watermark = byte_high,
      .inflight_command_byte_low_watermark = byte_low,
      .resource_wait_slice_ns = resource_wait_us * 1000,
   };
   enum nouveau_horizon_status status = nouveau_horizon_channel_create(
      nvpb->hdevice, &channel_info, &nvpb->hchannel);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      nouveau_pushbuf_del(&push);
      return nouveau_switch_horizon_status_to_errno(status);
   }
   nvpb->channel_ready = true;

   nvpb->completion_source =
      nouveau_switch_completion_source_create(nvpb->hchannel, nvpb->hdevice);
   if (!nvpb->completion_source) {
      nouveau_pushbuf_del(&push);
      return -ENOMEM;
   }

   status = nouveau_horizon_channel_bind_zcull(
      nvpb->hchannel, nvpb->bo_zcullctx->offset);
   if (status != NOUVEAU_HORIZON_SUCCESS) {
      nouveau_pushbuf_del(&push);
      return nouveau_switch_horizon_status_to_errno(status);
   }

   if (nvpb->log_enabled) {
      _debug_printf("nouveau/switch: pushbuf batching=%u batch_fence=%u "
                    "batch_limit=%u fast_refs=%u incremental_refs=%u "
                    "command_cache=uncached completion=gpu-default "
                    "credits=sub:%u/%u entry:%llu/%llu byte:%llu/%llu "
                    "resource_wait_us=%llu perf=%u "
                    "fence_watchdog_us=%u%s "
                    "log_interval=%u\n",
                    nvpb->batch_enabled, nvpb->batch_fence_enabled,
                     nvpb->batch_submit_limit, nvpb->fast_refs_enabled,
                     nvpb->incremental_refs_enabled,
                     inflight_low, inflight_high,
                     (unsigned long long)entry_low,
                     (unsigned long long)entry_high,
                     (unsigned long long)byte_low,
                     (unsigned long long)byte_high,
                     (unsigned long long)resource_wait_us,
                     nvpb->perf_enabled,
                    nvpb->fence_watchdog_us,
                    nvpb->fence_watchdog_us ? "" : " (disabled)",
                    nvpb->log_interval);
   }

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
      (void)ret;
   }

   if (nvpb->channel_ready) {
      if (nvpb->stats_enabled || nvpb->log_enabled)
         pushbuf_log_credits(nvpb, "final");
      pushbuf_log_perf(nvpb, "final");
   }

   /* A failed native kickoff may have consumed some or all queued entries
    * even when no trustworthy completion fence was returned.  Keep every
    * referenced BO alive and mapped until channel teardown proves completion;
    * only then is it safe to release the pending residency set.
    */
   if (nvpb->channel_ready) {
      const enum nouveau_horizon_status idle_status =
         nouveau_horizon_channel_wait_idle(nvpb->hchannel, UINT64_MAX);
      const bool idle_complete =
         idle_status == NOUVEAU_HORIZON_SUCCESS;

      /* Completion sources contain a borrowed channel pointer.  Retire the
       * source while the channel is still alive; this waits for any concurrent
       * BO upgrade holding the source lock and makes all surviving BO records
       * permanently complete (or permanently failed) without a channel/BO
       * reference cycle.
       */
      nouveau_switch_completion_source_retire(
         nvpb->completion_source, idle_complete);

      const enum nouveau_horizon_channel_put_result put_result =
         nouveau_horizon_channel_put(nvpb->hchannel);
      nvpb->hchannel = NULL;
      nvpb->channel_ready = false;

      if (!idle_complete ||
          put_result != NOUVEAU_HORIZON_CHANNEL_PUT_COMPLETE) {
         /* The native channel may still fetch any GPFIFO command or touch any
          * resource in its submitted residency set.  Keep this entire object
          * alive: pending_refs, validation krefs, command BOs, z-cull context,
          * and their client ownership links collectively pin all such storage.
          * Detach the public channel so a later BO lookup cannot submit through
          * this deliberately leaked pushbuf after its owner has gone away.
          */
         nvpb->base.channel = NULL;
         _debug_printf(
            "nouveau/switch: channel teardown did not establish completion "
            "(idle=%s result=%u); quarantining complete pushbuf residency and "
            "command storage\n",
            nouveau_horizon_status_string(idle_status),
            (unsigned)put_result);
         nouveau_switch_completion_source_unref(nvpb->completion_source);
         nvpb->completion_source = NULL;
         *out_push = NULL;
         return;
      }
   }

   nouveau_switch_completion_source_unref(nvpb->completion_source);
   nvpb->completion_source = NULL;

   if (nvpb->pending_ref_count)
      discard_pending_refs(&nvpb->base);

   if (nvpb->stats_enabled || nvpb->log_enabled) {
      _debug_printf(
         "nouveau/switch: pushbuf stats logical=%llu native=%llu "
         "entries=%llu/%llu merged=%llu dwords=%llu "
         "completion_gpu=%llu completion_cpu=%llu upgrades=%llu "
         "kick_fail=%llu retries=%llu "
         "throttle=%llu/%llu fences=%llu waits=%llu "
         "wait_timeout=%llu wait_fail=%llu "
         "channel_errors=%llu fatal_native=%llu acquire_groups=%llu "
         "acquire_fences=%llu full_barriers=%llu bo_switches=%llu "
         "final_error=%d native_rc=0x%x "
         "device_lost=%u\n",
         (unsigned long long)nvpb->stats.logical_submits,
         (unsigned long long)nvpb->stats.native_kickoffs,
         (unsigned long long)nvpb->stats.submitted_entries,
         (unsigned long long)nvpb->stats.logical_entries,
         (unsigned long long)nvpb->stats.merged_entries,
         (unsigned long long)nvpb->stats.submitted_dwords,
         (unsigned long long)nvpb->stats.gpu_completions,
         (unsigned long long)nvpb->stats.cpu_completions,
         (unsigned long long)nvpb->stats.cpu_completion_upgrades,
         (unsigned long long)nvpb->stats.native_kickoff_failures,
         (unsigned long long)nvpb->stats.resource_retries,
         (unsigned long long)nvpb->stats.inflight_throttle_waits,
         (unsigned long long)nvpb->stats.peak_inflight_submissions,
         (unsigned long long)nvpb->stats.gallium_fences_assigned,
         (unsigned long long)nvpb->stats.native_waits,
         (unsigned long long)nvpb->stats.native_wait_timeouts,
         (unsigned long long)nvpb->stats.native_wait_failures,
         (unsigned long long)nvpb->stats.channel_errors,
         (unsigned long long)nvpb->stats.fatal_native_errors,
         (unsigned long long)nvpb->stats.external_wait_groups,
         (unsigned long long)nvpb->stats.external_wait_fences,
         (unsigned long long)nvpb->stats.full_barriers,
         (unsigned long long)nvpb->stats.command_bo_switches,
         nvpb->submission_error, nvpb->submission_result,
         nvpb->channel_lost || nvpb->fatal_native);
   }
   nouveau_bo_ref(NULL, &nvpb->bo_zcullctx);

   while (nvpb->list) {
      struct nouveau_pushbuf_krec *krec = nvpb->list;
      struct drm_nouveau_gem_pushbuf_bo *kref = krec->buffer;

      while (krec->nr_buffer--) {
         struct nouveau_bo *bo = kref++->bo;
         nouveau_switch_client_kref_set(nvpb->base.client, bo, NULL, NULL);
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
   int command_bo_index = -1;
   bool flushed = false;
   int ret = 0;

   (void)relocs;

   if (nvpb->submission_error)
      return nvpb->submission_error;

   if (push->cur + dwords >= push->end) {
      if (nvpb->bo_next < nvpb->bo_nr) {
         command_bo_index = nvpb->bo_next;
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
      if (nvpb->diagnostics_enabled)
         nvpb->stats.command_bo_switches++;
      if (nvpb->log_enabled &&
          (nvpb->stats.command_bo_switches <= 8 ||
           (nvpb->log_interval &&
            nvpb->stats.command_bo_switches % nvpb->log_interval == 0))) {
         _debug_printf("nouveau/switch: command BO switch "
                       "(count=%llu ring_index=%d ring_size=%d "
                       "pending=%u entries=%u)\n",
                       (unsigned long long)
                          nvpb->stats.command_bo_switches,
                       command_bo_index, nvpb->bo_nr,
                       nvpb->pending_submits,
                       nvpb->queued_entries);
      }

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
      pushbuf_arm_current_command_record(nvpb);
   }

   if (!pushbuf_materialize_current_command_bo(push))
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
      struct drm_nouveau_gem_pushbuf_bo *kref = bo == nvpb->bo ?
         pushbuf_materialize_current_command_bo(push) :
         nouveau_switch_client_kref_get(push->client, bo);

      if (!kref_is_current(krec, kref, bo)) {
         pushbuf_invalid_state(nvpb,
                               bo == nvpb->bo ?
                                  "current command BO epoch did not materialize" :
                                  "non-current push BO has stale kref");
         return;
      }
      if (krec->nr_push < 0 || krec->nr_push >= NOUVEAU_GEM_MAX_PUSH) {
         report_invalid_push(nvpb, "push record array exhausted");
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
   const int ret = pushbuf_refn(push, true, refs, nr);
   return ret ? pushbuf_latch_error(nouveau_switch_pushbuf(push), ret) : 0;
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
   const int ret = pushbuf_validate(push, true);
   return ret ? pushbuf_latch_error(nouveau_switch_pushbuf(push), ret) : 0;
}

uint32_t
nouveau_pushbuf_refd(struct nouveau_pushbuf *push, struct nouveau_bo *bo)
{
   uint32_t flags = 0;

   if (nouveau_switch_client_push_get(push->client, bo) == push) {
      struct drm_nouveau_gem_pushbuf_bo *kref =
         nouveau_switch_client_kref_get(push->client, bo);
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
      return pushbuf_latch_error(nvpb, ret);
   if (nvpb->incremental_refs_enabled) {
      /* Prepare the next command BO without rebuilding full residency. */
      ret = nouveau_pushbuf_space(push, 0, 0, 0);
      struct drm_nouveau_gem_pushbuf_bo *command_kref =
         nvpb->bo ?
            nouveau_switch_client_kref_get(push->client, nvpb->bo) : NULL;
      if (ret || !nvpb->bo ||
          !kref_is_current(nvpb->krec, command_kref, nvpb->bo)) {
         return ret ? pushbuf_latch_error(nvpb, ret) :
                      pushbuf_invalid_state(
                         nvpb,
                         "deferred submit lost current command BO kref");
      }
      return 0;
   }
   ret = pushbuf_validate(push, false);
   return ret ? pushbuf_latch_error(nvpb, ret) : 0;
}

int
nouveau_switch_pushbuf_kick_full_barrier(
   struct nouveau_pushbuf *push, struct nouveau_object *chan)
{
   if (push == NULL)
      return -EINVAL;

   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (nvpb->submission_error)
      return nvpb->submission_error;
   if (!nvpb->channel_ready || nvpb->hchannel == NULL)
      return pushbuf_latch_error(nvpb, -ENODEV);

   const bool temporary_channel = push->channel == NULL;
   if (temporary_channel)
      push->channel = chan;
   int ret = pushbuf_flush_logical(push);
   if (temporary_channel)
      push->channel = NULL;
   if (ret)
      return pushbuf_latch_error(nvpb, ret);

   /* Keep the barrier local with the complete exec vector.  The shared
    * backend appends both and submits them under one channel lock, so neither
    * another producer nor a completion-only fence upgrade can split this
    * ordering boundary.
    */
   nvpb->tail_queued = true;
   nvpb->full_barrier_pending = true;
   if (nvpb->diagnostics_enabled)
      nvpb->stats.full_barriers++;
   if (nvpb->log_enabled &&
       (nvpb->stats.full_barriers <= 32 ||
        (nvpb->log_interval &&
         nvpb->stats.full_barriers % nvpb->log_interval == 0))) {
      _debug_printf("nouveau/switch: queued GM20B full barrier "
                    "(count=%llu pending_submits=%u)\n",
                    (unsigned long long)nvpb->stats.full_barriers,
                    nvpb->pending_submits);
   }

   ret = pushbuf_native_kick(push);
   if (!ret)
      ret = pushbuf_validate(push, false);
   return ret ? pushbuf_latch_error(nvpb, ret) : 0;
}

int
nouveau_switch_pushbuf_enqueue_nvmultifence(struct nouveau_pushbuf *push,
                                             const NvMultiFence *waits)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (!waits || waits->num_fences > NOUVEAU_SWITCH_WAIT_FENCES)
      return -EINVAL;
   if (!waits->num_fences)
      return 0;
   if (nvpb->submission_error)
      return nvpb->submission_error;

   for (uint32_t i = 0; i < waits->num_fences; i++) {
      if ((int32_t)waits->fences[i].id < 0 ||
          waits->fences[i].id > 0x00ffffffu)
         return -EINVAL;
   }

   /* Finish recording and physically submit all older work.  The new wait
    * entry is then the first entry ahead of the next frame's command buffers.
    */
   int ret = pushbuf_flush_logical(push);
   if (!ret)
      ret = pushbuf_native_kick(push);
   if (ret)
      return pushbuf_latch_error(nvpb, ret);

   struct nouveau_horizon_fence horizon_waits[NOUVEAU_SWITCH_WAIT_FENCES];
   for (uint32_t i = 0; i < waits->num_fences; i++) {
      horizon_waits[i].id = waits->fences[i].id;
      horizon_waits[i].value = waits->fences[i].value;
   }

   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_enqueue_waits(
         nvpb->hchannel, waits->num_fences, horizon_waits);
   if (status != NOUVEAU_HORIZON_SUCCESS)
      return pushbuf_latch_error(
         nvpb, nouveau_switch_horizon_status_to_errno(status));

   nvpb->pending_native_prefix_entries++;
   if (nvpb->diagnostics_enabled) {
      nvpb->stats.external_wait_groups++;
      nvpb->stats.external_wait_fences += waits->num_fences;
   }
   if (nvpb->log_enabled &&
       (nvpb->stats.external_wait_groups <= 4 ||
        (nvpb->log_interval &&
         nvpb->stats.external_wait_groups % nvpb->log_interval == 0))) {
      _debug_printf("nouveau/switch: queued GPU acquire wait "
                    "(fences=%u pending_entries=%u)\n",
                    waits->num_fences, nvpb->queued_entries);
   }
   return 0;
}

bool
nouveau_switch_pushbuf_get_last_fence(struct nouveau_pushbuf *push,
                                       NvFence *out_fence,
                                       bool *cpu_visible_out)
{
   if (!push || !out_fence)
      return false;

   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (!nvpb->last_native_fence_valid)
      return false;

   *out_fence = nvpb->last_native_fence;
   if (cpu_visible_out)
      *cpu_visible_out = nvpb->last_native_fence_cpu_visible;
   return true;
}

int
nouveau_switch_pushbuf_get_cpu_fence(struct nouveau_pushbuf *push,
                                      NvFence *out_fence)
{
   if (!push || !out_fence)
      return -EINVAL;

   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (nvpb->submission_error)
      return nvpb->submission_error;

   /* Set the demand before logical flushing: a batch-limit kick reached from
    * inside the flush must already select the CPU-visible completion mode.
    */
   nvpb->cpu_completion_requested = true;
   int ret = pushbuf_flush_logical(push);
   if (!ret) {
      /* Logical flushing may have crossed a capacity/batch boundary and
       * consumed the pre-armed CPU request in an intermediate native kick.
       * Re-arm it so the final physical tail returned to the caller is always
       * CPU-visible as well.
       */
      nvpb->cpu_completion_requested = true;
      ret = pushbuf_native_kick(push);
   }
   if (ret)
      return ret;

   if (!nvpb->last_native_fence_valid ||
       !nvpb->last_native_fence_cpu_visible)
      return -ENODATA;

   *out_fence = nvpb->last_native_fence;
   return 0;
}

int
nouveau_switch_pushbuf_upgrade_physical_cpu_fence(
   struct nouveau_pushbuf *push, const NvFence *physical_fence,
   NvFence *out_fence)
{
   if (!push || !physical_fence || !out_fence ||
       (int32_t)physical_fence->id < 0)
      return -EINVAL;

   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (nvpb->submission_error)
      return nvpb->submission_error;
   if (!nvpb->channel_ready || !nvpb->hchannel)
      return pushbuf_latch_error(nvpb, -ENODEV);
   if (!nvpb->last_native_fence_valid ||
       nvpb->last_native_fence.id != physical_fence->id)
      return -ENODATA;

   /* A newer CPU-visible completion on this channel also dominates the
    * already-associated physical fence.  Reuse it without another native
    * submission.  The caller only enters this path for a fence which was
    * originally associated as GPU-only, so count the resolved upgrade.
    */
   if (nvpb->last_native_fence_cpu_visible &&
       nvpb->last_native_fence.value == physical_fence->value) {
      *out_fence = nvpb->last_native_fence;
      if (nvpb->diagnostics_enabled)
         nvpb->stats.cpu_completion_upgrades++;
      return 0;
   }

   bool already_complete = false;
   const int ret = nouveau_switch_completion_source_upgrade_cpu_fence(
      nvpb->completion_source, physical_fence, out_fence,
      &already_complete);
   if (ret)
      return ret;
   if (already_complete || (int32_t)out_fence->id < 0)
      return pushbuf_invalid_state(
         nvpb, "active render channel returned no CPU completion fence");

   /* The completion-only channel waits on the exact render fence and performs
    * the cache-clean CPU completion without consuming any queued render exec,
    * Gallium marker, or BO ownership.  last_native_fence remains the render
    * channel tail; callers replace only their exact observed logical/BO fence.
    */
   if (nvpb->diagnostics_enabled)
      nvpb->stats.cpu_completion_upgrades++;
   return 0;
}

int
nouveau_switch_pushbuf_kick_cpu(struct nouveau_pushbuf *push,
                                struct nouveau_object *chan)
{
   if (!push)
      return -EINVAL;

   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   nvpb->cpu_completion_requested = true;

   const bool temporary_channel = push->channel == NULL;
   if (temporary_channel)
      push->channel = chan;
   int ret = pushbuf_flush_logical(push);
   if (temporary_channel)
      push->channel = NULL;
   if (!ret) {
      /* Preserve the early request for any intermediate kick, then re-arm it
       * for the final tail because an intermediate submission clears it.
       */
      nvpb->cpu_completion_requested = true;
      ret = pushbuf_native_kick(push);
   }
   return ret ? pushbuf_latch_error(nvpb, ret) : 0;
}

int
nouveau_switch_pushbuf_wait_fence(struct nouveau_pushbuf *push,
                                  const NvFence *fence,
                                  uint64_t timeout_ns)
{
   if (!push)
      return -EINVAL;
   return pushbuf_wait_native_fence(nouveau_switch_pushbuf(push), fence,
                                    timeout_ns, "Gallium fence wait");
}

int
nouveau_switch_pushbuf_wait_fence_required(struct nouveau_pushbuf *push,
                                            const NvFence *fence,
                                            uint64_t timeout_ns,
                                            const char *reason)
{
   if (!push)
      return -EINVAL;

   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   const int ret = pushbuf_wait_native_fence(nvpb, fence, timeout_ns,
                                             reason);
   if (ret != -ETIMEDOUT)
      return ret;

   /* A timeout from a public GL sync is not necessarily fatal, but an
    * internal dependency which must complete before command generation can
    * proceed has no safe recovery path.  Poison the channel immediately so
    * callers cannot keep queueing work behind an unsatisfied dependency.
    */
   pushbuf_mark_fatal_native(nvpb, reason, 0);
   return pushbuf_latch_error_at(nvpb, -EIO,
                                 reason ? reason : "required native wait");
}

int
nouveau_switch_pushbuf_get_error(struct nouveau_pushbuf *push)
{
   if (!push)
      return -EINVAL;

   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (!nvpb->submission_error)
      (void)gpu_channel_check_error(nvpb, "reset-status query");
   return nvpb->submission_error;
}

int
nouveau_switch_pushbuf_enable_mapped_completion(
   struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (!nvpb || !nvpb->channel_ready || !nvpb->hchannel)
      return -EINVAL;
   if (!env_bool("NOUVEAU_SWITCH_MAPPED_COMPLETION", false))
      return 0;

   const enum nouveau_horizon_status status =
      nouveau_horizon_channel_set_mapped_completion(
         nvpb->hchannel,
         NOUVEAU_HORIZON_MAPPED_COMPLETION_GM20B_3D_SUBCHANNEL_0);
   return nouveau_switch_horizon_status_to_errno(status);
}

bool
nouveau_switch_pushbuf_channel_lost(struct nouveau_pushbuf *push)
{
   if (!push)
      return false;

   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (!nvpb->channel_lost)
      (void)gpu_channel_check_error(nvpb, "reset-status query");
   return nvpb->channel_lost;
}

bool
nouveau_switch_pushbuf_device_lost(struct nouveau_pushbuf *push)
{
   if (!push)
      return false;

   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   if (!nvpb->channel_lost && !nvpb->fatal_native)
      (void)gpu_channel_check_error(nvpb, "reset-status query");
   return nvpb->channel_lost || nvpb->fatal_native;
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

void
nouveau_switch_pushbuf_set_native_kick_notify(
   struct nouveau_pushbuf *push,
   uint32_t (*native_kick_notify)(struct nouveau_pushbuf *,
                                  const NvFence *, bool cpu_visible))
{
   nouveau_switch_pushbuf(push)->native_kick_notify = native_kick_notify;
}

void
nouveau_switch_pushbuf_set_fence_batch_notify(
   struct nouveau_pushbuf *push,
   bool (*marker_notify)(struct nouveau_pushbuf *, uint64_t *cookie_out),
   uint32_t (*native_notify)(struct nouveau_pushbuf *, const NvFence *,
                             bool cpu_visible, uint64_t cookie))
{
   struct nouveau_pushbuf_priv *nvpb = nouveau_switch_pushbuf(push);
   nvpb->fence_marker_notify = marker_notify;
   nvpb->native_fence_batch_notify = native_notify;
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
      return ret ? pushbuf_latch_error(nvpb, ret) : 0;
   }

   ret = pushbuf_flush_logical(push);
   if (!ret)
      ret = pushbuf_native_kick(push);
   if (!ret)
      ret = pushbuf_validate(push, false);
   return ret ? pushbuf_latch_error(nvpb, ret) : 0;
}
