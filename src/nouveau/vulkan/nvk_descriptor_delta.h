/* SPDX-License-Identifier: MIT */
#ifndef NVK_DESCRIPTOR_DELTA_H
#define NVK_DESCRIPTOR_DELTA_H

#include <assert.h>
#include <stdint.h>

struct nvk_descriptor_delta {
   uint64_t changed;
   uint32_t first;
   uint32_t count;
};

static inline uint64_t
nvk_descriptor_range_mask(uint32_t first, uint32_t end)
{
   assert(first <= end && end <= 64);
   if (first == end)
      return 0;
   if (end - first == 64)
      return UINT64_MAX;
   return ((UINT64_C(1) << (end - first)) - 1) << first;
}

/* A single upload spans the changed words, including any gaps between them. */
static inline struct nvk_descriptor_delta
nvk_descriptor_delta(const uint32_t *old, const uint32_t *next,
                     uint64_t valid, uint32_t first, uint32_t end)
{
   assert(first <= end && end <= 64);
   struct nvk_descriptor_delta delta = { .first = end };
   for (uint32_t i = first; i < end; i++) {
      const uint64_t bit = UINT64_C(1) << i;
      if (old[i] != next[i])
         delta.changed |= bit;
      if ((delta.changed & bit) || !(valid & bit)) {
         if (delta.count == 0)
            delta.first = i;
         delta.count = i - delta.first + 1;
      }
   }
   return delta;
}

#endif
