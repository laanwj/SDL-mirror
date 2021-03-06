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
#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_KMSDRM

#include "SDL_assert.h"
#include "SDL_surface.h"

#include "SDL_kmsdrmvideo.h"
#include "SDL_kmsdrmmouse.h"
#include "SDL_kmsdrmdyn.h"

#include "../SDL_sysvideo.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/default_cursor.h"


static SDL_Cursor *KMSDRM_CreateDefaultCursor(void);
static SDL_Cursor *KMSDRM_CreateCursor(SDL_Surface * surface, int hot_x, int hot_y);
static int KMSDRM_ShowCursor(SDL_Cursor * cursor);
static void KMSDRM_MoveCursor(SDL_Cursor * cursor);
static void KMSDRM_FreeCursor(SDL_Cursor * cursor);
static void KMSDRM_WarpMouse(SDL_Window * window, int x, int y);
static int KMSDRM_WarpMouseGlobal(int x, int y);

static SDL_Cursor *
KMSDRM_CreateDefaultCursor(void)
{
    return SDL_CreateCursor(default_cdata, default_cmask, DEFAULT_CWIDTH, DEFAULT_CHEIGHT, DEFAULT_CHOTX, DEFAULT_CHOTY);
}

/* Create a cursor from a surface */
static SDL_Cursor *
KMSDRM_CreateCursor(SDL_Surface * surface, int hot_x, int hot_y)
{
    SDL_VideoDevice *dev = SDL_GetVideoDevice();
    SDL_VideoData *vdata = ((SDL_VideoData *)dev->driverdata);
    SDL_PixelFormat *pixlfmt = surface->format;
    KMSDRM_CursorData *curdata;
    SDL_Cursor *cursor;
    int i, ret;
    uint32_t bo_format, bo_stride;
    char *buffer = NULL;
    size_t bufsize;

    switch(pixlfmt->format) {
    case SDL_PIXELFORMAT_RGB332:
        bo_format = GBM_FORMAT_RGB332;
        break;
    case SDL_PIXELFORMAT_ARGB4444:
        bo_format = GBM_FORMAT_ARGB4444;
        break;
    case SDL_PIXELFORMAT_RGBA4444:
        bo_format = GBM_FORMAT_RGBA4444;
        break;
    case SDL_PIXELFORMAT_ABGR4444:
        bo_format = GBM_FORMAT_ABGR4444;
        break;
    case SDL_PIXELFORMAT_BGRA4444:
        bo_format = GBM_FORMAT_BGRA4444;
        break;
    case SDL_PIXELFORMAT_ARGB1555:
        bo_format = GBM_FORMAT_ARGB1555;
        break;
    case SDL_PIXELFORMAT_RGBA5551:
        bo_format = GBM_FORMAT_RGBA5551;
        break;
    case SDL_PIXELFORMAT_ABGR1555:
        bo_format = GBM_FORMAT_ABGR1555;
        break;
    case SDL_PIXELFORMAT_BGRA5551:
        bo_format = GBM_FORMAT_BGRA5551;
        break;
    case SDL_PIXELFORMAT_RGB565:
        bo_format = GBM_FORMAT_RGB565;
        break;
    case SDL_PIXELFORMAT_BGR565:
        bo_format = GBM_FORMAT_BGR565;
        break;
    case SDL_PIXELFORMAT_RGB888:
    case SDL_PIXELFORMAT_RGB24:
        bo_format = GBM_FORMAT_RGB888;
        break;
    case SDL_PIXELFORMAT_BGR888:
    case SDL_PIXELFORMAT_BGR24:
        bo_format = GBM_FORMAT_BGR888;
        break;
    case SDL_PIXELFORMAT_RGBX8888:
        bo_format = GBM_FORMAT_RGBX8888;
        break;
    case SDL_PIXELFORMAT_BGRX8888:
        bo_format = GBM_FORMAT_BGRX8888;
        break;
    case SDL_PIXELFORMAT_ARGB8888:
        bo_format = GBM_FORMAT_ARGB8888;
        break;
    case SDL_PIXELFORMAT_RGBA8888:
        bo_format = GBM_FORMAT_RGBA8888;
        break;
    case SDL_PIXELFORMAT_ABGR8888:
        bo_format = GBM_FORMAT_ABGR8888;
        break;
    case SDL_PIXELFORMAT_BGRA8888:
        bo_format = GBM_FORMAT_BGRA8888;
        break;
    case SDL_PIXELFORMAT_ARGB2101010:
        bo_format = GBM_FORMAT_ARGB2101010;
        break;
    default:
        SDL_SetError("Unsupported pixel format for cursor");
        return NULL;
    }

    if (!KMSDRM_gbm_device_is_format_supported(vdata->gbm, bo_format, GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE)) {
        SDL_SetError("Unsupported pixel format for cursor");
        return NULL;
    }

    cursor = (SDL_Cursor *) SDL_calloc(1, sizeof(*cursor));
    if (cursor == NULL) {
        SDL_OutOfMemory();
        return NULL;
    }
    curdata = (KMSDRM_CursorData *) SDL_calloc(1, sizeof(*curdata));
    if (curdata == NULL) {
        SDL_OutOfMemory();
        SDL_free(cursor);
        return NULL;
    }

    curdata->hot_x = hot_x;
    curdata->hot_y = hot_y;
    curdata->w = surface->w;
    curdata->h = surface->h;

    curdata->bo = KMSDRM_gbm_bo_create(vdata->gbm, surface->w, surface->h, bo_format,
                                       GBM_BO_USE_CURSOR | GBM_BO_USE_WRITE);
    if (curdata->bo == NULL) {
        SDL_SetError("Could not create GBM cursor BO");
        goto cleanup;
    }

    bo_stride = KMSDRM_gbm_bo_get_stride(curdata->bo);
    bufsize = bo_stride * surface->h;

    if (surface->pitch != bo_stride) {
        /* pitch doesn't match stride, must be copied to temp buffer  */
        buffer = SDL_malloc(bufsize);
        if (buffer == NULL) {
            SDL_OutOfMemory();
            goto cleanup;
        }

        if (SDL_MUSTLOCK(surface)) {
            if (SDL_LockSurface(surface) < 0) {
                /* Could not lock surface */
                goto cleanup;
            }
        }

        /* Copy to temporary buffer */
        for (i = 0; i < surface->h; i++) {
            SDL_memcpy(buffer + (i * bo_stride),
                       ((char *)surface->pixels) + (i * surface->pitch),
                       surface->w * pixlfmt->BytesPerPixel);
        }

        if (SDL_MUSTLOCK(surface)) {
            SDL_UnlockSurface(surface);
        }

        if (KMSDRM_gbm_bo_write(curdata->bo, buffer, bufsize)) {
            SDL_SetError("Could not write to GBM cursor BO");
            goto cleanup;
        }

        /* Free temporary buffer */
        SDL_free(buffer);
        buffer = NULL;
    } else {
        /* surface matches BO format */
        if (SDL_MUSTLOCK(surface)) {
            if (SDL_LockSurface(surface) < 0) {
                /* Could not lock surface */
                goto cleanup;
            }
        }

        ret = KMSDRM_gbm_bo_write(curdata->bo, surface->pixels, bufsize);

        if (SDL_MUSTLOCK(surface)) {
            SDL_UnlockSurface(surface);
        }

        if (ret) {
            SDL_SetError("Could not write to GBM cursor BO");
            goto cleanup;
        }
    }

    cursor->driverdata = curdata;

    return cursor;

cleanup:
    if (buffer != NULL) {
        SDL_free(buffer);
    }
    if (cursor != NULL) {
        SDL_free(cursor);
    }
    if (curdata != NULL) {
        if (curdata->bo != NULL) {
            KMSDRM_gbm_bo_destroy(curdata->bo);
        }
        SDL_free(curdata);
    }
    return NULL;
}

