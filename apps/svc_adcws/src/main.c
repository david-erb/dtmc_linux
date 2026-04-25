#include "main.h"

#include <stdio.h>
#include <string.h>

#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtadc.h>
#include <dtmc_base/dtadc_scan.h>
#include <dtmc_base/dtadc_scanlist.h>
#include <dtmc_base/dtbufferqueue.h>
#include <dtmc_base/dtframer.h>
#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtnetportal.h>
#include <dtmc_base/dtruntime.h>
#include <dtmc_base/dttasker.h>
#include <dtmc_base/dtuart_helpers.h>

// concrete objects used by this build
#include <dtmc/dtiox_linux_tcp.h>
#include <dtmc/dtiox_linux_websocket.h>

// services used by this build
#include <dtmc_services/dtservice.h>
#include <dtmc_services/dtservice_bq2framed_iox.h>
#include <dtmc_services/dtservice_iox2bq.h>

#include "svc_adcws.h"

#define TAG "main"

typedef struct main_t
{
    main_config_t config;

    uint64_t last_sequence_number;
} main_t;

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;
    dtiox_handle rx_iox_handle = NULL;
    dtiox_handle tx_iox_handle = NULL;

    dtservice_handle bq2framed_iox_service_handle = NULL;
    dtservice_handle iox2bq_service_handle = NULL;

    dtbufferqueue_handle free_bq = NULL;
    dtbufferqueue_handle full_bq = NULL;

#define BQ_POOL_SIZE 4
#define BQ_BUFFER_SIZE 2048
    dtbuffer_t* buffers[BQ_POOL_SIZE] = { 0 };

#define ADC_CHANNEL_COUNT 4

    svc_t* svc = NULL;

    main_t _self = { 0 }, *self = &_self;

    // -------------------------------------------------------------------------
    // Parse command line
    // -------------------------------------------------------------------------
    // help path: parser already printed usage and returned NULL.
    // If user invoked only --help, just exit success before doing any setup.
    DTERR_C(main_parse_args(argc, argv, &self->config));

    if (self->config.help_requested)
        goto cleanup;

    DTERR_C(dtbufferqueue_create(&free_bq, BQ_POOL_SIZE, true));

    for (int i = 0; i < BQ_POOL_SIZE; i++)
    {
        DTERR_C(dtbuffer_create(&buffers[i], BQ_BUFFER_SIZE));
        DTERR_C(dtbufferqueue_put(free_bq, buffers[i], DTTIMEOUT_NOWAIT, NULL));
    }

    DTERR_C(dtbufferqueue_create(&full_bq, BQ_POOL_SIZE, true));

    const char* rx_flowmonitor_name = NULL;
    const char* tx_flowmonitor_name = NULL;

    // -------------------------------------------------------------------------
    // rx always from tcp
    // -------------------------------------------------------------------------
    {
        dtiox_linux_tcp_config_t c = { 0 };
        c.mode = DTIOX_LINUX_TCP_MODE_SERVER;
        c.local_bind_host = self->config.rx_host;
        c.local_bind_port = self->config.rx_port;

        dtiox_linux_tcp_t* o = NULL;
        DTERR_C(dtiox_linux_tcp_create(&o));
        rx_iox_handle = (dtiox_handle)o;
        DTERR_C(dtiox_linux_tcp_configure(o, &c));
        rx_iox_handle = (dtiox_handle)o;

        rx_flowmonitor_name = "tcp";
    }

    {
        dtservice_iox2bq_config_t c = { 0 };
        c.iox_handle = rx_iox_handle;
        c.allocated_buffer_length = BQ_BUFFER_SIZE;
        c.free_bq = free_bq;
        c.full_bq = full_bq;
        c.recovery_cooldown_ms = 1000;
        c.poll_timeout_ms = 100;
        dtservice_iox2bq_t* o = NULL;
        DTERR_C(dtservice_iox2bq_create(&o));
        iox2bq_service_handle = (dtservice_handle)o;
        DTERR_C(dtservice_iox2bq_configure(o, &c));
    }

    // -------------------------------------------------------------------------
    // tx always to callback (via framer)
    // -------------------------------------------------------------------------
    {
        dtiox_linux_websocket_config_t c = { 0 };
        c.local_bind_host = self->config.tx_host;
        c.local_bind_port = self->config.tx_port;

        dtiox_linux_websocket_t* o = NULL;
        DTERR_C(dtiox_linux_websocket_create(&o));
        tx_iox_handle = (dtiox_handle)o;
        DTERR_C(dtiox_linux_websocket_configure(o, &c));
        tx_iox_handle = (dtiox_handle)o;

        tx_flowmonitor_name = "ws";
    }

    {
        dtservice_bq2framed_iox_config_t c = { 0 };
        c.framer_topic = "adc_scan";
        c.free_bq = free_bq;
        c.full_bq = full_bq;
        c.iox_handle = tx_iox_handle;
        c.recovery_cooldown_ms = 1000;
        c.poll_timeout_ms = 100;
        dtservice_bq2framed_iox_t* o = NULL;
        DTERR_C(dtservice_bq2framed_iox_create(&o));
        bq2framed_iox_service_handle = (dtservice_handle)o;
        DTERR_C(dtservice_bq2framed_iox_configure(o, &c));
    }

    // -------------------------------------------------------------------------
    // Create and configure svc
    // -------------------------------------------------------------------------
    {
        DTERR_C(svc_create(&svc));

        svc_config_t c = { 0 };

        c.rx_service_handle = iox2bq_service_handle;
        c.tx_service_handle = bq2framed_iox_service_handle;

        c.rx_flowmonitor_name = rx_flowmonitor_name;
        c.tx_flowmonitor_name = tx_flowmonitor_name;

        c.verbose = self->config.verbose;

        DTERR_C(svc_configure(svc, &c));
    }

    // -------------------------------------------------------------------------
    // Start svc
    // -------------------------------------------------------------------------
    DTERR_C(svc_start(svc));

cleanup:

    int rc = (dterr != NULL) ? -1 : 0;

    dtlog_dterr(TAG, dterr);
    dterr_dispose(dterr);

    svc_dispose(svc);

    dtservice_dispose(bq2framed_iox_service_handle);
    dtservice_dispose(iox2bq_service_handle);

    dtiox_dispose(tx_iox_handle);
    dtiox_dispose(rx_iox_handle);

    dtbufferqueue_dispose(full_bq);

    for (int i = 0; i < BQ_POOL_SIZE; i++)
    {
        dtbuffer_dispose(buffers[i]);
    }

    dtbufferqueue_dispose(free_bq);

    return rc;
}
