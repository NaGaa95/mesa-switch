#include "git_sha1.h"

#include "pipe/p_defines.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"

#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/format/u_format.h"
#include "util/format/u_format_s3tc.h"
#include "util/u_string.h"
#include "util/u_debug.h"
#include "util/hex.h"

#include "util/os_mman.h"
#include "util/os_time.h"

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include "drm-uapi/nouveau_drm.h"
#ifdef __SWITCH__
#include "drm-uapi/drm_fourcc.h"
#endif
#ifndef __SWITCH__
#include <xf86drm.h>
#endif
#include <nvif/class.h>
#include <nvif/cl0080.h>

#include "nouveau_winsys.h"
#include "nouveau_screen.h"
#include "nouveau_context.h"
#include "nouveau_fence.h"
#include "nouveau_mm.h"
#include "nouveau_buffer.h"

#include "util/disk_cache_os.h"

#include <compiler/glsl_types.h>

/* XXX this should go away */
#include "frontend/drm_driver.h"

/* Even though GPUs might allow addresses with more bits, some engines do not.
 * Stick with 40 for compatibility.
 */
#define NV_GENERIC_VM_LIMIT_SHIFT 39

int nouveau_mesa_debug = 0;

static const char *
nouveau_screen_get_name(struct pipe_screen *pscreen)
{
   struct nouveau_screen *screen = nouveau_screen(pscreen);
   return screen->chipset_name;
}

static const char *
nouveau_screen_get_vendor(struct pipe_screen *pscreen)
{
   return "Mesa";
}

static const char *
nouveau_screen_get_device_vendor(struct pipe_screen *pscreen)
{
   return "NVIDIA";
}

static uint64_t
nouveau_screen_get_timestamp(struct pipe_screen *pscreen)
{
   int64_t cpu_time = os_time_get_nano();

   /* getparam of PTIMER_TIME takes about x10 as long (several usecs) */

   return cpu_time + nouveau_screen(pscreen)->cpu_gpu_time_delta;
}

static struct disk_cache *
nouveau_screen_get_disk_shader_cache(struct pipe_screen *pscreen)
{
   return nouveau_screen(pscreen)->disk_shader_cache;
}

static void
nouveau_screen_fence_ref(struct pipe_screen *pscreen,
                         struct pipe_fence_handle **ptr,
                         struct pipe_fence_handle *pfence)
{
   nouveau_fence_ref((pfence ? nouveau_fence(pfence) : NULL),
                     (ptr ? (struct nouveau_fence **)ptr : NULL),
                     nouveau_screen(pscreen));
}

static bool
nouveau_screen_fence_finish(struct pipe_screen *screen,
                            struct pipe_context *ctx,
                            struct pipe_fence_handle *pfence,
                            uint64_t timeout)
{
   if (!timeout)
      return nouveau_fence_signalled(nouveau_fence(pfence));

#ifdef __SWITCH__
   return nouveau_fence_wait_timeout(nouveau_fence(pfence), NULL, timeout);
#else
   return nouveau_fence_wait(nouveau_fence(pfence), NULL);
#endif
}

#ifdef __SWITCH__
static bool
nouveau_switch_decode_nwindow_modifier(uint64_t modifier,
                                        uint8_t *pte_kind_out,
                                        uint16_t *tile_mode_out)
{
   if (modifier == DRM_FORMAT_MOD_INVALID ||
       modifier == DRM_FORMAT_MOD_LINEAR)
      return false;

   const uint64_t canonical =
      drm_fourcc_canonicalize_nvidia_format_mod(modifier);
   const uint8_t pte_kind = (canonical >> 12) & 0xff;
   const uint8_t block_height_log2 = canonical & 0xf;
   if (pte_kind == NvKind_Pitch || block_height_log2 > 5)
      return false;

   /* Current Horizon NWindow buffers use GM20B's Tegra sector layout,
    * Fermi-Volta kind generation and no compression metadata.
    */
   const uint64_t expected = DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(
      0, 0, 0, pte_kind, block_height_log2);
   if (canonical != expected)
      return false;

   *pte_kind_out = pte_kind;
   *tile_mode_out = (uint16_t)block_height_log2 << 4;
   return true;
}
#endif

