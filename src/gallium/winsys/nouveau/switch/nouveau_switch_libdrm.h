/*
 * Copyright 2026 Mesa Switch contributors
 * SPDX-License-Identifier: MIT
 *
 * Mesa-owned implementation details for the public nouveau.h compatibility
 * adapter.  Nothing in this file describes, or depends on, the layout of an
 * external libdrm_nouveau object.
 */

#ifndef NOUVEAU_SWITCH_LIBDRM_H
#define NOUVEAU_SWITCH_LIBDRM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <switch.h>

#include "nouveau.h"
#include "nouveau_drm.h"
#include "util/simple_mtx.h"
#include "util/u_atomic.h"

struct nouveau_horizon_device;
struct nouveau_horizon_channel;
struct nouveau_horizon_memory;
struct nouveau_horizon_va;
enum nouveau_horizon_status;
struct nv_device_info;
struct nouveau_switch_bo_pool;
struct nouveau_switch_completion_source;

/* Horizon cache-policy extension for NOUVEAU_BO_COHERENT allocations.
 *
 * NOUVEAU_BO_COHERENT selects CPU-uncached storage in the Switch adapter.
 * Small command and synchronization allocations are GPU-uncached as well.
 * Streaming GART data instead wants deko3D's proven default: CPU-uncached but
 * GPU-cached, with a channel cache-acquire before execution.  This private bit
 * requests that second policy without changing public libdrm_nouveau flags.
 */
#define NOUVEAU_BO_SWITCH_GPU_CACHED 0x08000000u

struct nouveau_switch_bo_pool_stats {
   uint64_t allocation_calls;
   uint64_t allocation_fallbacks;
   uint64_t chunks;
   uint64_t live_slices;
   uint64_t peak_slices;
   uint64_t live_requested_B;
   uint64_t peak_requested_B;
   uint64_t reserved_B;
};

#define NOUVEAU_SWITCH_BO_MAP_BUCKETS 31u

struct nouveau_switch_client_bo_entry {
   struct nouveau_switch_client_bo_entry *next;
   struct nouveau_switch_client_bo_entry **prev_next;
   struct drm_nouveau_gem_pushbuf_bo *kref;
   struct nouveau_pushbuf *push;
   struct nouveau_bo *bo;
};

struct nouveau_switch_client_bo_map {
   /* The last bucket is a free-list. */
   struct nouveau_switch_client_bo_entry
      *buckets[NOUVEAU_SWITCH_BO_MAP_BUCKETS + 1];
};

struct nouveau_switch_client {
   struct nouveau_client base;
   simple_mtx_t lock;
   struct nouveau_switch_client_bo_map bo_map;
};

struct nouveau_switch_bo {
   struct nouveau_bo base;
   int refcnt;
   simple_mtx_t lock;
   struct nouveau_horizon_memory *memory;
   struct nouveau_horizon_va *va;
   struct nouveau_switch_bo_pool *pool;
   uint64_t memory_offset_B;
   uint64_t allocation_size_B;
   NvFence fence;
   struct nouveau_switch_completion_source *completion_source;
   uint32_t access;
   int cpu_dirty;
   int cache_to_gpu_calls;
   bool gpu_dirty;
   bool fence_cpu_visible;
   bool registered;
   bool quarantined;
   struct nouveau_switch_bo *next_named;
};

struct nouveau_switch_device {
   struct nouveau_device base;
   int refcnt;
   simple_mtx_t lock;
   struct nouveau_horizon_device *hdev;
   struct nouveau_switch_bo *named_bos;
   struct nouveau_switch_bo_pool *bo_pools;
   struct nouveau_switch_bo_pool_stats pool_stats;
   uint32_t next_client_id;
   bool diagnostics_enabled;
   bool perf_enabled;
};

static inline struct nouveau_switch_client *
nouveau_switch_client(struct nouveau_client *client)
{
   return (struct nouveau_switch_client *)client;
}

static inline struct nouveau_switch_bo *
nouveau_switch_bo(struct nouveau_bo *bo)
{
   return (struct nouveau_switch_bo *)bo;
}

static inline const struct nouveau_switch_bo *
nouveau_switch_bo_const(const struct nouveau_bo *bo)
{
   return (const struct nouveau_switch_bo *)bo;
}

static inline struct nouveau_switch_device *
nouveau_switch_device(struct nouveau_device *dev)
{
   return (struct nouveau_switch_device *)dev;
}

static inline const struct nouveau_switch_device *
nouveau_switch_device_const(const struct nouveau_device *dev)
{
   return (const struct nouveau_switch_device *)dev;
}

void nouveau_switch_client_map_finish(struct nouveau_client *client);

struct drm_nouveau_gem_pushbuf_bo *
nouveau_switch_client_kref_get(struct nouveau_client *client,
                               struct nouveau_bo *bo);

struct nouveau_pushbuf *
nouveau_switch_client_push_get(struct nouveau_client *client,
                               struct nouveau_bo *bo);

int nouveau_switch_client_kref_set(
   struct nouveau_client *client, struct nouveau_bo *bo,
   struct drm_nouveau_gem_pushbuf_bo *kref, struct nouveau_pushbuf *push);

