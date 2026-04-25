#include <dtcore/dterr.h>

#include <dtcore/dtkvp.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtnvblob.h>

// these concrete objects are platform specific
#include <dtmc/dtnvblob_linux_file.h>

#include <dtmc_base_demos/demo_write_kvp.h>

#define TAG "main"

// --------------------------------------------------------------------------------------
int
main(int argc, char* argv[])
{
    dterr_t* dterr = NULL;
    dtkvp_list_t _demo_kvp_list = { 0 }, *kvp_list = &_demo_kvp_list;
    dtnvblob_handle nvblob_handle = NULL;
    const char* filename;
    demo_t* demo = NULL;

    // the kvp list we want to write
    {
        DTERR_C(dtkvp_list_init(kvp_list));

        DTERR_C(dtkvp_list_set(kvp_list, "signature", "dtradioconfig_t mark 1"));
        DTERR_C(dtkvp_list_set(kvp_list, "self_node_name", "RadiconfigNode"));
        DTERR_C(dtkvp_list_set(kvp_list, "wifi_ssid", "MyWiFiSSID"));
        DTERR_C(dtkvp_list_set(kvp_list, "wifi_password", "MyWiFiPassword"));
        DTERR_C(dtkvp_list_set(kvp_list, "mqtt_host", "mqtt://broker.example.com"));
        DTERR_C(dtkvp_list_set(kvp_list, "mqtt_port", "1883"));
        DTERR_C(dtkvp_list_set(kvp_list, "mqtt_wsport", "9001"));
        DTERR_C(dtkvp_list_set(kvp_list, "mqtt_user", "mqttuser"));
        DTERR_C(dtkvp_list_set(kvp_list, "mqtt_password", "mqttpassword"));
    }

    // the nvblob destination
    {
        filename = "kvp.nvblob";
        if (argc >= 2)
        {
            filename = argv[1];
        }

        dtnvblob_linux_file_t* o;
        DTERR_C(dtnvblob_linux_file_create(&o));
        nvblob_handle = (dtnvblob_handle)o;
        dtnvblob_linux_file_config_t c = { 0 };
        c.filename = filename;
        DTERR_C(dtnvblob_linux_file_configure(o, &c));
    }

    // the demo instance which will do the writing
    {
        DTERR_C(dtmc_base_demo_write_kvp_create(&demo));
        dtmc_base_demo_write_kvp_config_t config = { 0 };
        // give it the kvp list to write
        config.kvp_list = kvp_list;
        // give it the nvblob handle
        config.nvblob_handle = nvblob_handle;
        DTERR_C(dtmc_base_demo_write_kvp_configure(demo, &config));
    }

    // === start the demo ===
    DTERR_C(demo_start(demo));

    dtlog_info(TAG, "write to nvblob \"%s\" completed successfully", filename);

cleanup:
    int rc = (dterr != NULL) ? -1 : 0;

    // log and dispose error chain if any
    dtlog_dterr(TAG, dterr);
    dterr_dispose(dterr);

    // dispose the demo instance
    demo_dispose(demo);

    // dispose the objects
    dtnvblob_dispose(nvblob_handle);
    dtkvp_list_dispose(kvp_list);

    return rc;
}
