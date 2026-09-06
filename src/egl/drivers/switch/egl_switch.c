/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2014 Adrián Arroyo Calle <adrian.arroyocalle@gmail.com>
 * Copyright (C) 2018 Jules Blok
 * Copyright (C) 2018-2019 fincs
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "eglconfig.h"
#include "eglcontext.h"
#include "egldisplay.h"
#include "egldriver.h"
#include "eglcurrent.h"
#include "egllog.h"
#include "eglsurface.h"
#include "eglimage.h"
#include "egltypedefs.h"

#include <switch.h>

#include "target-helpers/inline_debug_helper.h"

#include "nouveau/switch/nouveau_switch_public.h"
#ifdef HAVE_SWITCH_ZINK
#include "gallium/drivers/zink/zink_kopper.h"
#include "gallium/drivers/zink/zink_public.h"
#include "kopper_interface.h"

#include <vulkan/vulkan_vi.h>
#endif

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"

#include "util/u_atomic.h"
#include "util/u_debug.h"
#include "util/log.h"
#include "util/os_misc.h"
#include "util/format/u_format.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"

#include "frontend/api.h"
#include "frontend/drm_driver.h"
#include "state_tracker/st_context.h"
#include "state_tracker/st_manager.h"

#include "main/glthread.h"
#include "mesa/glapi/glapi/glapi.h"

#define NUM_BUFFERS 3
#define SWITCH_CONTEXT_DESTROY_TIMEOUT_NS UINT64_C(5000000000)

#ifdef DEBUG
#	define TRACE(x...) _eglLog(_EGL_DEBUG, "egl_switch: " x)
#	define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#	define TRACE(x...)
#  define CALLED()
#endif
_EGL_DRIVER_STANDARD_TYPECASTS(switch_egl)

enum switch_gl_backend {
    SWITCH_GL_BACKEND_NVC0,
    SWITCH_GL_BACKEND_ZINK,
};

struct switch_egl_display
{
    struct pipe_frontend_screen *fscreen;
    struct st_config_options st_options;
    enum switch_gl_backend backend;
    int ref_count;
};

struct switch_egl_config
{
    _EGLConfig base;
    struct st_visual stvis;
};

struct switch_egl_context
{
    _EGLContext base;
    struct st_context *st;
    enum switch_gl_backend backend;
};

struct switch_egl_surface
{
    _EGLSurface base;
    struct pipe_frontend_drawable *drawable;
    struct pipe_resource *attachments[ST_ATTACHMENT_COUNT];
    enum switch_gl_backend backend;
#ifdef HAVE_SWITCH_ZINK
    struct kopper_loader_info kopper_info;
    struct pipe_fence_handle *front_throttle_fence;
    bool flushing_front;
#endif

    NWindow* nw;
    s32 cur_slot;
    struct pipe_resource *buffers[NUM_BUFFERS];
    /* Only populated if an NWindow failure leaves a replacement set with
     * uncertain compositor ownership.  Such memory must stay alive until
     * process teardown because Horizon has no force-detach primitive. */
    struct pipe_resource *quarantined_buffers[NUM_BUFFERS];
    NvFence fences[NUM_BUFFERS];

    bool allow_cpu_acquire_wait;
    bool quarantine_resources;
    int submission_error;
};

struct switch_framebuffer
{
   struct pipe_frontend_drawable base;
   struct switch_egl_display* display;
   struct switch_egl_surface* surface;
   struct pipe_resource template;
};

static inline struct switch_framebuffer *
switch_framebuffer(struct pipe_frontend_drawable *drawable)
{
    return (struct switch_framebuffer *)drawable;
}

//-----------------------------------------------------------------------------
// switch_framebuffer methods
//-----------------------------------------------------------------------------

static uint32_t drifb_ID = 0;

static void
switch_present_failure(struct switch_egl_surface *surface,
                       const char *operation, int error)
{
    mesa_loge("egl-switch present: %s failed (error=%d, slot=%d)",
              operation, error, surface->cur_slot);
}

static bool
switch_record_context_error(struct st_context *st,
                            struct switch_egl_surface *surface,
                            const char *operation)
{
    if (!st || !st->pipe)
        return false;

    int error;
#ifdef HAVE_SWITCH_ZINK
    if (surface->backend == SWITCH_GL_BACKEND_ZINK) {
        const enum pipe_reset_status status =
            st->pipe->get_device_reset_status ?
            st->pipe->get_device_reset_status(st->pipe) : PIPE_NO_RESET;
        error = status == PIPE_NO_RESET ? 0 : -EIO;
    } else
#endif
        error = nouveau_switch_context_get_error(st->pipe);

    if (!error)
        return false;

    surface->base.Lost = EGL_TRUE;
    if (!surface->submission_error) {
        surface->submission_error = error;
        mesa_loge("egl-switch present: %s exposed durable GPU submission "
                  "error %d; dequeued buffer is quarantined",
                  operation, error);
    }
    return true;
}

static bool
switch_surface_cancel_dequeued(struct switch_egl_surface *surface,
                               const NvMultiFence *release_fence,
                               const char *reason)
{
    if (!surface->nw || surface->cur_slot < 0)
        return true;

    const s32 slot = surface->cur_slot;
    Result rc = nwindowCancelBuffer(surface->nw, slot, release_fence);
    (void)reason;

    if (R_FAILED(rc)) {
        switch_present_failure(surface, "nwindowCancelBuffer", (int)rc);
        /* libnx preserves NWindow::cur_slot when cancellation fails.  Keep
         * our slot and backing resources too; forgetting either would let
         * cleanup recycle memory still owned by the producer/compositor. */
        surface->quarantine_resources = true;
        surface->base.Lost = EGL_TRUE;
        return false;
    }

    surface->cur_slot = -1;
    surface->attachments[ST_ATTACHMENT_BACK_LEFT] = NULL;
    return true;
}

static uint32_t
switch_compact_multifence(NvMultiFence *dst, const NvMultiFence *src)
{
    memset(dst, 0, sizeof(*dst));

    if (src->num_fences > ARRAY_SIZE(src->fences))
        return UINT32_MAX;

    for (uint32_t i = 0; i < src->num_fences; i++) {
        if ((int32_t)src->fences[i].id < 0)
            continue;
        dst->fences[dst->num_fences++] = src->fences[i];
    }

    return dst->num_fences;
}

static bool
switch_wait_for_acquire(struct st_context *st,
                        struct switch_egl_surface *surface,
                        const NvMultiFence *fence)
{
    if (!fence->num_fences)
        return true;

    int ret = nouveau_switch_context_wait_nvmultifence(st->pipe, fence);
    if (!ret)
        return true;

    if (switch_record_context_error(st, surface,
                                    "acquire-fence import"))
        return false;

    if (!surface->allow_cpu_acquire_wait) {
        switch_present_failure(surface, "GPU acquire-fence import", ret);
        return false;
    }

    /* Debug compatibility escape hatch. Normal operation must keep the
     * compositor dependency on the GPU timeline instead of blocking the
     * application thread here. */
    NvMultiFence wait_fence = *fence;
    Result rc = nvMultiFenceWait(&wait_fence, -1);
    if (R_FAILED(rc)) {
        switch_present_failure(surface, "nvMultiFenceWait", (int)rc);
        return false;
    }

    mesa_logw_once("egl-switch present: GPU acquire import returned %d; "
                   "used MESA_SWITCH_ACQUIRE_CPU_WAIT compatibility path",
                   ret);
    return true;
}

enum switch_release_status {
    SWITCH_RELEASE_SAFE,
    SWITCH_RELEASE_LIFECYCLE_FLUSH,
    SWITCH_RELEASE_GPU_ERROR,
};

/* Flush the dequeued image and export its release fence. If native fences
 * are unavailable, wait on the CPU before returning an empty multifence.
 */
static enum switch_release_status
switch_prepare_release_fence(struct st_context *st,
                             struct switch_egl_surface *surface,
                             NvMultiFence *release_fence)
{
    memset(release_fence, 0, sizeof(*release_fence));

    if (surface->submission_error)
        return SWITCH_RELEASE_GPU_ERROR;

    if (st) {
        st_context_flush(st, ST_FLUSH_END_OF_FRAME, NULL, NULL, NULL);
        if (switch_record_context_error(st, surface, "release flush"))
            return SWITCH_RELEASE_GPU_ERROR;
    }

    NvFence fence = { .id = UINT32_MAX };
    if (!st)
        return SWITCH_RELEASE_LIFECYCLE_FLUSH;

    const int fence_ret = nouveau_switch_context_get_cpu_fence(
        st->pipe, &fence);
    if (!fence_ret) {
        nvMultiFenceCreate(release_fence, &fence);
        return SWITCH_RELEASE_SAFE;
    }

    /* ENODATA means no native work was submitted. Other errors must not
     * fall back to a stale last-use fence.
     */
    if (fence_ret == -ENODATA)
        return SWITCH_RELEASE_SAFE;
    return SWITCH_RELEASE_GPU_ERROR;
}

struct switch_window_buffer_set {
    struct pipe_resource *resources[NUM_BUFFERS];
    NvGraphicBuffer native[NUM_BUFFERS];
};

static void
switch_window_buffer_set_finish(struct switch_window_buffer_set *set)
{
    for (unsigned i = 0; i < NUM_BUFFERS; i++)
        pipe_resource_reference(&set->resources[i], NULL);
}

static bool
switch_window_buffer_set_create(struct switch_framebuffer *fb,
                                uint32_t width, uint32_t height,
                                struct switch_window_buffer_set *set)
{
    struct pipe_screen *screen = fb->base.fscreen->screen;
    /* Attachment validation mutates fb->template's format and bind flags.
     * Describe presentation images independently.
     */
    struct pipe_resource templ = {
        .target = PIPE_TEXTURE_RECT,
        .format = fb->base.visual->color_format,
        .width0 = width,
        .height0 = height,
        .depth0 = 1,
        .array_size = 1,
        .usage = PIPE_USAGE_DEFAULT,
        /* NWindow receives an exported NvMap ID.  PIPE_BIND_SHARED keeps
         * even sub-1 MiB presentation images out of the private BO pool. */
        .bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SHARED,
    };

    memset(set, 0, sizeof(*set));

    for (unsigned i = 0; i < NUM_BUFFERS; i++) {
        set->resources[i] = screen->resource_create(screen, &templ);
        if (!set->resources[i]) {
            switch_window_buffer_set_finish(set);
            return false;
        }

        if (nouveau_switch_resource_get_buffer(set->resources[i],
                                               &set->native[i])) {
            switch_window_buffer_set_finish(set);
            return false;
        }
    }

    return true;
}

