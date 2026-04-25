#include <dtcore/dterr.h>

#include <dtcore/dterr.h>
#include <dtcore/dteventlogger.h>
#include <dtcore/dtheaper.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtobject.h>
#include <dtcore/dtparse.h>
#include <dtcore/dtpicosdk_helper.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtcpu.h>
#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtruntime.h>
#include <dtmc_base/dtsemaphore.h>

#include <dtmc_services/dtflowmonitor.h>
#include <dtmc_services/dtservice.h>

#include "svc_adcws.h"

#define TAG "svc_adcws"

// metrics for reporting
typedef struct metrics_t
{
    int32_t adc_scans_received;
    int32_t adc_scans_sent;
    int32_t adc_scan_bytes_received;
    int32_t adc_scan_bytes_sent;
} metrics_t;

// the svc's privates
typedef struct svc_t
{
    svc_config_t config;
    dttasker_handle rx_tasker_handle;
    dttasker_handle tx_tasker_handle;
    dttasker_handle zz_tasker_handle;
    metrics_t metrics;
} svc_t;

// forward declare the task functions
static dterr_t*
svc__rx_entrypoint(void* arg, dttasker_handle tasker_handle);
static dterr_t*
svc__tx_entrypoint(void* arg, dttasker_handle tasker_handle);
static dterr_t*
svc__zz_entrypoint(void* arg, dttasker_handle tasker_handle);
static dterr_t*
svc__task_info_callback(void* arg, dttasker_info_t* info);
static dterr_t*
svc__get_metric_int32(dtkvp_list_t* kvp_list, const char* key, int32_t* out_value);