/* Show the specified cursor, or hide if cursor is NULL */
static int
KMSDRM_ShowCursor(SDL_Cursor * cursor)
{
    SDL_VideoDevice *dev = SDL_GetVideoDevice();
    SDL_VideoData *vdata = ((SDL_VideoData *)dev->driverdata);
    SDL_Mouse *mouse;
    KMSDRM_CursorData *curdata;
    SDL_VideoDisplay *display = NULL;
    SDL_DisplayData *ddata = NULL;
    int ret;
    uint32_t bo_handle;

    mouse = SDL_GetMouse();
    if (mouse == NULL) {
        return SDL_SetError("No mouse.");
    }

    if (mouse->focus != NULL) {
        display = SDL_GetDisplayForWindow(mouse->focus);
        if (display != NULL) {
            ddata = (SDL_DisplayData*) display->driverdata;
        }
    }

    if (cursor == NULL) {
        /* Hide current cursor */
        if ( mouse->cur_cursor != NULL && mouse->cur_cursor->driverdata != NULL) {
            curdata = (KMSDRM_CursorData *) mouse->cur_cursor->driverdata;

            if (curdata->crtc_id != 0) {
                ret = KMSDRM_drmModeSetCursor(vdata->drm_fd, curdata->crtc_id, 0, 0, 0);
                if (ret) {
                    SDL_SetError("Could not hide current cursor with drmModeSetCursor().");
                    return ret;
                }
                /* Mark previous cursor as not-displayed */
                curdata->crtc_id = 0;

                return 0;
            }
        }
        /* otherwise if possible, hide global cursor */
        if (ddata != NULL && ddata->crtc_id != 0) {
            ret = KMSDRM_drmModeSetCursor(vdata->drm_fd, ddata->crtc_id, 0, 0, 0);
            if (ret) {
                SDL_SetError("Could not hide display's cursor with drmModeSetCursor().");
                return ret;
            }
            return 0;
        }

        return SDL_SetError("Couldn't find cursor to hide.");
    }
    /* If cursor != NULL, show new cursor on display */
    if (display == NULL) {
        return SDL_SetError("Could not get display for mouse.");
    }
    if (ddata == NULL) {
        return SDL_SetError("Could not get display driverdata.");
    }

    curdata = (KMSDRM_CursorData *) cursor->driverdata;
    if (curdata == NULL || curdata->bo == NULL) {
        return SDL_SetError("Cursor not initialized properly.");
    }

    bo_handle = KMSDRM_gbm_bo_get_handle(curdata->bo).u32;
    if (curdata->hot_x == 0 && curdata->hot_y == 0) {
        ret = KMSDRM_drmModeSetCursor(vdata->drm_fd, ddata->crtc_id, bo_handle,
                                      curdata->w, curdata->h);
    } else {
        ret = KMSDRM_drmModeSetCursor2(vdata->drm_fd, ddata->crtc_id, bo_handle,
                                       curdata->w, curdata->h,
                                       curdata->hot_x, curdata->hot_y);
    }
    if (ret) {
        SDL_SetError("drmModeSetCursor failed.");
        return ret;
    }

    curdata->crtc_id = ddata->crtc_id;

    return 0;
}