struct nouveau_switch_completion_source *
nouveau_switch_completion_source_create(
   struct nouveau_horizon_channel *channel,
   struct nouveau_horizon_device *device);
struct nouveau_switch_completion_source *
nouveau_switch_completion_source_ref(
   struct nouveau_switch_completion_source *source);
void nouveau_switch_completion_source_unref(
   struct nouveau_switch_completion_source *source);
void nouveau_switch_completion_source_retire(
   struct nouveau_switch_completion_source *source, bool complete);
int nouveau_switch_completion_source_upgrade_cpu_fence(
   struct nouveau_switch_completion_source *source,
   const NvFence *physical_fence, NvFence *fence_out,
   bool *already_complete_out);
void nouveau_switch_bo_record_submission(
   struct nouveau_bo *bo, const NvFence *fence, uint32_t access,
   struct nouveau_switch_completion_source *source, bool cpu_visible);
void nouveau_switch_bo_clear_submission(struct nouveau_bo *bo);
int nouveau_switch_bo_sync_to_gpu(struct nouveau_bo *bo);
void nouveau_switch_bo_mark_cpu_dirty(struct nouveau_bo *bo);

int nouveau_switch_bo_name_ref_explicit(
   struct nouveau_device *device, uint32_t name, uint64_t expected_size_B,
   uint8_t backing_kind, bool gpu_cacheable, uint8_t pte_kind,
   uint16_t tile_mode, struct nouveau_bo **out);

struct nouveau_horizon_device *
nouveau_switch_device_get_horizon(struct nouveau_device *device);

const struct nv_device_info *
nouveau_switch_device_get_info(const struct nouveau_device *device);

void nouveau_switch_device_get_memory_info(
   const struct nouveau_device *device, uint64_t *total_B,
   uint64_t *available_B, uint64_t *allocated_B);

void nouveau_switch_device_get_pool_stats(
   const struct nouveau_device *device,
   struct nouveau_switch_bo_pool_stats *stats);

int
nouveau_switch_horizon_status_to_errno(enum nouveau_horizon_status status);

/*
 * Keep the compatibility-list operations single-evaluation.  In particular,
 * callers append with an expression such as "head.prev".  Re-evaluating that
 * expression after changing head.prev makes an empty-list append self-link the
 * new node without linking it from the head, and corrupts subsequent appends.
 */
static inline void
nouveau_switch_list_init(struct nouveau_list *item)
{
   item->prev = item;
   item->next = item;
}

static inline void
nouveau_switch_list_add(struct nouveau_list *item, struct nouveau_list *after)
{
   struct nouveau_list *next = after->next;

   item->prev = after;
   item->next = next;
   next->prev = item;
   after->next = item;
}

static inline void
nouveau_switch_list_del(struct nouveau_list *item)
{
   struct nouveau_list *prev = item->prev;
   struct nouveau_list *next = item->next;

   prev->next = next;
   next->prev = prev;
}

static inline void
nouveau_switch_list_del_init(struct nouveau_list *item)
{
   nouveau_switch_list_del(item);
   nouveau_switch_list_init(item);
}

static inline bool
nouveau_switch_list_empty(const struct nouveau_list *item)
{
   return item->next == item;
}

static inline void
nouveau_switch_list_join(struct nouveau_list *list,
                         struct nouveau_list *join)
{
   if (!nouveau_switch_list_empty(list)) {
      struct nouveau_list *first = list->next;
      struct nouveau_list *last = list->prev;
      struct nouveau_list *join_next = join->next;

      first->prev = join;
      last->next = join_next;
      join_next->prev = last;
      join->next = first;
   }
}

#define NS_LIST_INIT(item) nouveau_switch_list_init(item)
#define NS_LIST_ADD(item, list) nouveau_switch_list_add(item, list)
#define NS_LIST_DEL(item) nouveau_switch_list_del(item)
#define NS_LIST_DEL_INIT(item) nouveau_switch_list_del_init(item)

#define NS_LIST_ENTRY(type, item, field)                                     \
   ((type *)((char *)(item) - offsetof(type, field)))

#define NS_LIST_EMPTY(item) nouveau_switch_list_empty(item)

#define NS_LIST_FOR_EACH_ENTRY(item, list, field)                            \
   for ((item) = NS_LIST_ENTRY(__typeof__(*(item)), (list)->next, field);     \
        &(item)->field != (list);                                             \
        (item) = NS_LIST_ENTRY(__typeof__(*(item)), (item)->field.next,       \
                               field))

#define NS_LIST_FOR_EACH_ENTRY_SAFE(item, temp, list, field)                 \
   for ((item) = NS_LIST_ENTRY(__typeof__(*(item)), (list)->next, field),     \
        (temp) = NS_LIST_ENTRY(__typeof__(*(item)), (item)->field.next,       \
                               field);                                        \
        &(item)->field != (list);                                             \
        (item) = (temp),                                                       \
        (temp) = NS_LIST_ENTRY(__typeof__(*(item)), (temp)->field.next,       \
                               field))

#define NS_LIST_JOIN(list, join) nouveau_switch_list_join(list, join)

#endif /* NOUVEAU_SWITCH_LIBDRM_H */
