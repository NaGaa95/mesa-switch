/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef NOUVEAU_HORIZON_GM20B_H
#define NOUVEAU_HORIZON_GM20B_H 1

#include "nouveau_horizon.h"

#include <stdint.h>

#define NOUVEAU_HORIZON_GM20B_CACHE_ACQUIRE_WORDS 8u
#define NOUVEAU_HORIZON_GM20B_FULL_BARRIER_WORDS 1u
#define NOUVEAU_HORIZON_GM20B_FULL_BARRIER_ACQUIRE_WORDS \
   (1u + NOUVEAU_HORIZON_GM20B_CACHE_ACQUIRE_WORDS)
#define NOUVEAU_HORIZON_GM20B_FENCE_GPU_WORDS 3u
#define NOUVEAU_HORIZON_GM20B_FENCE_CPU_WORDS 5u
#define NOUVEAU_HORIZON_GM20B_REPORT_WORDS 6u
#define NOUVEAU_HORIZON_GM20B_WAIT_WORDS_PER_FENCE 3u

uint32_t
nouveau_horizon_gm20b_build_cache_acquire(uint32_t *commands);

uint32_t
nouveau_horizon_gm20b_build_full_barrier(uint32_t *commands);

uint32_t
nouveau_horizon_gm20b_build_full_barrier_acquire(uint32_t *commands);

uint32_t
nouveau_horizon_gm20b_build_fence(
   uint32_t *commands, uint32_t syncpoint_id,
   enum nouveau_horizon_completion_mode completion_mode);

uint32_t
nouveau_horizon_gm20b_build_report(
   uint32_t *commands, uint64_t report_addr, uint32_t report_value);

uint32_t
nouveau_horizon_gm20b_build_waits(
   uint32_t *commands, uint32_t wait_count,
   const struct nouveau_horizon_fence *waits);

#endif /* NOUVEAU_HORIZON_GM20B_H */
