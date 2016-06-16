/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2016 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_KMSDRM

/* SDL internals */
#include "../SDL_sysvideo.h"
#include "SDL_version.h"
#include "SDL_syswm.h"
#include "SDL_loadso.h"
#include "SDL_events.h"
#include "SDL_log.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_keyboard_c.h"

#ifdef SDL_INPUT_LINUXEV
#include "../../core/linux/SDL_evdev.h"
#endif

/* KMS/DRM declarations */
#include "SDL_kmsdrmvideo.h"
#include "SDL_kmsdrmevents_c.h"
#include "SDL_kmsdrmopengles.h"
/* #include "SDL_kmsdrmmouse.h" */

static int
KMSDRM_Available(void)
{
    return 1;
}

static void
KMSDRM_Destroy(SDL_VideoDevice * device)
{
    if (device->driverdata != NULL) {
        SDL_free(device->driverdata);
        device->driverdata = NULL;
    }
}

static SDL_VideoDevice *
KMSDRM_Create(int devindex)
{
    SDL_VideoDevice *device;
    SDL_VideoData *phdata;

    if (devindex < 0 || devindex > 99) {
        SDL_SetError("devindex (%d) must be between 0 and 99.\n", devindex);
        return NULL;
    }

    /* Initialize SDL_VideoDevice structure */
    device = (SDL_VideoDevice *) SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (device == NULL) {
        SDL_OutOfMemory();
        goto cleanup;
    }

    /* Initialize internal data */
    phdata = (SDL_VideoData *) SDL_calloc(1, sizeof(SDL_VideoData));
    if (phdata == NULL) {
        SDL_OutOfMemory();
        goto cleanup;
    }
    phdata->devindex = devindex;

    device->driverdata = phdata;

    /* Setup amount of available displays and current display */
    device->num_displays = 0;

    /* Set device free function */
    device->free = KMSDRM_Destroy;

    /* Setup all functions which we can handle */
    device->VideoInit = KMSDRM_VideoInit;
    device->VideoQuit = KMSDRM_VideoQuit;
    device->GetDisplayModes = KMSDRM_GetDisplayModes;
    device->SetDisplayMode = KMSDRM_SetDisplayMode;
    device->CreateWindow = KMSDRM_CreateWindow;
    device->CreateWindowFrom = KMSDRM_CreateWindowFrom;
    device->SetWindowTitle = KMSDRM_SetWindowTitle;
    device->SetWindowIcon = KMSDRM_SetWindowIcon;
    device->SetWindowPosition = KMSDRM_SetWindowPosition;
    device->SetWindowSize = KMSDRM_SetWindowSize;
    device->ShowWindow = KMSDRM_ShowWindow;
    device->HideWindow = KMSDRM_HideWindow;
    device->RaiseWindow = KMSDRM_RaiseWindow;
    device->MaximizeWindow = KMSDRM_MaximizeWindow;
    device->MinimizeWindow = KMSDRM_MinimizeWindow;
    device->RestoreWindow = KMSDRM_RestoreWindow;
    device->SetWindowGrab = KMSDRM_SetWindowGrab;
    device->DestroyWindow = KMSDRM_DestroyWindow;
    device->GetWindowWMInfo = KMSDRM_GetWindowWMInfo;
    device->GL_LoadLibrary = KMSDRM_GLES_LoadLibrary;
    device->GL_GetProcAddress = KMSDRM_GLES_GetProcAddress;
    device->GL_UnloadLibrary = KMSDRM_GLES_UnloadLibrary;
    device->GL_CreateContext = KMSDRM_GLES_CreateContext;
    device->GL_MakeCurrent = KMSDRM_GLES_MakeCurrent;
    device->GL_SetSwapInterval = KMSDRM_GLES_SetSwapInterval;
    device->GL_GetSwapInterval = KMSDRM_GLES_GetSwapInterval;
    device->GL_SwapWindow = KMSDRM_GLES_SwapWindow;
    device->GL_DeleteContext = KMSDRM_GLES_DeleteContext;

    device->PumpEvents = KMSDRM_PumpEvents;

    return device;

cleanup:
    if (device != NULL)
        SDL_free(device);
    if (phdata != NULL)
        SDL_free(phdata);
    return NULL;
}

