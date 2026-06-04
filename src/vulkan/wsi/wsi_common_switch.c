/*
 * Copyright © 2026 Mesa-Switch contributors
 * SPDX-License-Identifier: MIT
 *
 * VK_NN_vi_surface backend for Nintendo Switch (libnx nwindow).
 *
 * Each swapchain image is a dedicated block-linear VkImage whose backing
 * VkDeviceMemory wraps a libnx NvMap. The MMU PTE kind and tile swizzle
 * chosen by NVK/nil are plumbed back out via nvk_switch_get_scanout_layout
 * and fed into the NvGraphicBuffer the compositor sees — so the Horizon
 * compositor scans out the image the GPU rendered directly, with no CPU
 * copy or staging buffer on the present path.
 *
 * queue_present extracts the render-done native fence payload from the
 * VkFence the WSI common layer wires up for each image and hands it to
 * nwindowQueueBuffer, letting the compositor wait for GPU completion
 * instead of the CPU.
 */

#define MESA_LOG_TAG "wsi-switch"

#include <switch.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

#include "util/macros.h"
#include "util/os_time.h"
#include "util/u_debug.h"
#include "vk_util.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"

#include "nouveau/vulkan/nvk_switch_wsi.h"

struct wsi_switch {
   struct wsi_interface base;
};

static bool
wsi_switch_present_trace_enabled(void)
{
   static int enabled = -1;

   if (enabled < 0)
      enabled = debug_get_bool_option("MESA_VK_WSI_SWITCH_PRESENT_TRACE", false) ? 1 : 0;

   return enabled != 0;
}

#define WSI_SWITCH_PRESENT_TRACE(fmt, ...)                                      \
   do {                                                                         \
      if (wsi_switch_present_trace_enabled())                                   \
         mesa_logi(fmt, ##__VA_ARGS__);                                         \
   } while (0)

static VkResult
wsi_switch_surface_get_support(VkIcdSurfaceBase *surface,
                               struct wsi_device *wsi_device,
                               uint32_t queueFamilyIndex,
                               VkBool32 *pSupported)
{
   *pSupported = true;
   return VK_SUCCESS;
}

static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_FIFO_KHR,
};

static bool
wsi_switch_get_format_info(VkFormat format,
                           uint32_t *pixel_format,
                           NvColorFormat *color_format)
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
      *pixel_format = PIXEL_FORMAT_RGBA_8888;
      *color_format = NvColorFormat_A8B8G8R8;
      return true;
   case VK_FORMAT_B8G8R8A8_UNORM:
      *pixel_format = PIXEL_FORMAT_BGRA_8888;
      *color_format = NvColorFormat_A8R8G8B8;
      return true;
   default:
      return false;
   }
}

static VkResult
wsi_switch_surface_get_capabilities(VkIcdSurfaceBase *icd_surface,
                                    struct wsi_device *wsi_device,
                                    VkSurfaceCapabilitiesKHR *caps)
{
   VkIcdSurfaceVi *surface = (VkIcdSurfaceVi *)icd_surface;
   NWindow *nw = (NWindow *)surface->window;

   uint32_t width = 1280, height = 720;
   if (nw && nwindowIsValid(nw))
      nwindowGetDimensions(nw, &width, &height);

   /* libnx supports up to 4 buffers per NWindow. */
   caps->minImageCount = 2;
   caps->maxImageCount = 4;

   caps->currentExtent = (VkExtent2D) { width, height };
   caps->minImageExtent = (VkExtent2D) { 1, 1 };
   caps->maxImageExtent = (VkExtent2D) {
      wsi_device->maxImageDimension2D,
      wsi_device->maxImageDimension2D,
   };

   caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->maxImageArrayLayers = 1;

   caps->supportedCompositeAlpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;

   caps->supportedUsageFlags = wsi_caps_get_image_usage();

   VK_FROM_HANDLE(vk_physical_device, pdevice, wsi_device->pdevice);
   if (pdevice->supported_extensions.EXT_attachment_feedback_loop_layout)
      caps->supportedUsageFlags |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;

   return VK_SUCCESS;
}

