/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */

#include "nouveau_horizon_gm20b.h"

#include "nouveau/headers/drf.h"
#include "nouveau/headers/nvidia/classes/cl906f.h"
#include "nouveau/headers/nvidia/classes/clb197.h"

#include <assert.h>

#define NOUVEAU_HORIZON_MEM_OP_L2_FLUSH_DIRTY 0x80000000u
#define NOUVEAU_HORIZON_MEM_OP_L2_SYSMEM_INVALIDATE 0x70000000u

static inline uint32_t
nouveau_horizon_gm20b_incr_header(uint32_t method, uint32_t count)
{
   return NVDEF(NV906F, DMA_INCR, OPCODE, VALUE) |
          NVVAL(NV906F, DMA_INCR, COUNT, count) |
          NVVAL(NV906F, DMA_INCR, SUBCHANNEL, 0) |
          NVVAL(NV906F, DMA_INCR, ADDRESS, method >> 2);
}

static inline uint32_t
nouveau_horizon_gm20b_immediate_header(uint32_t method, uint32_t value)
{
   assert((value & ~DRF_MASK(NV906F_DMA_IMMD_DATA)) == 0);
   return NVDEF(NV906F, DMA_IMMD, OPCODE, VALUE) |
          NVVAL(NV906F, DMA_IMMD, DATA, value) |
          NVVAL(NV906F, DMA_IMMD, SUBCHANNEL, 0) |
          NVVAL(NV906F, DMA_IMMD, ADDRESS, method >> 2);
}

uint32_t
nouveau_horizon_gm20b_build_cache_acquire(uint32_t *commands)
{
   uint32_t *cmd = commands;

   /* Use the public deko3d GM20B write-back acquire pattern.  The caller
    * follows this with a separate one-word GPFIFO entry carrying
    * NO_PREFETCH/SYNC.
    */
   *cmd++ = 0x00B | (6 << 13) | (1 << 16) | (1u << 29);
   *cmd++ = NOUVEAU_HORIZON_MEM_OP_L2_FLUSH_DIRTY;
   *cmd++ = 0x00B | (6 << 13) | (1 << 16) | (1u << 29);
   *cmd++ = NOUVEAU_HORIZON_MEM_OP_L2_SYSMEM_INVALIDATE;
   *cmd++ = 0x4A2 | (0 << 13) | (0 << 16) | (4u << 29);
   *cmd++ = 0x369 | (0 << 13) | (0x1011 << 16) | (4u << 29);
   *cmd++ = 0x50A | (0 << 13) | (0 << 16) | (4u << 29);
   *cmd++ = 0x509 | (0 << 13) | (0 << 16) | (4u << 29);

   assert((uint32_t)(cmd - commands) ==
          NOUVEAU_HORIZON_GM20B_CACHE_ACQUIRE_WORDS);
   return cmd - commands;
}

uint32_t
nouveau_horizon_gm20b_build_full_barrier(uint32_t *commands)
{
   /* Match deko3D's DkBarrier_Full exactly: an immediate SET_REFERENCE(0)
    * on the GPFIFO subchannel is isolated in one GPFIFO entry.  The host-WFI
    * takes effect when the following entry begins.
    */
   commands[0] = 0x014 | (6 << 13) | (4u << 29);

   return NOUVEAU_HORIZON_GM20B_FULL_BARRIER_WORDS;
}

uint32_t
nouveau_horizon_gm20b_build_full_barrier_acquire(uint32_t *commands)
{
   /* deko3D begins the post-SET_REFERENCE entry with 3D NO_OPERATION(0).
    * Follow it with Horizon's shared GM20B L2/shader/descriptor acquire;
    * the channel appends a final NO_PREFETCH entry after this block.
    */
   commands[0] = 0x040 | (0 << 13) | (4u << 29);
   const uint32_t acquire_words =
      nouveau_horizon_gm20b_build_cache_acquire(commands + 1);

   assert(1u + acquire_words ==
          NOUVEAU_HORIZON_GM20B_FULL_BARRIER_ACQUIRE_WORDS);
   return 1u + acquire_words;
}