static Result
switch_nwindow_configure_buffer_set(NWindow *nw,
                                    const NvGraphicBuffer native[NUM_BUFFERS])
{
    for (unsigned i = 0; i < NUM_BUFFERS; i++) {
        Result rc = nwindowConfigureBuffer(nw, i,
                                           (NvGraphicBuffer *)&native[i]);
        if (R_FAILED(rc))
            return rc;
    }

    return 0;
}

static void
switch_window_buffer_set_quarantine(struct switch_egl_surface *surface,
                                    struct switch_window_buffer_set *set)
{
    surface->quarantine_resources = true;
    surface->base.Lost = EGL_TRUE;
    for (unsigned i = 0; i < NUM_BUFFERS; i++) {
        assert(!surface->quarantined_buffers[i]);
        surface->quarantined_buffers[i] = set->resources[i];
        set->resources[i] = NULL;
    }
}

static void
switch_surface_invalidate_window_attachments(struct switch_egl_surface *surface,
                                             struct st_context *st,
                                             bool discard_sized_attachments)
{
    /* FRONT/BACK_LEFT are non-owning aliases of buffers[]. */
    surface->attachments[ST_ATTACHMENT_FRONT_LEFT] = NULL;
    surface->attachments[ST_ATTACHMENT_BACK_LEFT] = NULL;

    if (discard_sized_attachments) {
        for (unsigned i = 0; i < ST_ATTACHMENT_COUNT; i++) {
            if (i == ST_ATTACHMENT_FRONT_LEFT ||
                i == ST_ATTACHMENT_BACK_LEFT)
                continue;
            pipe_resource_reference(&surface->attachments[i], NULL);
        }
    }

    for (unsigned i = 0; i < NUM_BUFFERS; i++) {
        surface->fences[i].id = UINT32_MAX;
        surface->fences[i].value = 0;
    }

    p_atomic_inc(&surface->drawable->stamp);
    st_context_invalidate_state(st, ST_INVALIDATE_FB_STATE);
}

#ifdef HAVE_SWITCH_ZINK
struct switch_zink_before_flush_args {
   struct pipe_context *pipe;
   struct pipe_resource *resource;
};

static void
switch_zink_flush_resource_before_submit(void *data)
{
   struct switch_zink_before_flush_args *args = data;
   args->pipe->flush_resource(args->pipe, args->resource);
}
#endif

/* Called by the state tracker for GL_FRONT, glFlush and glFinish. */
static bool
switch_st_framebuffer_flush_front(struct st_context *st,
                                  struct pipe_frontend_drawable *drawable,
                                  enum st_attachment_type statt)
{
   struct switch_egl_surface *surface = switch_framebuffer(drawable)->surface;

#ifdef HAVE_SWITCH_ZINK
   if (surface->backend == SWITCH_GL_BACKEND_ZINK &&
       statt == ST_ATTACHMENT_FRONT_LEFT) {
      _mesa_glthread_finish(st->ctx);

      if (surface->flushing_front)
         return true;

      struct pipe_resource *res = surface->attachments[statt];
      if (!res)
         return false;

      surface->flushing_front = true;
      struct switch_zink_before_flush_args args = {
         .pipe = st->pipe,
         .resource = res,
      };
      struct pipe_fence_handle *new_fence = NULL;
      st_context_flush(st, ST_FLUSH_FRONT, &new_fence,
                       switch_zink_flush_resource_before_submit, &args);
      surface->flushing_front = false;

      if (surface->front_throttle_fence) {
         const bool complete = st->screen->fence_finish(
            st->screen, st->pipe, surface->front_throttle_fence,
            SWITCH_CONTEXT_DESTROY_TIMEOUT_NS);
         st->screen->fence_reference(st->screen, &surface->front_throttle_fence,
                                     NULL);
         if (!complete) {
            if (new_fence)
               st->screen->fence_reference(st->screen, &new_fence, NULL);
            surface->base.Lost = EGL_TRUE;
            return false;
         }
      }
      surface->front_throttle_fence = new_fence;

      st->screen->flush_frontbuffer(st->screen, st->pipe, res, 0, 0, drawable,
                                    0, NULL);
      p_atomic_inc(&drawable->stamp);
   }
#endif

    return true;
}

#ifdef HAVE_SWITCH_ZINK
static bool
switch_zink_framebuffer_validate(struct pipe_frontend_drawable *drawable,
                                 const enum st_attachment_type *statts,
                                 unsigned count, struct pipe_resource **out,
                                 struct pipe_resource **resolve)
{
    struct switch_framebuffer *fb = switch_framebuffer(drawable);
    struct switch_egl_surface *surface = fb->surface;
    struct pipe_screen *screen = drawable->fscreen->screen;
    struct pipe_screen *zscreen = kopper_get_zink_screen(screen);
    bool back_requested = false;

    if (resolve)
        *resolve = NULL;

    for (unsigned i = 0; i < count; i++)
        back_requested |= statts[i] == ST_ATTACHMENT_BACK_LEFT;

    for (unsigned i = 0; i < count; i++) {
        const enum st_attachment_type statt = statts[i];
        if (statt < 0 || statt >= ST_ATTACHMENT_COUNT)
            return false;

        struct pipe_resource *res = surface->attachments[statt];
        if (!res) {
            struct pipe_resource templ = fb->template;

            switch (statt) {
            case ST_ATTACHMENT_BACK_LEFT:
                templ.format = drawable->visual->color_format;
                templ.bind = PIPE_BIND_RENDER_TARGET |
                             PIPE_BIND_DISPLAY_TARGET;
                res = zscreen->resource_create_drawable(
                    zscreen, &templ, &surface->kopper_info);
                break;
            case ST_ATTACHMENT_FRONT_LEFT:
                if (!surface->attachments[ST_ATTACHMENT_BACK_LEFT] &&
                    back_requested) {
                    templ.format = drawable->visual->color_format;
                    templ.bind = PIPE_BIND_RENDER_TARGET |
                                 PIPE_BIND_DISPLAY_TARGET;
                    surface->attachments[ST_ATTACHMENT_BACK_LEFT] =
                        zscreen->resource_create_drawable(
                            zscreen, &templ, &surface->kopper_info);
                }

                templ.format = drawable->visual->color_format;
                if (surface->attachments[ST_ATTACHMENT_BACK_LEFT]) {
                    templ.bind = PIPE_BIND_RENDER_TARGET;
                    res = zscreen->resource_create_drawable(
                        zscreen, &templ,
                        surface->attachments[ST_ATTACHMENT_BACK_LEFT]);
                } else {
                    templ.bind = PIPE_BIND_RENDER_TARGET |
                                 PIPE_BIND_DISPLAY_TARGET;
                    res = zscreen->resource_create_drawable(
                        zscreen, &templ, &surface->kopper_info);
                }
                break;
            case ST_ATTACHMENT_DEPTH_STENCIL:
                templ.format = drawable->visual->depth_stencil_format;
                templ.bind = PIPE_BIND_DEPTH_STENCIL;
                res = screen->resource_create(screen, &templ);
                break;
            case ST_ATTACHMENT_ACCUM:
                templ.format = drawable->visual->accum_format;
                templ.bind = PIPE_BIND_RENDER_TARGET;
                res = screen->resource_create(screen, &templ);
                break;
            default:
                break;
            }

            if (!res)
                return false;
            surface->attachments[statt] = res;
        }

        pipe_resource_reference(&out[i], res);
    }

    return true;
}
#endif

// Called via st_framebuffer_validate.
static bool
switch_st_framebuffer_validate(struct st_context *st, struct pipe_frontend_drawable *drawable,
                   const enum st_attachment_type *statts, unsigned count, struct pipe_resource **out,
                   struct pipe_resource **resolve)
{
    struct switch_framebuffer *fb = switch_framebuffer(drawable);
    struct switch_egl_surface *surface = fb->surface;
    struct pipe_screen *screen = drawable->fscreen->screen;
    bool acquired_buffer = false;
    unsigned i;
    CALLED();

    if (!surface || !screen || !st || !st->pipe || !out ||
        (count && !statts)) {
        _eglError(EGL_BAD_SURFACE,
                  "switch_st_framebuffer_validate: invalid arguments");
        return false;
    }
    if (surface->base.Lost)
        return false;

#ifdef HAVE_SWITCH_ZINK
    if (surface->backend == SWITCH_GL_BACKEND_ZINK)
        return switch_zink_framebuffer_validate(drawable, statts, count, out,
                                                resolve);
#endif

    if (resolve)
        *resolve = NULL;

    for (i = 0; i < count; i++)
    {
        if (statts[i] < 0 || statts[i] >= ST_ATTACHMENT_COUNT) {
            if (acquired_buffer)
                switch_surface_cancel_dequeued(surface,
                                                NULL,
                                                "validation failed");
            _eglError(EGL_BAD_SURFACE,
                      "switch_st_framebuffer_validate: invalid attachment");
            return false;
        }

        struct pipe_resource* res = surface->attachments[statts[i]];
        if (!res)
        {
            switch (statts[i])
            {
                case ST_ATTACHMENT_BACK_LEFT:
                {
                    if (surface->cur_slot >= 0) {
                        switch_present_failure(surface,
                                               "duplicate buffer dequeue",
                                               -EBUSY);
                        surface->base.Lost = EGL_TRUE;
                        _eglError(EGL_BAD_SURFACE,
                                  "switch_st_framebuffer_validate: buffer already dequeued");
                        return false;
                    }

                    NvMultiFence acquire_fence = {0};
                    s32 slot = -1;
                    Result rc = nwindowDequeueBuffer(surface->nw, &slot,
                                                     &acquire_fence);
                    if (R_FAILED(rc)) {
                        switch_present_failure(surface,
                                               "nwindowDequeueBuffer",
                                               (int)rc);
                        surface->base.Lost = EGL_TRUE;
                        _eglError(EGL_BAD_SURFACE,
                                  "switch_st_framebuffer_validate: nwindowDequeueBuffer failed");
                        return false;
                    }

                    surface->cur_slot = slot;

                    if (slot < 0 || slot >= NUM_BUFFERS ||
                        !surface->buffers[slot]) {
                        switch_present_failure(surface,
                                               "invalid dequeued buffer slot",
                                               slot);
                        switch_surface_cancel_dequeued(surface,
                                                       NULL,
                                                       "invalid slot");
                        surface->base.Lost = EGL_TRUE;
                        _eglError(EGL_BAD_SURFACE,
                                  "switch_st_framebuffer_validate: invalid buffer slot");
                        return false;
                    }

                    NvMultiFence compact_fence = {0};
                    uint32_t fence_count =
                        switch_compact_multifence(&compact_fence,
                                                  &acquire_fence);
                    if (fence_count == UINT32_MAX) {
                        switch_present_failure(surface,
                                               "invalid acquire fence count",
                                               acquire_fence.num_fences);
                        switch_surface_cancel_dequeued(surface,
                                                       NULL,
                                                       "invalid fence");
                        surface->base.Lost = EGL_TRUE;
                        _eglError(EGL_BAD_SURFACE,
                                  "switch_st_framebuffer_validate: invalid acquire fence");
                        return false;
                    }

                    if (fence_count) {
                        if (!switch_wait_for_acquire(st, surface,
                                                     &compact_fence)) {
                            switch_surface_cancel_dequeued(surface,
                                                           NULL,
                                                           "acquire wait failed");
                            surface->base.Lost = EGL_TRUE;
                            _eglError(EGL_BAD_SURFACE,
                                      "switch_st_framebuffer_validate: acquire fence import failed");
                            return false;
                        }
                    }

                    acquired_buffer = true;

                    // Use the dequeued buffer as the back buffer
                    res = surface->buffers[slot];
                    break;
                }
                case ST_ATTACHMENT_DEPTH_STENCIL:
                case ST_ATTACHMENT_ACCUM:
                {
                    // Configure format/bind parameters
                    if (statts[i] == ST_ATTACHMENT_DEPTH_STENCIL)
                    {
                        fb->template.format = drawable->visual->depth_stencil_format;
                        fb->template.bind = PIPE_BIND_DEPTH_STENCIL;
                    } else if (statts[i] == ST_ATTACHMENT_ACCUM)
                    {
                        fb->template.format = drawable->visual->accum_format;
                        fb->template.bind = PIPE_BIND_RENDER_TARGET;
                    }

                    // Create the requested resource
                    res = screen->resource_create(screen, &fb->template);
                    break;
                }
                default:
                    break;
            }

            if (!res) {
                if (acquired_buffer)
                    switch_surface_cancel_dequeued(surface,
                                                    NULL,
                                                    "attachment allocation failed");
                _eglError(EGL_BAD_ALLOC,
                          "switch_st_framebuffer_validate: attachment allocation failed");
                return false;
            }

            // Register the attachment for future calls
            surface->attachments[statts[i]] = res;
        }
        pipe_resource_reference(&out[i], res);
    }
    return true;
}