static VkResult
wsi_switch_surface_get_capabilities2(VkIcdSurfaceBase *surface,
                                     struct wsi_device *wsi_device,
                                     const void *info_next,
                                     VkSurfaceCapabilities2KHR *caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   const VkSurfacePresentModeKHR *present_mode =
      vk_find_struct_const(info_next, SURFACE_PRESENT_MODE_EXT);

   VkResult result =
      wsi_switch_surface_get_capabilities(surface, wsi_device,
                                          &caps->surfaceCapabilities);

   vk_foreach_struct(ext, caps->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected = (void *)ext;
         protected->supportsProtected = VK_FALSE;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR: {
         VkSurfacePresentScalingCapabilitiesKHR *scaling = (void *)ext;
         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;
         scaling->minScaledImageExtent = caps->surfaceCapabilities.minImageExtent;
         scaling->maxScaledImageExtent = caps->surfaceCapabilities.maxImageExtent;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR: {
         VkSurfacePresentModeCompatibilityKHR *compat = (void *)ext;
         if (compat->pPresentModes == NULL) {
            if (!present_mode) {
               wsi_common_vk_warn_once("Use of VkSurfacePresentModeCompatibilityKHR "
                                       "without a VkSurfacePresentModeKHR set.\n");
            }
            compat->presentModeCount = 1;
         } else if (compat->presentModeCount) {
            assert(present_mode);
            compat->presentModeCount = 1;
            compat->pPresentModes[0] = present_mode->presentMode;
         }
         break;
      }
      default:
         break;
      }
   }

   return result;
}

static VkResult
wsi_switch_surface_get_formats(VkIcdSurfaceBase *icd_surface,
                               struct wsi_device *wsi_device,
                               uint32_t *pSurfaceFormatCount,
                               VkSurfaceFormatKHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);

   /* Switch compositor scans out RGBA8. */
   vk_outarray_append_typed(VkSurfaceFormatKHR, &out, out_fmt) {
      out_fmt->format = VK_FORMAT_R8G8B8A8_UNORM;
      out_fmt->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   }
   vk_outarray_append_typed(VkSurfaceFormatKHR, &out, out_fmt) {
      out_fmt->format = VK_FORMAT_B8G8R8A8_UNORM;
      out_fmt->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_switch_surface_get_formats2(VkIcdSurfaceBase *icd_surface,
                                struct wsi_device *wsi_device,
                                const void *info_next,
                                uint32_t *pSurfaceFormatCount,
                                VkSurfaceFormat2KHR *pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);

   vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, out_fmt) {
      out_fmt->surfaceFormat.format = VK_FORMAT_R8G8B8A8_UNORM;
      out_fmt->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   }
   vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, out_fmt) {
      out_fmt->surfaceFormat.format = VK_FORMAT_B8G8R8A8_UNORM;
      out_fmt->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_switch_surface_get_present_modes(VkIcdSurfaceBase *surface,
                                     struct wsi_device *wsi_device,
                                     uint32_t *pPresentModeCount,
                                     VkPresentModeKHR *pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }

   *pPresentModeCount = MIN2(*pPresentModeCount, ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pPresentModeCount);

   if (*pPresentModeCount < ARRAY_SIZE(present_modes))
      return VK_INCOMPLETE;
   return VK_SUCCESS;
}

static VkResult
wsi_switch_surface_get_present_rectangles(VkIcdSurfaceBase *icd_surface,
                                          struct wsi_device *wsi_device,
                                          uint32_t *pRectCount,
                                          VkRect2D *pRects)
{
   VkIcdSurfaceVi *surface = (VkIcdSurfaceVi *)icd_surface;
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);

   uint32_t width = 1280, height = 720;
   NWindow *nw = (NWindow *)surface->window;
   if (nw && nwindowIsValid(nw))
      nwindowGetDimensions(nw, &width, &height);

   vk_outarray_append_typed(VkRect2D, &out, rect) {
      *rect = (VkRect2D) {
         .offset = { 0, 0 },
         .extent = { width, height },
      };
   }

   return vk_outarray_status(&out);
}

struct wsi_switch_image {
   struct wsi_image base;

   /* libnx slot index assigned via nwindowConfigureBuffer; -1 until bound. */
   int32_t slot;

   bool busy_on_host;
   bool busy_on_device;
   bool have_acquire_fence;
   NvMultiFence acquire_fence;

   /* Dedicated memory backing this image. We do not use wsi_create_image
    * here because we need full control over the tiling / dedicated-alloc
    * plumbing so NVK picks block-linear + NvKind_Generic_16BX2 and the
    * compositor can scan it out directly.
    */
   VkDeviceMemory memory;
   struct nvk_switch_scanout_layout layout;
};

struct wsi_switch_swapchain {
   struct wsi_swapchain base;

   NWindow *nw;
   VkExtent2D extent;
   bool owns_nwindow_buffers;
   uint32_t pixel_format;
   NvColorFormat color_format;

   struct wsi_switch_image images[0];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wsi_switch_swapchain, base.base, VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)

static struct wsi_image *
wsi_switch_swapchain_get_wsi_image(struct wsi_swapchain *wsi_chain,
                                   uint32_t image_index)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;
   return &chain->images[image_index].base;
}

