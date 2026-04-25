/*
 * dtnetportal_mosquitto -- MQTT backend for the dtnetportal interface via libmosquitto.
 *
 * Implements the dtnetportal vtable over an MQTT broker connection managed
 * by libmosquitto. Configuration covers broker host, port, keepalive
 * interval, optional client identity and credentials, clean-session flag,
 * and mutual-TLS certificate paths. Application code publishes and
 * subscribes through the transport-agnostic dtnetportal_handle, leaving all
 * MQTT-specific concerns inside this backend.
 *
 * cdox v1.0.2
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dtcore/dterr.h>
#include <dtcore/dtobject.h>

#include <dtmc_base/dtnetportal.h>

typedef struct dtnetportal_mosquitto_config_t
{
    // Broker connection
    const char* host; // e.g. "localhost"
    int port;         // e.g. 1883 (or 8883 for TLS)
    int keepalive;    // seconds, e.g. 30

    // Client identity (all optional)
    const char* client_id; // if NULL, mosquitto picks one
    const char* username;  // optional
    const char* password;  // optional

    // Session behavior
    bool clean_start; // true => clean session on connect

    // TLS options
    bool use_tls;             // false to disable TLS
    const char* tls_ca_file;  // path to CA cert (PEM), optional if broker cert is otherwise trusted
    const char* tls_certfile; // client cert (PEM), optional
    const char* tls_keyfile;  // client key (PEM), optional
} dtnetportal_mosquitto_config_t;

// Handy defaults
#define DTNETPORTAL_MOSQUITTO_CONFIG_DEFAULTS                                                                                  \
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

typedef struct dtnetportal_mosquitto_t dtnetportal_mosquitto_t;

extern dterr_t*
dtnetportal_mosquitto_create(dtnetportal_mosquitto_t** self_ptr);

extern dterr_t*
dtnetportal_mosquitto_init(dtnetportal_mosquitto_t* self);

extern dterr_t*
dtnetportal_mosquitto_configure(dtnetportal_mosquitto_t* self, dtnetportal_mosquitto_config_t* config);

// --------------------------------------------------------------------------------------
// Interface plumbing.

DTNETPORTAL_DECLARE_API(dtnetportal_mosquitto);
DTOBJECT_DECLARE_API(dtnetportal_mosquitto);
