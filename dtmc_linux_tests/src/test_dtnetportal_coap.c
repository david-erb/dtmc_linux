#include <stdio.h>
#include <string.h>

#include <dtcore/dtbuffer.h>
#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>
#include <dtmc_base/dtbufferqueue.h>
#include <dtmc_base/dttasker.h>

#include <dtcore/dtunittest.h>
#include <dtmc_base/dtnetportal.h>

#include <dtmc/dtnetportal_coap.h>

#define TAG "test_dtnetportal_coap"

typedef struct simple_receiver_t
{
    dtbufferqueue_handle bufferqueue_handle;
} simple_receiver_t;

static dterr_t* test_dtnetportal_coap_tasker_dterrs = NULL;

// --------------------------------------------------------------------------------------------
static dterr_t*
test_dtnetportal_coap_topic1_receive_callback(void* receiver_self, const char* topic, dtbuffer_t* buffer)
{
    dterr_t* dterr = NULL;
    simple_receiver_t* receiver = (simple_receiver_t*)receiver_self;

    DTERR_C(dtbufferqueue_put(receiver->bufferqueue_handle, buffer, DTTIMEOUT_FOREVER, NULL));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
test_dtnetportal_coap_topic1_tasker_info_callback(void* caller_object, dttasker_info_t* tasker_info)
{
    dterr_t* dterr = NULL;

    if (tasker_info->dterr != NULL)
    {
        dterr_t* header_dterr =
          dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "<<<<<<<< errors occurred in tasker \"%s\" >>>>>>>", tasker_info->name);
        dterr_append(tasker_info->dterr, header_dterr);
        header_dterr = tasker_info->dterr;
        if (test_dtnetportal_coap_tasker_dterrs == NULL)
        {
            test_dtnetportal_coap_tasker_dterrs = header_dterr;
        }
        else
        {
            dterr_append(test_dtnetportal_coap_tasker_dterrs, header_dterr);
        }
    }

    dtlog_info(TAG,
      "%s(): tasker \"%s\" status changed to %d (%s) with error code %d",
      __func__,
      tasker_info->name,
      tasker_info->status,
      dttasker_state_to_string(tasker_info->status),
      tasker_info->dterr ? tasker_info->dterr->error_code : 0);

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
test_dtnetportal_coap_subscribe_twice(void)
{
    dterr_t* dterr = NULL;
    dtbufferqueue_handle bufferqueue1_handle = NULL;
    dtbufferqueue_handle bufferqueue2_handle = NULL;
    dtnetportal_handle netportal_handle = NULL;
    dtbuffer_t* send_buffer = NULL;
    dtbuffer_t* receiver_buffer = NULL;

    // make a bufferqueue for the first receiver
    DTERR_C(dtbufferqueue_create(&bufferqueue1_handle, 10, false));

    // make a bufferqueue for the second receiver
    DTERR_C(dtbufferqueue_create(&bufferqueue2_handle, 10, false));

    // make a netportal for sending/receiving
    {
        dtnetportal_coap_t* object = NULL;
        DTERR_C(dtnetportal_coap_create(&object));
        netportal_handle = (dtnetportal_handle)object;
        dtnetportal_coap_config_t config = { 0 };
        config.publish_to_host = "127.0.0.1";
        config.publish_to_port = 5683;
        config.tasker_info_callback = test_dtnetportal_coap_topic1_tasker_info_callback;
        config.tasker_info_callback_context = NULL;
        DTERR_C(dtnetportal_coap_configure(object, &config));
    }

    // make receivers objects (simple objects in this test)
    simple_receiver_t receiver1 = { .bufferqueue_handle = bufferqueue1_handle };
    simple_receiver_t receiver2 = { .bufferqueue_handle = bufferqueue2_handle };

    // Connect
    DTERR_C(dtnetportal_activate(netportal_handle));

    // Subscribe
    const char* topic1 = "test/topic1";
    const char* data1 = "Hello, Topic 1!";

    DTERR_C(dtnetportal_subscribe(netportal_handle, topic1, &receiver1, test_dtnetportal_coap_topic1_receive_callback));
    DTERR_C(dtnetportal_subscribe(netportal_handle, topic1, &receiver2, test_dtnetportal_coap_topic1_receive_callback));

    // Send
    DTERR_C(dtbuffer_create(&send_buffer, 32));
    strcpy(send_buffer->payload, data1);
    DTERR_C(dtnetportal_publish(netportal_handle, topic1, send_buffer));
    dtbuffer_dispose(send_buffer);

    // wait for first receiver to get its data
    DTERR_C(dtbufferqueue_get(bufferqueue1_handle, &receiver_buffer, 3000, NULL));
    DTUNITTEST_ASSERT_NOT_NULL(receiver_buffer);
    DTUNITTEST_ASSERT_EQUAL_STRING((char*)receiver_buffer->payload, data1);
    dtbuffer_dispose(receiver_buffer);

    // wait for second receiver to get its data
    DTERR_C(dtbufferqueue_get(bufferqueue2_handle, &receiver_buffer, 3000, NULL));
    DTUNITTEST_ASSERT_NOT_NULL(receiver_buffer);
    DTUNITTEST_ASSERT_EQUAL_STRING((char*)receiver_buffer->payload, data1);
    dtbuffer_dispose(receiver_buffer);

cleanup:

    if (test_dtnetportal_coap_tasker_dterrs != NULL)
    {
        if (dterr == NULL)
        {
            dterr = test_dtnetportal_coap_tasker_dterrs;
        }
        else
        {
            dterr_append(test_dtnetportal_coap_tasker_dterrs, dterr);
            dterr = test_dtnetportal_coap_tasker_dterrs;
        }
    }

    dtbuffer_dispose(receiver_buffer);
    dtbuffer_dispose(send_buffer);

    dtnetportal_dispose(netportal_handle);
    dtbufferqueue_dispose(bufferqueue2_handle);
    dtbufferqueue_dispose(bufferqueue1_handle);

    return dterr;
}

// --------------------------------------------------------------------------------------------
dterr_t*
test_dtnetportal_coap(void)
{
    dterr_t* dterr = NULL;
    DTERR_C(test_dtnetportal_coap_subscribe_twice());

cleanup:
    return dterr;
}
