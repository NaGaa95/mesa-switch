#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include "pipe/p_context.h"
#include "pipe/p_state.h"
#include "pipe/p_screen.h"
#include "util/format/u_format.h"
#include "util/u_memory.h"
#include "util/u_debug.h"
#include "util/u_inlines.h"
#include "util/u_hash_table.h"
#include "util/u_thread.h"

#include "frontend/drm_driver.h"
#include "drm-uapi/drm_fourcc.h"

#include "nouveau_switch_public.h"

#include "nouveau/nouveau_winsys.h"
#include "nouveau/nouveau_screen.h"
#include "nouveau/nouveau_context.h"
#include "nouveau/nouveau_buffer.h"
#include <nvif/class.h>
#include <nvif/cl0080.h>


#ifdef __SWITCH__
/* Switch lacks _MTX_INITIALIZER_NP. */
static mtx_t nouveau_screen_mutex;
static void __attribute__((constructor)) init_nouveau_screen_mutex(void) {
   mtx_init(&nouveau_screen_mutex, mtx_plain);
}
#else
static mtx_t nouveau_screen_mutex = _MTX_INITIALIZER_NP;
#endif

PUBLIC struct pipe_screen *
nouveau_switch_screen_create(void)
{
	struct nouveau_drm *drm = NULL;
	struct nouveau_device *dev = NULL;
	struct nouveau_screen *(*init)(struct nouveau_device *);
	struct nouveau_screen *screen = NULL;
	int ret;

	mtx_lock(&nouveau_screen_mutex);

	ret = nouveau_drm_new(0, &drm);
	if (ret)
		goto err;

	struct nv_device_v0 device_args = { .device = ~0ULL };
	ret = nouveau_device_new(&drm->client, NV_DEVICE, &device_args,
	                         sizeof(device_args), &dev);
	if (ret)
		goto err;

	/* BOs use Horizon's process memory entitlement.  Keep reporting the
	 * storage through Nouveau's GART domain internally, but reserve process
	 * headroom because Mesa and the application share this UMA heap.
	 */
	uint64_t total_memory = dev->gart_size;
	uint64_t queried_total = 0;
	uint64_t available_memory = 0;
	const bool have_total = os_get_total_physical_memory(&queried_total);
	const bool have_available =
		os_get_available_system_memory(&available_memory) &&
		available_memory > 0;

	if (have_total)
		total_memory = queried_total;
	if (!have_total && have_available)
		total_memory = available_memory;
	if (!total_memory) {
		debug_printf("nouveau/switch: failed to determine process memory entitlement\n");
		goto err;
	}
	if (!have_available)
		available_memory = total_memory;

	dev->gart_size = total_memory;
	dev->gart_limit =
		nouveau_switch_memory_budget(total_memory, available_memory);

	if (debug_get_bool_option("NOUVEAU_SWITCH_LOG", false) ||
	    debug_get_bool_option("NOUVEAU_SWITCH_STATS", false)) {
		debug_printf("nouveau/switch: UMA memory total=%" PRIu64
		             " MiB available=%" PRIu64 " MiB budget=%" PRIu64
		             " MiB headroom=%" PRIu64
		             " MiB (queries total=%s available=%s)\n",
		             total_memory >> 20, available_memory >> 20,
		             dev->gart_limit >> 20,
		             nouveau_switch_memory_headroom(total_memory) >> 20,
		             have_total ? "ok" : "fallback",
		             have_available ? "ok" : "fallback");
	}

	switch (dev->chipset & ~0xf) {
#if 0
	case 0x30:
	case 0x40:
	case 0x60:
		init = nv30_screen_create;
		break;
	case 0x50:
	case 0x80:
	case 0x90:
	case 0xa0:
		init = nv50_screen_create;
		break;
#endif
	case 0xc0:
	case 0xd0:
	case 0xe0:
	case 0xf0:
	case 0x100:
	case 0x110:
	case 0x120:
	case 0x130:
		init = nvc0_screen_create;
		break;
	default:
		debug_printf("%s: unknown chipset nv%02x\n", __func__,
			     dev->chipset);
		goto err;
	}

	screen = init(dev);
	if (!screen || !screen->base.context_create)
		goto err;

	/* Match the DRM winsys lifecycle contract.  Driver screen destructors use
	 * this flag to distinguish a fully constructed screen from an error unwind.
	 */
	screen->initialized = true;

	mtx_unlock(&nouveau_screen_mutex);
	return &screen->base;

err:
	if (screen) {
		screen->base.destroy(&screen->base);
	} else {
		nouveau_device_del(&dev);
		nouveau_drm_del(&drm);
	}
	mtx_unlock(&nouveau_screen_mutex);
	return NULL;
}