struct nouveau_bo *
nouveau_screen_bo_from_handle(struct pipe_screen *pscreen,
                              struct winsys_handle *whandle,
                              unsigned *out_stride, unsigned *out_offset)
{
   struct nouveau_device *dev = nouveau_screen(pscreen)->device;
   struct nouveau_bo *bo = NULL;
   int ret;

   if (whandle->offset != 0) {
      debug_printf("%s: attempt to import unsupported winsys offset %d\n",
                   __func__, whandle->offset);
      return NULL;
   }

   if (whandle->type != WINSYS_HANDLE_TYPE_SHARED &&
       whandle->type != WINSYS_HANDLE_TYPE_FD) {
      debug_printf("%s: attempt to import unsupported handle type %d\n",
                   __func__, whandle->type);
      return NULL;
   }

   if (whandle->type == WINSYS_HANDLE_TYPE_SHARED) {
#ifdef __SWITCH__
      uint8_t pte_kind;
      uint16_t tile_mode;
      if (!nouveau_switch_decode_nwindow_modifier(whandle->modifier,
                                                   &pte_kind, &tile_mode)) {
         debug_printf("%s: unsupported Switch modifier 0x%016" PRIx64 "\n",
                      __func__, whandle->modifier);
         return NULL;
      }
      ret = nouveau_switch_bo_name_ref_explicit(
         dev, whandle->handle, whandle->size, NvKind_Pitch,
         true, pte_kind, tile_mode, &bo);
#else
      ret = nouveau_bo_name_ref(dev, whandle->handle, &bo);
#endif
   } else {
      ret = nouveau_bo_prime_handle_ref(dev, whandle->handle, &bo);
   }

   if (ret) {
      debug_printf("%s: ref name 0x%08x failed with %d\n",
                   __func__, whandle->handle, ret);
      return NULL;
   }

#ifdef __SWITCH__
   /* A numeric NvMap name does not describe its layout.  The explicit import
    * above registers or validates complete NWindow metadata; also require the
    * resulting BO configuration to agree with the caller's modifier.
    */
   if (whandle->type == WINSYS_HANDLE_TYPE_SHARED) {
      bool compatible = whandle->modifier != DRM_FORMAT_MOD_INVALID;
      if (compatible && bo->config.nvc0.memtype == NvKind_Pitch) {
         compatible = whandle->modifier == DRM_FORMAT_MOD_LINEAR;
      } else if (compatible) {
         const uint64_t declared =
            drm_fourcc_canonicalize_nvidia_format_mod(whandle->modifier);
         const uint64_t expected = DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D(
            0, 0, 0, bo->config.nvc0.memtype,
            (bo->config.nvc0.tile_mode >> 4) & 0xf);
         compatible = declared == expected;
      }
      if (whandle->size != 0 && whandle->size != bo->size)
         compatible = false;

      if (!compatible) {
         debug_printf("%s: NvMap 0x%08x metadata does not match modifier "
                      "0x%016" PRIx64 "\n",
                      __func__, whandle->handle, whandle->modifier);
         nouveau_bo_ref(NULL, &bo);
         return NULL;
      }
   }
#endif

   *out_stride = whandle->stride;
   *out_offset = whandle->offset;
   return bo;
}


bool
nouveau_screen_bo_get_handle(struct pipe_screen *pscreen,
                             struct nouveau_bo *bo,
                             unsigned stride,
                             unsigned offset,
                             struct winsys_handle *whandle)
{
   whandle->stride = stride;
   whandle->offset = offset;