// --------------------------------------------------------------------------------------
// return a string description of the svc (the returned string is heap allocated)
dterr_t*
svc_describe(svc_t* self, char** out_description)
{
    dterr_t* dterr = NULL;
    char tmp[256];
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->config.rx_service_handle);
    DTERR_ASSERT_NOT_NULL(self->config.tx_service_handle);
    DTERR_ASSERT_NOT_NULL(out_description);

    *out_description = NULL;

    char* d = NULL;
    char* s = "\n    ";
    d = dtstr_concat_format(d, s, "Description of the svc:");

    d = dtstr_concat_format(d, s, "This service transfers data between from the rx to the tx service");

    dtservice_to_string(self->config.rx_service_handle, tmp, sizeof(tmp));
    d = dtstr_concat_format(d, s, "The rx service object is %s", tmp);

    dtservice_to_string(self->config.tx_service_handle, tmp, sizeof(tmp));
    d = dtstr_concat_format(d, s, "The tx service object is %s", tmp);

    *out_description = d;

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// Create a new instance, allocating memory as needed
dterr_t*
svc_create(svc_t** self_ptr)
{
    dterr_t* dterr = NULL;
    svc_t* self = NULL;
    DTERR_ASSERT_NOT_NULL(self_ptr);

    DTERR_C(dtheaper_alloc_and_zero(sizeof(svc_t), "svc_t instance", (void**)self_ptr));

    self = *self_ptr;

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// Configure the svc instance with handles to implementations and settings
dterr_t*
svc_configure( //
  svc_t* self,
  svc_config_t* config)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(config);
    DTERR_ASSERT_NOT_NULL(config->rx_service_handle);
    DTERR_ASSERT_NOT_NULL(config->tx_service_handle);

    self->config = *config;

    self->config.recover_cooldown_rx_ms = self->config.recover_cooldown_rx_ms > 0 ? self->config.recover_cooldown_rx_ms : 2000;
    self->config.recover_cooldown_tx_ms = self->config.recover_cooldown_tx_ms > 0 ? self->config.recover_cooldown_tx_ms : 100;

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// Run the svc logic (loops forever, blocking)
dterr_t*
svc_start(svc_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->config.rx_service_handle);
    DTERR_ASSERT_NOT_NULL(self->config.tx_service_handle);

    {
        char* description = NULL;
        DTERR_C(svc_describe(self, &description));
        dtlog_info(TAG, "%s", description);
        dtstr_dispose(description);
    }

    {
        dttasker_config_t c = { 0 };
        c.tasker_entry_point_fn = svc__rx_entrypoint;
        c.tasker_entry_point_arg = self;
        c.name = "dtservice_rx";
        c.stack_size = 4096;
        c.priority = DTTASKER_PRIORITY_NORMAL_MEDIUM;
        c.tasker_info_callback = svc__task_info_callback;
        c.tasker_info_callback_context = self;
        DTERR_C(dttasker_create(&self->rx_tasker_handle, &c));
    }

    {
        dttasker_config_t c = { 0 };
        c.tasker_entry_point_fn = svc__tx_entrypoint;
        c.tasker_entry_point_arg = self;
        c.name = "dtservice_tx";
        c.stack_size = 32768;
        c.priority = DTTASKER_PRIORITY_NORMAL_MEDIUM;
        c.tasker_info_callback = svc__task_info_callback;
        c.tasker_info_callback_context = self;
        DTERR_C(dttasker_create(&self->tx_tasker_handle, &c));
    }

    {
        dttasker_config_t c = { 0 };
        c.tasker_entry_point_fn = svc__zz_entrypoint;
        c.tasker_entry_point_arg = self;
        c.name = "dtservice_zz";
        c.stack_size = 4096;
        c.priority = DTTASKER_PRIORITY_BACKGROUND_MEDIUM;
        c.tasker_info_callback = svc__task_info_callback;
        c.tasker_info_callback_context = self;
        DTERR_C(dttasker_create(&self->zz_tasker_handle, &c));
    }

    DTERR_C(dttasker_start(self->rx_tasker_handle));
    DTERR_C(dttasker_start(self->tx_tasker_handle));
    DTERR_C(dttasker_start(self->zz_tasker_handle));

    while (true)
    {
        dttasker_info_t info;

        dtruntime_sleep_milliseconds(100);

        DTERR_C(dttasker_get_info(self->rx_tasker_handle, &info));
        if (info.dterr)
        {
            dterr = info.dterr;
            goto cleanup;
        }

        DTERR_C(dttasker_get_info(self->tx_tasker_handle, &info));
        if (info.dterr)
        {
            dterr = info.dterr;
            goto cleanup;
        }

        DTERR_C(dttasker_get_info(self->zz_tasker_handle, &info));
        if (info.dterr)
        {
            dterr = info.dterr;
            goto cleanup;
        }
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// do rx business logic

dterr_t*
svc__rx_entrypoint(void* arg, dttasker_handle tasker_handle)
{
    svc_t* self = (svc_t*)arg;
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dttasker_ready(tasker_handle));

    while (true)
    {
        dterr = dtservice_happy_loop(self->config.rx_service_handle);
        if (dterr)
        {
            if (self->config.verbose)
            {
                dtlog_dterr(TAG, dterr);
            }
            dterr_dispose(dterr);
            dterr = NULL;
        }

        DTERR_C(dtservice_recover(self->config.rx_service_handle));
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// do tx business logic

dterr_t*
svc__tx_entrypoint(void* arg, dttasker_handle tasker_handle)
{
    svc_t* self = (svc_t*)arg;
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dttasker_ready(tasker_handle));

    while (true)
    {
        dterr = dtservice_happy_loop(self->config.tx_service_handle);
        if (dterr)
        {
            if (self->config.verbose)
            {
                dtlog_dterr(TAG, dterr);
            }
            dterr_dispose(dterr);
            dterr = NULL;
        }

        DTERR_C(dtservice_recover(self->config.tx_service_handle));
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// monitor

dterr_t*
svc__zz_entrypoint(void* arg, dttasker_handle tasker_handle)
{
    svc_t* self = (svc_t*)arg;
    dterr_t* dterr = NULL;
    dtflowmonitor_t rx_flow_monitor = { 0 };
    dtflowmonitor_t tx_flow_monitor = { 0 };
    char* s = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dtflowmonitor_init(&rx_flow_monitor));
    rx_flow_monitor.name = self->config.rx_flowmonitor_name ? self->config.rx_flowmonitor_name : "rx";

    DTERR_C(dtflowmonitor_init(&tx_flow_monitor));
    tx_flow_monitor.name = self->config.tx_flowmonitor_name ? self->config.tx_flowmonitor_name : "tx";

    DTERR_C(dttasker_ready(tasker_handle));

    while (true)
    {
        dtruntime_sleep_milliseconds(1000);
        dtruntime_milliseconds_t now = dtruntime_now_milliseconds();

        if (self->config.rx_service_handle != NULL)
        {
            DTERR_C(dtflowmonitor_concat_rxtx(&rx_flow_monitor, self->config.rx_service_handle, now, s, NULL, &s));
        }

        s = dtstr_concat_format(s, NULL, " || ");

        if (self->config.tx_service_handle != NULL)
        {
            DTERR_C(dtflowmonitor_concat_rxtx(&tx_flow_monitor, self->config.tx_service_handle, now, s, NULL, &s));
        }

        dtlog_info(TAG, "%s", s);
        dtstr_dispose(s);
        s = NULL;
    }

cleanup:
    dtstr_dispose(s);
    return dterr;
}
// --------------------------------------------------------------------------------------
// callback when a task info update is available for the rx thread
static dterr_t*
svc__task_info_callback(void* arg, dttasker_info_t* info)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(info);

    dtlog_info(TAG, "%s %s", info->name, dttasker_state_to_string(info->status));
    if (info->dterr)
    {
        dtlog_dterr(TAG, info->dterr);
        dterr_dispose(info->dterr);
        info->dterr = NULL;
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// Stop, unregister and dispose of the svc instance resources
void
svc_dispose(svc_t* self)
{
    if (self == NULL)
        return;

    dttasker_dispose(self->tx_tasker_handle);
    dttasker_dispose(self->rx_tasker_handle);

    memset(self, 0, sizeof(*self));
    dtheaper_free(self);
}