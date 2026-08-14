/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef NOUVEAU_HORIZON_H
#define NOUVEAU_HORIZON_H 1

#include "nouveau/headers/nv_device_info.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct nouveau_horizon_channel;
struct nouveau_horizon_device;
struct nouveau_horizon_memory;
struct nouveau_horizon_runtime;
struct nouveau_horizon_va;

/* Ownership and threading:
 *
 * - Every successful get/create/import returns one strong reference; ref()
 *   adds one and put() releases one.
 * - Devices retain the runtime.  Memory, VA and channel objects retain their
 *   device.  Each active VA binding retains its memory object.
 * - Reference-count operations are thread-safe.  Operations on a single
 *   device, VA or channel are internally serialized, but callers must still
 *   keep an object reference alive for the whole call.
 * - Normal channel destruction submits pending work and waits for its final
 *   fence before releasing command storage.  If completion cannot be proven,
 *   the native channel and every backend-owned GPU-visible object are
 *   quarantined instead of being closed, unmapped or reused.  Destroying a VA
 *   unmaps all bindings before releasing the retained memory objects.
 */

enum nouveau_horizon_status {
   NOUVEAU_HORIZON_SUCCESS = 0,
   NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT = -1,
   NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY = -2,
   NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY = -3,
   NOUVEAU_HORIZON_ERROR_NOT_SUPPORTED = -4,
   NOUVEAU_HORIZON_ERROR_TIMEOUT = -5,
   NOUVEAU_HORIZON_ERROR_DEVICE_LOST = -6,
   NOUVEAU_HORIZON_ERROR_NO_SPACE = -7,
   NOUVEAU_HORIZON_ERROR_BUSY = -8,
   NOUVEAU_HORIZON_ERROR_SYSTEM = -9,
};

const char *
nouveau_horizon_status_string(enum nouveau_horizon_status status);

enum nouveau_horizon_log_level {
   NOUVEAU_HORIZON_LOG_DEBUG,
   NOUVEAU_HORIZON_LOG_INFO,
   NOUVEAU_HORIZON_LOG_WARNING,
   NOUVEAU_HORIZON_LOG_ERROR,
};

typedef void (*nouveau_horizon_log_func)(
   void *data, enum nouveau_horizon_log_level level, const char *message);

struct nouveau_horizon_logger {
   nouveau_horizon_log_func log;
   void *data;
};

/* Native error details are copied into an adapter-neutral structure.  The
 * native_result field contains the Horizon Result value for diagnostics only;
 * consumers should make decisions using status.
 */
struct nouveau_horizon_error {
   enum nouveau_horizon_status status;
   uint32_t native_result;
   uint64_t notification_timestamp;
   uint32_t notification_type;
   uint16_t notification_info;
   uint16_t notification_status;
   uint32_t channel_error_type;
   uint32_t channel_error_info[31];
};

struct nouveau_horizon_fence {
   uint32_t id;
   uint32_t value;
};

#define NOUVEAU_HORIZON_INVALID_FENCE_ID UINT32_MAX

static inline bool
nouveau_horizon_fence_is_valid(const struct nouveau_horizon_fence *fence)
{
   return fence != NULL && fence->id != NOUVEAU_HORIZON_INVALID_FENCE_ID;
}

struct nouveau_horizon_device_create_info {
   /* Serialize independent physical channel submissions by inserting a GPU
    * wait on the previous successful device submission.  This is intended as
    * a correctness bridge for clients which do not yet track per-resource
    * cross-channel dependencies.
    */
   bool total_order_channels;

   /* Enable optional diagnostics: aggregate counters, CPU-side timing and
    * informational lifecycle messages.  Retail applications leave this
    * false, so hot paths retain only state required for correctness and
    * warnings/errors.
    */
   bool enable_timing;
   struct nouveau_horizon_logger logger;
};