static uint32_t
wsi_switch_multifence_compact(NvMultiFence *dst, const NvMultiFence *src)
{
   if (dst == NULL)
      return 0;

   memset(dst, 0, sizeof(*dst));
   if (src == NULL)
      return 0;

   uint32_t valid_count = 0;
   const uint32_t fence_count = MIN2(src->num_fences, ARRAY_SIZE(src->fences));
   for (uint32_t i = 0; i < fence_count; i++) {
      if ((int32_t)src->fences[i].id < 0)
         continue;

      if (valid_count >= ARRAY_SIZE(dst->fences))
         break;

      dst->fences[valid_count++] = src->fences[i];
   }

   dst->num_fences = valid_count;
   return valid_count;
}

static void
wsi_switch_destroy_scanout_image(struct wsi_switch_swapchain *chain,
                                 struct wsi_switch_image *img)
{
   const struct wsi_device *wsi = chain->base.wsi;

   if (img->base.image != VK_NULL_HANDLE) {
      wsi->DestroyImage(chain->base.device, img->base.image, &chain->base.alloc);
      img->base.image = VK_NULL_HANDLE;
   }
   if (img->memory != VK_NULL_HANDLE) {
      wsi->FreeMemory(chain->base.device, img->memory, &chain->base.alloc);
      img->memory = VK_NULL_HANDLE;
   }

#ifndef _WIN32
   img->base.dma_buf_fd = -1;
#endif
   memset(&img->layout, 0, sizeof(img->layout));
   memset(&img->acquire_fence, 0, sizeof(img->acquire_fence));
   img->have_acquire_fence = false;
}

static VkResult
wsi_switch_create_scanout_image(struct wsi_switch_swapchain *chain,
                                const VkSwapchainCreateInfoKHR *pCreateInfo,
                                uint32_t slot,
                                struct wsi_switch_image *img)
{
   const struct wsi_device *wsi = chain->base.wsi;
   VkResult result;

   memset(&img->base, 0, sizeof(img->base));
#ifndef _WIN32
   img->base.dma_buf_fd = -1;
   for (uint32_t i = 0; i < WSI_ES_COUNT; i++)
      img->base.explicit_sync[i].fd = -1;
#endif

   /* Block-linear, dedicated-alloc image. NVK's dedicated-alloc gate (see
    * nvk_device_memory.c, the 5b.A hunk) will notice the tiling and
    * program pte_kind / tile_mode accordingly.
    */
   /* Mark the image as a scanout target.  NVK keys can_compress off
    * vk_image::wsi_legacy_scanout, so this keeps swapchain images
    * uncompressed — the Horizon compositor scans out raw block-linear and
    * cannot decode GPU compression tags. */
   const struct wsi_image_create_info wsi_image_info = {
      .sType = VK_STRUCTURE_TYPE_WSI_IMAGE_CREATE_INFO_MESA,
      .scanout = true,
   };
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &wsi_image_info,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = pCreateInfo->imageFormat,
      .extent = {
         .width = pCreateInfo->imageExtent.width,
         .height = pCreateInfo->imageExtent.height,
         .depth = 1,
      },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = pCreateInfo->imageUsage,
      .sharingMode = pCreateInfo->imageSharingMode,
      .queueFamilyIndexCount = pCreateInfo->queueFamilyIndexCount,
      .pQueueFamilyIndices = pCreateInfo->pQueueFamilyIndices,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   result = wsi->CreateImage(chain->base.device, &image_info,
                             &chain->base.alloc, &img->base.image);
   if (result != VK_SUCCESS)
      return result;

   VkMemoryRequirements mem_reqs;
   wsi->GetImageMemoryRequirements(chain->base.device, img->base.image, &mem_reqs);

