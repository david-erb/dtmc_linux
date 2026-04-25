#include <stdio.h>
#include <string.h>

#include <dtcore/dtbuffer.h>
#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>
#include <dtmc_base/dtbufferqueue.h>

#include <dtcore/dtunittest.h>
#include <dtmc_base/dtnetportal.h>

#include <dtmc/dtnetportal_mosquitto.h>

#define TAG "test_dtnetportal_mosquitto"

typedef struct simple_receiver_t
{
    dtbufferqueue_handle bufferqueue_handle;
} simple_receiver_t;

// --------------------------------------------------------------------------------------------
static dterr_t*
test_dtnetportal_mosquitto_topic1_receive_callback(void* receiver_self, const char* topic, dtbuffer_t* buffer)
{
    dterr_t* dterr = NULL;
    simple_receiver_t* receiver = (simple_receiver_t*)receiver_self;

    DTERR_C(dtbufferqueue_put(receiver->bufferqueue_handle, buffer, DTTIMEOUT_FOREVER, NULL));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
static dterr_t*
test_dtnetportal_mosquitto_subscribe_twice(void)
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
        dtnetportal_mosquitto_t* netportal_object = NULL;
        DTERR_C(dtnetportal_mosquitto_create(&netportal_object));
        netportal_handle = (dtnetportal_handle)netportal_object;
        dtnetportal_mosquitto_config_t config = { 0 };
        config.host = "192.168.0.157";
        // config.host = "10.246.69.2";
        DTERR_C(dtnetportal_mosquitto_configure(netportal_object, &config));
    }

    // make receivers objects (simple objects in this test)
    simple_receiver_t receiver1 = { .bufferqueue_handle = bufferqueue1_handle };
    simple_receiver_t receiver2 = { .bufferqueue_handle = bufferqueue2_handle };

    // Connect
    DTERR_C(dtnetportal_activate(netportal_handle));

    // Subscribe
    const char* topic1 = "test/topic1";
    const char* data1 = "Hello, Topic 1!";

    DTERR_C(dtnetportal_subscribe(netportal_handle, topic1, &receiver1, test_dtnetportal_mosquitto_topic1_receive_callback));
    DTERR_C(dtnetportal_subscribe(netportal_handle, topic1, &receiver2, test_dtnetportal_mosquitto_topic1_receive_callback));

    // Send
    DTERR_C(dtbuffer_create(&send_buffer, 128));
    strcpy(send_buffer->payload, data1);
    DTERR_C(dtnetportal_publish(netportal_handle, topic1, send_buffer));
    dtbuffer_dispose(send_buffer);

    // wait for first receiver to get its data
    DTERR_C(dtbufferqueue_get(bufferqueue1_handle, &receiver_buffer, DTTIMEOUT_FOREVER, NULL));
    DTUNITTEST_ASSERT_NOT_NULL(receiver_buffer);
    DTUNITTEST_ASSERT_EQUAL_STRING((char*)receiver_buffer->payload, data1);
    dtbuffer_dispose(receiver_buffer);

    // wait for second receiver to get its data
    DTERR_C(dtbufferqueue_get(bufferqueue2_handle, &receiver_buffer, DTTIMEOUT_FOREVER, NULL));
    DTUNITTEST_ASSERT_NOT_NULL(receiver_buffer);
    DTUNITTEST_ASSERT_EQUAL_STRING((char*)receiver_buffer->payload, data1);
    dtbuffer_dispose(receiver_buffer);

cleanup:
    dtbuffer_dispose(receiver_buffer);
    dtbuffer_dispose(send_buffer);

    dtnetportal_dispose(netportal_handle);
    dtbufferqueue_dispose(bufferqueue2_handle);
    dtbufferqueue_dispose(bufferqueue1_handle);

    return dterr;
}

// --------------------------------------------------------------------------------------------
dterr_t*
test_dtnetportal_mosquitto(void)
{
    dterr_t* dterr = NULL;
    DTERR_C(test_dtnetportal_mosquitto_subscribe_twice());

cleanup:
    return dterr;
}