struct nouveau_horizon_device_properties {
   uint32_t page_size_B;
   uint32_t bind_align_B;
   uint64_t va_start;
   uint64_t va_end;
   bool has_compression;
   bool total_order_channels;
};

struct nouveau_horizon_memory_info {
   uint64_t total_B;
   uint64_t available_B;
   uint64_t allocated_B;
   uint64_t peak_allocated_B;
};

/* GM20B exposes one libnx ZBC active-slot mask for the process-wide GPU
 * table.  The 3D class consumes the inverse mask (disabled slots) through
 * both its color and depth methods.  active_slot_mask is therefore a union,
 * not independently inferred color/depth state, and slot_disable_mask must
 * be programmed into both methods.  A disabled snapshot deliberately masks
 * every hardware slot so ordinary, uncompressed clears remain correct.
 */
#define NOUVEAU_HORIZON_ZBC_SLOT_COUNT 15u

struct nouveau_horizon_zbc_state {
   uint32_t active_slot_mask;
   uint32_t slot_disable_mask;
   uint32_t queried_slot;
   bool enabled;
   uint64_t generation;
};

enum nouveau_horizon_memory_flags {
   NOUVEAU_HORIZON_MEMORY_CPU_VISIBLE = 1u << 0,
   NOUVEAU_HORIZON_MEMORY_CPU_CACHED = 1u << 1,
   NOUVEAU_HORIZON_MEMORY_GPU_CACHED = 1u << 2,
   NOUVEAU_HORIZON_MEMORY_ZERO = 1u << 3,
};

struct nouveau_horizon_memory_layout {
   bool valid;
   uint8_t pte_kind;
   uint16_t tile_mode;
};

struct nouveau_horizon_memory_create_info {
   uint64_t size_B;
   uint64_t align_B;

   /* Physical NvMap kind.  Keep this independent from the PTE kind selected
    * on a VA object.  Pitch (zero) is the normal physical backing kind even
    * for block-linear or compressed GPU mappings.
    */
   uint8_t backing_kind;
   uint32_t flags;

   /* Optional immutable interpretation of this allocation.  Exportable
    * allocations must provide it so another logical Horizon device cannot
    * reinterpret the same NvMap with a different PTE kind or tile layout.
    * Private command/data allocations which are never exported may leave
    * valid false.
    */
   struct nouveau_horizon_memory_layout layout;
};

struct nouveau_horizon_memory_import_info {
   uint32_t nvmap_id;

   /* With require_existing, imports are restricted to an NvMap already
    * registered by this process-wide Horizon runtime.  The canonical
    * metadata is reused and no caller-supplied interpretation is needed.
    * This is the safe choice for legacy handle APIs which carry an ID but no
    * complete cache/layout descriptor.
    */
   bool require_existing;

   /* Set has_metadata only when the producer contract supplies every field
    * below.  It is required when require_existing is false.  expected_size_B
    * may be zero when the kernel-reported allocation size is authoritative;
    * all other fields are exact and are checked against an existing identity.
    */
   bool has_metadata;
   uint64_t expected_size_B;
   uint8_t backing_kind;
   /* Exact cross-device GPU mapping flags.  Only GPU_CACHED is accepted for
    * imports; a newly created import wrapper has no CPU mapping, so producer
    * CPU cacheability is intentionally not guessed.
    */
   uint32_t flags;
   struct nouveau_horizon_memory_layout layout;
};

enum nouveau_horizon_va_flags {
   NOUVEAU_HORIZON_VA_FIXED = 1u << 0,
   NOUVEAU_HORIZON_VA_SPARSE = 1u << 1,
};

struct nouveau_horizon_va_create_info {
   uint64_t size_B;
   uint64_t align_B;
   uint64_t fixed_addr;
   uint32_t flags;

   /* When false, mappings inherit the memory object's physical backing kind.
    * When true, pte_kind is applied only to the GPU VA mapping.
    */
   bool has_pte_kind;
   uint8_t pte_kind;
};