// Called via pipe_frontend_screen_flush_swapbuffers, which itself is only used during glFinish.
// We don't actually want to swap the buffers during glFinish, so our implementation is dummy.
static bool
switch_st_framebuffer_flush_swapbuffers(struct st_context *st, struct pipe_frontend_drawable *drawable)
{
    (void)st;
    (void)drawable;
    return true;
}

//-----------------------------------------------------------------------------
// EGL driver methods
//-----------------------------------------------------------------------------

static void
switch_egl_surface_cleanup(struct switch_egl_surface *surface)
{
    u32 i;
    const bool is_pbuffer = surface->nw == NULL;
    const bool zink_window = !is_pbuffer &&
        surface->backend == SWITCH_GL_BACKEND_ZINK;

    if (surface->nw && !zink_window)
    {
        if (surface->quarantine_resources || surface->submission_error) {
            /* A failed GPU channel or NWindow ownership transition cannot be
             * revoked.  Releasing registrations or BO references here could
             * recycle an NvMap still referenced by the GPU/compositor. */
            mesa_loge("egl-switch present: retaining lost surface and all "
                      "registered resources (GPU error=%d, slot=%d)",
                      surface->submission_error, surface->cur_slot);
            return;
        }

        if (surface->cur_slot >= 0) {
            NvMultiFence release_fence = {0};
            struct switch_egl_context *context =
                switch_egl_context(surface->base.CurrentContext);
            const enum switch_release_status release_status =
                switch_prepare_release_fence(context ? context->st : NULL,
                                             surface, &release_fence);
            if (release_status == SWITCH_RELEASE_GPU_ERROR) {
                /* A failed channel may still reference this memory.
                 * Quarantine the drawable until process teardown instead
                 * of exporting a stale fence.
                 */
                mesa_loge("egl-switch present: retaining lost surface slot "
                          "%d and its resources after GPU error %d",
                          surface->cur_slot, surface->submission_error);
                return;
            }
            if (release_status == SWITCH_RELEASE_LIFECYCLE_FLUSH)
                mesa_logw("egl-switch present: teardown found no current "
                          "context or native release fence; EGL lifecycle "
                          "flush is assumed complete");
            if (!switch_surface_cancel_dequeued(
                    surface,
                    release_fence.num_fences ? &release_fence : NULL,
                    "surface cleanup")) {
                return;
            }
        }
        Result rc = nwindowReleaseBuffers(surface->nw);
        if (R_FAILED(rc)) {
            switch_present_failure(surface, "nwindowReleaseBuffers",
                                   (int)rc);
            surface->quarantine_resources = true;
            surface->base.Lost = EGL_TRUE;
            mesa_loge("egl-switch present: NWindow release failed; retaining "
                      "surface resources with uncertain compositor ownership");
            return;
        }
    }

#ifdef HAVE_SWITCH_ZINK
    if (surface->front_throttle_fence && surface->drawable) {
        struct pipe_screen *screen = surface->drawable->fscreen->screen;
        screen->fence_reference(screen, &surface->front_throttle_fence, NULL);
    }
#endif

    // For window surfaces, FRONT_LEFT/BACK_LEFT are managed by buffers[].
    // For PBuffer surfaces, they're owned by attachments[].
    for (i = 0; i < ST_ATTACHMENT_COUNT; i ++)
    {
        if (!is_pbuffer && !zink_window &&
            (i == ST_ATTACHMENT_FRONT_LEFT ||
             i == ST_ATTACHMENT_BACK_LEFT))
            continue;
        pipe_resource_reference(&surface->attachments[i], NULL);
    }

    for (i = 0; i < NUM_BUFFERS; i ++)
        pipe_resource_reference(&surface->buffers[i], NULL);

    for (i = 0; i < NUM_BUFFERS; i ++)
        pipe_resource_reference(&surface->quarantined_buffers[i], NULL);

    if (surface->drawable) {
        st_api_destroy_drawable(surface->drawable);
        free(surface->drawable);
    }

    free(surface);
}

// Called via eglCreateWindowSurface(), drv->API.CreateWindowSurface().
static _EGLSurface *
switch_create_window_surface(_EGLDisplay *dpy,
    _EGLConfig *conf, void *native_window, const EGLint *attrib_list)
{
    struct switch_egl_surface *surface;
    struct switch_framebuffer *fb = NULL;
    struct switch_egl_display *display = switch_egl_display(dpy);
    struct switch_egl_config *config = switch_egl_config(conf);
    u32 width, height, i;
    CALLED();

    surface = (struct switch_egl_surface*) calloc(1, sizeof (*surface));
    if (!surface)
    {
        _eglError(EGL_BAD_ALLOC, "switch_create_window_surface: failed to allocate switch_egl_surface");
        return NULL;
    }
    surface->cur_slot = -1;
    surface->backend = display->backend;
    surface->allow_cpu_acquire_wait =
        debug_get_bool_option("MESA_SWITCH_ACQUIRE_CPU_WAIT", false);

    if (!_eglInitSurface(&surface->base, dpy, EGL_WINDOW_BIT, conf, attrib_list, native_window))
        goto cleanup;

    fb = (struct switch_framebuffer *) calloc(1, sizeof (*fb));
    if (!fb)
    {
        _eglError(EGL_BAD_ALLOC, "switch_create_window_surface: failed to allocate switch_framebuffer");
        goto cleanup;
    }

    NWindow *nw = (NWindow*)native_window;
    if (!nw || !nwindowIsValid(nw))
    {
        _eglError(EGL_BAD_NATIVE_WINDOW, "switch_create_window_surface: not a valid native window reference");
        goto cleanup;
    }
    surface->nw = nw;

    // Allocate framebuffers and attach them to the native window
    Result rc = nwindowGetDimensions(surface->nw, &width, &height);
    if (R_FAILED(rc) || width == 0 || height == 0 ||
        width > UINT16_MAX || height > UINT16_MAX)
    {
        _eglError(EGL_BAD_NATIVE_WINDOW,
                  "switch_create_window_surface: invalid native window dimensions");
        goto cleanup;
    }
    surface->base.Width = width;
    surface->base.Height = height;
    fb->display = display;
    fb->surface = surface;
    fb->template.target = display->backend == SWITCH_GL_BACKEND_ZINK ?
                          PIPE_TEXTURE_2D : PIPE_TEXTURE_RECT;
    fb->template.format = config->stvis.color_format;
    fb->template.width0 = (u16)width;
    fb->template.height0 = (u16)height;
    fb->template.depth0 = 1;
    fb->template.array_size = 1;
    fb->template.usage = PIPE_USAGE_DEFAULT;
#ifdef HAVE_SWITCH_ZINK
    if (display->backend == SWITCH_GL_BACKEND_ZINK) {
        static_assert(sizeof(struct kopper_vk_surface_create_storage) >=
                      sizeof(VkViSurfaceCreateInfoNN),
                      "Kopper VI surface storage is too small");
        VkViSurfaceCreateInfoNN *vi =
            (VkViSurfaceCreateInfoNN *)&surface->kopper_info.bos;
        vi->sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
        vi->window = surface->nw;
        surface->kopper_info.has_alpha = config->base.AlphaSize > 0;
        surface->kopper_info.initial_swap_interval = surface->base.SwapInterval;
        surface->kopper_info.present_opaque = surface->base.PresentOpaque;
        fb->template.bind = PIPE_BIND_RENDER_TARGET |
                            PIPE_BIND_DISPLAY_TARGET;
    } else {
#endif
        /* NVC0 exports its render targets directly to NWindow. */
        fb->template.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SHARED;
        for (i = 0; i < NUM_BUFFERS; i ++)
        {
            surface->fences[i].id = UINT32_MAX;
            surface->buffers[i] = display->fscreen->screen->resource_create(display->fscreen->screen, &fb->template);
            if (!surface->buffers[i])
            {
                _eglError(EGL_BAD_ALLOC, "switch_create_window_surface: failed to allocate framebuffers");
                goto cleanup;
            }

            NvGraphicBuffer grbuf;
            int err = nouveau_switch_resource_get_buffer(surface->buffers[i], &grbuf);
            if (err != 0)
            {
                _eglError(EGL_BAD_ALLOC, "switch_create_window_surface: nouveau_switch_resource_get_buffer failed");
                goto cleanup;
            }

            rc = nwindowConfigureBuffer(surface->nw, i, &grbuf);
            if (R_FAILED(rc)) {
                _eglError(EGL_BAD_NATIVE_WINDOW,
                          "switch_create_window_surface: nwindowConfigureBuffer failed");
                goto cleanup;
            }
        }
#ifdef HAVE_SWITCH_ZINK
    }
#endif

    surface->drawable = &fb->base;
    surface->cur_slot = -1;

    // Setup the pipe_frontend_drawable
    fb->base.visual = &config->stvis;
    fb->base.flush_front = switch_st_framebuffer_flush_front;
    fb->base.validate = switch_st_framebuffer_validate;
    fb->base.flush_swapbuffers = switch_st_framebuffer_flush_swapbuffers;
    p_atomic_set(&fb->base.stamp, 0);
    fb->base.ID = p_atomic_inc_return(&drifb_ID);
    fb->base.fscreen = display->fscreen;

    return &surface->base;

cleanup:
    if (fb && !surface->drawable)
        free(fb);
    switch_egl_surface_cleanup(surface);
    return NULL;
}


