#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include <dtcore/dtbuffer.h>
#include <dtcore/dtbytes.h>
#include <dtcore/dterr.h>
#include <dtcore/dtheaper.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtobject.h>
#include <dtcore/dtraster.h>
#include <dtcore/dtraster_rgba8888.h>
#include <dtcore/dtrgba8888.h>

#include <dtmc_base/dtmc_base_constants.h>

#include <dtmc_base/dtdisplay.h>
#include <dtmc_base/dtruntime.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>

#include <dtmc/dtdisplay_linux_sdl.h>

DTDISPLAY_INIT_VTABLE(dtdisplay_linux_sdl);
DTOBJECT_INIT_VTABLE(dtdisplay_linux_sdl);

#define TAG "dtdisplay_linux_sdl"

// the implementation
typedef struct dtdisplay_linux_sdl_t
{
    DTDISPLAY_COMMON_MEMBERS;
    dtdisplay_linux_sdl_config_t configuration;

    dtbuffer_t* backing_buffer;
    dtraster_handle backing_raster;

    uint32_t dirty_event_type;

    SDL_Window* sdl_window;
    SDL_Renderer* sdl_renderer;
    SDL_Texture* sdl_texture;

    dttasker_handle event_loop_tasker_handle;
    dtruntime_milliseconds_t event_loop_poll_ms;

    bool is_malloced;
    bool is_sdl_initialized;
    bool event_loop_should_quit;

} dtdisplay_linux_sdl_t;

// forward declarations
static dterr_t*
dtdisplay_linux_sdl__event_loop_entrypoint(void* self_, dttasker_handle tasker_handle);

// --------------------------------------------------------------------------------------------
static dterr_t*
dtdisplay_linux_sdl__setup(dtdisplay_linux_sdl_t* self DTDISPLAY_ATTACH_ARGS);
static dterr_t*
dtdisplay_linux_sdl__teardown(dtdisplay_linux_sdl_t* self DTDISPLAY_DETACH_ARGS);
static dterr_t*
dtdisplay_linux_sdl__event_loop(dtdisplay_linux_sdl_t* self);
static dterr_t*
dtdisplay_linux_sdl__handle_event(dtdisplay_linux_sdl_t* self, int rc, SDL_Event* event, bool* out_should_quit);
static dterr_t*
dtdisplay_linux_sdl__render_dirty(dtdisplay_linux_sdl_t* self);

// ----------------------------------------------------------------------------
// negative SDLError-based format string tokens
#define DTDISPLAY_SDLERROR_FORMAT "error %d (%s)"

// negative SDLError-based format string arguments
#define DTDISPLAY_SDLERROR_ARGS(ERR) (ERR), (SDL_GetError())

// negative SDLError-based return values
#define DTDISPLAY_SDLERROR_C(call)                                                                                             \
    do                                                                                                                         \
    {                                                                                                                          \
        int err;                                                                                                               \
        SDL_ClearError();                                                                                                      \
        if ((err = (call)) < 0)                                                                                                \
        {                                                                                                                      \
            dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, DTDISPLAY_SDLERROR_FORMAT, DTDISPLAY_SDLERROR_ARGS(err));           \
            goto cleanup;                                                                                                      \
        }                                                                                                                      \
    } while (0);
// ----------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------
dterr_t*
dtdisplay_linux_sdl_create(dtdisplay_linux_sdl_t** self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dtheaper_alloc_and_zero(sizeof(dtdisplay_linux_sdl_t), "dtdisplay_linux_sdl_t", (void**)self));
    DTERR_C(dtdisplay_linux_sdl_init(*self));

    (*self)->is_malloced = true;

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
static bool vtables_are_registered = false;