enum nouveau_horizon_channel_priority {
   NOUVEAU_HORIZON_CHANNEL_PRIORITY_LOW,
   NOUVEAU_HORIZON_CHANNEL_PRIORITY_MEDIUM,
   NOUVEAU_HORIZON_CHANNEL_PRIORITY_HIGH,
};

enum nouveau_horizon_mapped_completion_mode {
   NOUVEAU_HORIZON_MAPPED_COMPLETION_DISABLED,

   /* SET_REPORT_SEMAPHORE is a 3D-class method on GM20B subchannel zero.
    * Select this only when the client guarantees that class/subchannel bind;
    * video, copy-only and otherwise untyped channels must stay disabled.
    */
   NOUVEAU_HORIZON_MAPPED_COMPLETION_GM20B_3D_SUBCHANNEL_0,
};

struct nouveau_horizon_channel_create_info {
   enum nouveau_horizon_channel_priority priority;

   /* Enable a GPU-written, CPU-uncached physical completion timeline for
    * this channel.  It removes steady-state zero-timeout native fence ioctls,
    * while native syncpoints remain authoritative for blocking waits and
    * fault detection.  Adapters may enable it only for a validated GM20B 3D
    * channel whose class is bound on subchannel zero; other engine layouts
    * must select DISABLED.
    */
   enum nouveau_horizon_mapped_completion_mode mapped_completion_mode;

   /* Bound kernel-accepted work independently of the userspace GPFIFO.
    * Zero selects the backend default (256 high / 128 low) for submissions.
    * Entry and command-byte limits are disabled when their high watermark is
    * zero, but their current and peak occupancy is still tracked.  A zero low
    * watermark derives half of its corresponding nonzero high watermark.
    */
   uint32_t inflight_submit_high_watermark;
   uint32_t inflight_submit_low_watermark;
   uint64_t inflight_entry_high_watermark;
   uint64_t inflight_entry_low_watermark;
   uint64_t inflight_command_byte_high_watermark;
   uint64_t inflight_command_byte_low_watermark;

   /* Slice used while native resource pressure is waiting for the oldest
    * accepted fence.  Zero selects the backend default (5 ms).  Every slice
    * polls native channel errors and the overall recovery watchdog remains in
    * force; this is not a busy-wait interval.
    */
   uint64_t resource_wait_slice_ns;
};

struct nouveau_horizon_exec {
   uint64_t addr;
   uint32_t size_B;
   bool incomplete;
   bool no_prefetch;
};

enum nouveau_horizon_completion_mode {
   /* Queue ordering only. */
   NOUVEAU_HORIZON_COMPLETION_GPU,

   /* Completion may be observed by the CPU or compositor and includes the
    * GM20B cache-clean syncpoint workaround.
    */
   NOUVEAU_HORIZON_COMPLETION_CPU,
};

struct nouveau_horizon_channel_stats {
   uint64_t submissions;
   uint64_t submitted_entries;
   uint64_t submitted_dwords;
   uint64_t waits_enqueued;
   uint64_t full_barriers_enqueued;
   uint64_t inflight_throttle_waits;
   uint64_t peak_inflight_submissions;
   uint64_t current_inflight_submissions;
   uint64_t current_inflight_entries;
   uint64_t current_inflight_command_bytes;
   uint64_t peak_inflight_entries;
   uint64_t peak_inflight_command_bytes;
   uint64_t inflight_retired_submissions;
   uint64_t inflight_proactive_polls;
   uint64_t inflight_proactive_retired_submissions;
   uint64_t inflight_native_pressure_polls;
   uint64_t inflight_native_pressure_retired_submissions;
   uint64_t inflight_credit_waits;
   uint64_t inflight_submission_watermark_waits;
   uint64_t inflight_entry_watermark_waits;
   uint64_t inflight_command_byte_watermark_waits;
   uint64_t resource_recovery_waits;
   uint64_t resource_retries;
   uint64_t submit_failures;