static _EGLSurface *
switch_create_pixmap_surface(_EGLDisplay *disp,
    _EGLConfig *conf, void *native_pixmap, const EGLint *attrib_list)
{
    (void)disp;
    (void)conf;
    (void)native_pixmap;
    (void)attrib_list;
    CALLED();
    _eglError(EGL_BAD_MATCH,
              "switch_create_pixmap_surface: native pixmaps are unsupported");
    return NULL;
}


// PBuffer validate - simpler than window surface, no NWindow involved
static bool
switch_st_pbuffer_validate(struct st_context *st, struct pipe_frontend_drawable *drawable,
                   const enum st_attachment_type *statts, unsigned count, struct pipe_resource **out,
                   struct pipe_resource **resolve)
{
    struct switch_framebuffer *fb = switch_framebuffer(drawable);
    struct switch_egl_surface *surface = fb->surface;
    struct pipe_screen *screen = drawable->fscreen->screen;
    unsigned i;
    (void)st;
    CALLED();

    if (!surface || !screen || !out || (count && !statts)) {
        _eglError(EGL_BAD_SURFACE,
                  "switch_st_pbuffer_validate: invalid arguments");
        return false;
    }
    if (surface->base.Lost)
        return false;

    if (resolve)
        *resolve = NULL;

    for (i = 0; i < count; i++)
    {
        if (statts[i] < 0 || statts[i] >= ST_ATTACHMENT_COUNT) {
            _eglError(EGL_BAD_SURFACE,
                      "switch_st_pbuffer_validate: invalid attachment");
            return false;
        }

        struct pipe_resource* res = surface->attachments[statts[i]];
        if (!res)
        {
            // For PBuffer, all attachments are just regular pipe resources
            switch (statts[i])
            {
                case ST_ATTACHMENT_BACK_LEFT:
                case ST_ATTACHMENT_FRONT_LEFT:
                    fb->template.format = drawable->visual->color_format;
                    fb->template.bind = PIPE_BIND_RENDER_TARGET;
                    break;
                case ST_ATTACHMENT_DEPTH_STENCIL:
                    fb->template.format = drawable->visual->depth_stencil_format;
                    fb->template.bind = PIPE_BIND_DEPTH_STENCIL;
                    break;
                case ST_ATTACHMENT_ACCUM:
                    fb->template.format = drawable->visual->accum_format;
                    fb->template.bind = PIPE_BIND_RENDER_TARGET;
                    break;
                default:
                    _eglError(EGL_BAD_MATCH,
                              "switch_st_pbuffer_validate: unsupported attachment");
                    return false;
            }

            // Create the requested resource
            res = screen->resource_create(screen, &fb->template);
            if (!res)
                return false;

            // Register the attachment for future calls
            surface->attachments[statts[i]] = res;
        }
        pipe_resource_reference(&out[i], res);
    }

    return true;
}


static _EGLSurface *
switch_create_pbuffer_surface(_EGLDisplay *disp,
    _EGLConfig *conf, const EGLint *attrib_list)
{
    struct switch_egl_surface *surface;
    struct switch_framebuffer *fb = NULL;
    struct switch_egl_display *display = switch_egl_display(disp);
    struct switch_egl_config *config = switch_egl_config(conf);
    EGLint width, height;
    CALLED();

    surface = (struct switch_egl_surface*) calloc(1, sizeof (*surface));
    if (!surface)
    {
        _eglError(EGL_BAD_ALLOC, "switch_create_pbuffer_surface: failed to allocate switch_egl_surface");
        return NULL;
    }
    surface->backend = display->backend;

    if (!_eglInitSurface(&surface->base, disp, EGL_PBUFFER_BIT, conf, attrib_list, NULL))
        goto cleanup;

    fb = (struct switch_framebuffer *) calloc(1, sizeof (*fb));
    if (!fb)
    {
        _eglError(EGL_BAD_ALLOC, "switch_create_pbuffer_surface: failed to allocate switch_framebuffer");
        goto cleanup;
    }

    // Get dimensions from attrib_list (parsed by _eglInitSurface into base)
    width = surface->base.Width;
    height = surface->base.Height;

    // Validate dimensions
    if (width <= 0 || height <= 0)
    {
        _eglError(EGL_BAD_ATTRIBUTE, "switch_create_pbuffer_surface: invalid dimensions");
        goto cleanup;
    }

    // No NWindow for PBuffer
    surface->nw = NULL;
    surface->cur_slot = -1;

    // Setup template for resource creation
    fb->display = display;
    fb->surface = surface;
    fb->template.target = display->backend == SWITCH_GL_BACKEND_ZINK ?
                          PIPE_TEXTURE_2D : PIPE_TEXTURE_RECT;
    fb->template.format = config->stvis.color_format;
    fb->template.width0 = (u16)width;
    fb->template.height0 = (u16)height;
    fb->template.depth0 = 1;
    fb->template.array_size = 1;
    fb->template.usage = PIPE_USAGE_DEFAULT;
    fb->template.bind = PIPE_BIND_RENDER_TARGET;

    surface->drawable = &fb->base;

    // Setup the pipe_frontend_drawable
    fb->base.visual = &config->stvis;
    fb->base.flush_front = switch_st_framebuffer_flush_front;
    fb->base.validate = switch_st_pbuffer_validate;  // Use pbuffer-specific validate
    fb->base.flush_swapbuffers = switch_st_framebuffer_flush_swapbuffers;
    p_atomic_set(&fb->base.stamp, 0);
    fb->base.ID = p_atomic_inc_return(&drifb_ID);
    fb->base.fscreen = display->fscreen;

    TRACE("PBuffer created: %dx%d\n", width, height);
    return &surface->base;

cleanup:
    if (fb)
        free(fb);
    free(surface);
    return NULL;
}


static EGLBoolean
switch_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
    struct switch_egl_surface* surface = switch_egl_surface(surf);
    CALLED();

    if (_eglPutSurface(surf))
        switch_egl_surface_cleanup(surface);

    return EGL_TRUE;
}


static EGLBoolean
switch_add_config(_EGLDisplay *dpy, EGLint *id, enum pipe_format colorfmt, enum pipe_format depthfmt)
{
    CALLED();

    struct switch_egl_config* conf;
    conf = (struct switch_egl_config*) calloc(1, sizeof (*conf));
    if (!conf)
        return _eglError(EGL_BAD_ALLOC, "switch_add_config failed to alloc");

    TRACE("Initializing config\n");
    _eglInitConfig(&conf->base, dpy, ++*id);

    // General configuration
    /* Native pixmap rendering is not implemented, and the Switch backend
     * has not completed Khronos conformance testing. Keep these claims
     * conservative until the device test suite establishes otherwise. */
    conf->base.NativeRenderable = EGL_FALSE;
    conf->base.ConfigCaveat = EGL_NON_CONFORMANT_CONFIG;
    conf->base.SurfaceType = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; // Support window and pbuffer surfaces
    conf->base.RenderableType = EGL_OPENGL_BIT | EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR;
    conf->base.Conformant = 0;
    conf->base.MinSwapInterval = 0;
    /* libnx explicitly guarantees immediate (0) and FIFO (1) presentation
     * with three configured buffers.  Do not advertise larger intervals
     * merely because NWindow stores an unchecked u32. */
    conf->base.MaxSwapInterval = 1;

    // Color buffer configuration
    conf->base.RedSize    = util_format_get_component_bits(colorfmt, UTIL_FORMAT_COLORSPACE_RGB, 0);
    conf->base.GreenSize  = util_format_get_component_bits(colorfmt, UTIL_FORMAT_COLORSPACE_RGB, 1);
    conf->base.BlueSize   = util_format_get_component_bits(colorfmt, UTIL_FORMAT_COLORSPACE_RGB, 2);
    conf->base.AlphaSize  = util_format_get_component_bits(colorfmt, UTIL_FORMAT_COLORSPACE_RGB, 3);
    conf->base.BufferSize = conf->base.RedSize+conf->base.GreenSize+conf->base.BlueSize+conf->base.AlphaSize;
    switch (colorfmt) {
    case PIPE_FORMAT_R8G8B8A8_UNORM:
        conf->base.NativeVisualID = PIXEL_FORMAT_RGBA_8888;
        break;
    case PIPE_FORMAT_R8G8B8X8_UNORM:
        conf->base.NativeVisualID = PIXEL_FORMAT_RGBX_8888;
        break;
    case PIPE_FORMAT_B5G6R5_UNORM:
        conf->base.NativeVisualID = PIXEL_FORMAT_RGB_565;
        break;
    default:
        free(conf);
        return EGL_FALSE;
    }

    // Depth/stencil buffer configuration
    if (depthfmt != PIPE_FORMAT_NONE) {
        conf->base.DepthSize   = util_format_get_component_bits(depthfmt, UTIL_FORMAT_COLORSPACE_ZS, 0);
        conf->base.StencilSize = util_format_get_component_bits(depthfmt, UTIL_FORMAT_COLORSPACE_ZS, 1);
    }

    // PBuffer configuration - max size limits
    conf->base.MaxPbufferWidth = 4096;
    conf->base.MaxPbufferHeight = 4096;
    conf->base.MaxPbufferPixels = 4096 * 4096;

    // Visual
    conf->stvis.buffer_mask = ST_ATTACHMENT_FRONT_LEFT_MASK | ST_ATTACHMENT_BACK_LEFT_MASK;
    conf->stvis.color_format = colorfmt;
    conf->stvis.depth_stencil_format = depthfmt;
    conf->stvis.accum_format = PIPE_FORMAT_NONE;

    if (!_eglValidateConfig(&conf->base, EGL_FALSE)) {
        _eglLog(_EGL_DEBUG, "Switch: failed to validate config");
        free(conf);
        return EGL_FALSE;
    }

    _eglLinkConfig(&conf->base);
    return EGL_TRUE;
}