VideoBootStrap KMSDRM_bootstrap = {
    "KMSDRM",
    "KMS/DRM Video Driver",
    KMSDRM_Available,
    KMSDRM_Create
};


static void
KMSDRM_FBDestroyCallback(struct gbm_bo *bo, void *data)
{
    KMSDRM_FBInfo *fb_info = (KMSDRM_FBInfo *)data;

    if (fb_info && fb_info->drm_fd > 0 && fb_info->fb_id != 0) {
        drmModeRmFB(fb_info->drm_fd, fb_info->fb_id);
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Delete DRM FB %u", fb_info->fb_id);
    }

    free(fb_info);
}

KMSDRM_FBInfo *
KMSDRM_FBFromBO(_THIS, struct gbm_bo *bo)
{
    uint32_t w, h, stride, handle;
    int ret;
    SDL_VideoData *vdata = ((SDL_VideoData *)_this->driverdata);
    KMSDRM_FBInfo *fb_info;

    fb_info = (KMSDRM_FBInfo *)gbm_bo_get_user_data(bo);
    if (fb_info != NULL) {
        /* Have a previously used framebuffer, return it */
        return fb_info;
    }

    /* Here a new DRM FB must be created */
    fb_info = (KMSDRM_FBInfo *)SDL_calloc(1, sizeof(KMSDRM_FBInfo));
    fb_info->drm_fd = vdata->drm_fd;

    w  = gbm_bo_get_width(bo);
    h = gbm_bo_get_height(bo);
    stride = gbm_bo_get_stride(bo);
    handle = gbm_bo_get_handle(bo).u32;

    ret = drmModeAddFB(vdata->drm_fd, w, h, 24, 32, stride, handle, &fb_info->fb_id);
    if (ret < 0) {
       free(fb_info);
       return NULL;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "New DRM FB (%u): %ux%u, stride %u from BO %p", fb_info->fb_id, w, h, stride, (void *)bo);

    /* Associate our DRM framebuffer with this buffer object */
    gbm_bo_set_user_data(bo, fb_info, KMSDRM_FBDestroyCallback);
    return fb_info;
}

SDL_bool
KMSDRM_WaitPageFlip(_THIS, SDL_WindowData *wdata, int timeout) {
    SDL_VideoData *vdata = ((SDL_VideoData *)_this->driverdata);

    while (wdata->waiting_for_flip) {
        vdata->drm_pollfd.revents = 0;
        if (poll(&vdata->drm_pollfd, 1, timeout) < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "DRM poll error");
            return SDL_FALSE;
        }

        if (vdata->drm_pollfd.revents & (POLLHUP | POLLERR)) {
            SDL_LogError(SDL_LOG_CATEGORY_VIDEO, "DRM poll hup or error");
            return SDL_FALSE;
        }

        if (vdata->drm_pollfd.revents & POLLIN) {
            /* Page flip? If so, drmHandleEvent will unset wdata->waiting_for_flip */
            drmHandleEvent(vdata->drm_fd, &vdata->drm_evctx);
        } else {
            /* Timed out and page flip didn't happen */
            SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Dropping frame while waiting_for_flip");
            return SDL_FALSE;
        }
    }
    return SDL_TRUE;
}

static void
KMSDRM_FlipHandler(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data)
{
    *((SDL_bool *) data) = SDL_FALSE;
}


