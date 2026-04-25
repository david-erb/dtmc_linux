#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dtcore/dterr.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dttasker.h>

#include <dtmc_services/dtservice.h>

// some local macros referring to the current svc to make it a bit easier to read the code
#define svc_name "svc_adcsink"
#define svc_t svc_adcsink_t
#define svc_config_t svc_adcsink_config_t
#define svc_describe svc_adcsink_describe
#define svc_create svc_adcsink_create
#define svc_configure svc_adcsink_configure
#define svc_start svc_adcsink_start
#define svc_entrypoint svc_adcsink_entrypoint
#define svc_dispose svc_adcsink_dispose

// forward declare the svc's privates
typedef struct svc_t svc_t;

// how the svc can be configured
typedef struct svc_config_t
{
    dtservice_handle rx_service_handle;
    dtservice_handle tx_service_handle;

    // how long to wait before trying to recover from an error
    int32_t recover_cooldown_rx_ms;
    int32_t recover_cooldown_tx_ms;

    const char* rx_flowmonitor_name;
    const char* tx_flowmonitor_name;

    bool verbose;
} svc_config_t;

// create a new instance, allocating memory as needed
extern dterr_t*
svc_create(svc_t** self);

// configure the svc instance with handles to implementations and settings
extern dterr_t*
svc_configure(svc_t* self, svc_config_t* config);

// run the svc logic, typically returning leaving tasks running and callbacks registered
extern dterr_t*
svc_start(svc_t* self);

// stop, unregister and dispose of the svc instance resources
extern void
svc_dispose(svc_t* self);

// handle if a task changes state
extern dterr_t*
svc_adcsink_task_info_callback(void* arg, dttasker_info_t* info);
