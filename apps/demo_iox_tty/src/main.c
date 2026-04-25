#include <dtcore/dterr.h>

#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtruntime.h>

#include <dtmc_base/dtframer.h>
#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtnetportal.h>
#include <dtmc_base/dtuart_helpers.h>

// these concrete objects are platform specific
#include <dtmc/dtiox_linux_tty.h>

#include <dtmc_base_demos/demo_iox.h>

#define TAG "main"

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;
    dtiox_handle iox_handle = NULL;

    const char* device_path = "/dev/ttyUSB0";
    if (argc >= 2)
    {
        device_path = argv[1];
    }

    char node_name[64] = { 0 };
    snprintf(node_name, sizeof(node_name), "dtmc_linux:%s", device_path);

    demo_t* demo = NULL;

    // === create the concrete IOX object we need ===
    {
        dtiox_linux_tty_t* o = NULL;
        DTERR_C(dtiox_linux_tty_create(&o));
        iox_handle = (dtiox_handle)o;
        dtiox_linux_tty_config_t c = { 0 };
        c.device_path = device_path;
        c.uart_config = dtuart_helper_default_config;
        DTERR_C(dtiox_linux_tty_configure(o, &c));
    }

    // === create and configure the demo instance ===
    {
        DTERR_C(demo_create(&demo));
        demo_config_t c = { 0 };
        c.iox_handle = iox_handle;
        c.node_name = node_name;
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

    return rc;
}