/*****************************************************************************/
/* SDL Video and Display initialization/handling functions                   */
/* _this is a SDL_VideoDevice *                                              */
/*****************************************************************************/
int
KMSDRM_VideoInit(_THIS)
{
    int i;
    int ret = 0;
    char *devname;
    SDL_VideoData *vdata = ((SDL_VideoData *)_this->driverdata);
    drmModeRes *resources = NULL;
    drmModeConnector *connector = NULL;
    drmModeEncoder *encoder = NULL;
    SDL_DisplayMode current_mode;
    SDL_VideoDisplay display;

    /* Allocate display internal data */
    SDL_DisplayData *data = (SDL_DisplayData *) SDL_calloc(1, sizeof(SDL_DisplayData));
    if (data == NULL) {
        return SDL_OutOfMemory();
    }

    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "KMSDRM_VideoInit()");

    /* Open /dev/dri/cardNN */
    devname = (char *) SDL_calloc(1, 16);
    snprintf(devname, 16, "/dev/dri/card%d", vdata->devindex);
    vdata->drm_fd = open(devname, O_RDWR | O_CLOEXEC);
    SDL_free(devname);

    if (vdata->drm_fd < 1) {
        ret = SDL_SetError("Could not open /dev/dri/card%d.", vdata->devindex);
        goto cleanup;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Opened DRM FD (%d)", vdata->drm_fd);

    vdata->gbm = gbm_create_device(vdata->drm_fd);
    if (vdata->gbm == NULL) {
        ret = SDL_SetError("Couldn't create gbm device.");
        goto cleanup;
    }

    /* Find the first available connector with modes */
    resources = drmModeGetResources(vdata->drm_fd);
    if (!resources) {
        ret = SDL_SetError("drmModeGetResources(%d) failed", vdata->drm_fd);
        goto cleanup;
    }

    for (i = 0; i < resources->count_connectors; i++) {
        connector = drmModeGetConnector(vdata->drm_fd, resources->connectors[i]);
        if (connector == NULL)
            continue;

        if (connector->connection == DRM_MODE_CONNECTED &&
            connector->count_modes > 0) {
            SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Found connector %d with %d modes.",
                         connector->connector_id, connector->count_modes);
            vdata->saved_conn_id = connector->connector_id;
            break;
        }

        drmModeFreeConnector(connector);
    }

    if (i == resources->count_connectors) {
        ret = SDL_SetError("No currently active connector found.");
        goto cleanup;
    }

    for (i = 0; i < resources->count_encoders; i++) {
        encoder = drmModeGetEncoder(vdata->drm_fd, resources->encoders[i]);

        if (encoder == NULL)
            continue;

        if (encoder->encoder_id == connector->encoder_id) {
            SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Found encoder %d.", encoder->encoder_id);
            data->encoder_id = encoder->encoder_id;
            break;
        }

        drmModeFreeEncoder(encoder);
    }

    if (i == resources->count_encoders) {
        ret = SDL_SetError("No connected encoder found.");
        goto cleanup;
    }

    vdata->saved_crtc = drmModeGetCrtc(vdata->drm_fd, encoder->crtc_id);
    if (vdata->saved_crtc == NULL) {
        ret = SDL_SetError("No CRTC found.");
        goto cleanup;
    }
    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Saved crtc_id %u, fb_id %u, (%u,%u), %ux%u",
                 vdata->saved_crtc->crtc_id, vdata->saved_crtc->buffer_id, vdata->saved_crtc->x,
                 vdata->saved_crtc->y, vdata->saved_crtc->width, vdata->saved_crtc->height);
    data->crtc_id = encoder->crtc_id;
    data->cur_mode = vdata->saved_crtc->mode;

    SDL_zero(current_mode);

    current_mode.w = vdata->saved_crtc->mode.hdisplay;
    current_mode.h = vdata->saved_crtc->mode.vdisplay;
    current_mode.refresh_rate = vdata->saved_crtc->mode.vrefresh;

    /* FIXME ?
    drmModeFB *fb = drmModeGetFB(vdata->drm_fd, vdata->saved_crtc->buffer_id);
    current_mode.format = drmToSDLPixelFormat(fb->bpp, fb->depth);
    drmModeFreeFB(fb);
    */
    current_mode.format = SDL_PIXELFORMAT_ARGB8888;

    current_mode.driverdata = NULL;

    SDL_zero(display);
    display.desktop_mode = current_mode;
    display.current_mode = current_mode;

    display.driverdata = data;
    /* SDL_VideoQuit will later SDL_free(display.driverdata) */
    SDL_AddVideoDisplay(&display);

    /* Setup page flip handler */
    vdata->drm_pollfd.fd = vdata->drm_fd;
    vdata->drm_pollfd.events = POLLIN;
    vdata->drm_evctx.version = DRM_EVENT_CONTEXT_VERSION;
    vdata->drm_evctx.page_flip_handler = KMSDRM_FlipHandler;