/* Free a window manager cursor */
static void
KMSDRM_FreeCursor(SDL_Cursor * cursor)
{
    KMSDRM_CursorData *curdata;
    int drm_fd;

    if (cursor != NULL) {
        curdata = (KMSDRM_CursorData *) cursor->driverdata;

        if (curdata != NULL) {
            if (curdata->bo != NULL) {
                if (curdata->crtc_id != 0) {
                    drm_fd = KMSDRM_gbm_device_get_fd(KMSDRM_gbm_bo_get_device(curdata->bo));
                    /* Hide the cursor if previously shown on a CRTC */
                    KMSDRM_drmModeSetCursor(drm_fd, curdata->crtc_id, 0, 0, 0);
                    curdata->crtc_id = 0;
                }
                KMSDRM_gbm_bo_destroy(curdata->bo);
                curdata->bo = NULL;
            }
            SDL_free(cursor->driverdata);
        }
        SDL_free(cursor);
    }
}

/* Warp the mouse to (x,y) */
static void
KMSDRM_WarpMouse(SDL_Window * window, int x, int y)
{
    /* Only one global/fullscreen window is supported */
    KMSDRM_WarpMouseGlobal(x, y);
}

/* Warp the mouse to (x,y) */
static int
KMSDRM_WarpMouseGlobal(int x, int y)
{
    KMSDRM_CursorData *curdata;
    SDL_Mouse *mouse = SDL_GetMouse();

    if (mouse != NULL && mouse->cur_cursor != NULL && mouse->cur_cursor->driverdata != NULL) {
        curdata = (KMSDRM_CursorData *) mouse->cur_cursor->driverdata;
        if (curdata->bo != NULL) {
            if (curdata->crtc_id != 0) {
                int ret, drm_fd;
                drm_fd = KMSDRM_gbm_device_get_fd(KMSDRM_gbm_bo_get_device(curdata->bo));
                ret = KMSDRM_drmModeMoveCursor(drm_fd, curdata->crtc_id, x, y);

                if (ret) {
                    SDL_SetError("drmModeMoveCursor() failed.");
                }

                return ret;
            } else {
                return 0;
            }
        } else {
            return SDL_SetError("Cursor not initialized properly.");
        }
    } else {
        return SDL_SetError("No mouse or current cursor.");
    }
}

void
KMSDRM_InitMouse(_THIS)
{
    /* FIXME: Using UDEV it should be possible to scan all mice
     * but there's no point in doing so as there's no multimice support...yet!
     */
    SDL_Mouse *mouse = SDL_GetMouse();

    mouse->CreateCursor = KMSDRM_CreateCursor;
    mouse->ShowCursor = KMSDRM_ShowCursor;
    mouse->MoveCursor = KMSDRM_MoveCursor;
    mouse->FreeCursor = KMSDRM_FreeCursor;
    mouse->WarpMouse = KMSDRM_WarpMouse;
    mouse->WarpMouseGlobal = KMSDRM_WarpMouseGlobal;

    SDL_SetDefaultCursor(KMSDRM_CreateDefaultCursor());
}

void
KMSDRM_QuitMouse(_THIS)
{
    /* TODO: ? */
}

/* This is called when a mouse motion event occurs */
static void
KMSDRM_MoveCursor(SDL_Cursor * cursor)
{
    SDL_Mouse *mouse = SDL_GetMouse();
    KMSDRM_WarpMouse(mouse->focus, mouse->x, mouse->y);
}

#endif /* SDL_VIDEO_DRIVER_KMSDRM */

/* vi: set ts=4 sw=4 expandtab: */