static EGLBoolean
switch_add_configs_for_visuals(_EGLDisplay *dpy)
{
    CALLED();
    struct switch_egl_display *display = switch_egl_display(dpy);
    struct pipe_screen *screen = display->fscreen->screen;

    // List of supported color buffer formats
    static const enum pipe_format colorfmts[] = {
        PIPE_FORMAT_R8G8B8A8_UNORM,
        PIPE_FORMAT_R8G8B8X8_UNORM,
        PIPE_FORMAT_B5G6R5_UNORM,
    };

    // List of supported depth buffer formats
    static const enum pipe_format depthfmts[] = {
        PIPE_FORMAT_NONE,
        PIPE_FORMAT_S8_UINT,
        PIPE_FORMAT_Z16_UNORM,
        PIPE_FORMAT_Z24X8_UNORM,
        PIPE_FORMAT_Z24_UNORM_S8_UINT,
        PIPE_FORMAT_Z32_FLOAT,
        PIPE_FORMAT_Z32_FLOAT_S8X24_UINT,
    };

    // Add all combinations of color/depth buffer formats
    EGLint config_id = 0;
    EGLint i, j;
    const unsigned colorfmt_count =
        display->backend == SWITCH_GL_BACKEND_ZINK ?
        ARRAY_SIZE(colorfmts) - 1 : ARRAY_SIZE(colorfmts);
    for (i = 0; i < colorfmt_count; i ++) {
        if (display->backend == SWITCH_GL_BACKEND_ZINK &&
            (!screen->is_format_supported(
                 screen, colorfmts[i], PIPE_TEXTURE_2D, 0, 0,
                 PIPE_BIND_RENDER_TARGET) ||
             !screen->is_format_supported(
                 screen, colorfmts[i], PIPE_TEXTURE_2D, 0, 0,
                 PIPE_BIND_RENDER_TARGET | PIPE_BIND_DISPLAY_TARGET)))
            continue;

        for (j = 0; j < sizeof(depthfmts)/sizeof(depthfmts[0]); j ++) {
            if (display->backend == SWITCH_GL_BACKEND_ZINK &&
                depthfmts[j] != PIPE_FORMAT_NONE &&
                !screen->is_format_supported(
                    screen, depthfmts[j], PIPE_TEXTURE_2D, 0, 0,
                    PIPE_BIND_DEPTH_STENCIL))
                continue;

            EGLBoolean rc = switch_add_config(dpy, &config_id, colorfmts[i], depthfmts[j]);
            if (!rc)
                return rc;
        }
    }

    if (config_id == 0)
        return _eglError(EGL_NOT_INITIALIZED,
                         "switch_add_configs_for_visuals: no supported configs");

    return EGL_TRUE;
}

// Called from st_api_create_context. This is only ever used for detecting
// whether the ST_MANAGER_BROKEN_INVALIDATE workaround is required.
static int
switch_st_get_param(struct pipe_frontend_screen *fscreen, enum st_manager_param param)
{
    (void)fscreen;
    (void)param;
    return 0;
}

static void
switch_st_set_background_context(struct st_context *st,
                                 struct util_queue_monitoring *queue_info)
{
    /* GLthread requires this callback before unmarshalling its first batch. */
    (void)st;
    (void)queue_info;
}

static bool
switch_glthread_requested(void)
{
    bool enabled = true;

    if (os_get_option("mesa_glthread"))
        enabled = debug_get_bool_option("mesa_glthread", enabled);
    if (os_get_option("MESA_GLTHREAD"))
        enabled = debug_get_bool_option("MESA_GLTHREAD", enabled);
    if (os_get_option("MESA_SWITCH_GLTHREAD"))
        enabled = debug_get_bool_option("MESA_SWITCH_GLTHREAD", enabled);

    return enabled;
}

static enum switch_gl_backend
switch_gl_backend_requested(const _EGLDisplay *dpy)
{
    const char *name = os_get_option("MESA_SWITCH_GL_DRIVER");

    if (name && (strcmp(name, "nvc0") == 0 ||
                 strcmp(name, "nouveau") == 0))
        return SWITCH_GL_BACKEND_NVC0;

    if (name && strcmp(name, "zink") == 0)
        return SWITCH_GL_BACKEND_ZINK;

    if (name && name[0])
        mesa_logw("egl-switch: unknown MESA_SWITCH_GL_DRIVER=%s; using "
                  "the EGL-selected backend", name);

    if (dpy->Options.Zink)
        return SWITCH_GL_BACKEND_ZINK;

    return SWITCH_GL_BACKEND_NVC0;
}

static void
switch_display_destroy(_EGLDisplay *dpy)
{
    struct switch_egl_display *display = switch_egl_display(dpy);
    if (!display)
        return;

    if (display->fscreen) {
        struct pipe_screen *screen = display->fscreen->screen;
        st_screen_destroy(display->fscreen);
        if (screen)
            screen->destroy(screen);
        free(display->fscreen);
    }

    dpy->DriverData = NULL;
    free(display);
}

static void
switch_display_release(_EGLDisplay *dpy)
{
    if (!dpy)
        return;

    struct switch_egl_display *display = switch_egl_display(dpy);
    assert(display && display->ref_count > 0);
    if (!p_atomic_dec_zero(&display->ref_count))
        return;

    _eglCleanupDisplay(dpy);
    switch_display_destroy(dpy);
}

static EGLBoolean
switch_initialize(_EGLDisplay *dpy)
{
    struct switch_egl_display *display;
    struct pipe_frontend_screen *stmgr;
    struct pipe_screen *screen;
    CALLED();

    // Default to a single-file shader cache to avoid SD card overhead on the Switch
    setenv("MESA_DISK_CACHE_SINGLE_FILE", "1", 0);

    display = switch_egl_display(dpy);
    if (display) {
        p_atomic_inc(&display->ref_count);
        return EGL_TRUE;
    }

    display = (struct switch_egl_display*) calloc(1, sizeof (*display));
    if (!display) {
        _eglError(EGL_BAD_ALLOC, "switch_initialize");
        return EGL_FALSE;
    }
    dpy->DriverData = display;
    dpy->Version = 14;
    if (dpy->Options.ForceSoftware) {
        _eglError(EGL_NOT_INITIALIZED,
                  "switch_initialize: no software renderer is available");
        switch_display_destroy(dpy);
        return EGL_FALSE;
    }
    display->backend = switch_gl_backend_requested(dpy);
#ifndef HAVE_SWITCH_ZINK
    if (display->backend == SWITCH_GL_BACKEND_ZINK) {
        _eglError(EGL_NOT_INITIALIZED,
                  "switch_initialize: Zink support is not present in this SDK");
        switch_display_destroy(dpy);
        return EGL_FALSE;
    }
#endif

    dpy->ClientAPIs = 0;
    if (_eglIsApiValid(EGL_OPENGL_API))
        dpy->ClientAPIs |= EGL_OPENGL_BIT;
    if (_eglIsApiValid(EGL_OPENGL_ES_API))
        dpy->ClientAPIs |= EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR;

    dpy->Extensions.EXT_create_context_robustness = EGL_TRUE;
    dpy->Extensions.KHR_create_context = EGL_TRUE;
    dpy->Extensions.KHR_create_context_no_error = EGL_TRUE;
    dpy->Extensions.KHR_surfaceless_context = EGL_TRUE;
    dpy->Extensions.MESA_horizon_surface_resize = EGL_TRUE;
    /* Reject release-behavior NONE until pending shared-channel work can
     * be retained and retired safely during surface destruction.
     */
    dpy->Extensions.KHR_context_flush_control = EGL_FALSE;

    /* The frontend does not plumb driconf into st_config_options. */
    display->st_options.allow_glsl_extension_directive_midshader = true;

    stmgr = CALLOC_STRUCT(pipe_frontend_screen);
    if (!stmgr) {
        _eglError(EGL_BAD_ALLOC, "switch_initialize");
        switch_display_destroy(dpy);
        return EGL_FALSE;
    }
    display->fscreen = stmgr;

    stmgr->get_param = switch_st_get_param;
    stmgr->set_background_context = switch_st_set_background_context;

#ifdef HAVE_SWITCH_ZINK
    if (display->backend == SWITCH_GL_BACKEND_ZINK)
        screen = zink_create_screen(NULL, NULL);
    else
#endif
        screen = nouveau_switch_screen_create();
    if (!screen)
    {
        mesa_loge("egl-switch: failed to create %s GL screen",
                  display->backend == SWITCH_GL_BACKEND_ZINK ?
                     "zink" : "nvc0");
        switch_display_destroy(dpy);
        return EGL_FALSE;
    }

    TRACE("Using %s GL backend\n",
          display->backend == SWITCH_GL_BACKEND_ZINK ?
             "zink over NVK" : "nvc0");

    /* Gallium's generic debug wrappers do not all forward
     * resource_create_drawable, which Kopper requires for window images. */
#ifdef HAVE_SWITCH_ZINK
    if (display->backend == SWITCH_GL_BACKEND_ZINK)
        stmgr->screen = screen;
    else
#endif
        stmgr->screen = debug_screen_wrap(screen);

    if (!switch_add_configs_for_visuals(dpy)) {
        _eglCleanupDisplay(dpy);
        switch_display_destroy(dpy);
        return EGL_FALSE;
    }

    p_atomic_set(&display->ref_count, 1);
    return EGL_TRUE;
}


static EGLBoolean
switch_terminate(_EGLDisplay* dpy)
{
    CALLED();

    // Release all non-current Context/Surfaces
    _eglReleaseDisplayResources(dpy);
    switch_display_release(dpy);

    return EGL_TRUE;
}


