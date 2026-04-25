#include <dtcore/dterr.h>

#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtruntime.h>

#include <dtmc_base/dtdisplay.h>
#include <dtmc_base/dtsemaphore.h>

#include <dtmc/dtdisplay_linux_sdl.h>

#include <dtmc_base_demos/demo_display_hello.h>
#include <dtmc_base_demos/demo_helpers.h>

#define TAG "main"

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;
    dtsemaphore_handle join_semaphore = NULL;
    dtdisplay_handle display_handle = NULL;
    demo_t* demo = NULL;

    DTERR_C(dtsemaphore_create(&join_semaphore, 0, 0));

    {
        dtdisplay_linux_sdl_t* o = NULL;
        DTERR_C(dtdisplay_linux_sdl_create(&o));
        display_handle = (dtdisplay_handle)o;
        dtdisplay_linux_sdl_config_t c = {
            .window_width = 320,
            .window_height = 240,
        };
        c.join_semaphore = join_semaphore;
        DTERR_C(dtdisplay_linux_sdl_config(o, &c));
    }

    // === create and configure the demo instance ===
    {
        DTERR_C(demo_create(&demo));
        demo_config_t c = { 0 };
        c.display_handle = display_handle;
        DTERR_C(demo_configure(demo, &c));
    }

    dtlog_info(TAG, "demo starting...");

    // === start the demo ===
    DTERR_C(demo_start(demo));

    dtlog_info(TAG, "demo started successfully");

    // === watch for events ===
    bool was_timeout;
    while (1)
    {
        DTERR_C(dtsemaphore_wait(join_semaphore, 100, &was_timeout));
        if (!was_timeout)
        {
            break;
        }
    }

    dtlog_info(TAG, "demo event loop signaled to quit, cleaning up...");

cleanup:
    int rc = (dterr != NULL) ? -1 : 0;

    // log and dispose error chain if any
    dtlog_dterr(TAG, dterr);
    dterr_dispose(dterr);

    // dispose the demo instance
    demo_dispose(demo);

    // dispose the objects
    dtdisplay_dispose(display_handle);
    dtsemaphore_dispose(join_semaphore);
    return rc;
}