   /* Pick the first device-local type. On Switch there's no dedicated VRAM;
    * all types end up in sysmem, and NVK only advertises DEVICE_LOCAL heaps
    * that map to NvMap-backed allocations. */
   uint32_t mem_type_index = UINT32_MAX;
   const VkPhysicalDeviceMemoryProperties *mem_props = &wsi->memory_props;
   for (uint32_t t = 0; t < mem_props->memoryTypeCount; t++) {
      if (!(mem_reqs.memoryTypeBits & (1u << t)))
         continue;
      if (mem_props->memoryTypes[t].propertyFlags &
          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
         mem_type_index = t;
         break;
      }
   }
   if (mem_type_index == UINT32_MAX) {
      for (uint32_t t = 0; t < mem_props->memoryTypeCount; t++) {
         if (mem_reqs.memoryTypeBits & (1u << t)) {
            mem_type_index = t;
            break;
         }
      }
   }
   if (mem_type_index == UINT32_MAX) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto fail;
   }

   const VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = img->base.image,
   };
   const VkMemoryAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated,
      .allocationSize = mem_reqs.size,
      .memoryTypeIndex = mem_type_index,
   };
   result = wsi->AllocateMemory(chain->base.device, &alloc_info,
                                &chain->base.alloc, &img->memory);
   if (result != VK_SUCCESS)
      goto fail;

   result = wsi->BindImageMemory(chain->base.device, img->base.image,
                                 img->memory, 0);
   if (result != VK_SUCCESS)
      goto fail;

   result = nvk_switch_get_scanout_layout(img->base.image, img->memory,
                                          &img->layout);
   if (result != VK_SUCCESS)
      goto fail;

   /* Populate the wsi_image fields consumers in wsi_common expect. */
   img->base.memory = img->memory;
   img->base.num_planes = 1;
   img->base.sizes[0] = img->layout.size_B;
   img->base.offsets[0] = img->layout.offset_B;
   img->base.row_pitches[0] = img->layout.row_stride_B;

   /* Mirror deko3d's dk_swapchain.cpp NvGraphicBuffer layout exactly —
    * that driver is the known-working baseline for libnx scanout on this
    * GPU. Any field deko3d leaves at zero we also leave at zero via the
    * aggregate initializer below. In particular: no grbuf.type, no
    * planes[0].scan, no per-plane flags.
    *
    * planes[0].kind is hardcoded to NvKind_Generic_16BX2 because the
    * Horizon compositor always decodes scanout surfaces with that kind,
    * regardless of what NVK chose for the GPU MMU PTE. NVK's dedicated
    * scanout alloc (nvk_device_memory.c, the 5b.A hunk) programs the
    * matching PTE kind so the GPU and compositor agree.
    */
   NvGraphicBuffer grbuf = { 0 };
   grbuf.header.num_ints =
      (sizeof(NvGraphicBuffer) - sizeof(NativeHandle)) / 4;
   grbuf.unk0 = -1;
   grbuf.nvmap_id = nvMapGetId(img->layout.nvmap);
   grbuf.magic = 0xDAFFCAFF;
   grbuf.pid = 42;
   grbuf.usage =
      GRALLOC_USAGE_HW_COMPOSER |
      GRALLOC_USAGE_HW_RENDER |
      GRALLOC_USAGE_HW_TEXTURE;
   grbuf.format = chain->pixel_format;
   grbuf.ext_format = chain->pixel_format;
   grbuf.stride = img->layout.row_stride_px;
   grbuf.total_size = img->layout.size_B;
   grbuf.num_planes = 1;
   grbuf.planes[0].width = pCreateInfo->imageExtent.width;
   grbuf.planes[0].height = pCreateInfo->imageExtent.height;
   grbuf.planes[0].color_format = chain->color_format;
   grbuf.planes[0].layout = NvLayout_BlockLinear;
   grbuf.planes[0].pitch = img->layout.row_stride_B;
   grbuf.planes[0].kind = NvKind_Generic_16BX2;
   grbuf.planes[0].block_height_log2 = img->layout.block_height_log2;
   grbuf.planes[0].size = img->layout.size_B;
   grbuf.planes[0].offset = img->layout.offset_B;

   Result rc = nwindowConfigureBuffer(chain->nw, slot, &grbuf);
   if (R_FAILED(rc)) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto fail;
   }

   img->slot = slot;
   img->busy_on_host = false;
   img->busy_on_device = false;
   img->have_acquire_fence = false;
   memset(&img->acquire_fence, 0, sizeof(img->acquire_fence));
   return VK_SUCCESS;

fail:
   wsi_switch_destroy_scanout_image(chain, img);
   return result;
}

static void
wsi_switch_swapchain_close_nwindow_buffers(struct wsi_switch_swapchain *chain)
{
   if (chain->owns_nwindow_buffers && chain->nw)
      nwindowReleaseBuffers(chain->nw);

   chain->owns_nwindow_buffers = false;
}

/* EGL explicitly cancels any currently dequeued slot before releasing
 * NWindow buffers. Mirror that behavior here so a stale dequeued slot
 * from the previous run cannot block subsequent swapchain init/present.
 */
static void
wsi_switch_swapchain_cancel_outstanding_images(struct wsi_switch_swapchain *chain)
{
   if (!chain->nw)
      return;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      struct wsi_switch_image *img = &chain->images[i];

      if (img->slot < 0 || !img->busy_on_host)
         continue;

      Result rc = nwindowCancelBuffer(chain->nw, img->slot, NULL);
      WSI_SWITCH_PRESENT_TRACE("cancel: image=%u slot=%d rc=0x%x",
                               i, img->slot, (unsigned)rc);
      img->busy_on_host = false;
      img->busy_on_device = false;
      img->have_acquire_fence = false;
      memset(&img->acquire_fence, 0, sizeof(img->acquire_fence));
   }
}

