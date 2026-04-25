#pragma once

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
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
#include <dtmc_base/dtbufferqueue.h>
#include <dtmc_base/dtmanifold.h>
#include <dtmc_base/dtmc_base_constants.h>
#include <dtmc_base/dtnetportal.h>
#include <dtmc_base/dtsemaphore.h>

#include <dtmc/dtnetportal_coap.h>

dterr_t*
dtnetportal_coap__pubwork_debuf(dtbuffer_t* buffer, char** topic, void** data, int32_t* data_len);
dterr_t*
dtnetportal_coap__pubwork_enbuf(dtbuffer_t** buffer, const char* topic, dtbuffer_t* data);

// --------------------------------------------------------------------------------------
// private structure used internally
typedef struct dtnetportal_coap_t
{
    DTNETPORTAL_COMMON_MEMBERS;

    dtnetportal_coap_config_t config;

    dtsemaphore_handle manifold_semaphore;
    dtmanifold_t _manifold, *manifold;
    dttasker_handle ioloop_tasker_handle;
    dtbufferqueue_handle bufferqueue;

    bool _is_connected;
    bool _is_malloced;

    // pointer to the global context (we do not own it; just use it)
    coap_context_t* coap_context;

    // --- client/publisher side
    coap_uri_t* publish_uri;
    coap_address_t publish_address;
    coap_session_t* coap_session;

    // --- server/listener side (for incoming POST on topic resources)
    coap_address_t listen_address;
    coap_endpoint_t* coap_endpoint;

    coap_str_const_t* coap_resource_path;
    coap_resource_t* coap_resource;

    // Wake + queue for async sends
    int wake_pipe[2]; // [0]=read, [1]=write (non-blocking)
    bool wake_pipe_ok;

    int ioloop_should_stop;
} dtnetportal_coap_t;

// ---- ioloop methods
extern dterr_t*
dtnetportal_coap__ioloop_tasker_entrypoint(void* self_arg, dttasker_handle tasker_handle);
extern dterr_t*
dtnetportal_coap__ioloop_publish_one(dtnetportal_coap_t* self, const char* topic, void* data, int32_t data_len);
extern dterr_t*
dtnetportal_coap__ioloop_publish_all(dtnetportal_coap_t* self);
extern dterr_t*
dtnetportal_coap__ioloop_check_task(dtnetportal_handle netportal_handle);

// ---- helper methods
extern dterr_t*
dtnetportal_coap__create_sessions(dtnetportal_coap_t* self);
extern dterr_t*
dtnetportal_coap__create_endpoints(dtnetportal_coap_t* self);
extern dterr_t*
dtnetportal_coap__create_resource(dtnetportal_coap_t* self);

// ---- libcoap callbacks
extern int
dtnetportal_coap__on_event(coap_session_t* session, coap_event_t event);
extern coap_response_t
dtnetportal_coap__on_response(coap_session_t* session, const coap_pdu_t* sent, const coap_pdu_t* received, const coap_mid_t id);
extern void
dtnetportal_coap__on_post_topic(coap_resource_t* resource,
  coap_session_t* session,
  const coap_pdu_t* incoming_pdu,
  const coap_string_t* query,
  coap_pdu_t* response_pdu);