   if (whandle->type == WINSYS_HANDLE_TYPE_SHARED) {
      return nouveau_bo_name_get(bo, &whandle->handle) == 0;
   } else if (whandle->type == WINSYS_HANDLE_TYPE_KMS) {
      int fd;
      int ret;

      /* The handle is exported in this case, but the global list of
       * handles is in libdrm and there is no libdrm API to add
       * handles to the list without additional side effects. The
       * closest API available also gets a fd for the handle, which
       * is not necessary in this case. Call it and close the fd.
       */
      ret = nouveau_bo_set_prime(bo, &fd);
      if (ret != 0)
        return false;

      close(fd);

      whandle->handle = bo->handle;
      return true;
   } else if (whandle->type == WINSYS_HANDLE_TYPE_FD) {
      return nouveau_bo_set_prime(bo, (int *)&whandle->handle) == 0;
   } else {
      return false;
   }
}

static void
nouveau_disk_cache_create(struct nouveau_screen *screen)
{
   blake3_hasher ctx;
   unsigned char blake3[BLAKE3_KEY_LEN];
   char cache_id[BLAKE3_HEX_LEN];
   uint64_t driver_flags = 0;

   _mesa_blake3_init(&ctx);
   if (!disk_cache_get_function_identifier(nouveau_disk_cache_create,
                                           &ctx))
      return;

   _mesa_blake3_final(&ctx, blake3);
   mesa_bytes_to_hex(cache_id, blake3, BLAKE3_KEY_LEN);

   driver_flags |= NOUVEAU_SHADER_CACHE_FLAGS_IR_NIR;

   screen->disk_shader_cache =
      disk_cache_create(nouveau_screen_get_name(&screen->base),
                        cache_id, driver_flags);
}

