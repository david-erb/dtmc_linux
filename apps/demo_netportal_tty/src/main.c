#include <dtcore/dterr.h>

#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtruntime.h>

#include <dtmc_base/dtframer.h>
#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtnetportal.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>

// we can cover these concrete objects platform agnostically
#include <dtmc_base/dtframer_simple.h>
#include <dtmc_base/dtnetportal_iox.h>

// these concrete objects are platform specific
#include <dtmc/dtiox_linux_tty.h>

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

    const char* device_path = "/dev/ttyUSB0";
    if (argc >= 2)
    {
        device_path = argv[1];
    }

    // === create the concrete IOX object we need ===
    {
        dtiox_linux_tty_t* o = NULL;
        DTERR_C(dtiox_linux_tty_create(&o));
        iox_handle = (dtiox_handle)o;
        dtiox_linux_tty_config_t c = { 0 };
        c.device_path = device_path;
        c.uart_config.baudrate = 115200;
        c.uart_config.parity = DTUART_PARITY_NONE;
        c.uart_config.data_bits = DTUART_DATA_BITS_8;
        c.uart_config.stop_bits = DTUART_STOPBITS_1;
        c.uart_config.flow = DTUART_FLOW_NONE;
        DTERR_C(dtiox_linux_tty_configure(o, &c));
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
        c.is_server = true;
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