uint32_t
nouveau_horizon_gm20b_build_fence(
   uint32_t *commands, uint32_t syncpoint_id,
   enum nouveau_horizon_completion_mode completion_mode)
{
   uint32_t *cmd = commands;
   const bool cpu_visible =
      completion_mode == NOUVEAU_HORIZON_COMPLETION_CPU;

   uint32_t syncpoint_increment =
      NVVAL(NVB197, INCREMENT_SYNC_POINT, INDEX, syncpoint_id) |
      NVDEF(NVB197, INCREMENT_SYNC_POINT, CONDITION, ROP_WRITES_DONE);

   /* Only a CPU or compositor read needs the GPU L2 written back. */
   if (cpu_visible)
      syncpoint_increment |=
         NVDEF(NVB197, INCREMENT_SYNC_POINT, CLEAN_L2, TRUE);

   *cmd++ = nouveau_horizon_gm20b_immediate_header(
      NVB197_FLUSH_PENDING_WRITES, 0);
   *cmd++ = nouveau_horizon_gm20b_incr_header(
      NVB197_INCREMENT_SYNC_POINT, 1);
   *cmd++ = syncpoint_increment;

   /* GM20B CPU/compositor-visible completion needs the duplicated
    * cache-clean syncpoint action used by public deko3d on this GPU.
    */
   if (cpu_visible) {
      *cmd++ = nouveau_horizon_gm20b_incr_header(
         NVB197_INCREMENT_SYNC_POINT, 1);
      *cmd++ = syncpoint_increment;
   }

   assert((uint32_t)(cmd - commands) ==
          (cpu_visible ? NOUVEAU_HORIZON_GM20B_FENCE_CPU_WORDS
                       : NOUVEAU_HORIZON_GM20B_FENCE_GPU_WORDS));
   return cmd - commands;
}

uint32_t
nouveau_horizon_gm20b_build_report(
   uint32_t *commands, uint64_t report_addr, uint32_t report_value)
{
   uint32_t *cmd = commands;

   /* Flush the preceding native syncpoint action, then release the 32-bit
    * progress value after all earlier writes complete.
    */
   *cmd++ = nouveau_horizon_gm20b_immediate_header(
      NVB197_FLUSH_PENDING_WRITES, 0);
   *cmd++ = nouveau_horizon_gm20b_incr_header(
      NVB197_SET_REPORT_SEMAPHORE_A, 4);
   *cmd++ = NVVAL(NVB197, SET_REPORT_SEMAPHORE_A, OFFSET_UPPER,
                  report_addr >> 32);
   *cmd++ = NVVAL(NVB197, SET_REPORT_SEMAPHORE_B, OFFSET_LOWER,
                  report_addr);
   *cmd++ = NVVAL(NVB197, SET_REPORT_SEMAPHORE_C, PAYLOAD, report_value);
   *cmd++ =
      NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, OPERATION, RELEASE) |
      NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, RELEASE,
            AFTER_ALL_PRECEEDING_WRITES_COMPLETE) |
      NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, PIPELINE_LOCATION, ALL) |
      NVDEF(NVB197, SET_REPORT_SEMAPHORE_D, STRUCTURE_SIZE, ONE_WORD);

   assert((uint32_t)(cmd - commands) ==
          NOUVEAU_HORIZON_GM20B_REPORT_WORDS);
   return cmd - commands;
}

uint32_t
nouveau_horizon_gm20b_build_waits(
   uint32_t *commands, uint32_t wait_count,
   const struct nouveau_horizon_fence *waits)
{
   uint32_t *cmd = commands;

   for (uint32_t i = 0; i < wait_count; i++) {
      /* SyncpointPayload followed by a wait-on-switch syncpoint packet. */
      *cmd++ = 0x01C | (0 << 13) | (2 << 16) | (1u << 29);
      *cmd++ = waits[i].value;
      *cmd++ = (1u << 4) | (waits[i].id << 8);
   }

   return cmd - commands;
}