   uint64_t mapped_completion_polls;
   uint64_t mapped_completion_hits;
   uint64_t mapped_completion_retired_submissions;
   uint64_t mapped_completion_native_fallbacks;
   uint64_t mapped_completion_report_lag_events;

   uint64_t exec_calls;
   uint64_t exec_cpu_ns;
   uint64_t exec_max_cpu_ns;
   uint64_t submit_calls;
   uint64_t submit_cpu_ns;
   uint64_t submit_max_cpu_ns;
   uint64_t channel_lock_wait_ns;
   uint64_t channel_lock_max_wait_ns;
   uint64_t submit_lock_wait_ns;
   uint64_t submit_lock_max_wait_ns;
   uint64_t kickoff_calls;
   uint64_t kickoff_cpu_ns;
   uint64_t kickoff_max_cpu_ns;
   uint64_t inflight_throttle_wait_ns;
   uint64_t inflight_throttle_max_wait_ns;
   uint64_t inflight_credit_wait_ns;
   uint64_t inflight_credit_max_wait_ns;
   uint64_t inflight_submission_watermark_wait_ns;
   uint64_t inflight_entry_watermark_wait_ns;
   uint64_t inflight_command_byte_watermark_wait_ns;
   uint64_t resource_recovery_wait_ns;
   uint64_t resource_recovery_max_wait_ns;
   uint64_t inflight_native_pressure_poll_ns;
   uint64_t inflight_native_pressure_poll_max_ns;
   uint64_t mapped_completion_poll_ns;
   uint64_t mapped_completion_poll_max_ns;
   uint64_t mapped_completion_report_lag_wait_ns;
   uint64_t mapped_completion_report_lag_max_wait_ns;
};

/* Process/backend diagnostics.  These fields are populated only when
 * device_create_info::enable_timing is true.  They are intentionally not
 * correctness or lifetime state.
 */
struct nouveau_horizon_device_debug_stats {
   uint64_t memory_create_calls;
   uint64_t memory_create_failures;
   uint64_t memory_import_calls;
   uint64_t memory_import_failures;
   uint64_t native_memories_live;
   uint64_t native_memories_peak;
   /* Live native allocations grouped as <=64 KiB, <=256 KiB, <=1 MiB,
    * <=4 MiB and >4 MiB.  These bins distinguish object-count pressure from
    * byte pressure and guide any future backing-pool design.
    */
   uint64_t native_memory_live_by_size[5];
   uint64_t native_memory_created_by_size[5];
   uint64_t memory_wrappers_live;
   uint64_t memory_wrappers_peak;

   uint64_t va_create_calls;
   uint64_t va_create_failures;
   uint64_t vas_live;
   uint64_t vas_peak;
   uint64_t va_bind_calls;
   uint64_t va_bind_failures;
   uint64_t mappings_live;
   uint64_t mappings_peak;

   uint64_t fence_wait_calls;
   uint64_t fence_wait_timeouts;
   uint64_t fence_wait_failures;
   uint64_t fence_wait_ns;
   uint64_t fence_wait_max_ns;

   uint64_t cache_to_gpu_calls;
   uint64_t cache_to_gpu_bytes;
   uint64_t cache_to_gpu_ns;
   uint64_t cache_to_gpu_max_ns;
   uint64_t cache_from_gpu_calls;
   uint64_t cache_from_gpu_bytes;
   uint64_t cache_from_gpu_ns;
   uint64_t cache_from_gpu_max_ns;

