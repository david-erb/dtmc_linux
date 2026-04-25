#include <dtcore/dterr.h>

#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtruntime.h>

#include <dtmc_base/dtframer.h>
#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtnetportal.h>

// we can cover these concrete objects platform agnostically
#include <dtmc_base/dtframer_simple.h>
#include <dtmc_base/dtnetportal_iox.h>

// these concrete objects are platform specific
#include <dtmc/dtiox_linux_modbus_rtu_master.h>
#include <dtmc/dtiox_linux_modbus_tcp_master.h>
#include <dtmc/dtiox_linux_modbus_tcp_slave.h>

#include <dtmc_base_demos/demo_netportal.h>

#define TAG "main"

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;
    dtiox_handle iox_handle = NULL;
    dtframer_handle framer_handle = NULL;
    dtnetportal_handle netportal_handle = NULL;

    demo_t* demo = NULL;

    const char* device = "/dev/serial0";
    if (argc >= 2)
    {
        device = argv[1];
    }

    bool is_server = false;

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

        is_server = false; // modbus RTU always acts as master which is the client
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

        is_server = true; // modbus TCP slave acts as server
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

        is_server = false; // modbus TCP master acts as client
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

    // === create and configure the demo instance ===
    {
        DTERR_C(demo_create(&demo));
        demo_config_t c = { 0 };
        c.netportal_handle = netportal_handle;
        c.is_server = is_server;
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

    dtnetportal_dispose(netportal_handle);

    dtframer_dispose(framer_handle);

    dtiox_dispose(iox_handle);

    return rc;
}