PUBLIC int
nouveau_switch_resource_get_syncpoint(struct pipe_resource *resource, unsigned int *out_threshold)
{
	struct nv04_resource* priv = nv04_resource(resource);
	return nouveau_bo_get_syncpoint(priv->bo, out_threshold);
}

PUBLIC int
nouveau_switch_context_wait_nvmultifence(struct pipe_context *context,
                                         const NvMultiFence *fence)
{
	if (!context || !fence || fence->num_fences > 4)
		return -EINVAL;
	if (!fence->num_fences)
		return 0;

	struct nouveau_context *nv = nouveau_context(context);
	struct nouveau_pushbuf_priv *ppush = nv->pushbuf->user_priv;

	/* The Switch screen owns one native channel.  Serialize acquisition with
	 * command generation and bind the shared pushbuf to the context that will
	 * render the acquired image.
	 */
	nouveau_screen_submission_lock(ppush->screen);
	simple_mtx_lock(&ppush->screen->fence.lock);
	nouveau_pushbuf_bind_context(nv->pushbuf, nv);
	const int ret =
		nouveau_switch_pushbuf_enqueue_nvmultifence(nv->pushbuf, fence);
	simple_mtx_unlock(&ppush->screen->fence.lock);
	nouveau_screen_submission_unlock(ppush->screen);

	if (ret)
		_debug_printf("nouveau/switch: failed to enqueue acquire fence: %d\n",
		              ret);
	return ret;
}

PUBLIC int
nouveau_switch_context_get_error(struct pipe_context *context)
{
	if (!context)
		return -EINVAL;

	struct nouveau_context *nv = nouveau_context(context);
	if (!nv || !nv->pushbuf || !nv->screen)
		return -EINVAL;

	nouveau_screen_submission_lock(nv->screen);
	simple_mtx_lock(&nv->screen->fence.lock);
	const int error = nouveau_switch_pushbuf_get_error(nv->pushbuf);
	simple_mtx_unlock(&nv->screen->fence.lock);
	nouveau_screen_submission_unlock(nv->screen);
	return error;
}

PUBLIC int
nouveau_switch_context_get_cpu_fence(struct pipe_context *context,
                                     NvFence *out_fence)
{
   if (!context || !out_fence)
      return -EINVAL;

   struct nouveau_context *nv = nouveau_context(context);
   if (!nv || !nv->pushbuf || !nv->screen)
      return -EINVAL;

   nouveau_screen_submission_lock(nv->screen);
   simple_mtx_lock(&nv->screen->fence.lock);
   nouveau_pushbuf_bind_context(nv->pushbuf, nv);
   const int ret = nouveau_switch_pushbuf_get_cpu_fence(
      nv->pushbuf, out_fence);
   simple_mtx_unlock(&nv->screen->fence.lock);
   nouveau_screen_submission_unlock(nv->screen);
   return ret;
}

PUBLIC int
nouveau_switch_context_finish_required(struct pipe_context *context,
                                       uint64_t timeout_ns,
                                       const char *reason)
{
	if (!context)
		return -EINVAL;

	struct nouveau_context *nv = nouveau_context(context);
	if (!nv || !nv->pushbuf || !nv->screen)
		return -EINVAL;

	NvFence fence = { .id = UINT32_MAX };
	nouveau_screen_submission_lock(nv->screen);
	simple_mtx_lock(&nv->screen->fence.lock);
	nouveau_pushbuf_bind_context(nv->pushbuf, nv);
	int ret = nouveau_switch_pushbuf_get_cpu_fence(nv->pushbuf, &fence);
	if (ret == -ENODATA)
		ret = nouveau_switch_pushbuf_get_error(nv->pushbuf);
	else if (!ret)
		ret = nouveau_switch_pushbuf_wait_fence_required(
			nv->pushbuf, &fence, timeout_ns, reason);
	simple_mtx_unlock(&nv->screen->fence.lock);
	nouveau_screen_submission_unlock(nv->screen);

	if (ret)
		_debug_printf("nouveau/switch: required context finish failed at %s: "
		              "%d (native=%u:%u)\n",
		              reason ? reason : "unknown", ret, fence.id,
		              fence.value);
	return ret;
}

