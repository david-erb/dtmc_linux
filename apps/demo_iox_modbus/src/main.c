#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <dtcore/dterr.h>
#include <dtcore/dtledger.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtruntime.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtnetportal.h>

// these concrete objects are platform specific
#include <dtmc/dtiox_linux_modbus_rtu_master.h>
#include <dtmc/dtiox_linux_modbus_tcp_master.h>
#include <dtmc/dtiox_linux_modbus_tcp_slave.h>

#include <dtmc_base_demos/demo_iox.h>

#define TAG "main"

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;
    demo_t* demo = NULL;
    dtiox_handle iox_handle = NULL;
    const char* node_name;
    bool inhibit_read = false;
    bool inhibit_write = false;
    bool should_read_before_first_write = false;

    const char* device = "/dev/serial0";
    if (argc >= 2)
    {
        device = argv[1];
    }

    // === create the concrete IOX object we need ===
    // if this is a device path, create a Modbus RTU Master over Linux UART
    if (strncmp(device, "/dev/", 5) == 0)
    {
        dtiox_linux_modbus_rtu_master_t* o = NULL;
        DTERR_C(dtiox_linux_modbus_rtu_master_create(&o));
        iox_handle = (dtiox_handle)o;
        dtiox_linux_modbus_rtu_master_config_t c = { 0 };
        c.device = device;
        c.uart_config = dtuart_helper_default_config;
        c.slave_id = 13;
        DTERR_C(dtiox_linux_modbus_rtu_master_configure(o, &c));
        node_name = "dtiox_linux/modbus_rtu_master";
    }

    // else if argument is exactly "slave", create a Modbus TCP Slave
    else if (strcmp(device, "slave") == 0)
    {
        dtiox_linux_modbus_tcp_slave_t* o = NULL;
        DTERR_C(dtiox_linux_modbus_tcp_slave_create(&o));
        iox_handle = (dtiox_handle)o;
        dtiox_linux_modbus_tcp_slave_config_t c = { 0 };
        c.port = 1502; // non-standard port for testing
        DTERR_C(dtiox_linux_modbus_tcp_slave_configure(o, &c));
        node_name = "dtiox_linux/modbus_tcp_slave";
        should_read_before_first_write = true;
    }

    // otherwise, assume it's an IP address and create a Modbus TCP Master
    else
    {
        dtiox_linux_modbus_tcp_master_t* o = NULL;
        DTERR_C(dtiox_linux_modbus_tcp_master_create(&o));
        iox_handle = (dtiox_handle)o;
        dtiox_linux_modbus_tcp_master_config_t c = { 0 };
        c.ip = device;
        c.port = 1502; // non-standard port for testing
        DTERR_C(dtiox_linux_modbus_tcp_master_configure(o, &c));
        node_name = "dtiox_linux/modbus_tcp_master";
    }

    // === create and configure the demo instance ===
    {
        DTERR_C(demo_create(&demo));
        demo_config_t c = { 0 };
        c.iox_handle = iox_handle;
        c.node_name = node_name;
        c.inhibit_read = inhibit_read;
        c.inhibit_write = inhibit_write;
        c.should_read_before_first_write = should_read_before_first_write;
        c.max_attach_retries = 0;
        c.max_cycles_to_run = 0;
        DTERR_C(demo_configure(demo, &c));
    }

    // === start the demo ===
    DTERR_C(demo_start(demo));

cleanup:
    int rc = (dterr != NULL) ? -1 : 0;

    // log and dispose error chain if any
    dtlog_dterr(TAG, dterr);
    dterr_dispose(dterr);

    // dispose the demo instance
    demo_dispose(demo);

    // dispose the objects
    dtiox_dispose(iox_handle);

    // print ledger
    {
        char buffer[512];
        dtledger_to_string(dterr_ledger, buffer, sizeof(buffer));
        dtlog_info(TAG, "%s", buffer);
    }

    return rc;
}
