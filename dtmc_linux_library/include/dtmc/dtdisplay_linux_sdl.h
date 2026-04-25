/*
 * dtdisplay_linux_sdl -- SDL2 display backend for the dtdisplay raster interface.
 *
 * Implements the dtdisplay vtable using SDL2, opening a titled window of
 * configurable pixel dimensions and blitting dtraster frames to it. An
 * optional join semaphore lets the owning task block until the SDL event
 * loop exits. Compatible raster buffers are created in the display's native
 * pixel format through the standard dtdisplay_create_compatible_raster path,
 * so rendering code is unchanged when the backend is swapped.
 *
 * cdox v1.0.2
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <dtcore/dterr.h>
#include <dtcore/dtobject.h>

#include <dtmc_base/dtdisplay.h>
#include <dtmc_base/dtsemaphore.h>

// configuration for this implementation
typedef struct dtdisplay_linux_sdl_config_t
{
    char title[64];
    uint32_t window_width;
    uint32_t window_height;
    dtsemaphore_handle join_semaphore;
} dtdisplay_linux_sdl_config_t;

typedef struct dtdisplay_linux_sdl_t dtdisplay_linux_sdl_t;

dterr_t*
dtdisplay_linux_sdl_create(dtdisplay_linux_sdl_t** this);
dterr_t*
dtdisplay_linux_sdl_init(dtdisplay_linux_sdl_t* this);
dterr_t*
dtdisplay_linux_sdl_config(dtdisplay_linux_sdl_t* this, dtdisplay_linux_sdl_config_t* configuration);
dterr_t*
dtdisplay_linux_sdl_register_vtables(void);

dterr_t*
dtdisplay_linux_sdl_blit(dtdisplay_linux_sdl_t* self DTDISPLAY_BLIT_ARGS);
dterr_t*
dtdisplay_linux_sdl_attach(dtdisplay_linux_sdl_t* self DTDISPLAY_ATTACH_ARGS);
dterr_t*
dtdisplay_linux_sdl_detach(dtdisplay_linux_sdl_t* self DTDISPLAY_DETACH_ARGS);
dterr_t*
dtdisplay_linux_sdl_create_compatible_raster(dtdisplay_linux_sdl_t* self DTDISPLAY_CREATE_COMPATIBLE_RASTER_ARGS);

// --------------------------------------------------------------------------------------------

// facade implementations
DTDISPLAY_DECLARE_API(dtdisplay_linux_sdl);
DTOBJECT_DECLARE_API(dtdisplay_linux_sdl);