static void*
reserve_vma(uintptr_t start, uint64_t reserved_size)
{
   void *reserved = os_mmap((void*)start, reserved_size, PROT_NONE,
                            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
   if (reserved == MAP_FAILED)
      return NULL;
   return reserved;
}

static void
nouveau_query_memory_info(struct pipe_screen *pscreen,
                          struct pipe_memory_info *info)
{
   const struct nouveau_screen *screen = nouveau_screen(pscreen);
   struct nouveau_device *dev = screen->device;

#ifdef __SWITCH__
   /* GM20B has no dedicated VRAM on Switch: all GPU allocations consume the
    * process UMA entitlement.  Report that pool once as device memory rather
    * than double-counting it as both device and staging memory.  Refresh the
    * free amount for every query because application allocations share it.
    */
   uint64_t current_available = 0;
   uint64_t allocated = 0;
   nouveau_switch_device_get_memory_info(
      dev, NULL, &current_available, &allocated);

   const uint64_t budget_remaining =
      allocated < dev->gart_limit ? dev->gart_limit - allocated : 0;
   const uint64_t headroom =
      nouveau_switch_memory_headroom(dev->gart_size);
   const uint64_t system_remaining =
      current_available > headroom ? current_available - headroom : 0;
   const uint64_t available = MIN2(budget_remaining, system_remaining);

   memset(info, 0, sizeof(*info));
   info->total_device_memory = dev->gart_size / 1024;
   info->avail_device_memory = available / 1024;
#else
   info->total_device_memory = dev->vram_size / 1024;
   info->total_staging_memory = dev->gart_size / 1024;

   info->avail_device_memory = dev->vram_limit / 1024;
   info->avail_staging_memory = dev->gart_limit / 1024;
#endif
}

static bool
nouveau_pushbuf_cb(struct nouveau_pushbuf *push)
{
   struct nouveau_pushbuf_priv *p = (struct nouveau_pushbuf_priv *)push->user_priv;

   if (p->context) {
      if (!p->context->kick_notify(p->context))
         return false;
   } else {
      _nouveau_fence_update(p->screen, true);
   }

   NOUVEAU_DRV_STAT(p->screen, pushbuf_count, 1);
   return true;
}

#ifdef __SWITCH__
static bool
nouveau_pushbuf_fence_marker_cb(struct nouveau_pushbuf *push,
                                uint64_t *cookie_out)
{
   struct nouveau_pushbuf_priv *p = push ? push->user_priv : NULL;
   if (!p || !cookie_out || push != p->screen->pushbuf)
      return false;

   simple_mtx_assert_locked(&p->screen->fence.lock);

   /* kick_notify has emitted the logical fence command but the winsys has not
    * necessarily converted its command record yet.  Return an opaque boundary
    * only; the winsys publishes it after that conversion succeeds.
    */
   struct nouveau_fence *fence = p->screen->fence.tail;
   if (!fence || fence->state < NOUVEAU_FENCE_STATE_EMITTED ||
       fence->state >= NOUVEAU_FENCE_STATE_SIGNALLED ||
       fence->native_fence_valid)
      return false;

   *cookie_out = fence->batch_cookie;
   return true;
}

static void
nouveau_fence_native_perf_log_locked(struct nouveau_fence_list *fence,
                                     const char *reason)
{
   if (!fence->perf_enabled || fence->native_assign_calls == 0)
      return;

   _debug_printf(
      "nouveau/switch: fence association perf %s calls=%llu scanned=%llu "
      "assigned=%llu avg/max_scan=%llu/%llu cpu=%llums avg/max=%llu/%lluus\n",
      reason, (unsigned long long)fence->native_assign_calls,
      (unsigned long long)fence->native_assign_scanned,
      (unsigned long long)fence->native_assign_assigned,
      (unsigned long long)(fence->native_assign_scanned /
                           fence->native_assign_calls),
      (unsigned long long)fence->native_assign_max_scanned,
      (unsigned long long)(fence->native_assign_cpu_ns / 1000000),
      (unsigned long long)(fence->native_assign_cpu_ns /
                           fence->native_assign_calls / 1000),
      (unsigned long long)(fence->native_assign_max_cpu_ns / 1000));
}

static uint32_t
nouveau_pushbuf_native_cb(struct nouveau_pushbuf *push,
                          const NvFence *native_fence,
                          bool cpu_visible, uint64_t cookie)
{
   struct nouveau_pushbuf_priv *p = push ? push->user_priv : NULL;
   if (!p || !native_fence || push != p->screen->pushbuf)
      return 0;

   simple_mtx_assert_locked(&p->screen->fence.lock);

   struct nouveau_fence_list *fence_list = &p->screen->fence;
   struct nouveau_fence *boundary = NULL;
   for (struct nouveau_fence *fence = fence_list->head;
        fence != NULL; fence = fence->next) {
      if (fence->batch_cookie == cookie) {
         boundary = fence;
         break;
      }
   }

   /* Missing a marker is a bookkeeping fault, never permission to associate
    * a newer fence.  Fail closed and let a later explicit fence wait force its
    * own physical submission/completion instead of risking early retirement.
    */
   if (!boundary) {
      _debug_printf("nouveau/switch: physical fence %u:%u has no matching "
                    "Gallium batch cookie %llu; assigning no logical fences\n",
                    native_fence->id, native_fence->value,
                    (unsigned long long)cookie);
      return 0;
   }

   const uint64_t start_ns = fence_list->perf_enabled ?
      os_time_get_nano() : 0;
   uint32_t assigned = 0;
   uint64_t scanned = 0;
   for (struct nouveau_fence *fence = fence_list->head;
        fence != NULL; fence = fence->next) {
      const bool at_boundary = fence == boundary;
      scanned++;
      if (fence->state >= NOUVEAU_FENCE_STATE_EMITTED &&
          fence->state < NOUVEAU_FENCE_STATE_SIGNALLED &&
          !fence->native_fence_valid) {
         fence->native_fence = *native_fence;
         fence->native_fence_valid = true;
         fence->native_fence_cpu_visible = cpu_visible;
         fence->state = NOUVEAU_FENCE_STATE_FLUSHED;
         assigned++;
      }

      if (at_boundary)
         break;
   }

   if (fence_list->perf_enabled) {
      const uint64_t elapsed_ns = os_time_get_nano() - start_ns;
      fence_list->native_assign_calls++;
      fence_list->native_assign_scanned += scanned;
      fence_list->native_assign_assigned += assigned;
      fence_list->native_assign_cpu_ns += elapsed_ns;
      fence_list->native_assign_max_cpu_ns =
         MAX2(fence_list->native_assign_max_cpu_ns, elapsed_ns);
      fence_list->native_assign_max_scanned =
         MAX2(fence_list->native_assign_max_scanned, scanned);
      if (fence_list->native_assign_calls == 1 ||
          (fence_list->perf_log_interval &&
           fence_list->native_assign_calls %
              fence_list->perf_log_interval == 0)) {
         nouveau_fence_native_perf_log_locked(fence_list, "periodic");
      }
   }

   return assigned;
}
#endif

#ifdef __SWITCH__
void
nouveau_pushbuf_bind_context(struct nouveau_pushbuf *push,
                             struct nouveau_context *context)
{
   struct nouveau_pushbuf_priv *p = push ? push->user_priv : NULL;

   if (p)
      p->context = context;
}

void
nouveau_pushbuf_unbind_context(struct nouveau_pushbuf *push,
                               struct nouveau_context *context)
{
   struct nouveau_pushbuf_priv *p = push ? push->user_priv : NULL;

   if (p && p->context == context)
      p->context = NULL;
}
#endif

int
nouveau_pushbuf_create(struct nouveau_screen *screen, struct nouveau_context *context,
                       struct nouveau_client *client, struct nouveau_object *chan, int nr,
                       uint32_t size, struct nouveau_pushbuf **push)
{
   int ret;
#ifdef __SWITCH__
   ret = nouveau_pushbuf_new(client, chan, nr, size, true, push);
#else
   ret = nouveau_pushbuf_new(client, chan, nr, size, push);
#endif
   if (ret)
      return ret;

#ifdef __SWITCH__
   if (chan == screen->channel) {
      ret = nouveau_switch_pushbuf_enable_mapped_completion(*push);
      if (ret) {
         nouveau_pushbuf_del(push);
         return ret;
      }
   }
#endif

   struct nouveau_pushbuf_priv *p = MALLOC_STRUCT(nouveau_pushbuf_priv);
   if (!p) {
      nouveau_pushbuf_del(push);
      return -ENOMEM;
   }
   p->screen = screen;
   p->context = context;
#ifdef __SWITCH__
   nouveau_switch_pushbuf_set_kick_notify(*push, nouveau_pushbuf_cb);
#else
   (*push)->kick_notify = nouveau_pushbuf_cb;
#endif
   (*push)->user_priv = p;
#ifdef __SWITCH__
   if (context == NULL) {
      nouveau_switch_pushbuf_set_fence_batch_notify(
         *push, nouveau_pushbuf_fence_marker_cb,
         nouveau_pushbuf_native_cb);
   }
#endif
   return 0;
}

void
nouveau_pushbuf_destroy(struct nouveau_pushbuf **push)
{
   if (!*push)
      return;

#ifdef __SWITCH__
   /* nouveau_pushbuf_del() may submit a final pending batch.  Detach driver
    * callbacks before releasing their user data so teardown cannot call into
    * a stale nouveau_pushbuf_priv.
    */
   nouveau_switch_pushbuf_set_kick_notify(*push, NULL);
   nouveau_switch_pushbuf_set_native_kick_notify(*push, NULL);
   nouveau_switch_pushbuf_set_fence_batch_notify(*push, NULL, NULL);
#endif
   void *user_priv = (*push)->user_priv;
   (*push)->user_priv = NULL;
   nouveau_pushbuf_del(push);
   FREE(user_priv);
}

static int
nouveau_screen_get_fd(struct pipe_screen *pscreen)
{
   const struct nouveau_screen *screen = nouveau_screen(pscreen);

   return screen->drm->fd;
}

static void
nouveau_driver_uuid(struct pipe_screen *screen, char *uuid)
{
   const char* driver = PACKAGE_VERSION MESA_GIT_SHA1;
   blake3_hasher blake3_ctx;
   uint8_t blake3[BLAKE3_KEY_LEN];

   _mesa_blake3_init(&blake3_ctx);
   _mesa_blake3_update(&blake3_ctx, driver, strlen(driver));
   _mesa_blake3_final(&blake3_ctx, blake3);
   memcpy(uuid, blake3, PIPE_UUID_SIZE);
}

static void
nouveau_device_uuid(struct pipe_screen *pscreen, char *uuid)
{
   const struct nouveau_screen *screen = nouveau_screen(pscreen);
   struct nv_device_info storage;
   const struct nv_device_info *info =
      nouveau_device_get_info(screen->device, &storage);
   nv_device_uuid(info, (void *)uuid, PIPE_UUID_SIZE, false);
}

int
nouveau_screen_init(struct nouveau_screen *screen, struct nouveau_device *dev)
{
   struct pipe_screen *pscreen = &screen->base;
   struct nv04_fifo nv04_data = { .vram = 0xbeef0201, .gart = 0xbeef0202 };
   struct nvc0_fifo nvc0_data = { };
   struct nve0_fifo nve0_data = { .engine = NOUVEAU_FIFO_ENGINE_GR };
   uint64_t time;
   int size, ret;
   void *data;
   union nouveau_bo_config mm_config;

   glsl_type_singleton_init_or_ref();

   const char *nv_dbg = os_get_option("NOUVEAU_MESA_DEBUG");
   if (nv_dbg)
      nouveau_mesa_debug = atoi(nv_dbg);

   screen->disable_fences = debug_get_bool_option("NOUVEAU_DISABLE_FENCES", false);

   /* These must be set before any failure is possible, as the cleanup
    * paths assume they're responsible for deleting them.
    */
   screen->drm = nouveau_drm(&dev->object);
   screen->device = dev;
   screen->initialized = false;

   if (dev->chipset < 0xc0) {
      data = &nv04_data;
      size = sizeof(nv04_data);
   } else if (dev->chipset < 0xe0) {
      data = &nvc0_data;
      size = sizeof(nvc0_data);
   } else {
      data = &nve0_data;
      size = sizeof(nve0_data);
   }

   bool enable_svm = debug_get_bool_option("NOUVEAU_SVM", false);
   screen->has_svm = false;
#ifndef __SWITCH__
   /* we only care about HMM with OpenCL enabled */
   if (dev->chipset > 0x130 && enable_svm) {
      /* Before being able to enable SVM we need to carve out some memory for
       * driver bo allocations. Let's just base the size on the available VRAM.
       *
       * 40 bit is the biggest we care about and for 32 bit systems we don't
       * want to allocate all of the available memory either.
       *
       * Also we align the size we want to reserve to the next POT to make use
       * of hugepages.
       */
      const int vram_shift = util_logbase2_ceil64(dev->vram_size);
      const int limit_bit =
         MIN2(sizeof(void*) * 8 - 1, NV_GENERIC_VM_LIMIT_SHIFT);
      screen->svm_cutout_size =
         BITFIELD64_BIT(MIN2(sizeof(void*) == 4 ? 26 : NV_GENERIC_VM_LIMIT_SHIFT, vram_shift));

      size_t start = screen->svm_cutout_size;
      do {
         screen->svm_cutout = reserve_vma(start, screen->svm_cutout_size);
         if (!screen->svm_cutout) {
            start += screen->svm_cutout_size;
            continue;
         }

         struct drm_nouveau_svm_init svm_args = {
            .unmanaged_addr = (uintptr_t)screen->svm_cutout,
            .unmanaged_size = screen->svm_cutout_size,
         };

         ret = drmCommandWrite(screen->drm->fd, DRM_NOUVEAU_SVM_INIT,
                               &svm_args, sizeof(svm_args));
         screen->has_svm = !ret;
         if (!screen->has_svm)
            os_munmap(screen->svm_cutout, screen->svm_cutout_size);
         break;
      } while ((start + screen->svm_cutout_size) < BITFIELD64_MASK(limit_bit));
   }
#endif

   switch (dev->chipset) {
   case 0x0ea: /* TK1, GK20A */
   case 0x120: /* Tegra X1 (Switch) */
   case 0x12b: /* TX1, GM20B */
   case 0x13b: /* TX2, GP10B */
      screen->tegra_sector_layout = true;
      break;
   default:
      /* Xavier's GPU and everything else */
      screen->tegra_sector_layout = false;
      break;
   }

   /*
    * Set default VRAM domain if not overridden
    */
   if (!screen->vram_domain) {
      if (dev->vram_size > 0)
         screen->vram_domain = NOUVEAU_BO_VRAM;
      else
         screen->vram_domain = NOUVEAU_BO_GART;
   }

   ret = nouveau_object_new(&dev->object, 0, NOUVEAU_FIFO_CHANNEL_CLASS,
                            data, size, &screen->channel);
   if (ret)
      goto err;

   ret = nouveau_client_new(screen->device, &screen->client);
   if (ret)
      goto err;
   ret = nouveau_pushbuf_create(screen, NULL, screen->client, screen->channel,
                                4, 512 * 1024, &screen->pushbuf);
   if (ret)
      goto err;

   /* getting CPU time first appears to be more accurate */
   screen->cpu_gpu_time_delta = os_time_get();

   ret = nouveau_getparam(dev, NOUVEAU_GETPARAM_PTIMER_TIME, &time);
   if (!ret)
      screen->cpu_gpu_time_delta = time - screen->cpu_gpu_time_delta * 1000;

   snprintf(screen->chipset_name, sizeof(screen->chipset_name), "NV%02X", dev->chipset);
   pscreen->get_name = nouveau_screen_get_name;
   pscreen->get_screen_fd = nouveau_screen_get_fd;
   pscreen->get_vendor = nouveau_screen_get_vendor;
   pscreen->get_device_vendor = nouveau_screen_get_device_vendor;
   pscreen->get_disk_shader_cache = nouveau_screen_get_disk_shader_cache;

   pscreen->get_timestamp = nouveau_screen_get_timestamp;

   pscreen->fence_reference = nouveau_screen_fence_ref;
   pscreen->fence_finish = nouveau_screen_fence_finish;

   pscreen->query_memory_info = nouveau_query_memory_info;
   pscreen->get_driver_uuid = nouveau_driver_uuid;
   pscreen->get_device_uuid = nouveau_device_uuid;

   nouveau_disk_cache_create(screen);

   screen->transfer_pushbuf_threshold = 192;
   screen->lowmem_bindings = PIPE_BIND_GLOBAL; /* gallium limit */
   screen->vidmem_bindings =
      PIPE_BIND_RENDER_TARGET | PIPE_BIND_DEPTH_STENCIL |
      PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_SCANOUT |
      PIPE_BIND_CURSOR |
      PIPE_BIND_SAMPLER_VIEW |
      PIPE_BIND_SHADER_BUFFER | PIPE_BIND_SHADER_IMAGE |
      PIPE_BIND_GLOBAL;
   screen->sysmem_bindings =
      PIPE_BIND_SAMPLER_VIEW | PIPE_BIND_STREAM_OUTPUT |
      PIPE_BIND_COMMAND_ARGS_BUFFER;

   struct nv_device_info info_storage;
   const struct nv_device_info *dev_info =
      nouveau_device_get_info(dev, &info_storage);
   screen->is_uma = dev_info->type != NV_DEVICE_TYPE_DIS;

   memset(&mm_config, 0, sizeof(mm_config));
   nouveau_fence_list_init(&screen->fence);
#ifdef __SWITCH__
   screen->fence.perf_enabled = false;
   const int64_t fence_perf_interval =
      debug_get_num_option("NOUVEAU_SWITCH_LOG_INTERVAL", 300);
   screen->fence.perf_log_interval =
      fence_perf_interval > 0 && fence_perf_interval <= UINT32_MAX ?
         (uint32_t)fence_perf_interval : 300;
#endif

   uint32_t gart_domain = NOUVEAU_BO_GART | NOUVEAU_BO_MAP;
#ifdef __SWITCH__
   const bool cpu_uncached_gart =
      debug_get_bool_option("NOUVEAU_SWITCH_GART_CPU_UNCACHED", true);
   if (cpu_uncached_gart) {
      /* Match deko3D's default streaming-memory policy.  CPU-uncached GART
       * avoids both stale partial-line writeback and cleaning an entire 4 MiB
       * Nouveau slab for every small dynamic-buffer update.  Keep the GPU
       * mapping cached; Horizon emits the GM20B L2 sysmem acquire before the
       * first command entry of every native submission.
       */
      gart_domain |= NOUVEAU_BO_COHERENT |
                     NOUVEAU_BO_SWITCH_GPU_CACHED;
   }
   if (debug_get_bool_option("NOUVEAU_SWITCH_LOG", false) ||
       debug_get_bool_option("NOUVEAU_SWITCH_STATS", false) ||
       screen->fence.perf_enabled) {
      _debug_printf("nouveau/switch: GART slabs cache policy=%s/gpu-cached "
                    "(NOUVEAU_SWITCH_GART_CPU_UNCACHED=%u)\n",
                    cpu_uncached_gart ? "cpu-uncached" : "cpu-cached",
                    cpu_uncached_gart);
   }
#endif
   screen->mm_GART = nouveau_mm_create(dev, gart_domain, &mm_config);
   screen->mm_VRAM = nouveau_mm_create(dev, NOUVEAU_BO_VRAM, &mm_config);

   return 0;

err:
   if (screen->svm_cutout)
      os_munmap(screen->svm_cutout, screen->svm_cutout_size);
   return ret;
}

void
nouveau_screen_fini(struct nouveau_screen *screen)
{
   int fd = screen->drm->fd;

   glsl_type_singleton_decref();
   if (screen->has_svm)
      os_munmap(screen->svm_cutout, screen->svm_cutout_size);

   nouveau_mm_destroy(screen->mm_GART);
   nouveau_mm_destroy(screen->mm_VRAM);

   nouveau_pushbuf_destroy(&screen->pushbuf);

#ifdef __SWITCH__
   if (screen->fence.perf_enabled) {
      simple_mtx_lock(&screen->fence.lock);
      nouveau_fence_native_perf_log_locked(&screen->fence, "final");
      simple_mtx_unlock(&screen->fence.lock);
   }
#endif

   nouveau_client_del(&screen->client);
   nouveau_object_del(&screen->channel);

   nouveau_device_del(&screen->device);
   nouveau_drm_del(&screen->drm);

   disk_cache_destroy(screen->disk_shader_cache);
   nouveau_fence_list_destroy(&screen->fence);
}

static void
nouveau_set_debug_callback(struct pipe_context *pipe,
                           const struct util_debug_callback *cb)
{
   struct nouveau_context *context = nouveau_context(pipe);

   if (cb)
      context->debug = *cb;
   else
      memset(&context->debug, 0, sizeof(context->debug));
}

int
nouveau_context_init(struct nouveau_context *context, struct nouveau_screen *screen)
{
   int ret;

   context->pipe.set_debug_callback = nouveau_set_debug_callback;
   context->screen = screen;

#ifdef __SWITCH__
   /* Switch: share screen's pushbuf/GPU channel (like Mesa 22.2 did).
    * Each nouveau_pushbuf_new creates a new NvGpuChannel, but the 3D engine
    * state was initialized on the screen's channel. Using a separate channel
    * for draw calls would send GPU commands to an uninitialized channel.
    */
   context->client = screen->client;
   context->pushbuf = screen->pushbuf;
   return 0;
#else
   ret = nouveau_client_new(screen->device, &context->client);
   if (ret)
      return ret;

   ret = nouveau_pushbuf_create(screen, context, context->client, screen->channel,
                                4, 512 * 1024, &context->pushbuf);
   if (ret)
      return ret;

   return 0;
#endif
}
