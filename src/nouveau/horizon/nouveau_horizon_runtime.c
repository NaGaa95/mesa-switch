/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nouveau_horizon_private.h"

#include "util/u_debug.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static simple_mtx_t nouveau_horizon_runtime_mutex = SIMPLE_MTX_INITIALIZER;
static struct nouveau_horizon_runtime nouveau_horizon_runtime
   __attribute__((aligned(64)));

static uint32_t
nouveau_horizon_zbc_full_slot_mask(void)
{
   return (UINT32_C(1) << NOUVEAU_HORIZON_ZBC_SLOT_COUNT) - 1u;
}

void
nouveau_horizon_runtime_get_zbc_state(
   struct nouveau_horizon_runtime *runtime,
   struct nouveau_horizon_zbc_state *state_out)
{
   uint32_t sequence_before;
   uint32_t sequence_after;

   assert(runtime != NULL);
   assert(state_out != NULL);

   /* zbc_mutex serializes writers; an odd sequence brackets atomic
    * updates. Lock-free readers retry if a refresh overlaps their
    * snapshot.
    */
   for (;;) {
      sequence_before = p_atomic_read(&runtime->zbc_sequence);
      if (sequence_before & 1u)
         continue;

      state_out->active_slot_mask =
         p_atomic_read(&runtime->zbc_state.active_slot_mask);
      state_out->slot_disable_mask =
         p_atomic_read(&runtime->zbc_state.slot_disable_mask);
      state_out->queried_slot =
         p_atomic_read(&runtime->zbc_state.queried_slot);
      state_out->enabled = p_atomic_read(&runtime->zbc_state.enabled);
      state_out->generation =
         p_atomic_read(&runtime->zbc_state.generation);
      sequence_after = p_atomic_read(&runtime->zbc_sequence);
      if (sequence_before == sequence_after && !(sequence_after & 1u))
         break;
   }
}

void
nouveau_horizon_runtime_refresh_zbc(
   struct nouveau_horizon_runtime *runtime)
{
   uint32_t queried_slot = UINT32_MAX;
   uint32_t active_mask = 0;

   assert(runtime != NULL);

   simple_mtx_lock(&runtime->zbc_mutex);
   runtime->zbc_query_calls++;

   const Result rc =
      nvGpuZbcGetActiveSlotMask(&queried_slot, &active_mask);
   const uint32_t full_mask = nouveau_horizon_zbc_full_slot_mask();
   const bool valid = R_SUCCEEDED(rc) &&
                      queried_slot < NOUVEAU_HORIZON_ZBC_SLOT_COUNT &&
                      (active_mask & ~full_mask) == 0;

   if (!valid) {
      runtime->zbc_query_failures++;
      queried_slot = UINT32_MAX;
      active_mask = 0;
   }

   const bool enabled = valid && active_mask != 0;
   const uint32_t disable_mask =
      enabled ? full_mask & ~active_mask : full_mask;
   const uint64_t old_generation =
      p_atomic_read(&runtime->zbc_state.generation);
   uint64_t generation = old_generation;
   if (old_generation == 0 ||
       p_atomic_read(&runtime->zbc_state.enabled) != enabled ||
       p_atomic_read(&runtime->zbc_state.active_slot_mask) != active_mask ||
       p_atomic_read(&runtime->zbc_state.slot_disable_mask) != disable_mask) {
      generation++;
      runtime->zbc_state_changes++;
   }

   p_atomic_inc(&runtime->zbc_sequence);
   p_atomic_set(&runtime->zbc_state.enabled, enabled);
   p_atomic_set(&runtime->zbc_state.queried_slot, queried_slot);
   p_atomic_set(&runtime->zbc_state.active_slot_mask, active_mask);
   p_atomic_set(&runtime->zbc_state.slot_disable_mask, disable_mask);
   p_atomic_set(&runtime->zbc_state.generation, generation);
   p_atomic_inc(&runtime->zbc_sequence);
   simple_mtx_unlock(&runtime->zbc_mutex);
}

