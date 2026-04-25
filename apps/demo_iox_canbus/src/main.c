#include <dtcore/dterr.h>

#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtruntime.h>

#include <dtmc_base/dtframer.h>
#include <dtmc_base/dtiox.h>

// these concrete objects are platform specific
#include <dtmc/dtiox_linux_canbus.h>

#include <dtmc_base_demos/demo_iox.h>

#define TAG "main"

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;
    dtiox_handle iox_handle = NULL;

    const char* ifname = "can0";
    if (argc >= 2)
    {
        ifname = argv[1];
    }

    demo_t* demo = NULL;

    // === create the concrete IOX object we need ===
    {
        dtiox_linux_canbus_t* o = NULL;
        DTERR_C(dtiox_linux_canbus_create(&o));
        iox_handle = (dtiox_handle)o;
        dtiox_linux_canbus_config_t c = { 0 };
        c.interface_name = ifname;
        c.txid = 0x100;
        DTERR_C(dtiox_linux_canbus_configure(o, &c));
    }

    // === create and configure the demo instance ===
    {
        DTERR_C(demo_create(&demo));
        demo_config_t c = { 0 };
        c.iox_handle = iox_handle;
        c.node_name = ifname;
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
