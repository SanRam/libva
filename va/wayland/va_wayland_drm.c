/*
 * va_wayland_drm.c - Wayland/DRM helpers
 *
 * Copyright (c) 2012 Intel Corporation. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL INTEL AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <xf86drm.h>
#include "va_drmcommon.h"
#include "va_wayland_drm.h"
#include "va_wayland_private.h"
#include "wayland-drm-client-protocol.h"

/* XXX: wayland-drm currently lives in libEGL.so.* library */
#define LIBEGL_NAME "libEGL.so.1"

static void
drm_handle_device(void *data, struct wl_drm *drm, const char *device)
{
    VADisplayContextP const pDisplayContext = data;
    VADriverContextP const ctx = pDisplayContext->pDriverContext;
    VADisplayContextWaylandP const wl_ctx = pDisplayContext->opaque;
    VADisplayContextWaylandDRM * const wl_drm_ctx = &wl_ctx->backend.drm;
    struct drm_state * const drm_state = ctx->drm_state;
    drm_magic_t magic;
    struct stat st;

    if (stat(device, &st) < 0) {
        va_wayland_error("failed to identify %s: %s (errno %d)",
                         device, strerror(errno), errno);
        return;
    }

    if (!S_ISCHR(st.st_mode)) {
        va_wayland_error("%s is not a device", device);
        return;
    }

    drm_state->fd = open(device, O_RDWR);
    if (drm_state->fd < 0) {
        va_wayland_error("failed to open %s: %s (errno %d)",
                         device, strerror(errno), errno);
        return;
    }

    drmGetMagic(drm_state->fd, &magic);
    wl_drm_authenticate(wl_drm_ctx->drm, magic);
}

static void
drm_handle_format(void *data, struct wl_drm *drm, uint32_t format)
{
}

static void
drm_handle_authenticated(void *data, struct wl_drm *drm)
{
    VADisplayContextP const pDisplayContext = data;
    VADriverContextP const ctx = pDisplayContext->pDriverContext;
    VADisplayContextWaylandP const wl_ctx = pDisplayContext->opaque;
    struct drm_state * const drm_state = ctx->drm_state;

    wl_ctx->backend.drm.is_authenticated = 1;
    drm_state->auth_type                 = VA_DRM_AUTH_CUSTOM;
}

static const struct wl_drm_listener drm_listener = {
    drm_handle_device,
    drm_handle_format,
    drm_handle_authenticated
};

struct driver_name_map {
    const char *key;
    int key_len;
    const char *name;
};

static const struct driver_name_map g_driver_name_map[] = {
    { "i915",       4, "i965"  }, // Intel OTC GenX driver
    { "pvrsrvkm",   8, "pvr"   }, // Intel UMG PVR driver
    { "emgd",       4, "emgd"  }, // Intel ECG PVR driver
    { NULL, }
};

static VAStatus
va_DisplayContextGetDriverName(
    VADisplayContextP pDisplayContext,
    char            **driver_name_ptr
)
{
    VADriverContextP const ctx = pDisplayContext->pDriverContext;
    struct drm_state * const drm_state = ctx->drm_state;
    drmVersionPtr drm_version;
    char *driver_name = NULL;
    const struct driver_name_map *m;

    *driver_name_ptr = NULL;

    drm_version = drmGetVersion(drm_state->fd);
    if (!drm_version)
        return VA_STATUS_ERROR_UNKNOWN;

    for (m = g_driver_name_map; m->key != NULL; m++) {
        if (drm_version->name_len >= m->key_len &&
            strncmp(drm_version->name, m->key, m->key_len) == 0)
            break;
    }
    drmFreeVersion(drm_version);

    if (!m->name)
        return VA_STATUS_ERROR_UNKNOWN;

    driver_name = strdup(m->name);
    if (!driver_name)
        return VA_STATUS_ERROR_ALLOCATION_FAILED;

    *driver_name_ptr = driver_name;
    return VA_STATUS_SUCCESS;
}

static void
va_wayland_drm_finalize(VADisplayContextP pDisplayContext)
{
    VADriverContextP const ctx = pDisplayContext->pDriverContext;
    VADisplayContextWaylandP const wl_ctx = pDisplayContext->opaque;
    VADisplayContextWaylandDRM * const wl_drm_ctx = &wl_ctx->backend.drm;
    struct drm_state * const drm_state = ctx->drm_state;

    if (wl_drm_ctx->drm) {
        wl_drm_destroy(wl_drm_ctx->drm);
        wl_drm_ctx->drm = NULL;
    }
    wl_drm_ctx->is_authenticated = 0;

    if (wl_drm_ctx->libEGL_handle) {
        dlclose(wl_drm_ctx->libEGL_handle);
        wl_drm_ctx->libEGL_handle = NULL;
    }

    if (drm_state) {
        if (drm_state->fd >= 0) {
            close(drm_state->fd);
            drm_state->fd = -1;
        }
        free(ctx->drm_state);
        ctx->drm_state = NULL;
    }
}

bool
va_wayland_drm_init(VADisplayContextP pDisplayContext)
{
    VADriverContextP const ctx = pDisplayContext->pDriverContext;
    VADisplayContextWaylandP const wl_ctx = pDisplayContext->opaque;
    VADisplayContextWaylandDRM * const wl_drm_ctx = &wl_ctx->backend.drm;
    struct drm_state *drm_state;
    uint32_t id;

    wl_drm_ctx->drm                     = NULL;
    wl_drm_ctx->is_authenticated        = 0;
    wl_ctx->finalize                    = va_wayland_drm_finalize;
    pDisplayContext->vaGetDriverName    = va_DisplayContextGetDriverName;

    drm_state = calloc(1, sizeof(struct drm_state));
    if (!drm_state)
        return false;
    drm_state->fd        = -1;
    drm_state->auth_type = 0;
    ctx->drm_state       = drm_state;

    id = wl_display_get_global(ctx->native_dpy, "wl_drm", 1);
    if (!id) {
        wl_display_roundtrip(ctx->native_dpy);
        id = wl_display_get_global(ctx->native_dpy, "wl_drm", 1);
        if (!id)
            return false;
    }

    wl_drm_ctx->libEGL_handle = dlopen(LIBEGL_NAME, RTLD_LAZY|RTLD_LOCAL);
    if (!wl_drm_ctx->libEGL_handle)
        return false;

    wl_drm_ctx->drm_interface =
        dlsym(wl_drm_ctx->libEGL_handle, "wl_drm_interface");
    if (!wl_drm_ctx->drm_interface)
        return false;

    wl_drm_ctx->drm =
        wl_display_bind(ctx->native_dpy, id, wl_drm_ctx->drm_interface);
    if (!wl_drm_ctx->drm)
        return false;

    wl_drm_add_listener(wl_drm_ctx->drm, &drm_listener, pDisplayContext);
    wl_display_roundtrip(ctx->native_dpy);
    if (drm_state->fd < 0)
        return false;

    wl_display_roundtrip(ctx->native_dpy);
    if (!wl_drm_ctx->is_authenticated)
        return false;
    return true;
}