   /* Process-wide ZBC state plus per-device adapter activity.  Query errors
    * disable every ZBC slot and are diagnostic only; they never imply device
    * loss.  Registration is intentionally not attempted until its public
    * value/format contract is independently established.
    */
   uint32_t zbc_active_slot_mask;
   uint32_t zbc_slot_disable_mask;
   uint32_t zbc_queried_slot;
   bool zbc_enabled;
   uint64_t zbc_generation;
   uint64_t zbc_query_calls;
   uint64_t zbc_query_failures;
   uint64_t zbc_state_changes;
   uint64_t zbc_refresh_calls;
   uint64_t zbc_programs;
   uint64_t zbc_add_failures;
};

/* The runtime is a process-wide, reference-counted libnx service guard. */
enum nouveau_horizon_status
nouveau_horizon_runtime_get(struct nouveau_horizon_runtime **runtime_out);

struct nouveau_horizon_runtime *
nouveau_horizon_runtime_ref(struct nouveau_horizon_runtime *runtime);

void
nouveau_horizon_runtime_put(struct nouveau_horizon_runtime *runtime);

/* Fill all GM20B information which is independent of a logical address
 * space.  This call does not acquire libnx services.
 */
void
nouveau_horizon_get_gm20b_info(struct nv_device_info *info_out);

enum nouveau_horizon_status
nouveau_horizon_device_create(
   struct nouveau_horizon_runtime *runtime,
   const struct nouveau_horizon_device_create_info *create_info,
   struct nouveau_horizon_device **device_out);

struct nouveau_horizon_device *
nouveau_horizon_device_ref(struct nouveau_horizon_device *device);

void
nouveau_horizon_device_put(struct nouveau_horizon_device *device);

const struct nv_device_info *
nouveau_horizon_device_get_info(struct nouveau_horizon_device *device);

void
nouveau_horizon_device_get_properties(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_device_properties *properties_out);

void
nouveau_horizon_device_get_memory_info(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_memory_info *memory_info_out);

void
nouveau_horizon_device_get_debug_stats(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_device_debug_stats *stats_out);

/* Copy the process-wide ZBC snapshot.  Adapters may call this at cheap 3D
 * work boundaries and reprogram both disable-mask methods only when the
 * generation differs from their channel-local cached generation.
 */
void
nouveau_horizon_device_get_zbc_state(
   struct nouveau_horizon_device *device,
   struct nouveau_horizon_zbc_state *state_out);

/* Single atomic load for hot paths.  Fetch the full seqlock snapshot only
 * when this value differs from the channel-local generation.
 */
uint64_t
nouveau_horizon_device_get_zbc_generation(
   struct nouveau_horizon_device *device);

/* Re-query the public libnx active-slot mask and publish a new generation
 * when its effective state changes.  Query failure publishes the safe
 * all-disabled state and is non-fatal to the device.
 */
void
nouveau_horizon_device_refresh_zbc_state(
   struct nouveau_horizon_device *device);

/* Adapter telemetry hook called after both 3D disable-mask methods have been
 * emitted for a channel generation.
 */
void
nouveau_horizon_device_record_zbc_program(
   struct nouveau_horizon_device *device);

enum nouveau_horizon_status
nouveau_horizon_memory_create(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_memory_create_info *create_info,
   struct nouveau_horizon_memory **memory_out);

/* Import an NvMap ID owned by another Horizon object.  A new import wrapper is
 * not CPU mappable; same-device canonical reuse may return the producer's
 * existing CPU-mappable wrapper.  NvMap ownership and physical/cache/PTE/tile
 * metadata are canonical process-wide, while each logical device keeps its
 * own lightweight wrapper for VA binding.  An unknown ID is accepted only with
 * a complete explicit descriptor; otherwise require_existing rejects it.
 */
enum nouveau_horizon_status
nouveau_horizon_memory_import(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_memory_import_info *import_info,
   struct nouveau_horizon_memory **memory_out);

struct nouveau_horizon_memory *
nouveau_horizon_memory_ref(struct nouveau_horizon_memory *memory);

void
nouveau_horizon_memory_put(struct nouveau_horizon_memory *memory);