static VkResult
wsi_switch_swapchain_release_images(struct wsi_swapchain *wsi_chain,
                                    uint32_t count, const uint32_t *indices)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;

   for (uint32_t i = 0; i < count; i++) {
      uint32_t index = indices[i];
      assert(index < chain->base.image_count);

      /* release_images can be called on images that are still dequeued but
       * not yet presented. Ensure the slot is returned to NWindow.
       */
      if (chain->images[index].slot >= 0 && chain->images[index].busy_on_host)
         nwindowCancelBuffer(chain->nw, chain->images[index].slot, NULL);

      chain->images[index].busy_on_device = false;
      chain->images[index].busy_on_host = false;
      chain->images[index].have_acquire_fence = false;
      memset(&chain->images[index].acquire_fence, 0,
             sizeof(chain->images[index].acquire_fence));
   }

   return VK_SUCCESS;
}

/* Timed reimplementation of libnx's nwindowDequeueBuffer().
 *
 * The stock function hard-codes eventWait(UINT64_MAX), so it cannot honor
 * VkAcquireNextImageInfoKHR::timeout — a timeout==0 poll would block, and a
 * finite timeout would be ignored.  This mirrors its logic exactly (the
 * NWindow struct fields and the bq and event calls it uses are all public
 * libnx API) but waits on the BufferQueue release event with the caller's
 * deadline instead.
 *
 * Returns VK_SUCCESS (slot/fence written), VK_NOT_READY (timeout==0, nothing
 * available), VK_TIMEOUT (deadline expired), or VK_ERROR_OUT_OF_DATE_KHR on a
 * hard dequeue failure.
 */
static VkResult
wsi_switch_dequeue_buffer(NWindow *nw, uint64_t timeout_ns,
                          int32_t *out_slot, NvMultiFence *out_fence)
{
   mutexLock(&nw->mutex);

   if (!nw->slots_configured || nw->cur_slot >= 0) {
      mutexUnlock(&nw->mutex);
      return VK_ERROR_OUT_OF_DATE_KHR;
   }

   const bool infinite = timeout_ns == UINT64_MAX;
   const uint64_t deadline =
      infinite ? UINT64_MAX : os_time_get_nano() + timeout_ns;

   NvMultiFence fence;
   s32 slot;
   Result rc;

   if (eventActive(&nw->event)) {
      /* Async BufferQueue: the release event is signaled whenever a buffer
       * becomes dequeuable.  Wait on it with the remaining time, then poll
       * the dequeue; loop on a lost race the same way nwindowDequeueBuffer
       * does. */
      for (;;) {
         uint64_t wait_ns = UINT64_MAX;
         if (!infinite) {
            const uint64_t now = os_time_get_nano();
            wait_ns = now >= deadline ? 0 : deadline - now;
         }

         rc = eventWait(&nw->event, wait_ns);
         if (R_FAILED(rc)) {
            mutexUnlock(&nw->mutex);
            return timeout_ns == 0 ? VK_NOT_READY : VK_TIMEOUT;
         }

         rc = bqDequeueBuffer(&nw->bq, true, nw->width, nw->height,
                              nw->format, nw->usage, &slot, &fence);
         if (R_VALUE(rc) != MAKERESULT(Module_LibnxBinder,
                                       LibnxBinderError_WouldBlock))
            break;
      }
   } else {
      /* No release event: bqDequeueBuffer(false) is an unbounded blocking
       * call we cannot interrupt, so a zero timeout must bail out instead of
       * entering it. */
      if (timeout_ns == 0) {
         mutexUnlock(&nw->mutex);
         return VK_NOT_READY;
      }
      rc = bqDequeueBuffer(&nw->bq, false, nw->width, nw->height,
                           nw->format, nw->usage, &slot, &fence);
   }

   if (R_SUCCEEDED(rc) && !(nw->slots_requested & (1UL << slot))) {
      rc = bqRequestBuffer(&nw->bq, slot, NULL);
      if (R_FAILED(rc))
         bqCancelBuffer(&nw->bq, slot, &fence);
      else
         nw->slots_requested |= 1UL << slot;
   }

   if (R_SUCCEEDED(rc)) {
      nw->cur_slot = slot;
      *out_slot = slot;
      *out_fence = fence;
   }

   mutexUnlock(&nw->mutex);

   return R_SUCCEEDED(rc) ? VK_SUCCESS : VK_ERROR_OUT_OF_DATE_KHR;
}

