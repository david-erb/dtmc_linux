#include <dtcore/dterr.h>

#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtnetportal.h>
#include <dtmc_base/dtruntime.h>

// these concrete objects are platform specific
#include <dtmc/dtnetportal_mosquitto.h>

#include <dtmc_base_demos/demo_netportal.h>

#define TAG "main"

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;
    dtnetportal_handle netportal_handle = NULL;

    demo_t* demo = NULL;

    bool is_server = false;
    if (argc >= 2)
    {
        is_server = (strcmp(argv[1], "server") == 0);
    }

    // === the netportal ===
    {
        dtnetportal_mosquitto_t* o = NULL;
        DTERR_C(dtnetportal_mosquitto_create(&o));
        netportal_handle = (dtnetportal_handle)o;
        dtnetportal_mosquitto_config_t c = { 0 };

        // host: "7a7dfa1e355d4c75887d41776932fd10.s1.eu.hivemq.cloud"
        // port: 8883
        // username: "tdx.uiauto"
        // password: "tdx.uiauto1"
        c.host = "80bb9fae0b2940fa96238b13fd9c854b.s1.eu.hivemq.cloud";
        c.port = 8883;
        c.username = "litup2";
        c.password = "Litup2Pass1";
        c.use_tls = true;

        DTERR_C(dtnetportal_mosquitto_configure(o, &c));
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

    return rc;
}
