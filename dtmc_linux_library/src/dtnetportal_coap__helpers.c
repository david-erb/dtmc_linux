// POSIX dtnetportal over MQTT using libcoap, with dtmanifold fanout.

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
#include <dtmc_base/dtmanifold.h>
#include <dtmc_base/dtmc_base_constants.h>
#include <dtmc_base/dtnetportal.h>

#include <dtmc/dtnetportal_coap.h>

#include "dtnetportal_coap__private.h"

#define TAG "dtnetportal_coap__helpers"

// comment out the logging here
// #define dtlog_debug(TAG, ...)

// --------------------------------------------------------------------------------------
// Always bind on any interface, UDP only (no TCP, no DTLS/TLS, no WebSockets).
dterr_t*
dtnetportal_coap__create_sessions(dtnetportal_coap_t* self)
{
    dterr_t* dterr = NULL;
    unsigned created = 0;

    if (!self || !self->coap_context)
    {
        return dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "invalid args");
    }

    // ---- set up a session (one for now)
    char url[256];
    snprintf(url,
      sizeof(url),
      "coap%s://[%s]:%d",
      self->config.use_tls ? "s" : "",
      self->config.publish_to_host,
      self->config.publish_to_port);
    self->publish_uri = coap_new_uri((const uint8_t*)url, strlen(url));
    if (!self->publish_uri)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "invalid publish URI: \"%s\"", url);
        goto cleanup;
    }

    coap_address_init(&self->publish_address);
    if (strstr(self->config.publish_to_host, ":") == NULL)
    {
        self->publish_address.addr.sa.sa_family = AF_INET;

        struct sockaddr_in* sin = &self->publish_address.addr.sin;
        memset(sin, 0, sizeof(*sin));
        sin->sin_family = AF_INET;
        sin->sin_port = htons(self->config.publish_to_port);

        if (inet_pton(AF_INET, self->config.publish_to_host, &sin->sin_addr) != 1)
        {
            dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "invalid IPv4 address: \"%s\"", self->config.publish_to_host);
            goto cleanup;
        }
    }
    else
    {
        self->publish_address.addr.sa.sa_family = AF_INET6;

        struct sockaddr_in6* sin6 = &self->publish_address.addr.sin6;
        memset(sin6, 0, sizeof(*sin6));
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(self->config.publish_to_port);

        if (inet_pton(AF_INET6, self->config.publish_to_host, &sin6->sin6_addr) != 1)
        {
            dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "invalid IPv6 address: \"%s\"", self->config.publish_to_host);
            goto cleanup;
        }
    }

    self->coap_session = coap_new_client_session(self->coap_context, NULL, &self->publish_address, COAP_PROTO_UDP);

    char addrbuf[128];
    coap_print_addr(&self->publish_address, addrbuf, sizeof addrbuf);
    if (!self->coap_session)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "CoAP UDP session creation failed on %s", addrbuf);
        goto cleanup;
    }

    dtlog_debug(TAG, "CoAP publisher session created to %s (%s)", url, addrbuf);
    coap_session_set_app_data(self->coap_session, self);

cleanup:

    if (dterr)
    {
        dterr = dterr_new(dterr->error_code,
          DTERR_LOC,
          dterr,
          "could not create UDP client session(s) %s:%u",
          self->config.publish_to_host,
          self->config.publish_to_port);
    }
    return dterr;
}

// --------------------------------------------------------------------------------------
// Always bind on any interface, UDP only (no TCP, no DTLS/TLS, no WebSockets).
dterr_t*
dtnetportal_coap__create_endpoints(dtnetportal_coap_t* self)
{
    dterr_t* dterr = NULL;
    unsigned created = 0;

    if (!self || !self->coap_context)
    {
        return dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "invalid args");
    }

    // ---- set up a listening endpoint (one for now)
    if (strstr(self->config.listen_host, ":") == NULL)
    {
        coap_address_init(&self->listen_address);
        self->listen_address.addr.sa.sa_family = AF_INET;
        self->listen_address.addr.sin.sin_port = htons(self->config.listen_port);
    }
    else
    {
        coap_address_init(&self->listen_address);
        self->listen_address.addr.sin6.sin6_family = AF_INET6;
        self->listen_address.addr.sin6.sin6_port = htons(self->config.listen_port);
        // inet_pton(AF_INET6, "::", &self->listen_address.addr.sin6.sin6_addr);
    }

    char addrbuf[128];
    coap_print_addr(&self->listen_address, addrbuf, sizeof addrbuf);

    self->coap_endpoint = coap_new_endpoint(self->coap_context, &self->listen_address, COAP_PROTO_UDP);
    if (!self->coap_endpoint)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "CoAP UDP bind failed on %s", addrbuf);
        goto cleanup;
    }

    dtlog_debug(TAG, "CoAP listener bound on %s", addrbuf);

cleanup:

    if (dterr)
    {
        dterr = dterr_new(
          dterr->error_code, DTERR_LOC, dterr, "could not create UDP listening endpoint(s) *:%u", self->config.listen_port);
    }
    return dterr;
}

// --------------------------------------------------------------------------------------
// we use ingress for all incoming messages
// topic is passed as URI query parameter
// fan-out of received messages is done via dtmanifold

dterr_t*
dtnetportal_coap__create_resource(dtnetportal_coap_t* self)
{
    dterr_t* dterr = NULL;

    const char* topic = "ingress";

    // create a persistent const string for the resource path
    self->coap_resource_path = coap_new_str_const((const uint8_t*)topic, strlen(topic));
    if (!self->coap_resource_path)
    {
        dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "coap_new_str_const(\"%s\") failed", topic);
        goto cleanup;
    }

    // create resource and attach only POST handler (hard requirement)
    self->coap_resource = coap_resource_init(self->coap_resource_path, 0 /*flags*/);
    if (!self->coap_resource)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "coap_resource_init(\"%s\") failed", topic);
        goto cleanup;
    }

    // POST only
    coap_register_handler(self->coap_resource, COAP_REQUEST_POST, dtnetportal_coap__on_post_topic);
    coap_add_resource(self->coap_context, self->coap_resource);

    dtlog_debug(TAG, "created CoAP resource for \"%s\"", topic);

cleanup:
    if (dterr)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "unable to create resource");
    return dterr;
}

// --------------------------------------------------------------------------------------