static _EGLContext*
switch_create_context(_EGLDisplay *dpy, _EGLConfig *conf,
    _EGLContext *share_list, const EGLint *attrib_list)
{
    struct switch_egl_context *context;
    struct switch_egl_context *share_ctx = switch_egl_context(share_list);
    struct switch_egl_display *display = switch_egl_display(dpy);
    struct switch_egl_config *config = switch_egl_config(conf);
    CALLED();

    context = (struct switch_egl_context*) calloc(1, sizeof (*context));
    if (!context) {
        _eglError(EGL_BAD_ALLOC, "switch_create_context");
        return NULL;
    }
    context->backend = display->backend;

    if (!_eglInitContext(&context->base, dpy, conf, share_list, attrib_list))
        goto cleanup;

    struct st_context_attribs attribs;
    memset(&attribs, 0, sizeof(attribs));

    attribs.major = context->base.ClientMajorVersion;
    attribs.minor = context->base.ClientMinorVersion;
    attribs.visual = config->stvis;
    attribs.options = display->st_options;

    switch (eglQueryAPI()) {
        case EGL_OPENGL_API:
            switch (context->base.Profile) {
                case EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR:
                    /* EGL_KHR_create_context ignores the profile mask
                     * before OpenGL 3.2.
                     */

                    if (attribs.major > 3 || (attribs.major == 3 && attribs.minor >= 2)) {
                        attribs.profile = API_OPENGL_CORE;
                        break;
                    }
                    /* fall-through */
                case EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT_KHR:
                    attribs.profile = API_OPENGL_COMPAT;
                    break;
                default:
                    _eglError(EGL_BAD_CONFIG, "switch_create_context");
                    goto cleanup;
            }
            break;
        case EGL_OPENGL_ES_API:
            switch (context->base.ClientMajorVersion) {
            case 1:
                attribs.profile = API_OPENGLES;
                break;
            case 2:
            case 3: // API_OPENGLES2 is used for OpenGL ES 3.x too
                attribs.profile = API_OPENGLES2;
                break;
            default:
                _eglError(EGL_BAD_CONFIG, "switch_create_context");
                goto cleanup;
            }
            break;
        default:
            _eglError(EGL_BAD_CONFIG, "switch_create_context");
            goto cleanup;
    }

    enum st_context_error error;

    if (context->base.Flags & EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR)
        attribs.flags |= ST_CONTEXT_FLAG_DEBUG;
    if (context->base.Flags & EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE_BIT_KHR)
        attribs.flags |= ST_CONTEXT_FLAG_FORWARD_COMPATIBLE;
    if (context->base.Flags & EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR)
        attribs.context_flags |= PIPE_CONTEXT_ROBUST_BUFFER_ACCESS;
    if (context->base.NoError)
        attribs.flags |= ST_CONTEXT_FLAG_NO_ERROR;

    if (context->base.ResetNotificationStrategy != EGL_NO_RESET_NOTIFICATION_KHR)
        attribs.context_flags |= PIPE_CONTEXT_LOSE_CONTEXT_ON_RESET;

    context->st = st_api_create_context(display->fscreen, &attribs, &error,
                                        share_ctx ? share_ctx->st : NULL);
    if (!context->st || error != ST_CONTEXT_SUCCESS) {
        EGLint egl_error = error == ST_CONTEXT_ERROR_NO_MEMORY ?
                           EGL_BAD_ALLOC : EGL_BAD_MATCH;
        const char *reason = error == ST_CONTEXT_ERROR_NO_MEMORY ?
                             "out of memory" :
                             error == ST_CONTEXT_ERROR_BAD_VERSION ?
                             "unsupported GL version" :
                             "unknown state-tracker error";

        _eglLog(_EGL_WARNING,
                "Switch: context creation failed: requested=%d.%d "
                "profile=%d flags=0x%x pipe_flags=0x%x, error=%d (%s)",
                attribs.major, attribs.minor, attribs.profile, attribs.flags,
                attribs.context_flags, error, reason);
        _eglError(egl_error, "switch_create_context");
        goto cleanup;
    }

    context->st->frontend_context = context;

    /* Enabling GLthread changes public GL dispatch. */
    const bool enable_glthread = switch_glthread_requested();
    if (enable_glthread)
        _mesa_glthread_init(context->st->ctx);

    return &context->base;

cleanup:
    free(context);
    return NULL;
}

static EGLBoolean
switch_destroy_context(_EGLDisplay *disp, _EGLContext *ctx)
{
   if (!ctx)
      return EGL_TRUE;

   struct switch_egl_context *context = switch_egl_context(ctx);
   _EGLDisplay *owner_disp = ctx->Resource.Display;
   (void)disp;
   CALLED();

   if (_eglPutContext(ctx)) {
      _mesa_glthread_finish(context->st->ctx);

      if (context->backend == SWITCH_GL_BACKEND_ZINK) {
         struct pipe_fence_handle *fence = NULL;
         st_context_flush(context->st, ST_FLUSH_END_OF_FRAME, &fence, NULL,
                          NULL);
         if (fence) {
            const bool complete = context->st->screen->fence_finish(
               context->st->screen, context->st->pipe, fence,
               SWITCH_CONTEXT_DESTROY_TIMEOUT_NS);
            context->st->screen->fence_reference(context->st->screen, &fence,
                                                 NULL);
            if (!complete) {
               mesa_loge("egl-switch: zink context completion timed out "
                         "during destruction; retaining the context "
                         "and display ownership graph");
               p_atomic_inc(&switch_egl_display(owner_disp)->ref_count);
               return EGL_TRUE;
            }
         }
      } else {
         /* NVC0 shares one channel and shader text heap between contexts.
          * Retire the exact native completion before releasing either. */
         st_context_flush(context->st, ST_FLUSH_END_OF_FRAME, NULL, NULL, NULL);
         const int finish_error = nouveau_switch_context_finish_required(
            context->st->pipe, SWITCH_CONTEXT_DESTROY_TIMEOUT_NS,
            "EGL context destruction");
         if (finish_error) {
            mesa_loge("egl-switch: quarantining context %p after required "
                      "destroy wait failed: %d",
                      (void *)context, finish_error);
            p_atomic_inc(&switch_egl_display(owner_disp)->ref_count);
            return EGL_TRUE;
         }
      }

      st_destroy_context(context->st);
      free(context);
      ctx = NULL;
   }
   return EGL_TRUE;
}

static EGLBoolean
switch_make_current(_EGLDisplay* dpy, _EGLSurface *dsurf,
    _EGLSurface *rsurf, _EGLContext *ctx)
{
    struct switch_egl_context* cont = switch_egl_context(ctx);
    struct switch_egl_surface* draw_surf = switch_egl_surface(dsurf);
    struct switch_egl_surface* read_surf = switch_egl_surface(rsurf);
    CALLED();

    _EGLContext *old_ctx;
    _EGLSurface *old_dsurf, *old_rsurf;
    _EGLDisplay *old_dpy;
    _EGLContext *failed_ctx = NULL, *unbound_ctx = NULL;
    _EGLSurface *failed_dsurf = NULL, *failed_rsurf = NULL;
    _EGLSurface *unbound_dsurf = NULL, *unbound_rsurf = NULL;
    bool restored = false;

    if (!_eglBindContext(ctx, dsurf, rsurf, &old_ctx, &old_dsurf, &old_rsurf))
        return EGL_FALSE;
    old_dpy = old_ctx ? old_ctx->Resource.Display : NULL;

    if (old_ctx == ctx && old_dsurf == dsurf && old_rsurf == rsurf) {
        _eglPutSurface(old_dsurf);
        _eglPutSurface(old_rsurf);
        _eglPutContext(old_ctx);
        return EGL_TRUE;
    }

    /* Drain workers before rebinding state-tracker contexts. */
    struct switch_egl_context *old_cont = switch_egl_context(old_ctx);
    if (old_cont) {
        _mesa_glthread_finish(old_cont->st->ctx);
    }
    if (cont && cont != old_cont) {
        _mesa_glthread_finish(cont->st->ctx);
    }

    /* The state tracker does not flush when rebinding the same context.
     * Fence the old drawable before it can be destroyed.
     */
    struct switch_egl_surface *releasing_draw =
        old_dsurf != dsurf ? switch_egl_surface(old_dsurf) : NULL;
    struct switch_egl_surface *releasing_read =
        old_rsurf != rsurf ? switch_egl_surface(old_rsurf) : NULL;
    const bool release_window_buffer = old_cont &&
        ((releasing_draw && releasing_draw->nw &&
          releasing_draw->cur_slot >= 0) ||
         (releasing_read && releasing_read != releasing_draw &&
          releasing_read->nw && releasing_read->cur_slot >= 0));
    if (release_window_buffer) {
        st_context_flush(old_cont->st, ST_FLUSH_END_OF_FRAME,
                         NULL, NULL, NULL);
        if (releasing_draw)
            switch_record_context_error(old_cont->st, releasing_draw,
                                        "drawable release flush");
        if (releasing_read && releasing_read != releasing_draw)
            switch_record_context_error(old_cont->st, releasing_read,
                                        "read drawable release flush");
    }

    EGLBoolean ret = st_api_make_current(cont ? cont->st : NULL,
        draw_surf ? draw_surf->drawable : NULL,
        read_surf ? read_surf->drawable : NULL);

    if (!ret) {
        /* Restore the previous EGL and state-tracker binding. */
        _eglBindContext(old_ctx, old_dsurf, old_rsurf, &failed_ctx,
                        &failed_dsurf, &failed_rsurf);
        assert((cont ? &cont->base : NULL) == failed_ctx &&
               failed_dsurf == dsurf && failed_rsurf == rsurf);

        struct switch_egl_surface *old_draw = switch_egl_surface(old_dsurf);
        struct switch_egl_surface *old_read = switch_egl_surface(old_rsurf);
        restored = st_api_make_current(old_cont ? old_cont->st : NULL,
            old_draw ? old_draw->drawable : NULL,
            old_read ? old_read->drawable : NULL);

        if (!restored) {
            /* Keep EGL unbound if the old state cannot be restored. */
            _eglBindContext(NULL, NULL, NULL, &unbound_ctx, &unbound_dsurf,
                            &unbound_rsurf);
            assert(unbound_ctx == old_ctx && unbound_dsurf == old_dsurf &&
                   unbound_rsurf == old_rsurf);
            st_api_make_current(NULL, NULL, NULL);
        }
    }

    if (ret && ctx)
        p_atomic_inc(&switch_egl_display(dpy)->ref_count);
    else if (restored && old_ctx)
        p_atomic_inc(&switch_egl_display(old_dpy)->ref_count);

    switch_destroy_surface(dpy, failed_dsurf);
    switch_destroy_surface(dpy, failed_rsurf);
    switch_destroy_context(dpy, failed_ctx);

    switch_destroy_surface(dpy, unbound_dsurf);
    switch_destroy_surface(dpy, unbound_rsurf);
    switch_destroy_context(dpy, unbound_ctx);

    switch_destroy_surface(dpy, old_dsurf);
    switch_destroy_surface(dpy, old_rsurf);
    if (old_ctx) {
       switch_destroy_context(dpy, old_ctx);
       switch_display_release(old_dpy);
    }

    if (!ret)
       return _eglError(EGL_BAD_MATCH, "switch_make_current");

    return EGL_TRUE;
}

