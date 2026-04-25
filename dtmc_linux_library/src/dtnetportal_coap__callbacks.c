// POSIX dtnetportal over MQTT using libcoap, with dtmanifold fanout.

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <pthread.h>

#include <coap3/coap.h>

#include <dtcore/dtbuffer.h>
#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>
#include <dtmc_base/dtmanifold.h>
#include <dtmc_base/dtnetportal.h>
#include <dtmc_base/dtsemaphore.h>

#include <dtmc/dtnetportal_coap.h>

#include "dtnetportal_coap__private.h"

#define TAG "dtnetportal_coap__callbacks"

// comment out the logging here
// #define dtlog_debug(TAG, ...)

// --------------------------------------------------------------------------------------
coap_response_t
dtnetportal_coap__on_response(coap_session_t* session, const coap_pdu_t* sent, const coap_pdu_t* received, const coap_mid_t id)
{
    dtnetportal_coap_t* self = (dtnetportal_coap_t*)coap_session_get_app_data(session);

    (void)sent;
    (void)id;

    if (self)
    {
        self->_is_connected = true;
    }

    size_t len = 0;
    const uint8_t* data = NULL;
    coap_pdu_code_t code = coap_pdu_get_code(received);
    coap_get_data(received, &len, &data);

    return COAP_RESPONSE_OK;
}

// --------------------------------------------------------------------------------------
int
dtnetportal_coap__on_event(coap_session_t* session, coap_event_t event)
{
    dtnetportal_coap_t* self = (dtnetportal_coap_t*)coap_session_get_app_data(session);

    if (!self)
        return 0;

    switch (event)
    {
        // “connected” / “established” signals (covers TCP and DTLS);
        // for UDP non-secure, we treat first successful response as “connected”.
#ifdef COAP_EVENT_SESSION_CONNECTED
        case COAP_EVENT_SESSION_CONNECTED:
            self->_is_connected = true;
            break;
#endif
#ifdef COAP_EVENT_DTLS_CONNECTED
        case COAP_EVENT_DTLS_CONNECTED:
            self->_is_connected = true;
            break;
#endif
#ifdef COAP_EVENT_TCP_CONNECTED
        case COAP_EVENT_TCP_CONNECTED:
            self->_is_connected = true;
            break;
#endif

            // “closed” / “error” signals -> mark disconnected so we’ll recreate
#ifdef COAP_EVENT_SESSION_CLOSED
        case COAP_EVENT_SESSION_CLOSED:
            self->_is_connected = false;
            break;
#endif
#ifdef COAP_EVENT_DTLS_CLOSED
        case COAP_EVENT_DTLS_CLOSED:
            self->_is_connected = false;
            break;
#endif
#ifdef COAP_EVENT_TCP_CLOSED
        case COAP_EVENT_TCP_CLOSED:
            self->_is_connected = false;
            break;
#endif
#ifdef COAP_EVENT_DTLS_ERROR
        case COAP_EVENT_DTLS_ERROR:
            self->_is_connected = false;
            break;
#endif
        default:
            break;
    }

    return 0;
}

// --------------------------------------------------------------------------------------

// Single POST handler for ALL dynamic "topic" resources.
// Hard-codes method to POST and treats payload as a byte array.
void
dtnetportal_coap__on_post_topic(coap_resource_t* resource,
  coap_session_t* session,
  const coap_pdu_t* incoming_pdu,
  const coap_string_t* query,
  coap_pdu_t* response_pdu)
{
    dterr_t* dterr = NULL;
    dtbuffer_t _buffer = { 0 }, *buffer = &_buffer;

    dtnetportal_coap_t* self = (dtnetportal_coap_t*)coap_get_app_data(coap_session_get_context(session));
    if (!self || !self->manifold || !resource || !incoming_pdu || !response_pdu)
    {
        // 5.00 Internal Server Error
        coap_pdu_set_code(response_pdu, COAP_RESPONSE_CODE(500));
        return;
    }

    char topic[256];
    memcpy(topic, query->s, query->length);
    topic[query->length] = '\0';

    // Extract payload from request
    size_t len = 0;
    const uint8_t* data = NULL;
    coap_get_data(incoming_pdu, &len, &data);

    dtlog_debug(
      TAG, "%s(): self %p received %d bytes on topic \"%s\" for manifold %p", __func__, self, (int)len, topic, self->manifold);

    // Wrap (no copy) like your mosquitto example
    DTERR_C(dtbuffer_wrap(buffer, (void*)data, (int32_t)len));

    // Publish to local recipients via manifold
    DTERR_C(dtmanifold_publish(self->manifold, (const char*)topic, buffer));

    // 2.04 Changed is a reasonable default for POST processing success
    coap_pdu_set_code(response_pdu, COAP_RESPONSE_CODE(204));

cleanup:
    if (dterr)
    {
        dtlog_error(TAG, "%s(): manifold publish failed: %s", __func__, dterr->message);
        coap_pdu_set_code(response_pdu, COAP_RESPONSE_CODE(500));
        dterr_dispose(dterr);
    }
}
