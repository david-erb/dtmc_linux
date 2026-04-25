#include <dtcore/dterr.h>

#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtruntime.h>

#include <dtmc_base/dtframer.h>
#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtnetportal.h>
#include <dtmc_base/dttasker.h>

// we can cover these concrete objects platform agnostically
#include <dtmc_base/dtframer_simple.h>
#include <dtmc_base/dtnetportal_iox.h>

// these concrete objects are platform specific
#include <dtmc/dtiox_linux_modbus_rtu_master.h>
#include <dtmc/dtiox_linux_modbus_tcp_master.h>
#include <dtmc/dtiox_linux_modbus_tcp_slave.h>

#include <dtmc_base_benchmarks/benchmark_netportal_simplex.h>

#include "main.h"

#define TAG "main"

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;
    dtiox_handle iox_handle = NULL;
    dtframer_handle framer_handle = NULL;
    dttasker_handle rx_tasker_handle = NULL;
    dtnetportal_handle netportal_handle = NULL;
    bool is_server = false;

    benchmark_t* benchmark = NULL;

    main_config_t main_config = { 0 };
    DTERR_C(main_parse_args(argc, argv, &main_config));

    // === create the concrete IOX object we need ===
    // if this is a device path, create a Modbus RTU Master over Linux UART aka "client"
    if (main_config.mode == MAIN_MODE_RTU_MASTER)
    {
        dtiox_linux_modbus_rtu_master_t* o = NULL;
        DTERR_C(dtiox_linux_modbus_rtu_master_create(&o));
        iox_handle = (dtiox_handle)o;
        dtiox_linux_modbus_rtu_master_config_t c = { 0 };
        c.device = main_config.device;
        c.uart_config = dtuart_helper_default_config;
        c.slave_id = main_config.slave_id;
        DTERR_C(dtiox_linux_modbus_rtu_master_configure(o, &c));
    }

    // else if argument is exactly "server", create a Modbus TCP Slave aka "server"
    else if (main_config.mode == MAIN_MODE_TCP_SLAVE)
    {
        dtiox_linux_modbus_tcp_slave_t* o = NULL;
        DTERR_C(dtiox_linux_modbus_tcp_slave_create(&o));
        iox_handle = (dtiox_handle)o;
        dtiox_linux_modbus_tcp_slave_config_t c = { 0 };
        c.port = main_config.port; // use the port from CLI
        DTERR_C(dtiox_linux_modbus_tcp_slave_configure(o, &c));
        is_server = true;
    }

    // otherwise, assume it's an IP address and create a Modbus TCP Master aka "client"
    else if (main_config.mode == MAIN_MODE_TCP_MASTER)
    {
        dtiox_linux_modbus_tcp_master_t* o = NULL;
        DTERR_C(dtiox_linux_modbus_tcp_master_create(&o));
        iox_handle = (dtiox_handle)o;
        dtiox_linux_modbus_tcp_master_config_t c = { 0 };
        c.ip = main_config.ip;
        c.port = main_config.port;
        DTERR_C(dtiox_linux_modbus_tcp_master_configure(o, &c));
    }
    else
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "Invalid mode: specify one of --rtu, --tcp-master, --tcp-slave");
        goto cleanup;
    }

    // === the framer ===
    {
        dtframer_simple_t* o = NULL;
        DTERR_C(dtframer_simple_create(&o));
        framer_handle = (dtframer_handle)o;
        dtframer_simple_config_t c = { 0 };
        DTERR_C(dtframer_simple_configure(o, &c));
    }

    // === the netportal ===
    {
        dtnetportal_iox_t* o = NULL;
        DTERR_C(dtnetportal_iox_create(&o));
        netportal_handle = (dtnetportal_handle)o;
        dtnetportal_iox_config_t c = { 0 };
        c.iox_handle = iox_handle;
        c.framer_handle = framer_handle;
        DTERR_C(dtnetportal_iox_configure(o, &c));
    }

    // === create and configure the benchmark instance ===
    {
        DTERR_C(benchmark_create(&benchmark));
        benchmark_config_t c = { 0 };
        c.netportal_handle = netportal_handle;
        c.message_count = main_config.message_count;
        c.payload_size = main_config.payload_size;
        c.is_server = is_server;
        DTERR_C(benchmark_configure(benchmark, &c));
    }

    // === start the benchmark ===
    DTERR_C(benchmark_start(benchmark));

cleanup:
    int rc = (dterr != NULL) ? -1 : 0;

    // log and dispose error chain if any
    dtlog_dterr(TAG, dterr);
    dterr_dispose(dterr);

    // dispose the benchmark instance
    benchmark_dispose(benchmark);

    // dispose the objects

    dtnetportal_dispose(netportal_handle);

    dttasker_dispose(rx_tasker_handle);

    dtframer_dispose(framer_handle);

    dtiox_dispose(iox_handle);

    return rc;
}
