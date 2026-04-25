#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dtcore/dterr.h>

// some local macros referring to the current demo to make it a bit easier to read the code
#define demo_name "dtmc_base_demo_lvgl_card"
#define demo_t dtmc_base_demo_lvgl_card_t
#define demo_config_t dtmc_base_demo_lvgl_card_config_t
#define demo_describe dtmc_base_demo_lvgl_card_describe
#define demo_create dtmc_base_demo_lvgl_card_create
#define demo_configure dtmc_base_demo_lvgl_card_configure
#define demo_start dtmc_base_demo_lvgl_card_start
#define demo_entrypoint dtmc_base_demo_lvgl_card_entrypoint
#define demo_dispose dtmc_base_demo_lvgl_card_dispose

// forward declare the demo's privates
typedef struct demo_t demo_t;

// how the demo can be configured
typedef struct demo_config_t
{
    int32_t placeholder; // for now, no configuration options, but we need the struct to be non-empty
} demo_config_t;

// create a new instance, allocating memory as needed
extern dterr_t*
demo_create(demo_t** self);

// configure the demo instance with handles to implementations and settings
extern dterr_t*
demo_configure(demo_t* self, demo_config_t* config);

// run the demo logic, typically returning leaving tasks running and callbacks registered
extern dterr_t*
demo_start(demo_t* self);

// stop, unregister and dispose of the demo instance resources
extern void
demo_dispose(demo_t* self);