static bool
nouveau_horizon_result_is_timeout(Result rc)
{
   if (R_VALUE(rc) == R_VALUE(KERNELRESULT(TimedOut)))
      return true;

   switch (R_MODULE(rc)) {
   case Module_Libnx:
      return R_DESCRIPTION(rc) == LibnxError_Timeout;
   case Module_LibnxNvidia:
      return R_DESCRIPTION(rc) == LibnxNvidiaError_Timeout;
   case Module_LibnxBinder:
      return R_DESCRIPTION(rc) == LibnxBinderError_TimedOut;
   default:
      return false;
   }
}

enum nouveau_horizon_status
nouveau_horizon_status_from_result(Result rc,
                                   enum nouveau_horizon_status fallback)
{
   if (R_SUCCEEDED(rc))
      return NOUVEAU_HORIZON_SUCCESS;

   if (nouveau_horizon_result_is_timeout(rc))
      return NOUVEAU_HORIZON_ERROR_TIMEOUT;

   if (rc == MAKERESULT(Module_LibnxNvidia,
                        LibnxNvidiaError_InsufficientMemory))
      return NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY;

   return fallback;
}

const char *
nouveau_horizon_status_string(enum nouveau_horizon_status status)
{
   switch (status) {
   case NOUVEAU_HORIZON_SUCCESS:
      return "success";
   case NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT:
      return "invalid argument";
   case NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY:
      return "out of host memory";
   case NOUVEAU_HORIZON_ERROR_OUT_OF_DEVICE_MEMORY:
      return "out of device memory";
   case NOUVEAU_HORIZON_ERROR_NOT_SUPPORTED:
      return "not supported";
   case NOUVEAU_HORIZON_ERROR_TIMEOUT:
      return "timeout";
   case NOUVEAU_HORIZON_ERROR_DEVICE_LOST:
      return "device lost";
   case NOUVEAU_HORIZON_ERROR_NO_SPACE:
      return "no queue space";
   case NOUVEAU_HORIZON_ERROR_BUSY:
      return "busy";
   case NOUVEAU_HORIZON_ERROR_SYSTEM:
      return "system error";
   default:
      return "unknown error";
   }
}

void
nouveau_horizon_error_reset(struct nouveau_horizon_error *error)
{
   if (error == NULL)
      return;

   memset(error, 0, sizeof(*error));
   error->status = NOUVEAU_HORIZON_SUCCESS;
}

void
nouveau_horizon_error_set(struct nouveau_horizon_error *error,
                          enum nouveau_horizon_status status,
                          Result native_result)
{
   if (error == NULL)
      return;

   error->status = status;
   error->native_result = R_VALUE(native_result);
}

void
nouveau_horizon_log(struct nouveau_horizon_device *device,
                    enum nouveau_horizon_log_level level,
                    const char *format, ...)
{
   /* Production keeps warnings and errors, while verbose lifecycle and
    * performance messages are enabled together with timing diagnostics.
    */
   if (level <= NOUVEAU_HORIZON_LOG_INFO &&
       (device == NULL || !device->enable_timing))
      return;

   char message[512];
   va_list args;

   va_start(args, format);
   vsnprintf(message, sizeof(message), format, args);
   va_end(args);

   if (device != NULL && device->logger.log != NULL) {
      device->logger.log(device->logger.data, level, message);
      return;
   }

   _debug_printf("nouveau/horizon: %s\n", message);
}

uint64_t
nouveau_horizon_align_u64(uint64_t value, uint64_t align)
{
   if (align == 0)
      return value;

   const uint64_t remainder = value % align;
   if (remainder == 0)
      return value;

   const uint64_t add = align - remainder;
   if (value > UINT64_MAX - add)
      return 0;

   return value + add;
}

