#include <dtcore/dterr.h>

#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtnetportal.h>
#include <dtmc_base/dtruntime.h>
#include <dtmc_base/dtsemaphore.h>

// these concrete objects are platform specific
#include <dtmc/dtnetportal_coap.h>
#include <dtmc/dtsemaphore_linux.h>

#include <dtmc_base_demos/demo_netportal.h>

#define TAG "main"

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;
    dtnetportal_handle netportal_handle = NULL;
    dtsemaphore_handle pong_semaphore_handle = NULL;

    demo_t* demo = NULL;

    const char* host = "192.168.0.157";
    if (argc >= 2)
    {
        host = argv[1];
    }

    // === the netportal ===
    {
        dtnetportal_coap_t* o = NULL;
        DTERR_C(dtnetportal_coap_create(&o));
        netportal_handle = (dtnetportal_handle)o;
        dtnetportal_coap_config_t c = { 0 };
        c.publish_to_host = "127.0.0.1";
        c.publish_to_port = 5683;
        c.tasker_info_callback = test_dtnetportal_coap_topic1_tasker_info_callback;
        c.tasker_info_callback_context = NULL;
        DTERR_C(dtnetportal_coap_configure(o, &c));
    }

    // === create and configure the demo instance ===
    {
        DTERR_C(demo_create(&demo));
        demo_config_t c = { 0 };
        c.netportal_handle = netportal_handle;
        c.pong_semaphore_handle = pong_semaphore_handle;
        DTERR_C(demo_configure(demo, &c));
    }

    // === the semaphore for pingpong in the demo to use ===
    {
        dtsemaphore_linux_t* o = NULL;
        DTERR_C(dtsemaphore_linux_create(&o));
        pong_semaphore_handle = (dtsemaphore_handle)o;
        dtsemaphore_linux_config_t c = { 0 };
        c.initial_count = 0;
        DTERR_C(dtsemaphore_linux_configure(o, &c));
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