static EGLBoolean
switch_swap_interval(_EGLDisplay *dpy, _EGLSurface *surf, EGLint interval)
{
   CALLED();
   struct switch_egl_surface *surface = switch_egl_surface(surf);

   if (!surface->nw)
      return _eglError(EGL_BAD_SURFACE, "switch_swap_interval");

#ifdef HAVE_SWITCH_ZINK
   if (surface->backend == SWITCH_GL_BACKEND_ZINK) {
      surface->kopper_info.initial_swap_interval = interval;
      struct pipe_resource *res =
         surface->attachments[ST_ATTACHMENT_BACK_LEFT]
            ? surface->attachments[ST_ATTACHMENT_BACK_LEFT]
            : surface->attachments[ST_ATTACHMENT_FRONT_LEFT];
      if (res) {
         struct switch_egl_context *context =
            switch_egl_context(surface->base.CurrentContext);
         if (context && context->st)
            _mesa_glthread_finish(context->st->ctx);

         struct switch_framebuffer *fb = switch_framebuffer(surface->drawable);
         struct pipe_screen *zscreen =
            kopper_get_zink_screen(fb->display->fscreen->screen);
         zink_kopper_set_swap_interval(zscreen, res, interval);
         if (!zink_kopper_check(res)) {
            surface->base.Lost = EGL_TRUE;
            return _eglError(
               EGL_BAD_SURFACE,
               "switch_swap_interval: zink swapchain recreation failed");
         }
      }
      return EGL_TRUE;
   }
#endif

   Result rc = nwindowSetSwapInterval(surface->nw, interval);
   if (R_FAILED(rc)) {
      switch_present_failure(surface, "nwindowSetSwapInterval", (int)rc);
      NvMultiFence release_fence = {0};
      struct switch_egl_context *context =
         switch_egl_context(surface->base.CurrentContext);
      if (context && context->st)
         _mesa_glthread_finish(context->st->ctx);
      const enum switch_release_status release_status =
         switch_prepare_release_fence(context ? context->st : NULL, surface,
                                      &release_fence);
      if (release_status != SWITCH_RELEASE_GPU_ERROR)
         switch_surface_cancel_dequeued(
            surface, release_fence.num_fences ? &release_fence : NULL,
            "swap interval failed");
      surface->base.Lost = EGL_TRUE;
      return _eglError(EGL_BAD_SURFACE,
                       "switch_swap_interval: nwindowSetSwapInterval failed");
   }
   return EGL_TRUE;
}

#ifdef HAVE_SWITCH_ZINK
static EGLBoolean
switch_resize_zink_surface(struct switch_egl_surface *surface,
                           struct switch_egl_context *context,
                           struct switch_framebuffer *fb,
                           EGLint width, EGLint height)
{
    if (context->backend != SWITCH_GL_BACKEND_ZINK ||
        _eglGetCurrentContext() != &context->base ||
        context->base.DrawSurface != &surface->base)
        return _eglError(EGL_BAD_MATCH,
                         "switch_resize_surface: zink surface is not current");

    _mesa_glthread_finish(context->st->ctx);

    struct pipe_fence_handle *fence = NULL;
    st_context_flush(context->st, ST_FLUSH_END_OF_FRAME,
                     &fence, NULL, NULL);
    bool complete = true;
    if (fence) {
        complete = context->st->screen->fence_finish(
            context->st->screen, context->st->pipe, fence,
            SWITCH_CONTEXT_DESTROY_TIMEOUT_NS);
        context->st->screen->fence_reference(context->st->screen,
                                              &fence, NULL);
    }
    if (!complete ||
        switch_record_context_error(context->st, surface, "zink resize")) {
        surface->base.Lost = EGL_TRUE;
        return _eglError(EGL_BAD_SURFACE,
                         "switch_resize_surface: zink GPU drain failed");
    }

    struct pipe_resource *back =
        surface->attachments[ST_ATTACHMENT_BACK_LEFT];
    struct pipe_resource *front =
        surface->attachments[ST_ATTACHMENT_FRONT_LEFT];
    struct pipe_resource *primary = back ? back : front;
    struct pipe_resource *secondary =
        back && front && back != front ? front : NULL;

    if (primary && !zink_kopper_resize(context->st->screen, primary,
                                       secondary, width, height)) {
        surface->base.Lost = EGL_TRUE;
        return _eglError(EGL_BAD_SURFACE,
                         "switch_resize_surface: zink swapchain release failed");
    }

    Result rc = nwindowSetDimensions(surface->nw, width, height);
    if (R_FAILED(rc)) {
        surface->base.Lost = EGL_TRUE;
        mesa_loge("egl-switch: zink resize to %dx%d failed after "
                  "swapchain release (0x%x)", width, height, rc);
        return _eglError(EGL_BAD_SURFACE,
                         "switch_resize_surface: NWindow resize failed");
    }

    if (primary && !zink_kopper_finish_resize(context->st->screen,
                                              primary)) {
        surface->base.Lost = EGL_TRUE;
        return _eglError(EGL_BAD_SURFACE,
                         "switch_resize_surface: zink swapchain recreation failed");
    }

    for (unsigned i = 0; i < ST_ATTACHMENT_COUNT; i++) {
        struct pipe_resource *res = surface->attachments[i];
        if (res == primary &&
            (i == ST_ATTACHMENT_BACK_LEFT ||
             i == ST_ATTACHMENT_FRONT_LEFT))
            continue;
        pipe_resource_reference(&surface->attachments[i], NULL);
    }

    fb->template.width0 = width;
    fb->template.height0 = height;
    surface->base.Width = width;
    surface->base.Height = height;
    surface->cur_slot = -1;
    p_atomic_inc(&surface->drawable->stamp);
    st_context_invalidate_state(context->st, ST_INVALIDATE_FB_STATE);
    return EGL_TRUE;
}
#endif

static EGLBoolean
switch_resize_surface(_EGLDisplay *dpy, _EGLSurface *surf,
                      EGLint width, EGLint height)
{
    (void)dpy;
    struct switch_egl_surface *surface = switch_egl_surface(surf);
    struct switch_egl_context *context =
        switch_egl_context(surface->base.CurrentContext);
    struct switch_framebuffer *fb = switch_framebuffer(surface->drawable);
    struct switch_window_buffer_set replacement = {0};
    struct switch_window_buffer_set rollback = {0};
    const uint32_t old_width = surface->base.Width;
    const uint32_t old_height = surface->base.Height;
    Result rc;

    if (surface->base.Lost)
        return _eglError(EGL_BAD_SURFACE,
                         "switch_resize_surface: surface is lost");
    if (!surface->nw || !context || !context->st || !fb ||
        width <= 0 || height <= 0 ||
        width > UINT16_MAX || height > UINT16_MAX)
        return _eglError(EGL_BAD_PARAMETER,
                         "switch_resize_surface: invalid arguments");
    if ((uint32_t)width == old_width && (uint32_t)height == old_height)
        return EGL_TRUE;

#ifdef HAVE_SWITCH_ZINK
    if (surface->backend == SWITCH_GL_BACKEND_ZINK)
        return switch_resize_zink_surface(surface, context, fb,
                                          width, height);
#endif

    /* Allocate/export every replacement before disturbing the live NWindow.
     * This makes ordinary allocation failures completely transactional. */
    if (!switch_window_buffer_set_create(fb, width, height, &replacement))
        return _eglError(EGL_BAD_ALLOC,
                         "switch_resize_surface: replacement allocation failed");

    _mesa_glthread_finish(context->st->ctx);

    if (surface->cur_slot >= 0) {
        NvMultiFence release_fence = {0};
        const enum switch_release_status release_status =
            switch_prepare_release_fence(context->st, surface,
                                         &release_fence);
        if (release_status == SWITCH_RELEASE_GPU_ERROR) {
            switch_window_buffer_set_finish(&replacement);
            return _eglError(EGL_BAD_SURFACE,
                             "switch_resize_surface: GPU release failed");
        }
        if (!switch_surface_cancel_dequeued(
                surface,
                release_fence.num_fences ? &release_fence : NULL,
                "surface resize")) {
            switch_window_buffer_set_finish(&replacement);
            return _eglError(EGL_BAD_SURFACE,
                             "switch_resize_surface: buffer cancellation failed");
        }
    } else {
        /* There may still be deferred draws which do not currently reference
         * a dequeued color image.  Drain them before old depth/accum resources
         * or registered presentation buffers can be released. */
        struct pipe_fence_handle *wait_fence = NULL;
        st_context_flush(context->st,
                         ST_FLUSH_END_OF_FRAME | ST_FLUSH_WAIT,
                         &wait_fence, NULL, NULL);
        if (wait_fence)
            context->st->screen->fence_reference(context->st->screen,
                                                  &wait_fence, NULL);
        if (switch_record_context_error(context->st, surface,
                                        "resize drain")) {
            switch_window_buffer_set_finish(&replacement);
            return _eglError(EGL_BAD_SURFACE,
                             "switch_resize_surface: GPU drain failed");
        }
    }

    /* Keep independent references for rollback.  NWindow owns only the NvMap
     * registration metadata, not Gallium resource lifetimes. */
    for (unsigned i = 0; i < NUM_BUFFERS; i++) {
        pipe_resource_reference(&rollback.resources[i], surface->buffers[i]);
        if (nouveau_switch_resource_get_buffer(rollback.resources[i],
                                               &rollback.native[i])) {
            switch_window_buffer_set_finish(&rollback);
            switch_window_buffer_set_finish(&replacement);
            surface->base.Lost = EGL_TRUE;
            return _eglError(EGL_BAD_SURFACE,
                             "switch_resize_surface: rollback export failed");
        }
    }

    rc = nwindowReleaseBuffers(surface->nw);
    if (R_FAILED(rc)) {
        switch_present_failure(surface, "resize nwindowReleaseBuffers",
                               (int)rc);
        /* The producer may have disconnected even when an IPC error was
         * reported; ownership is unknowable, so neither old nor replacement
         * storage may be recycled. */
        switch_window_buffer_set_quarantine(surface, &replacement);
        switch_window_buffer_set_finish(&rollback);
        return _eglError(EGL_BAD_SURFACE,
                         "switch_resize_surface: NWindow release failed");
    }

    rc = nwindowSetDimensions(surface->nw, width, height);
    if (R_SUCCEEDED(rc))
        rc = switch_nwindow_configure_buffer_set(surface->nw,
                                                  replacement.native);

    if (R_FAILED(rc)) {
        const Result configure_error = rc;

        /* A partial configure has compositor references to replacement BOs.
         * Detach it before attempting to restore the last known-good set. */
        Result release_rc = nwindowReleaseBuffers(surface->nw);
        Result restore_rc = release_rc;
        if (R_SUCCEEDED(restore_rc))
            restore_rc = nwindowSetDimensions(surface->nw,
                                               old_width, old_height);
        if (R_SUCCEEDED(restore_rc))
            restore_rc = switch_nwindow_configure_buffer_set(surface->nw,
                                                               rollback.native);

        if (R_FAILED(release_rc)) {
            /* Replacement registrations may still be live. */
            switch_window_buffer_set_quarantine(surface, &replacement);
        } else {
            switch_window_buffer_set_finish(&replacement);
        }

        if (R_FAILED(restore_rc)) {
            switch_present_failure(surface, "resize rollback", (int)restore_rc);
            surface->quarantine_resources = true;
            surface->base.Lost = EGL_TRUE;
            /* Old buffers may be partially registered after rollback.  Their
             * primary surface references deliberately remain intact. */
            switch_window_buffer_set_finish(&rollback);
            mesa_loge("egl-switch present: resize to %dx%d failed (0x%x) "
                      "and rollback failed (0x%x); surface quarantined",
                      width, height, configure_error, restore_rc);
            return _eglError(EGL_BAD_SURFACE,
                             "switch_resize_surface: reconfiguration and rollback failed");
        }

        surface->cur_slot = -1;
        switch_surface_invalidate_window_attachments(surface, context->st,
                                                      false);
        switch_window_buffer_set_finish(&rollback);
        mesa_logw("egl-switch present: resize to %dx%d failed (0x%x); "
                  "restored %ux%u buffer set",
                  width, height, configure_error, old_width, old_height);
        return _eglError(EGL_BAD_ALLOC,
                         "switch_resize_surface: NWindow reconfiguration failed");
    }

    /* Transfer replacement registrations before releasing the old
     * resources. BO destruction waits for native reads/writes and
     * quarantines storage if completion is unknown.
     */
    switch_surface_invalidate_window_attachments(surface, context->st, true);
    for (unsigned i = 0; i < NUM_BUFFERS; i++) {
        pipe_resource_reference(&surface->buffers[i], NULL);
        surface->buffers[i] = replacement.resources[i];
        replacement.resources[i] = NULL;
    }
    switch_window_buffer_set_finish(&rollback);

    fb->template.width0 = width;
    fb->template.height0 = height;
    surface->base.Width = width;
    surface->base.Height = height;
    surface->cur_slot = -1;
    return EGL_TRUE;
}

