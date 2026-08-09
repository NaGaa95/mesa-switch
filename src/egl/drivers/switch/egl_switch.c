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

#include "pipe/p_context.h"
#include "pipe/p_screen.h"
#include "pipe/p_state.h"

#include "util/u_atomic.h"
#include "util/u_debug.h"
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

#ifdef DEBUG
#	define TRACE(x...) _eglLog(_EGL_DEBUG, "egl_switch: " x)
#	define CALLED() TRACE("CALLED: %s\n", __PRETTY_FUNCTION__)
#else
#	define TRACE(x...)
#  define CALLED()
#endif
_EGL_DRIVER_STANDARD_TYPECASTS(switch_egl)

struct switch_egl_display
{
    struct pipe_frontend_screen *fscreen;
    struct st_config_options st_options;
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
};

struct switch_egl_surface
{
    _EGLSurface base;
    struct pipe_frontend_drawable *drawable;
    struct pipe_resource *attachments[ST_ATTACHMENT_COUNT];

    NWindow* nw;
    s32 cur_slot;
    struct pipe_resource *buffers[NUM_BUFFERS];
    NvFence fences[NUM_BUFFERS];
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

// Called via pipe_frontend_screen_flush_frontbuffer. Users of this function include:
// - st_context_flush with ST_FLUSH_FRONT
// - glFlush
// - glFinish
// We don't support rendering to the front buffer, so our implementation is dummy.
static bool
switch_st_framebuffer_flush_front(struct st_context *st, struct pipe_frontend_drawable *drawable, enum st_attachment_type statt)
{
    (void)st;
    (void)drawable;
    (void)statt;
    return true;
}

// Called via st_framebuffer_validate.
static bool
switch_st_framebuffer_validate(struct st_context *st, struct pipe_frontend_drawable *drawable,
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
                  "switch_st_framebuffer_validate: invalid arguments");
        return false;
    }

    if (resolve)
        *resolve = NULL;

    for (i = 0; i < count; i++)
    {
        if (statts[i] < 0 || statts[i] >= ST_ATTACHMENT_COUNT) {
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
                    Result rc = nwindowDequeueBuffer(surface->nw, &surface->cur_slot, NULL);
                    if (R_FAILED(rc)) {
                        surface->base.Lost = EGL_TRUE;
                        _eglError(EGL_BAD_SURFACE,
                                  "switch_st_framebuffer_validate: nwindowDequeueBuffer failed");
                        return false;
                    }
                    if (surface->cur_slot < 0 ||
                        surface->cur_slot >= NUM_BUFFERS) {
                        surface->base.Lost = EGL_TRUE;
                        _eglError(EGL_BAD_SURFACE,
                                  "switch_st_framebuffer_validate: invalid buffer slot");
                        return false;
                    }

                    // Use the dequeued buffer as the back buffer
                    res = surface->buffers[surface->cur_slot];
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

    // For window surfaces, FRONT_LEFT/BACK_LEFT are managed by buffers[]
    // For PBuffer surfaces, they're owned by attachments[] and must be released
    bool is_pbuffer = (surface->nw == NULL);

    for (i = 0; i < ST_ATTACHMENT_COUNT; i ++)
    {
        // Skip FRONT/BACK for window surfaces (handled by buffers[])
        if (!is_pbuffer && (i == ST_ATTACHMENT_FRONT_LEFT || i == ST_ATTACHMENT_BACK_LEFT))
            continue;
        pipe_resource_reference(&surface->attachments[i], NULL);
    }

    if (surface->nw)
    {
        if (surface->cur_slot >= 0)
            nwindowCancelBuffer(surface->nw, surface->cur_slot, NULL);
        nwindowReleaseBuffers(surface->nw);
    }

    for (i = 0; i < NUM_BUFFERS; i ++)
        pipe_resource_reference(&surface->buffers[i], NULL);

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
    fb->template.target = PIPE_TEXTURE_RECT;
    fb->template.format = config->stvis.color_format;
    fb->template.width0 = (u16)width;
    fb->template.height0 = (u16)height;
    fb->template.depth0 = 1;
    fb->template.array_size = 1;
    fb->template.usage = PIPE_USAGE_DEFAULT;
    fb->template.bind = PIPE_BIND_RENDER_TARGET;
    for (i = 0; i < NUM_BUFFERS; i ++)
    {
        // Allocate a framebuffer
        surface->fences[i].id = UINT32_MAX;
        surface->buffers[i] = display->fscreen->screen->resource_create(display->fscreen->screen, &fb->template);
        if (!surface->buffers[i])
        {
            _eglError(EGL_BAD_ALLOC, "switch_create_window_surface: failed to allocate framebuffers");
            goto cleanup;
        }

        // Retrieve the native graphic buffer struct associated with this framebuffer
        NvGraphicBuffer grbuf;
        int err = nouveau_switch_resource_get_buffer(surface->buffers[i], &grbuf);
        if (err != 0)
        {
            _eglError(EGL_BAD_ALLOC, "switch_create_window_surface: nouveau_switch_resource_get_buffer failed");
            goto cleanup;
        }

        // Attach the framebuffer to the native window
        rc = nwindowConfigureBuffer(surface->nw, i, &grbuf);
        if (R_FAILED(rc)) {
            _eglError(EGL_BAD_NATIVE_WINDOW,
                      "switch_create_window_surface: nwindowConfigureBuffer failed");
            goto cleanup;
        }
    }

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
    CALLED();
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
    (void)resolve;
    CALLED();

    for (i = 0; i < count; i++)
    {
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
                    continue;
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
    fb->template.target = PIPE_TEXTURE_RECT;
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
    conf->base.NativeRenderable = EGL_TRUE;
    conf->base.SurfaceType = EGL_WINDOW_BIT | EGL_PBUFFER_BIT; // Support window and pbuffer surfaces
    conf->base.RenderableType = EGL_OPENGL_BIT | EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR;
    conf->base.Conformant = conf->base.RenderableType;
    conf->base.MinSwapInterval = 0;
    conf->base.MaxSwapInterval = INT32_MAX;

    // Color buffer configuration
    conf->base.RedSize    = util_format_get_component_bits(colorfmt, UTIL_FORMAT_COLORSPACE_RGB, 0);
    conf->base.GreenSize  = util_format_get_component_bits(colorfmt, UTIL_FORMAT_COLORSPACE_RGB, 1);
    conf->base.BlueSize   = util_format_get_component_bits(colorfmt, UTIL_FORMAT_COLORSPACE_RGB, 2);
    conf->base.AlphaSize  = util_format_get_component_bits(colorfmt, UTIL_FORMAT_COLORSPACE_RGB, 3);
    conf->base.BufferSize = conf->base.RedSize+conf->base.GreenSize+conf->base.BlueSize+conf->base.AlphaSize;

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
    conf->stvis.accum_format = PIPE_FORMAT_R16G16B16A16_FLOAT;

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

    // List of supported color buffer formats
    static const enum pipe_format colorfmts[] = {
        PIPE_FORMAT_R8G8B8A8_UNORM,
        //PIPE_FORMAT_R8G8B8X8_UNORM,
        //PIPE_FORMAT_B5G6R5_UNORM,
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
    for (i = 0; i < sizeof(colorfmts)/sizeof(colorfmts[0]); i ++) {
        for (j = 0; j < sizeof(depthfmts)/sizeof(depthfmts[0]); j ++) {
            EGLBoolean rc = switch_add_config(dpy, &config_id, colorfmts[i], depthfmts[j]);
            if (!rc)
                return rc;
        }
    }

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

    dpy->ClientAPIs = 0;
    if (_eglIsApiValid(EGL_OPENGL_API))
        dpy->ClientAPIs |= EGL_OPENGL_BIT;
    if (_eglIsApiValid(EGL_OPENGL_ES_API))
        dpy->ClientAPIs |= EGL_OPENGL_ES_BIT | EGL_OPENGL_ES2_BIT | EGL_OPENGL_ES3_BIT_KHR;

    dpy->Extensions.EXT_create_context_robustness = EGL_TRUE;
    dpy->Extensions.KHR_create_context = EGL_TRUE;
    dpy->Extensions.KHR_create_context_no_error = EGL_TRUE;
    dpy->Extensions.KHR_surfaceless_context = EGL_TRUE;
    dpy->Extensions.KHR_context_flush_control = EGL_TRUE;

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

    // Create nouveau screen
    TRACE("Creating nouveau screen\n");
    screen = nouveau_switch_screen_create();
    if (!screen)
    {
        TRACE("Failed to create nouveau screen\n");
        switch_display_destroy(dpy);
        return EGL_FALSE;
    }

    // Inject optional trace/debug/etc wrappers
    TRACE("Wrapping screen\n");
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
                    /* There are no profiles before OpenGL 3.2.  The
                     * EGL_KHR_create_context spec says:
                     *
                     *     "If the requested OpenGL version is less than 3.2,
                     *      EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR is ignored and the functionality
                     *      of the context is determined solely by the requested version.."
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
    if (context->base.Flags & EGL_CONTEXT_OPENGL_NO_ERROR_KHR || context->base.NoError)
        attribs.flags |= ST_CONTEXT_FLAG_NO_ERROR;

    if (context->base.ResetNotificationStrategy != EGL_NO_RESET_NOTIFICATION_KHR)
        attribs.context_flags |= PIPE_CONTEXT_LOSE_CONTEXT_ON_RESET;

    context->st = st_api_create_context(display->fscreen, &attribs, &error,
                                        share_ctx ? share_ctx->st : NULL);
    if (!context->st || error != ST_CONTEXT_SUCCESS) {
        _eglError(EGL_BAD_MATCH, "switch_create_context");
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
switch_destroy_context(_EGLDisplay *disp, _EGLContext* ctx)
{
    struct switch_egl_context* context = switch_egl_context(ctx);
    (void)disp;
    CALLED();

    if (_eglPutContext(ctx))
    {
        _mesa_glthread_finish(context->st->ctx);
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

    EGLBoolean ret = st_api_make_current(cont ? cont->st : NULL,
        draw_surf ? draw_surf->drawable : NULL,
        read_surf ? read_surf->drawable : NULL);

    if (!ret) {
        _EGLContext *tmp_ctx;
        _EGLSurface *tmp_dsurf, *tmp_rsurf;

        /* Restore the previous EGL and state-tracker binding. */
        _eglBindContext(old_ctx, old_dsurf, old_rsurf, &ctx, &tmp_dsurf,
                        &tmp_rsurf);
        assert((cont ? &cont->base : NULL) == ctx &&
               tmp_dsurf == dsurf && tmp_rsurf == rsurf);

        _eglPutSurface(dsurf);
        _eglPutSurface(rsurf);
        _eglPutContext(ctx);
        _eglPutSurface(old_dsurf);
        _eglPutSurface(old_rsurf);
        _eglPutContext(old_ctx);

        struct switch_egl_surface *old_draw = switch_egl_surface(old_dsurf);
        struct switch_egl_surface *old_read = switch_egl_surface(old_rsurf);
        if (st_api_make_current(old_cont ? old_cont->st : NULL,
                old_draw ? old_draw->drawable : NULL,
                old_read ? old_read->drawable : NULL))
            return _eglError(EGL_BAD_MATCH, "switch_make_current");

        /* Keep EGL unbound if the old state cannot be restored. */
        _eglBindContext(NULL, NULL, NULL, &tmp_ctx, &tmp_dsurf, &tmp_rsurf);
        assert(tmp_ctx == old_ctx && tmp_dsurf == old_dsurf &&
               tmp_rsurf == old_rsurf);
        st_api_make_current(NULL, NULL, NULL);
    }

    if (ret && ctx)
        p_atomic_inc(&switch_egl_display(dpy)->ref_count);

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
    struct switch_egl_surface* surface = switch_egl_surface(surf);

    if (!surface->nw)
        return _eglError(EGL_BAD_SURFACE, "switch_swap_interval");

    Result rc = nwindowSetSwapInterval(surface->nw, interval);
    if (R_FAILED(rc)) {
        surface->base.Lost = EGL_TRUE;
        return _eglError(EGL_BAD_SURFACE,
                         "switch_swap_interval: nwindowSetSwapInterval failed");
    }
    return EGL_TRUE;
}


static EGLBoolean
switch_swap_buffers(_EGLDisplay *dpy, _EGLSurface *surf)
{
    (void)dpy;
    CALLED();
    struct switch_egl_surface* surface = switch_egl_surface(surf);
    struct switch_egl_context* context = switch_egl_context(surface->base.CurrentContext);

    if (!context || !context->st)
        return _eglError(EGL_BAD_CONTEXT,
                         "switch_swap_buffers: surface has no current context");

    /* Drain queued draws before inspecting or presenting the back buffer. */
    _mesa_glthread_finish(context->st->ctx);

    if (surface->cur_slot < 0) {
        TRACE("Nothing to do\n");
        return EGL_TRUE;
    }

    TRACE("Flushing context\n");
    st_context_flush(context->st, ST_FLUSH_END_OF_FRAME, NULL, NULL, NULL);

    NvMultiFence mf = {0};
    NvFence fence;
    struct pipe_resource *old_back = surface->attachments[ST_ATTACHMENT_BACK_LEFT];
    fence.id = nouveau_switch_resource_get_syncpoint(old_back, &fence.value);
    if ((int)fence.id >= 0) {
        NvFence* surf_fence = &surface->fences[surface->cur_slot];
        if (surf_fence->id != fence.id || surf_fence->value != fence.value) {
            TRACE("Using fence: {%d,%u}\n", (int)fence.id, fence.value);
            *surf_fence = fence;
            nvMultiFenceCreate(&mf, &fence);
        }
    } else {
        struct pipe_fence_handle *wait_fence = NULL;
        st_context_flush(context->st,
                         ST_FLUSH_END_OF_FRAME | ST_FLUSH_WAIT,
                         &wait_fence, NULL, NULL);
    }

    TRACE("Queuing buffer\n");
    Result rc = nwindowQueueBuffer(surface->nw, surface->cur_slot, &mf);
    if (R_FAILED(rc)) {
        surface->base.Lost = EGL_TRUE;
        return _eglError(EGL_BAD_SURFACE,
                         "switch_swap_buffers: nwindowQueueBuffer failed");
    }

    // Update framebuffer state
    surface->cur_slot = -1;
    surface->attachments[ST_ATTACHMENT_BACK_LEFT] = NULL;
    surface->attachments[ST_ATTACHMENT_FRONT_LEFT] = old_back;
    p_atomic_inc(&surface->drawable->stamp);

    /* Invalidate framebuffer state so the state tracker re-validates
     * attachments on the next draw call (lightweight flag set, matching
     * the DRI frontend pattern — NOT st_manager_validate_framebuffers
     * which would eagerly call nwindowDequeueBuffer). */
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
    .SwapInterval = switch_swap_interval,
    .SwapBuffers = switch_swap_buffers,

};