static VkResult
wsi_switch_swapchain_acquire_next_image(struct wsi_swapchain *wsi_chain,
                                        const VkAcquireNextImageInfoKHR *info,
                                        uint32_t *image_index)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;

   int32_t slot;
   NvMultiFence acquire_mf = { 0 };

   VkResult dq = wsi_switch_dequeue_buffer(chain->nw, info->timeout,
                                           &slot, &acquire_mf);
   if (dq != VK_SUCCESS) {
      WSI_SWITCH_PRESENT_TRACE("acquire: nw=%p dequeue failed result=%d "
                               "timeout=%" PRIu64,
                               (void *)chain->nw, dq, info->timeout);
      return dq;
   }

   WSI_SWITCH_PRESENT_TRACE("acquire: nw=%p slot=%d image_count=%u",
                            (void *)chain->nw, slot, chain->base.image_count);

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].slot == slot) {
         NvMultiFence compact_mf = { 0 };
         const uint32_t valid_fence_count =
            wsi_switch_multifence_compact(&compact_mf, &acquire_mf);

         chain->images[i].have_acquire_fence = false;
         memset(&chain->images[i].acquire_fence, 0,
                sizeof(chain->images[i].acquire_fence));

         if (valid_fence_count > 0) {
            chain->images[i].acquire_fence = compact_mf;
            chain->images[i].have_acquire_fence = true;
         }

         *image_index = i;
         chain->images[i].busy_on_host = true;
         chain->images[i].busy_on_device = true;
         WSI_SWITCH_PRESENT_TRACE("acquire: slot=%d mapped image=%u",
                                  slot, i);
         return VK_SUCCESS;
      }
   }

   WSI_SWITCH_PRESENT_TRACE("acquire: slot=%d did not match any image, cancelling",
                            slot);
   nwindowCancelBuffer(chain->nw, slot, NULL);
   return VK_ERROR_OUT_OF_DATE_KHR;
}

static VkResult
wsi_switch_swapchain_signal_acquire_semaphore(struct wsi_swapchain *wsi_chain,
                                              uint32_t image_index,
                                              VkSemaphore semaphore)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;
   struct wsi_switch_image *img = &chain->images[image_index];

   if (!img->have_acquire_fence)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   return nvk_switch_semaphore_import_nvmultifence(chain->base.device,
                                                   semaphore,
                                                   &img->acquire_fence);
}

static VkResult
wsi_switch_swapchain_signal_acquire_fence(struct wsi_swapchain *wsi_chain,
                                          uint32_t image_index,
                                          VkFence fence)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;
   struct wsi_switch_image *img = &chain->images[image_index];

   if (!img->have_acquire_fence)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   return nvk_switch_fence_import_nvmultifence(chain->base.device, fence,
                                               &img->acquire_fence);
}

static VkResult
wsi_switch_swapchain_queue_present(struct wsi_swapchain *wsi_chain,
                                   uint32_t image_index,
                                   uint64_t present_id,
                                   const VkPresentRegionKHR *damage)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;

   assert(image_index < chain->base.image_count);

   struct wsi_switch_image *img = &chain->images[image_index];
   VkFence vk_fence = chain->base.fences[image_index];

   NvMultiFence mf = { 0 };
   bool have_multifence = false;

   WSI_SWITCH_PRESENT_TRACE("present: begin chain=%p nw=%p image=%u slot=%d present_id=%" PRIu64
                            " fence=%p",
                            (void *)chain, (void *)chain->nw, image_index,
                            img->slot, present_id, (void *)vk_fence);

   if (vk_fence != VK_NULL_HANDLE) {
      have_multifence = nvk_switch_fence_peek_nvmultifence(vk_fence, &mf);
      WSI_SWITCH_PRESENT_TRACE("present: fence peek image=%u have_multifence=%d num_fences=%u",
                               image_index, have_multifence ? 1 : 0,
                               have_multifence ? mf.num_fences : 0);
   } else {
      WSI_SWITCH_PRESENT_TRACE("present: image=%u has no VkFence", image_index);
   }

   if (have_multifence) {
      WSI_SWITCH_PRESENT_TRACE("present: image=%u using native fence payload num_fences=%u",
                               image_index, mf.num_fences);
   } else if (vk_fence != VK_NULL_HANDLE) {
      /* The active sync isn't a native NvFence (or the kickoff hasn't
       * imported a fence yet). Block host-side so we never hand the
       * compositor a buffer the GPU is still writing.
       */
      WSI_SWITCH_PRESENT_TRACE("present: image=%u falling back to host wait",
                               image_index);
      VkResult wait =
         chain->base.wsi->WaitForFences(chain->base.device, 1, &vk_fence,
                                        true, UINT64_MAX);
      WSI_SWITCH_PRESENT_TRACE("present: image=%u host wait result=%d",
                               image_index, wait);
      if (wait != VK_SUCCESS)
         return wait;
   }

   Result rc = nwindowQueueBuffer(chain->nw, img->slot,
                                  have_multifence ? &mf : NULL);
   WSI_SWITCH_PRESENT_TRACE("present: image=%u queue slot=%d have_native_fence=%d rc=0x%x",
                            image_index, img->slot, have_multifence ? 1 : 0,
                            (unsigned)rc);
   if (R_FAILED(rc)) {
      return VK_ERROR_SURFACE_LOST_KHR;
   }

   img->busy_on_host = false;
   img->busy_on_device = false;
   WSI_SWITCH_PRESENT_TRACE("present: image=%u complete", image_index);

   return VK_SUCCESS;
}