PUBLIC int
nouveau_switch_resource_get_buffer(struct pipe_resource *resource, NvGraphicBuffer *buffer)
{
	struct winsys_handle whandle = {0};

	if ((resource->target != PIPE_TEXTURE_2D && resource->target != PIPE_TEXTURE_RECT) || resource->last_level != 0 || resource->depth0 != 1 || resource->array_size > 1) {
		debug_printf("%s: unsupported resource type\n", __func__);
		return -1;
	}

	whandle.type = WINSYS_HANDLE_TYPE_SHARED;
	if (!resource->screen->resource_get_handle(resource->screen, NULL, resource, &whandle, 0)) {
		debug_printf("%s: resource_get_handle failed\n", __func__);
		return -2;
	}

	u32 block_height_log2 = 0;
	if (whandle.modifier == DRM_FORMAT_MOD_LINEAR) {
		debug_printf("%s: linear is unsupported format\n", __func__);
		return -3;
	} else if ((whandle.modifier >> 56) == DRM_FORMAT_MOD_VENDOR_NVIDIA) {
		/* Extract block_height_log2 from DRM_FORMAT_MOD_NVIDIA_BLOCK_LINEAR_2D lower 4 bits */
		block_height_log2 = whandle.modifier & 0xf;
	} else if (whandle.modifier == DRM_FORMAT_MOD_INVALID) {
		debug_printf("%s: invalid modifier\n", __func__);
		return -3;
	} else {
		debug_printf("%s: unsupported modifier %llx\n", __func__, (unsigned long long)whandle.modifier);
		return -3;
	}


	u32 format;
	NvColorFormat colorfmt;
	switch (resource->format) {
		case PIPE_FORMAT_R8G8B8A8_UNORM:
			format = PIXEL_FORMAT_RGBA_8888;
			colorfmt = NvColorFormat_A8B8G8R8;
			break;
		case PIPE_FORMAT_R8G8B8X8_UNORM:
			format = PIXEL_FORMAT_RGBX_8888;
			colorfmt = NvColorFormat_X8B8G8R8;
			break;
		case PIPE_FORMAT_B5G6R5_UNORM:
			format = PIXEL_FORMAT_RGB_565;
			colorfmt = NvColorFormat_R5G6B5;
			break;
		default:
			debug_printf("%s: unsupported resource format\n", __func__);
			return -4;
	}

	const u32 bytes_per_pixel = ((u64)colorfmt >> 3) & 0x1F;
	const u32 block_height = 8 * (1U << block_height_log2);
	const u32 width_aligned = whandle.stride / bytes_per_pixel;
	const u32 height_aligned = (resource->height0 + block_height - 1) &~ (block_height - 1);
	const u32 fb_size = whandle.stride*height_aligned;

	memset(buffer, 0, sizeof(*buffer));
	buffer->header.num_ints = (sizeof(NvGraphicBuffer) - sizeof(NativeHandle)) / 4;
	buffer->unk0 = -1;
	buffer->nvmap_id = whandle.handle;
	buffer->unk2 = 0;
	buffer->magic = 0xDAFFCAFF;
	buffer->pid = 42;
	buffer->type = 2; // ?
	buffer->usage = GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE;
	buffer->format = format;
	buffer->ext_format = format;
	buffer->stride = width_aligned;
	buffer->total_size = fb_size;
	buffer->num_planes = 1;
	buffer->unk12 = 0;
	buffer->planes[0].width = resource->width0;
	buffer->planes[0].height = resource->height0;
	buffer->planes[0].color_format = colorfmt;
	buffer->planes[0].layout = NvLayout_BlockLinear;
	buffer->planes[0].pitch = whandle.stride;
	buffer->planes[0].unused = 0;
	buffer->planes[0].offset = whandle.offset;
	buffer->planes[0].kind = NvKind_Generic_16BX2;
	buffer->planes[0].block_height_log2 = block_height_log2;
	buffer->planes[0].scan = NvDisplayScanFormat_Progressive;
	buffer->planes[0].second_field_offset = 0;
	buffer->planes[0].flags = 0;
	buffer->planes[0].size = fb_size;

	return 0;
}