enum nouveau_horizon_status
nouveau_horizon_memory_map(struct nouveau_horizon_memory *memory,
                           void **map_out);

void
nouveau_horizon_memory_sync_to_gpu(
   struct nouveau_horizon_memory *memory,
   uint64_t offset_B, uint64_t range_B);

void
nouveau_horizon_memory_sync_from_gpu(
   struct nouveau_horizon_memory *memory,
   uint64_t offset_B, uint64_t range_B);

/* Cached memory synchronization contract:
 *
 * - each requested range is expanded outwards to the device's non-coherent
 *   atom size (128 bytes on GM20B), which also covers CPU cache-line rounding;
 * - callers own the whole expanded atom range: sync_to_gpu must be called
 *   after CPU writes and before any GPU access, and no CPU thread may access
 *   any byte in those atoms while GPU work is in flight;
 * - after GPU writes complete, sync_from_gpu makes them CPU-visible.
 *
 * This ordering also guarantees that the conservative cache-flush fallback
 * used on Horizon systems without the invalidate-cache SVC cannot write stale
 * dirty CPU lines over completed GPU results.  Coherently mapped clients
 * should allocate CPU-uncached memory instead.
 */

uint64_t
nouveau_horizon_memory_get_size(struct nouveau_horizon_memory *memory);

uint8_t
nouveau_horizon_memory_get_backing_kind(
   struct nouveau_horizon_memory *memory);

void
nouveau_horizon_memory_get_layout(
   struct nouveau_horizon_memory *memory,
   struct nouveau_horizon_memory_layout *layout_out);

uint32_t
nouveau_horizon_memory_get_nvmap_handle(
   struct nouveau_horizon_memory *memory);

uint32_t
nouveau_horizon_memory_get_nvmap_id(struct nouveau_horizon_memory *memory);

/* Publish this memory's complete process-wide identity before returning its
 * NvMap ID for export.  Internal callers which only compare an already-owned
 * allocation should use nouveau_horizon_memory_get_nvmap_id().
 */
uint32_t
nouveau_horizon_memory_export_nvmap_id(
   struct nouveau_horizon_memory *memory);

enum nouveau_horizon_status
nouveau_horizon_va_create(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_va_create_info *create_info,
   struct nouveau_horizon_va **va_out);

struct nouveau_horizon_va *
nouveau_horizon_va_ref(struct nouveau_horizon_va *va);

void
nouveau_horizon_va_put(struct nouveau_horizon_va *va);

uint64_t
nouveau_horizon_va_get_addr(struct nouveau_horizon_va *va);

uint64_t
nouveau_horizon_va_get_size(struct nouveau_horizon_va *va);

enum nouveau_horizon_status
nouveau_horizon_va_bind(struct nouveau_horizon_va *va,
                        uint64_t va_offset_B,
                        struct nouveau_horizon_memory *memory,
                        uint64_t memory_offset_B,
                        uint64_t range_B);

enum nouveau_horizon_status
nouveau_horizon_va_unbind(struct nouveau_horizon_va *va,
                          uint64_t va_offset_B,
                          uint64_t range_B);

enum nouveau_horizon_status
nouveau_horizon_channel_create(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_channel_create_info *create_info,
   struct nouveau_horizon_channel **channel_out);

/* A channel reference may own work which still reads adapter-managed command
 * or residency storage.  Callers releasing that storage must only do so after
 * PUT_COMPLETE.  PUT_RETAINED means another channel reference remains and its
 * completion is not established by this call.  PUT_QUARANTINED means final
 * native teardown could not prove completion; the backend intentionally leaks
 * the channel and its own GPU-visible storage and marks the device lost.
 */
enum nouveau_horizon_channel_put_result {
   NOUVEAU_HORIZON_CHANNEL_PUT_COMPLETE,
   NOUVEAU_HORIZON_CHANNEL_PUT_RETAINED,
   NOUVEAU_HORIZON_CHANNEL_PUT_QUARANTINED,
};