static uint32_t
wsi_switch_swap_interval_for_present_mode(VkPresentModeKHR mode)
{
   switch (mode) {
   case VK_PRESENT_MODE_FIFO_KHR:
      return 1;
   default:
      return 1;
   }
}

static VkResult
wsi_switch_swapchain_apply_present_mode(struct wsi_switch_swapchain *chain,
                                        VkPresentModeKHR mode)
{
   const uint32_t interval = wsi_switch_swap_interval_for_present_mode(mode);
   Result rc = nwindowSetSwapInterval(chain->nw, interval);

   WSI_SWITCH_PRESENT_TRACE("present-mode: mode=%u interval=%u rc=0x%x",
                            mode, interval, (unsigned)rc);

   if (R_FAILED(rc))
      return VK_ERROR_INITIALIZATION_FAILED;

   chain->base.present_mode = mode;
   return VK_SUCCESS;
}

static void
wsi_switch_swapchain_set_present_mode(struct wsi_swapchain *wsi_chain,
                                      VkPresentModeKHR mode)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;

   (void)wsi_switch_swapchain_apply_present_mode(chain, mode);
}

static VkResult
wsi_switch_swapchain_destroy(struct wsi_swapchain *wsi_chain,
                             const VkAllocationCallbacks *pAllocator)
{
   struct wsi_switch_swapchain *chain =
      (struct wsi_switch_swapchain *)wsi_chain;

   wsi_switch_swapchain_cancel_outstanding_images(chain);

   /* Keep the scanout images alive until after the NWindow slots are released.
    * Horizon may still be scanning out the most recently queued buffer when
    * the swapchain is destroyed on app exit; freeing the backing NvMap first
    * can expose a brief white/garbage flash during layer teardown.
    *
    * The old EGL Switch path does the same thing in practice:
    * cancel/dequeue cleanup first, nwindowReleaseBuffers next, then drop the
    * backing resources once the window side has let go of them.
    */
   wsi_switch_swapchain_close_nwindow_buffers(chain);

   for (uint32_t i = 0; i < chain->base.image_count; i++)
      wsi_switch_destroy_scanout_image(chain, &chain->images[i]);

   wsi_swapchain_finish(&chain->base);

   vk_free(pAllocator, chain);

   return VK_SUCCESS;
}

