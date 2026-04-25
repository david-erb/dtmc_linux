#include <stdlib.h>
#include <string.h>

#include <dtcore/dtbuffer.h>
#include <dtcore/dterr.h>

#include "dtnetportal_coap__private.h"

#define TAG "dtnetportal_coap__pubwork"

// comment out the logging here
#define dtlog_debug(TAG, ...)

// -------------------------------------------------------------------------------
dterr_t*
dtnetportal_coap__pubwork_enbuf(dtbuffer_t** buffer, const char* topic, dtbuffer_t* data)
{
    dterr_t* dterr = NULL;

    int topic_len = strlen(topic) + 1;
    int data_len = data->length;
    DTERR_C(dtbuffer_create(buffer, topic_len + data_len));
    memcpy((*buffer)->payload, topic, topic_len);
    memcpy((char*)((*buffer)->payload) + topic_len, data->payload, data_len);

    dtlog_debug(TAG,
      "%s(): encoded topic \"%s\" (%d bytes) and data (%d bytes) into buffer %p length %d",
      __func__,
      topic,
      topic_len,
      data_len,
      *buffer,
      (*buffer)->length);

cleanup:
    if (dterr)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "unable to encode topic \"%s\"", topic);
    return dterr;
}

// -------------------------------------------------------------------------------
dterr_t*
dtnetportal_coap__pubwork_debuf(dtbuffer_t* buffer, char** topic, void** data, int32_t* data_len)
{
    dterr_t* dterr = NULL;
    if (buffer == NULL || buffer->payload == NULL || buffer->length < 2 || topic == NULL || data == NULL || data_len == NULL)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "bad argument");
        goto cleanup;
    }

    *topic = (char*)buffer->payload;
    *data = (void*)(*topic + strlen(*topic) + 1);
    *data_len = buffer->length - (strlen(*topic) + 1);

    dtlog_debug(TAG,
      "%s(): decoded topic \"%s\" (%d bytes) and data (%d bytes) from buffer %p length %d",
      __func__,
      *topic,
      (int)(strlen(*topic) + 1),
      *data_len,
      buffer,
      buffer->length);

cleanup:
    if (dterr)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "unable to decode buffer");
    return dterr;
}
