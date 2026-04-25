#include "main.h"

#include <stdio.h>
#include <string.h>

#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtframer.h>
#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtnetportal.h>
#include <dtmc_base/dtruntime.h>

// concrete object used by this build
#include <dtmc/dtiox_linux_tcp.h>

#include <dtmc_base_demos/demo_iox.h>

#define TAG "main"

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;
    dtiox_handle iox_handle = NULL;
    demo_t* demo = NULL;

    main_config_t main_cfg;
    dtiox_linux_tcp_config_t tcp_cfg;

    char node_name[128] = { 0 };

    // -------------------------------------------------------------------------
    // Parse command line
    // -------------------------------------------------------------------------
    // help path: parser already printed usage and returned NULL.
    // If user invoked only --help, just exit success before doing any setup.
    DTERR_C(main_parse_args(argc, argv, &main_cfg));

    if (main_cfg.help_requested)
        goto cleanup;

    if (main_cfg.mode != MAIN_MODE_TCP_MASTER && main_cfg.mode != MAIN_MODE_TCP_SLAVE)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "Expected --tcp-master or --tcp-slave");
        goto cleanup;
    }

    // -------------------------------------------------------------------------
    // Build node name from parsed config
    // -------------------------------------------------------------------------
    if (main_cfg.mode == MAIN_MODE_TCP_MASTER)
    {
        snprintf(
          node_name, sizeof(node_name), "dtmc_linux:tcp-client:%s:%d", main_cfg.host ? main_cfg.host : "?", main_cfg.port);
    }
    else
    {
        snprintf(node_name,
          sizeof(node_name),
          "dtmc_linux:tcp-server:%s:%d",
          main_cfg.host ? main_cfg.host : "0.0.0.0",
          main_cfg.port);
    }
    node_name[sizeof(node_name) - 1] = '\0';

    // -------------------------------------------------------------------------
    // Create and configure IOX
    // -------------------------------------------------------------------------
    {
        dtiox_linux_tcp_t* o = NULL;

        DTERR_C(dtiox_linux_tcp_create(&o));
        iox_handle = (dtiox_handle)o;

        DTERR_C(main_build_tcp_config(&main_cfg, &tcp_cfg));
        DTERR_C(dtiox_linux_tcp_configure(o, &tcp_cfg));
    }

    // -------------------------------------------------------------------------
    // Create and configure demo
    // -------------------------------------------------------------------------
    {
        DTERR_C(demo_create(&demo));

        demo_config_t c = { 0 };
        c.iox_handle = iox_handle;
        c.node_name = node_name;

        DTERR_C(demo_configure(demo, &c));
    }

    // -------------------------------------------------------------------------
    // Start demo
    // -------------------------------------------------------------------------
    DTERR_C(demo_start(demo));

cleanup:
{
    int rc = (dterr != NULL) ? -1 : 0;

    dtlog_dterr(TAG, dterr);
    dterr_dispose(dterr);

    demo_dispose(demo);
    dtiox_dispose(iox_handle);

    return rc;
}
}