struct nouveau_horizon_channel *
nouveau_horizon_channel_ref(struct nouveau_horizon_channel *channel);

enum nouveau_horizon_channel_put_result
nouveau_horizon_channel_put(struct nouveau_horizon_channel *channel);

enum nouveau_horizon_status
nouveau_horizon_channel_bind_zcull(
   struct nouveau_horizon_channel *channel, uint64_t addr);

enum nouveau_horizon_status
nouveau_horizon_channel_enqueue_waits(
   struct nouveau_horizon_channel *channel,
   uint32_t wait_count,
   const struct nouveau_horizon_fence *waits);

/* Enqueue a GM20B full engine barrier.  This is the expensive host-WFI path
 * used at rare cross-engine visibility boundaries: GPFIFO SET_REFERENCE is
 * isolated in its own entry, a 3D no-op begins the following entry, and the
 * shared L2/shader/descriptor acquire sequence is signed off with
 * NO_PREFETCH.  All commands already queued on this channel are ordered
 * before commands enqueued after this call.
 */
enum nouveau_horizon_status
nouveau_horizon_channel_full_barrier(
   struct nouveau_horizon_channel *channel);

enum nouveau_horizon_status
nouveau_horizon_channel_exec(
   struct nouveau_horizon_channel *channel,
   uint32_t exec_count,
   const struct nouveau_horizon_exec *execs);

/* Append all execs, optionally append a full engine barrier, and submit the
 * resulting batch while holding the channel mutex continuously.  This is the
 * transaction-safe form for adapters which must prevent another producer on
 * the shared channel from inserting or submitting work between their command
 * entries and completion fence.
 */
enum nouveau_horizon_status
nouveau_horizon_channel_exec_submit(
   struct nouveau_horizon_channel *channel,
   uint32_t exec_count,
   const struct nouveau_horizon_exec *execs,
   bool full_barrier,
   enum nouveau_horizon_completion_mode completion_mode,
   struct nouveau_horizon_fence *fence_out);

enum nouveau_horizon_status
nouveau_horizon_channel_submit(
   struct nouveau_horizon_channel *channel,
   enum nouveau_horizon_completion_mode completion_mode,
   struct nouveau_horizon_fence *fence_out);

/* Change mapped completion only on a pristine channel, before any command or
 * wait has been enqueued.  This lets ABI-constrained adapters declare their
 * engine binding immediately after constructing a private channel.
 */
enum nouveau_horizon_status
nouveau_horizon_channel_set_mapped_completion(
   struct nouveau_horizon_channel *channel,
   enum nouveau_horizon_mapped_completion_mode mode);

enum nouveau_horizon_status
nouveau_horizon_channel_wait_idle(
   struct nouveau_horizon_channel *channel, uint64_t timeout_ns);

/* Wait for a fence with channel-local mapped-completion acceleration when
 * the fence belongs to this channel.  Foreign or untracked fences fall back
 * to the native syncpoint path.  A zero timeout is a nonblocking poll.
 */
enum nouveau_horizon_status
nouveau_horizon_channel_fence_wait(
   struct nouveau_horizon_channel *channel,
   const struct nouveau_horizon_fence *fence,
   uint64_t timeout_ns);

enum nouveau_horizon_status
nouveau_horizon_channel_get_error(
   struct nouveau_horizon_channel *channel,
   struct nouveau_horizon_error *error_out);

void
nouveau_horizon_channel_get_stats(
   struct nouveau_horizon_channel *channel,
   struct nouveau_horizon_channel_stats *stats_out);

enum nouveau_horizon_status
nouveau_horizon_fence_wait(
   struct nouveau_horizon_device *device,
   const struct nouveau_horizon_fence *fence,
   uint64_t timeout_ns);

#ifdef __cplusplus
}
#endif

#endif /* NOUVEAU_HORIZON_H */