#ifdef SDL_INPUT_LINUXEV
    SDL_EVDEV_Init();
#endif

    /*
    KMSDRM_InitMouse(_this);
    */

cleanup:
    if (encoder != NULL)
        drmModeFreeEncoder(encoder);
    if (connector != NULL)
        drmModeFreeConnector(connector);
    if (resources != NULL)
        drmModeFreeResources(resources);

    if (ret != 0) {
        /* Error (complete) cleanup */
        SDL_free(data);
        if(vdata->saved_crtc != NULL) {
            drmModeFreeCrtc(vdata->saved_crtc);
            vdata->saved_crtc = NULL;
        }
        if (vdata->gbm != NULL) {
            gbm_device_destroy(vdata->gbm);
            vdata->gbm = NULL;
        }
        if (vdata->drm_fd > 0) {
            close(vdata->drm_fd);
            vdata->drm_fd = 0;
        }
    }
    return ret;
}

void
KMSDRM_VideoQuit(_THIS)
{
    SDL_VideoData *vdata = ((SDL_VideoData *)_this->driverdata);

    SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "KMSDRM_VideoQuit()");

    if(vdata->saved_crtc != NULL) {
        if(vdata->drm_fd > 0 && vdata->saved_conn_id > 0) {
            /* Restore saved CRTC settings */
            drmModeCrtc *crtc = vdata->saved_crtc;
            if(drmModeSetCrtc(vdata->drm_fd, crtc->crtc_id, crtc->buffer_id,
                              crtc->x, crtc->y, &vdata->saved_conn_id, 1,
                              &crtc->mode) != 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "Could not restore original CRTC mode\n");
            }
        }
        drmModeFreeCrtc(vdata->saved_crtc);
        vdata->saved_crtc = NULL;
    }
    if (vdata->gbm != NULL) {
        gbm_device_destroy(vdata->gbm);
        vdata->gbm = NULL;
    }
    if (vdata->drm_fd > 0) {
        close(vdata->drm_fd);
        SDL_LogDebug(SDL_LOG_CATEGORY_VIDEO, "Closed DRM FD %d", vdata->drm_fd);
        vdata->drm_fd = 0;
    }
#ifdef SDL_INPUT_LINUXEV
    SDL_EVDEV_Quit();
#endif
}

void
KMSDRM_GetDisplayModes(_THIS, SDL_VideoDisplay * display)
{
    /* Only one display mode available, the current one */
    SDL_AddDisplayMode(display, &display->current_mode);
}

int
KMSDRM_SetDisplayMode(_THIS, SDL_VideoDisplay * display, SDL_DisplayMode * mode)
{
    return 0;
}

int
KMSDRM_CreateWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData *wdata;
    SDL_VideoDisplay *display;
    SDL_VideoData *vdata = ((SDL_VideoData *)_this->driverdata);
    SDL_DisplayData *displaydata;

    /* Allocate window internal data */
    wdata = (SDL_WindowData *) SDL_calloc(1, sizeof(SDL_WindowData));
    if (wdata == NULL) {
        return SDL_OutOfMemory();
    }

    wdata->waiting_for_flip = SDL_FALSE;
    display = SDL_GetDisplayForWindow(window);
    displaydata = ((SDL_DisplayData *)display->driverdata);

    /* Windows have one size for now */
    window->w = display->desktop_mode.w;
    window->h = display->desktop_mode.h;

    /* Maybe you didn't ask for a fullscreen OpenGL window, but that's what you get */
    window->flags |= (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_OPENGL);

    wdata->gs = gbm_surface_create(vdata->gbm, window->w, window->h,
        GBM_BO_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);

