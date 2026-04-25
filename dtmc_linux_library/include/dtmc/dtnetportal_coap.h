/*
 * dtnetportal_coap -- CoAP network portal backend for the dtnetportal interface.
 *
 * Implements the dtnetportal vtable using CoAP, binding a local UDP listener
 * on a configurable host and port. Configuration includes optional upstream
 * publish target, client identity, TLS certificate paths, and an IO-loop
 * processing timeout. Subscribe callbacks deliver topic payloads to
 * application code through the common dtnetportal receive callback signature,
 * keeping CoAP transport details out of application logic.
 *
 * cdox v1.0.2
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dtcore/dterr.h>
#include <dtcore/dtobject.h>

#include <dtmc_base/dttasker.h>

#include <dtmc_base/dtnetportal.h>

#define DTNETPORTAL_COAP_FLAVOR "dtnetportal_coap"
#define DTNETPORTAL_COAP_VERSION "0.1.0"

typedef struct dtnetportal_coap_config_t
{
    const char* publish_to_host; // e.g. "localhost"
    int publish_to_port;         // e.g. 1883 (or 8883 for TLS)
    int keepalive;               // seconds, e.g. 30

    // Client identity (all optional)
    const char* client_id; // if NULL, coap picks one
    const char* username;  // optional
    const char* password;  // optional

    // Session behavior
    bool clean_start; // true => clean session on connect

    // TLS options
    bool use_tls;             // false to disable TLS
    const char* tls_ca_file;  // path to CA cert (PEM), optional if broker cert is otherwise trusted
    const char* tls_certfile; // client cert (PEM), optional
    const char* tls_keyfile;  // client key (PEM), optional
    // --- NEW: local listener configuration
    const char* listen_host;   // e.g. "::" or "127.0.0.1"
    uint16_t listen_port;      // default 5683
    int io_process_timeout_ms; // default 100

    // optional; for status changes on the ioloop tasker
    dttasker_info_callback_t tasker_info_callback;
    void* tasker_info_callback_context;

} dtnetportal_coap_config_t;

// Handy defaults
#define DTNETPORTAL_COAP_CONFIG_DEFAULTS                                                                                       \
    { .host = "localhost",                                                                                                     \
        .port = 1883,                                                                                                          \
        .keepalive = 30,                                                                                                       \
        .client_id = NULL,                                                                                                     \
        .username = NULL,                                                                                                      \
        .password = NULL,                                                                                                      \
        .clean_start = true,                                                                                                   \
        .use_tls = false,                                                                                                      \
        .tls_ca_file = NULL,                                                                                                   \
        .tls_certfile = NULL,                                                                                                  \
        .tls_keyfile = NULL }

typedef struct dtnetportal_coap_t dtnetportal_coap_t;

extern dterr_t*
dtnetportal_coap_create(dtnetportal_coap_t** self_ptr);

extern dterr_t*
dtnetportal_coap_init(dtnetportal_coap_t* self);

extern dterr_t*
dtnetportal_coap_configure(dtnetportal_coap_t* self, dtnetportal_coap_config_t* config);

// --------------------------------------------------------------------------------------
// Interface plumbing.

DTNETPORTAL_DECLARE_API(dtnetportal_coap);
DTOBJECT_DECLARE_API(dtnetportal_coap);