static VkResult
wsi_switch_surface_create_swapchain(VkIcdSurfaceBase *icd_surface,
                                    VkDevice device,
                                    struct wsi_device *wsi_device,
                                    const VkSwapchainCreateInfoKHR *pCreateInfo,
                                    const VkAllocationCallbacks *pAllocator,
                                    struct wsi_swapchain **swapchain_out)
{
   VkIcdSurfaceVi *surface = (VkIcdSurfaceVi *)icd_surface;
   struct wsi_switch_swapchain *chain;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   uint32_t num_images = pCreateInfo->minImageCount;
   if (num_images > 4)
      num_images = 4;

   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);
   chain = vk_zalloc(pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   chain->nw = (NWindow *)surface->window;
   chain->extent = pCreateInfo->imageExtent;
   chain->owns_nwindow_buffers = false;

   if (!wsi_switch_get_format_info(pCreateInfo->imageFormat,
                                   &chain->pixel_format,
                                   &chain->color_format)) {
      vk_free(pAllocator, chain);
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }

   /* wsi_swapchain_init still needs a wsi_base_image_params to fill in the
    * blit/fence infrastructure; cpu_params gives us a no-blit swapchain
    * whose cached image_info we simply never use. The real per-image
    * allocation happens below via wsi_switch_create_scanout_image.
    */
   struct wsi_cpu_image_params cpu_params = {
      .base.image_type = WSI_IMAGE_TYPE_CPU,
   };

   result = wsi_swapchain_init(wsi_device, &chain->base, device,
                               pCreateInfo, &cpu_params.base, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, chain);
      return result;
   }

   chain->base.destroy = wsi_switch_swapchain_destroy;
   chain->base.get_wsi_image = wsi_switch_swapchain_get_wsi_image;
   chain->base.acquire_next_image = wsi_switch_swapchain_acquire_next_image;
   chain->base.signal_acquire_semaphore =
      wsi_switch_swapchain_signal_acquire_semaphore;
   chain->base.signal_acquire_fence =
      wsi_switch_swapchain_signal_acquire_fence;
   chain->base.release_images = wsi_switch_swapchain_release_images;
   chain->base.queue_present = wsi_switch_swapchain_queue_present;
   chain->base.set_present_mode = wsi_switch_swapchain_set_present_mode;
   chain->base.present_mode = wsi_swapchain_get_present_mode(wsi_device, pCreateInfo);
   chain->base.image_count = num_images;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      chain->images[i].slot = -1;
      chain->images[i].busy_on_host = false;
      chain->images[i].busy_on_device = false;
      chain->images[i].memory = VK_NULL_HANDLE;
   }

   if (!chain->nw || !nwindowIsValid(chain->nw)) {
      result = VK_ERROR_SURFACE_LOST_KHR;
      goto fail;
   }

   Result rc = nwindowSetDimensions(chain->nw,
                                    pCreateInfo->imageExtent.width,
                                    pCreateInfo->imageExtent.height);
   if (R_FAILED(rc)) {
      result = VK_ERROR_INITIALIZATION_FAILED;
      goto fail;
   }

   result = wsi_switch_swapchain_apply_present_mode(chain,
                                                   chain->base.present_mode);
   if (result != VK_SUCCESS)
      goto fail;

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      result = wsi_switch_create_scanout_image(chain, pCreateInfo, i,
                                               &chain->images[i]);
      if (result != VK_SUCCESS) {
         for (uint32_t j = 0; j < i; j++)
            wsi_switch_destroy_scanout_image(chain, &chain->images[j]);
         nwindowReleaseBuffers(chain->nw);
         goto fail;
      }
   }

   chain->owns_nwindow_buffers = true;

   *swapchain_out = &chain->base;

   return VK_SUCCESS;

fail:
   wsi_switch_swapchain_destroy(&chain->base, pAllocator);
   return result;
}

VkResult
wsi_switch_init_wsi(struct wsi_device *wsi_device,
                    const VkAllocationCallbacks *alloc,
                    VkPhysicalDevice physical_device)
{
   struct wsi_switch *wsi;

   /* We hand wsi_swapchain_init a wsi_cpu_image_params just to satisfy its
    * API contract; the real per-image allocation happens in
    * wsi_switch_create_scanout_image. wsi_cpu_image_needs_buffer_blit
    * defaults to true, which would make wsi_common's queue_present submit
    * a blit command buffer from image->blit.cmd_buffers[] before invoking
    * our backend — but we never populate that array, so the deref faults
    * inside vkQueuePresentKHR. Forcing wants_linear = true is the
    * documented escape hatch: it makes the cpu-image path pick
    * WSI_SWAPCHAIN_NO_BLIT, so wsi_common skips blit cmd_pool allocation
    * and the blit submit path entirely. The LINEAR tiling side effect on
    * chain->image_info is dead state for us — we never call wsi_create_image
    * against that template.
    */
   wsi_device->wants_linear = true;

   wsi = vk_alloc(alloc, sizeof(*wsi), 8,
                  VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi) {
      wsi_device->wsi[VK_ICD_WSI_PLATFORM_VI] = NULL;
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   wsi->base.get_support = wsi_switch_surface_get_support;
   wsi->base.get_capabilities2 = wsi_switch_surface_get_capabilities2;
   wsi->base.get_formats = wsi_switch_surface_get_formats;
   wsi->base.get_formats2 = wsi_switch_surface_get_formats2;
   wsi->base.get_present_modes = wsi_switch_surface_get_present_modes;
   wsi->base.get_present_rectangles = wsi_switch_surface_get_present_rectangles;
   wsi->base.create_swapchain = wsi_switch_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_VI] = &wsi->base;

   return VK_SUCCESS;
}

void
wsi_switch_finish_wsi(struct wsi_device *wsi_device,
                      const VkAllocationCallbacks *alloc)
{
   struct wsi_switch *wsi =
      (struct wsi_switch *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_VI];
   if (!wsi)
      return;

   vk_free(alloc, wsi);
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_VI] = NULL;
}

VkResult
wsi_CreateViSurfaceNN(VkInstance _instance,
                      const VkViSurfaceCreateInfoNN *pCreateInfo,
                      const VkAllocationCallbacks *pAllocator,
                      VkSurfaceKHR *pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   VkIcdSurfaceVi *surface;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN);

   surface = vk_alloc2(&instance->alloc, pAllocator, sizeof(*surface), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_VI;
   surface->window = pCreateInfo->window;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}