enum nouveau_horizon_status
nouveau_horizon_runtime_get(struct nouveau_horizon_runtime **runtime_out)
{
   Result rc = 0;

   if (runtime_out == NULL)
      return NOUVEAU_HORIZON_ERROR_INVALID_ARGUMENT;

   *runtime_out = NULL;
   simple_mtx_lock(&nouveau_horizon_runtime_mutex);

   if (nouveau_horizon_runtime.refcnt == 0) {
      rc = nvInitialize();
      if (R_FAILED(rc))
         goto fail_nv;

      rc = nvFenceInit();
      if (R_FAILED(rc))
         goto fail_fence;

      rc = nvMapInit();
      if (R_FAILED(rc))
         goto fail_map;

      rc = nvGpuInit();
      if (R_FAILED(rc))
         goto fail_gpu;

      simple_mtx_init(&nouveau_horizon_runtime.zbc_mutex, mtx_plain);
      nouveau_horizon_runtime.zbc_sequence = 0;
      nouveau_horizon_runtime.zbc_state =
         (struct nouveau_horizon_zbc_state) {0};
      nouveau_horizon_runtime.zbc_query_calls = 0;
      nouveau_horizon_runtime.zbc_query_failures = 0;
      nouveau_horizon_runtime.zbc_state_changes = 0;
      nouveau_horizon_runtime.zbc_refresh_calls = 0;
      nouveau_horizon_runtime.zbc_add_failures = 0;
      nouveau_horizon_runtime_refresh_zbc(&nouveau_horizon_runtime);

      simple_mtx_init(&nouveau_horizon_runtime.memory_identity_mutex,
                      mtx_plain);
      nouveau_horizon_runtime.memory_identities =
         _mesa_hash_table_u64_create(NULL);
      if (nouveau_horizon_runtime.memory_identities == NULL) {
         simple_mtx_destroy(&nouveau_horizon_runtime.memory_identity_mutex);
         simple_mtx_destroy(&nouveau_horizon_runtime.zbc_mutex);
         nvGpuExit();
         nvMapExit();
         nvFenceExit();
         nvExit();
         simple_mtx_unlock(&nouveau_horizon_runtime_mutex);
         return NOUVEAU_HORIZON_ERROR_OUT_OF_HOST_MEMORY;
      }
      nouveau_horizon_runtime.initialized = true;
   }

   nouveau_horizon_runtime.refcnt++;
   *runtime_out = &nouveau_horizon_runtime;
   simple_mtx_unlock(&nouveau_horizon_runtime_mutex);
   return NOUVEAU_HORIZON_SUCCESS;

fail_gpu:
   nvMapExit();
fail_map:
   nvFenceExit();
fail_fence:
   nvExit();
fail_nv:
   simple_mtx_unlock(&nouveau_horizon_runtime_mutex);
   return nouveau_horizon_status_from_result(
      rc, NOUVEAU_HORIZON_ERROR_SYSTEM);
}

struct nouveau_horizon_runtime *
nouveau_horizon_runtime_ref(struct nouveau_horizon_runtime *runtime)
{
   if (runtime == NULL)
      return NULL;

   simple_mtx_lock(&nouveau_horizon_runtime_mutex);
   assert(runtime == &nouveau_horizon_runtime);
   assert(runtime->initialized && runtime->refcnt > 0);
   runtime->refcnt++;
   simple_mtx_unlock(&nouveau_horizon_runtime_mutex);
   return runtime;
}

void
nouveau_horizon_runtime_put(struct nouveau_horizon_runtime *runtime)
{
   if (runtime == NULL)
      return;

   simple_mtx_lock(&nouveau_horizon_runtime_mutex);
   assert(runtime == &nouveau_horizon_runtime);
   assert(runtime->refcnt > 0);

   runtime->refcnt--;
   if (runtime->refcnt == 0) {
      assert(runtime->initialized);
      assert(_mesa_hash_table_u64_num_entries(runtime->memory_identities) ==
             0);
      _mesa_hash_table_u64_destroy(runtime->memory_identities);
      runtime->memory_identities = NULL;
      simple_mtx_destroy(&runtime->memory_identity_mutex);
      simple_mtx_destroy(&runtime->zbc_mutex);
      nvGpuExit();
      nvMapExit();
      nvFenceExit();
      nvExit();
      runtime->initialized = false;
   }

   simple_mtx_unlock(&nouveau_horizon_runtime_mutex);
}