dterr_t*
dtdisplay_linux_sdl_register_vtables(void)
{
    dterr_t* dterr = NULL;

    if (!vtables_are_registered)
    {
        int32_t model_number = DTMC_BASE_CONSTANTS_DISPLAY_MODEL_LINUX_SDL2;

        DTERR_C(dtdisplay_set_vtable(model_number, &dtdisplay_linux_sdl_display_vt));
        DTERR_C(dtobject_set_vtable(model_number, &dtdisplay_linux_sdl_object_vt));

        vtables_are_registered = true;
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
dterr_t*
dtdisplay_linux_sdl_init(dtdisplay_linux_sdl_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    memset(self, 0, sizeof(*self));
    self->model_number = DTMC_BASE_CONSTANTS_DISPLAY_MODEL_LINUX_SDL2;

    DTERR_C(dtdisplay_linux_sdl_register_vtables());

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
dterr_t*
dtdisplay_linux_sdl_config(dtdisplay_linux_sdl_t* self, dtdisplay_linux_sdl_config_t* configuration)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(configuration);

    if (self->backing_buffer != NULL || self->backing_raster != NULL)
    {
        dterr = dterr_new(DTERR_BADCONFIG, DTERR_LOC, NULL, "cannot configure after configuring once");
        goto cleanup;
    }

    self->configuration = *configuration;

    if (self->configuration.window_width <= 0 || //
        self->configuration.window_height <= 0)
    {
        dterr = dterr_new(DTERR_BADCONFIG,
          DTERR_LOC,
          NULL,
          "window size [%" PRIu32 "x%" PRIu32 "] is invalid",
          self->configuration.window_width,
          self->configuration.window_height);
        goto cleanup;
    }

    if (self->configuration.title[0] == '\0')
    {
        snprintf(self->configuration.title,
          sizeof(self->configuration.title),
          "dtdisplay_linux_sdl [%" PRIu32 "x%" PRIu32 "]",
          self->configuration.window_width,
          self->configuration.window_height);
    }

    // buffer holds the raw pixels
    DTERR_C(dtbuffer_create(               //
      &self->backing_buffer,               //
      self->configuration.window_height *  //
        self->configuration.window_width * //
        (int32_t)sizeof(dtrgba8888_t)));
    memset(self->backing_buffer->payload, 0x44, (size_t)self->backing_buffer->length);

    // raster organizes the buffer and provides pixel access
    {
        dtraster_rgba8888_t* o;
        DTERR_C(dtraster_rgba8888_create(&o));
        self->backing_raster = (dtraster_handle)o;
        dtraster_rgba8888_config_t c = {
            .w = self->configuration.window_width,
            .h = self->configuration.window_height,
            .stride_bytes = self->configuration.window_width * sizeof(dtrgba8888_t),
        };
        DTERR_C(dtraster_rgba8888_config(o, &c));
    }

    DTERR_C(dtraster_use_buffer(self->backing_raster, self->backing_buffer));

    // how long the event loop waits for events before timing out and checking if it should quit
    self->event_loop_poll_ms = 10;

    // configure event loop task
    {
        dttasker_config_t c = { 0 };
        c.name = "display";
        c.tasker_entry_point_fn = dtdisplay_linux_sdl__event_loop_entrypoint;
        c.tasker_entry_point_arg = self;
        DTERR_C(dttasker_create(&self->event_loop_tasker_handle, &c));
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
// create a raster of the same type as the backing raster
// this is used by clients who want to create a raster compatible with the display for blitting
dterr_t*
dtdisplay_linux_sdl_create_compatible_raster(dtdisplay_linux_sdl_t* self DTDISPLAY_CREATE_COMPATIBLE_RASTER_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_raster_handle);

    DTERR_C(dtraster_create_compatible_raster(self->backing_raster, out_raster_handle, w, h));

cleanup:

    return dterr;
}

// --------------------------------------------------------------------------------------------
dterr_t*
dtdisplay_linux_sdl_attach(dtdisplay_linux_sdl_t* self DTDISPLAY_ATTACH_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    if (self->is_sdl_initialized)
    {
        dterr = dterr_new(DTERR_BADCONFIG, DTERR_LOC, NULL, "cannot attach after attaching once");
        goto cleanup;
    }
    // start event loop task
    DTERR_C(dttasker_start(self->event_loop_tasker_handle));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
dterr_t*
dtdisplay_linux_sdl_detach(dtdisplay_linux_sdl_t* self DTDISPLAY_DETACH_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    self->event_loop_should_quit = true;
    dtruntime_sleep_milliseconds(self->event_loop_poll_ms * 2); // give the event loop some time to quit

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
dterr_t*
dtdisplay_linux_sdl_blit(dtdisplay_linux_sdl_t* self DTDISPLAY_BLIT_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->backing_raster);
    DTERR_ASSERT_NOT_NULL(raster_handle);

    // blit the incoming raster to the backing raster
    DTERR_C(dtraster_blit(self->backing_raster, raster_handle, x, y));

    // if I call render_dirty from here, it works, but not if only called from the event loop in response to the dirty event
    // DTERR_C(dtdisplay_linux_sdl__render_dirty(self));

    SDL_Event event;
    SDL_zero(event);
    event.type = self->dirty_event_type;
    SDL_PushEvent(&event);

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
// dtobject implementation
// --------------------------------------------------------------------------------------------
void
dtdisplay_linux_sdl_copy(dtdisplay_linux_sdl_t* self, dtdisplay_linux_sdl_t* that_handle)
{
    // no fields to copy
}

// --------------------------------------------------------------------------------------------
void
dtdisplay_linux_sdl_dispose(dtdisplay_linux_sdl_t* self)
{
    if (self == NULL)
        return;

    dtraster_dispose(self->backing_raster);
    dtbuffer_dispose(self->backing_buffer);

    bool is_malloced = self->is_malloced;
    memset(self, 0, sizeof(*self));

    if (is_malloced)
        dtheaper_free(self);
}

// --------------------------------------------------------------------------------------------
bool
dtdisplay_linux_sdl_equals(dtdisplay_linux_sdl_t* a, dtdisplay_linux_sdl_t* b)
{
    return false;
}

// --------------------------------------------------------------------------------------------
const char*
dtdisplay_linux_sdl_get_class(dtdisplay_linux_sdl_t* self)
{
    (void)self;
    return "dtdisplay_linux_sdl_t";
}

// --------------------------------------------------------------------------------------------
bool
dtdisplay_linux_sdl_is_iface(dtdisplay_linux_sdl_t* self, const char* iface_name)
{
    (void)self;

    if (iface_name == NULL)
        return false;

    return strcmp(iface_name, "dtdisplay_iface") == 0 || //
           strcmp(iface_name, "dtobject_iface") == 0;
}

// --------------------------------------------------------------------------------------------
void
dtdisplay_linux_sdl_to_string(dtdisplay_linux_sdl_t* self, char* buffer, size_t buffer_size)
{
    snprintf(buffer,
      buffer_size,
      "dtdisplay_linux_sdl \"%s\" [%" PRIu32 "x%" PRIu32 "]",
      self->configuration.title,
      self->configuration.window_width,
      self->configuration.window_height);
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
dterr_t*
dtdisplay_linux_sdl__event_loop_entrypoint(void* self_, dttasker_handle tasker_handle)
{
    dtdisplay_linux_sdl_t* self = (dtdisplay_linux_sdl_t*)self_;
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(tasker_handle);

    // set up the SDL environment and related resources
    DTERR_C(dtdisplay_linux_sdl__setup(self));

    // signal caller that the event loop is ready
    DTERR_C(dttasker_ready(tasker_handle));

    // run the event loop until it signals to quit
    DTERR_C(dtdisplay_linux_sdl__event_loop(self));

cleanup:
    if (dterr)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, dterr, "rx task exiting");
    }

    // if configured, notify the listener that the event loop is exiting
    if (self->configuration.join_semaphore)
    {
        dterr_t* dterr_semaphore = dtsemaphore_post(self->configuration.join_semaphore);
        if (dterr_semaphore)
        {
            dterr_append(dterr, dterr_semaphore);
        }
    }

    // tear down the SDL environment and related resources
    {
        dterr_t* dterr_teardown = dtdisplay_linux_sdl__teardown(self);
        if (dterr_teardown)
        {
            dterr_append(dterr, dterr_teardown);
        }
    }

    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
dtdisplay_linux_sdl__setup(dtdisplay_linux_sdl_t* self DTDISPLAY_ATTACH_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    if (self->is_sdl_initialized)
    {
        dterr = dterr_new(DTERR_BADCONFIG, DTERR_LOC, NULL, "cannot setup after setting up once");
        goto cleanup;
    }

    DTDISPLAY_SDLERROR_C((SDL_INIT_VIDEO));
    self->is_sdl_initialized = true;

    // make a custom SDL event for signaling when the backing buffer has been updated and needs to be re-rendered
    SDL_ClearError();
    self->dirty_event_type = SDL_RegisterEvents(1);
    if (self->dirty_event_type == (Uint32)-1)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "failed to register SDL event: %s", SDL_GetError());
        goto cleanup;
    }

    SDL_ClearError();
    self->sdl_window = SDL_CreateWindow(self->configuration.title,
      SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED,
      self->configuration.window_width,
      self->configuration.window_height,
      0);
    if (self->sdl_window == NULL)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "failed to create SDL window: %s", SDL_GetError());
        goto cleanup;
    }

    SDL_ClearError();
    self->sdl_renderer = SDL_CreateRenderer(self->sdl_window, -1, SDL_RENDERER_ACCELERATED);
    if (self->sdl_renderer == NULL)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "failed to create SDL renderer: %s", SDL_GetError());
        goto cleanup;
    }

    SDL_ClearError();
    self->sdl_texture = SDL_CreateTexture(self->sdl_renderer,
      SDL_PIXELFORMAT_ABGR8888,
      SDL_TEXTUREACCESS_STREAMING,
      self->configuration.window_width,
      self->configuration.window_height);
    if (self->sdl_texture == NULL)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "failed to create SDL texture: %s", SDL_GetError());
        goto cleanup;
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
dtdisplay_linux_sdl__teardown(dtdisplay_linux_sdl_t* self DTDISPLAY_DETACH_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    if (self->sdl_texture)
        SDL_DestroyTexture(self->sdl_texture);
    if (self->sdl_renderer)
        SDL_DestroyRenderer(self->sdl_renderer);
    if (self->sdl_window)
        SDL_DestroyWindow(self->sdl_window);

    if (self->is_sdl_initialized)
        SDL_Quit();

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
static dterr_t*
dtdisplay_linux_sdl__event_loop(dtdisplay_linux_sdl_t* self)
{
    dterr_t* dterr = NULL;

    SDL_Event event;
    int rc;
    bool out_should_quit = false;

    while (true)
    {
        SDL_ClearError();
        rc = SDL_WaitEventTimeout(&event, self->event_loop_poll_ms);
        if (self->event_loop_should_quit)
            break;

        DTERR_C(dtdisplay_linux_sdl__handle_event(self, rc, &event, &out_should_quit));
        if (out_should_quit)
            break;
    }

cleanup:
    if (dterr)
    {
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "rx task server loop exiting due to error");
    }
    else
    {
        dtlog_debug(TAG,
          "rx task server loop exiting normally on because %s",
          self->event_loop_should_quit ? "commanded event_loop_should_quit"
          : out_should_quit            ? "SDL mechanism quit"
                                       : "unknown");
    }

    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
dtdisplay_linux_sdl__handle_event(dtdisplay_linux_sdl_t* self, int rc, SDL_Event* event, bool* out_should_quit)
{
    dterr_t* dterr = NULL;

    *out_should_quit = false;
    if (rc == 0)
    {
        const char* message = SDL_GetError();
        if (message == NULL || message[0] == '\0')
            goto cleanup;
    }
    if (event->type == SDL_QUIT)
    {
        *out_should_quit = true;
    }
    else if (event->type == SDL_WINDOWEVENT)
    {
        if (event->window.event == SDL_WINDOWEVENT_CLOSE)
        {
            *out_should_quit = true;
        }
    }
    else if (event->type == self->dirty_event_type)
    {
        DTERR_C(dtdisplay_linux_sdl__render_dirty(self));
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
dtdisplay_linux_sdl__render_dirty(dtdisplay_linux_sdl_t* self)
{
    dterr_t* dterr = NULL;

    {
        char s[256];
        dtbytes_compose_hex(self->backing_buffer->payload, 32, s, sizeof(s));
        dtlog_debug(TAG, "%s is the dirty backing buffer", s);
    }

    // update the SDL texture with the new pixel data from the backing buffer
    DTDISPLAY_SDLERROR_C(SDL_UpdateTexture( //
      self->sdl_texture,
      NULL,
      self->backing_buffer->payload,
      self->configuration.window_width * sizeof(dtrgba8888_t)));

    DTDISPLAY_SDLERROR_C(SDL_RenderClear(self->sdl_renderer));

    DTDISPLAY_SDLERROR_C(SDL_RenderCopy(self->sdl_renderer, self->sdl_texture, NULL, NULL));

    SDL_RenderPresent(self->sdl_renderer);

cleanup:
    return dterr;
}
