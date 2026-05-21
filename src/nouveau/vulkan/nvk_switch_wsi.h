/*
 * Copyright © 2026 Mesa Switch port contributors
 * SPDX-License-Identifier: MIT
 *
 * Minimal NVK-side shim for the libnx-backed WSI. This header is
 * deliberately kept free of NVK internals so that wsi_common_switch.c,
 * which lives in src/vulkan/wsi/ and only has src/ in its include path,
 * can consume it via "nouveau/vulkan/nvk_switch_wsi.h".
 */
#ifndef NVK_SWITCH_WSI_H
#define NVK_SWITCH_WSI_H 1

#ifdef __SWITCH__

#include <stdbool.h>
#include <stdint.h>

#include <switch.h>
#include <vulkan/vulkan_core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Layout information needed to hand a VkImage/VkDeviceMemory pair to the
 * Horizon compositor via NvGraphicBuffer.
 *
 * For now we only support the GM20B scanout-compatible configuration:
 * a single-plane, non-disjoint, block-linear image whose dedicated
 * VkDeviceMemory is backed by a libnx NvMap.
 */
struct nvk_switch_scanout_layout {
   /* libnx NvMap handle owning the pixels. Borrowed — do not close. */
   NvMap *nvmap;

   /* Offset of the image inside the NvMap and total byte size of the
    * image (nil plane size, already block-aligned).
    */
   uint32_t offset_B;
   uint32_t size_B;

   /* Row stride of level 0 in bytes and in pixels.
    * NvGraphicBuffer.stride is pixels; planes[0].pitch is bytes.
    */
   uint32_t row_stride_B;
   uint32_t row_stride_px;

   /* Block height (tile Y) as log2(GOBs). Must match what NVK fed into
    * nil; the compositor uses this to deswizzle scanout reads.
    */
   uint8_t block_height_log2;

   /* Page-table kind. Must match the MMU PTE kind the driver programmed
    * or compositor scanout will read garbage. For BGRA8/RGBA8 on GM20B
    * this is NvKind_Generic_16BX2.
    */
   uint8_t pte_kind;
};

/* Populate scanout layout for a bound (image, memory) pair.
 *
 * Preconditions:
 *   - `image` and `memory` belong to the same VkDevice.
 *   - `image` is single-plane, non-disjoint, tiled (pte_kind != 0), and
 *     was allocated with a dedicated VkMemoryDedicatedAllocateInfo.
 *   - `memory` is backed by the Switch nvkmd backend.
 *
 * Returns VK_ERROR_FEATURE_NOT_PRESENT if any precondition is violated,
 * so the WSI can cleanly fall back to the CPU-blit path.
 */
VkResult nvk_switch_get_scanout_layout(VkImage image,
                                       VkDeviceMemory memory,
                                       struct nvk_switch_scanout_layout *out);

/* Extract the libnx native fence payload currently installed on a VkFence.
 * Returns true iff the fence has a real GPU-side payload attached
 * (post-submit or copied from another native sync); false if the fence is
 * unsignaled / CPU-only, in which case the caller should fall back to
 * waiting on the VkFence host-side before presenting.
 */
bool nvk_switch_fence_peek_nvmultifence(VkFence fence, NvMultiFence *out);
bool nvk_switch_fence_peek_nvfence(VkFence fence, NvFence *out);

/* Install a libnx release fence as a temporary Vulkan acquire payload.
 *
 * Returns VK_ERROR_FEATURE_NOT_PRESENT if the fence cannot be represented by
 * the Switch-native sync type, allowing the caller to fall back to a CPU wait
 * plus dummy sync.
 */
VkResult nvk_switch_semaphore_import_nvfence(VkDevice device,
                                             VkSemaphore semaphore,
                                             const NvFence *fence);
VkResult nvk_switch_semaphore_import_nvmultifence(VkDevice device,
                                                  VkSemaphore semaphore,
                                                  const NvMultiFence *fence);
VkResult nvk_switch_fence_import_nvfence(VkDevice device,
                                         VkFence fence,
                                         const NvFence *fence_in);
VkResult nvk_switch_fence_import_nvmultifence(VkDevice device,
                                              VkFence fence,
                                              const NvMultiFence *fence_in);

#ifdef __cplusplus
}
#endif

#endif /* __SWITCH__ */

#endif /* NVK_SWITCH_WSI_H */