#if SDL_VIDEO_OPENGL_EGL
    if (!_this->egl_data) {
        if (SDL_GL_LoadLibrary(NULL) < 0) {
            goto error;
        }
    }
    wdata->egl_surface = SDL_EGL_CreateSurface(_this, (NativeWindowType) wdata->gs);

    if (wdata->egl_surface == EGL_NO_SURFACE) {
        SDL_SetError("Could not create EGL window surface");
        goto error;
    }
#endif /* SDL_VIDEO_OPENGL_EGL */

    /* Setup driver data for this window */
    window->driverdata = wdata;

    /* One window, it always has focus */
    SDL_SetMouseFocus(window);
    SDL_SetKeyboardFocus(window);

    /* Window has been successfully created */
    return 0;

error:
    if (wdata != NULL) {
#if SDL_VIDEO_OPENGL_EGL
        if (wdata->egl_surface != EGL_NO_SURFACE)
            SDL_EGL_DestroySurface(_this, wdata->egl_surface);
#endif /* SDL_VIDEO_OPENGL_EGL */
        if (wdata->gs != NULL)
            gbm_surface_destroy(wdata->gs);
        SDL_free(wdata);
    }
    return -1;
}

void
KMSDRM_DestroyWindow(_THIS, SDL_Window * window)
{
    SDL_WindowData *data = (SDL_WindowData *) window->driverdata;
    if(data) {
        /* Wait for any pending page flips and unlock buffer */
        KMSDRM_WaitPageFlip(_this, data, -1);
        if (data->locked_bo != NULL) {
            gbm_surface_release_buffer(data->gs, data->locked_bo);
            data->locked_bo = NULL;
        }
#if SDL_VIDEO_OPENGL_EGL
        if (data->egl_surface != EGL_NO_SURFACE) {
            SDL_EGL_DestroySurface(_this, data->egl_surface);
        }
#endif /* SDL_VIDEO_OPENGL_EGL */
        if (data->gs != NULL) {
            gbm_surface_destroy(data->gs);
            data->gs = NULL;
        }
        SDL_free(data);
        window->driverdata = NULL;
    }
}

int
KMSDRM_CreateWindowFrom(_THIS, SDL_Window * window, const void *data)
{
    return -1;
}

void
KMSDRM_SetWindowTitle(_THIS, SDL_Window * window)
{
}
void
KMSDRM_SetWindowIcon(_THIS, SDL_Window * window, SDL_Surface * icon)
{
}
void
KMSDRM_SetWindowPosition(_THIS, SDL_Window * window)
{
}
void
KMSDRM_SetWindowSize(_THIS, SDL_Window * window)
{
}
void
KMSDRM_ShowWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_HideWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_RaiseWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_MaximizeWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_MinimizeWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_RestoreWindow(_THIS, SDL_Window * window)
{
}
void
KMSDRM_SetWindowGrab(_THIS, SDL_Window * window, SDL_bool grabbed)
{

}

/*****************************************************************************/
/* SDL Window Manager function                                               */
/*****************************************************************************/
SDL_bool
KMSDRM_GetWindowWMInfo(_THIS, SDL_Window * window, struct SDL_SysWMinfo *info)
{
    if (info->version.major <= SDL_MAJOR_VERSION) {
        return SDL_TRUE;
    } else {
        SDL_SetError("application not compiled with SDL %d.%d\n",
                     SDL_MAJOR_VERSION, SDL_MINOR_VERSION);
        return SDL_FALSE;
    }

    /* Failed to get window manager information */
    return SDL_FALSE;
}

#endif /* SDL_VIDEO_DRIVER_KMSDRM */

/* vi: set ts=4 sw=4 expandtab: */