static EGLBoolean
switch_swap_buffers(_EGLDisplay *dpy, _EGLSurface *surf)
{
   (void)dpy;
   CALLED();
   struct switch_egl_surface *surface = switch_egl_surface(surf);
   struct switch_egl_context *context =
      switch_egl_context(surface->base.CurrentContext);

   if (surface->base.Lost)
      return _eglError(EGL_BAD_SURFACE, "switch_swap_buffers: surface is lost");

   if (!context || !context->st)
      return _eglError(EGL_BAD_CONTEXT,
                       "switch_swap_buffers: surface has no current context");

   /* Drain queued draws before inspecting or presenting the back buffer. */
   _mesa_glthread_finish(context->st->ctx);

   if (!surface->nw) {
      st_context_flush(context->st, ST_FLUSH_END_OF_FRAME, NULL, NULL, NULL);
      if (switch_record_context_error(context->st, surface,
                                      "pbuffer swap flush"))
         return _eglError(EGL_BAD_SURFACE,
                          "switch_swap_buffers: pbuffer flush failed");
      return EGL_TRUE;
   }

#ifdef HAVE_SWITCH_ZINK
   if (surface->backend == SWITCH_GL_BACKEND_ZINK) {
      struct pipe_resource *back =
         surface->attachments[ST_ATTACHMENT_BACK_LEFT];
      if (!back) {
         st_manager_validate_framebuffers(context->st);
         back = surface->attachments[ST_ATTACHMENT_BACK_LEFT];
      }
      if (!back)
         return _eglError(EGL_BAD_SURFACE,
                          "switch_swap_buffers: no zink back buffer");

      struct switch_zink_before_flush_args args = {
         .pipe = context->st->pipe,
         .resource = back,
      };
      st_context_flush(context->st, ST_FLUSH_END_OF_FRAME, NULL,
                       switch_zink_flush_resource_before_submit, &args);
      context->st->screen->flush_frontbuffer(context->st->screen,
                                             context->st->pipe, back, 0, 0,
                                             surface->drawable, 0, NULL);
      if (switch_record_context_error(context->st, surface, "zink present") ||
          !zink_kopper_check(back)) {
         surface->base.Lost = EGL_TRUE;
         return _eglError(EGL_BAD_SURFACE,
                          "switch_swap_buffers: zink present failed");
      }

      if (surface->attachments[ST_ATTACHMENT_FRONT_LEFT]) {
         surface->attachments[ST_ATTACHMENT_BACK_LEFT] =
            surface->attachments[ST_ATTACHMENT_FRONT_LEFT];
         surface->attachments[ST_ATTACHMENT_FRONT_LEFT] = back;
      }

      p_atomic_inc(&surface->drawable->stamp);
      st_context_invalidate_state(context->st, ST_INVALIDATE_FB_STATE);
      return EGL_TRUE;
   }
#endif

   if (surface->cur_slot < 0) {
      TRACE("Nothing to do\n");
      return EGL_TRUE;
   }

   if (!surface->nw || surface->cur_slot >= NUM_BUFFERS ||
       !surface->buffers[surface->cur_slot]) {
      switch_present_failure(surface, "invalid queued buffer slot",
                             surface->cur_slot);
      NvMultiFence release_fence = {0};
      const enum switch_release_status release_status =
         switch_prepare_release_fence(context->st, surface, &release_fence);
      if (release_status != SWITCH_RELEASE_GPU_ERROR)
         switch_surface_cancel_dequeued(
            surface, release_fence.num_fences ? &release_fence : NULL,
            "invalid queue state");
      surface->base.Lost = EGL_TRUE;
      return _eglError(EGL_BAD_SURFACE,
                       "switch_swap_buffers: invalid buffer slot");
   }

   /* Keep the exact rendered attachment while the native release fence is
    * upgraded and queued.  Ownership moves from BACK_LEFT to FRONT_LEFT
    * only after nwindowQueueBuffer succeeds below. */
   struct pipe_resource *old_back =
      surface->attachments[ST_ATTACHMENT_BACK_LEFT];

   TRACE("Flushing context\n");
   st_context_flush(context->st, ST_FLUSH_END_OF_FRAME, NULL, NULL, NULL);
   if (switch_record_context_error(context->st, surface, "swap flush"))
      return _eglError(EGL_BAD_SURFACE,
                       "switch_swap_buffers: GPU submission failed");

   NvMultiFence mf = {0};
   NvFence fence = {.id = UINT32_MAX};
   const int fence_ret =
      nouveau_switch_context_get_cpu_fence(context->st->pipe, &fence);
   if (!fence_ret) {
      NvFence *surf_fence = &surface->fences[surface->cur_slot];
      if (surf_fence->id != fence.id || surf_fence->value != fence.value) {
         TRACE("Using fence: {%d,%u}\n", (int)fence.id, fence.value);
         *surf_fence = fence;
         nvMultiFenceCreate(&mf, &fence);
      }
   } else if (fence_ret == -ENODATA) {
      TRACE("No native work for this frame; using an empty release fence\n");
   } else {
      struct pipe_fence_handle *wait_fence = NULL;
      st_context_flush(context->st, ST_FLUSH_END_OF_FRAME | ST_FLUSH_WAIT,
                       &wait_fence, NULL, NULL);
      if (wait_fence)
         context->st->screen->fence_reference(context->st->screen, &wait_fence,
                                              NULL);
      if (switch_record_context_error(context->st, surface, "swap CPU wait"))
         return _eglError(EGL_BAD_SURFACE,
                          "switch_swap_buffers: GPU wait failed");
      mesa_logw_once("egl-switch present: render resource had no native "
                     "syncpoint; used a CPU render wait");
   }

   TRACE("Queuing buffer\n");
   const s32 queued_slot = surface->cur_slot;
   Result rc = nwindowQueueBuffer(surface->nw, queued_slot, &mf);
   if (R_FAILED(rc)) {
      switch_present_failure(surface, "nwindowQueueBuffer", (int)rc);
      switch_surface_cancel_dequeued(surface, mf.num_fences ? &mf : NULL,
                                     "queue failed");
      surface->base.Lost = EGL_TRUE;
      return _eglError(EGL_BAD_SURFACE,
                       "switch_swap_buffers: nwindowQueueBuffer failed");
   }

   // Update framebuffer state
   surface->cur_slot = -1;
   surface->attachments[ST_ATTACHMENT_BACK_LEFT] = NULL;
   surface->attachments[ST_ATTACHMENT_FRONT_LEFT] = old_back;
   p_atomic_inc(&surface->drawable->stamp);

   /* Defer attachment validation to the next draw; eager validation would
    * dequeue an NWindow buffer here.
    */
   st_context_invalidate_state(context->st, ST_INVALIDATE_FB_STATE);

   return EGL_TRUE;
}

/*
 * Called from eglGetProcAddress() via drv->API.GetProcAddress().
 */
static _EGLProc
switch_get_proc_address(const char *procname)
{
    return _mesa_glapi_get_proc_address(procname);
}

/* Required eglWaitClient/eglWaitGL hook: finish rendering for the current
 * context.
 */
static EGLBoolean
switch_wait_client(_EGLDisplay *disp, _EGLContext *ctx)
{
    struct switch_egl_context *context = switch_egl_context(ctx);

    (void)disp;
    if (!context || !context->st)
        return EGL_TRUE;

    _mesa_glthread_finish(context->st->ctx);

    struct pipe_fence_handle *fence = NULL;
    st_context_flush(context->st, ST_FLUSH_END_OF_FRAME, &fence, NULL, NULL);
    if (fence) {
        context->st->screen->fence_finish(context->st->screen,
                                          context->st->pipe, fence,
                                          UINT64_MAX);
        context->st->screen->fence_reference(context->st->screen,
                                             &fence, NULL);
    }
    return EGL_TRUE;
}

static EGLBoolean
switch_wait_native(EGLint engine)
{
    if (engine != EGL_CORE_NATIVE_ENGINE)
        return _eglError(EGL_BAD_PARAMETER, "eglWaitNative");

    /* No native rendering engine draws into EGL surfaces on Horizon. */
    return EGL_TRUE;
}

static EGLBoolean
switch_copy_buffers(_EGLDisplay *disp, _EGLSurface *surface,
                    void *native_pixmap_target)
{
    (void)disp;
    (void)surface;
    (void)native_pixmap_target;

    /* Horizon has no native pixmap type. */
    return _eglError(EGL_BAD_NATIVE_PIXMAP, "eglCopyBuffers");
}


/**
 * This is the main entrypoint into the driver, referenced by libEGL.
 */
const _EGLDriver _eglDriver = {
    .Initialize = switch_initialize,
    .Terminate = switch_terminate,
    .CreateContext = switch_create_context,
    .DestroyContext = switch_destroy_context,
    .MakeCurrent = switch_make_current,
    .CreateWindowSurface = switch_create_window_surface,
    .CreatePixmapSurface = switch_create_pixmap_surface,
    .CreatePbufferSurface = switch_create_pbuffer_surface,
    .DestroySurface = switch_destroy_surface,
    .ResizeSurfaceMESA = switch_resize_surface,
    .SwapInterval = switch_swap_interval,
    .SwapBuffers = switch_swap_buffers,
    .WaitClient = switch_wait_client,
    .WaitNative = switch_wait_native,
    .CopyBuffers = switch_copy_buffers,

};
