#include <lvgl.h>

#include <dtcore/dterr.h>

#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtruntime.h>

#include <dtmc_base_demos/demo_helpers.h>
#include <dtmc_base_demos/demo_lvgl_card.h>

#define TAG "main"

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;

    demo_t* demo = NULL;

    // === create and configure the demo instance ===
    {
        DTERR_C(demo_create(&demo));
        demo_config_t c = { 0 };
        DTERR_C(demo_configure(demo, &c));
    }

    // === initialize Linux-specific LVGL and its SDL drivers ===
    lv_init();

    lv_display_t * display = lv_sdl_window_create(320, 240);
    if(display == NULL) 
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "lv_sdl_window_create failed");
        goto cleanup;
    }

    lv_sdl_mouse_create();
    lv_sdl_mousewheel_create();
    lv_sdl_keyboard_create();

    dtlog_info(TAG, "demo starting...");

    // === start the demo ===
    DTERR_C(demo_start(demo));

    dtlog_info(TAG, "demo started successfully");

    // === watch for events ===
    while(1) {
        lv_timer_handler();
        lv_delay_ms(5);
    }
    

cleanup:
    int rc = (dterr != NULL) ? -1 : 0;

    // log and dispose error chain if any
    dtlog_dterr(TAG, dterr);
    dterr_dispose(dterr);

    // dispose the demo instance
    demo_dispose(demo);

    // dispose the objects

    return rc;
}
