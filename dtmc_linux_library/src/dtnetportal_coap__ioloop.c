// POSIX dtnetportal over MQTT using libcoap, with dtmanifold fanout.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // read, write, pipe, close

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <pthread.h>

#include <coap3/coap.h>

#include <dtcore/dtbuffer.h>
#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>

#include <dtmc/dtnetportal_coap.h>
#include <dtmc_base/dtbufferqueue.h>
#include <dtmc_base/dtmanifold.h>
#include <dtmc_base/dtmc_base_constants.h>
#include <dtmc_base/dtnetportal.h>

#include "dtnetportal_coap__private.h"

#define TAG "dtnetportal_coap__ioloop"

// comment out the logging here
#define dtlog_debug(TAG, ...)

// -------------------------------------------------------------------------------
static void
drain_pipe(int fd)
{
    uint8_t buf[256];
    for (;;)
    {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r > 0)
            continue;
        if (r == -1 && errno == EINTR)
            continue;
        break; // 0 (EOF) or -1/EAGAIN -> drained
    }
}

// -------------------------------------------------------------------------------
// this executes the main logic of the task
dterr_t*
dtnetportal_coap__ioloop_tasker_entrypoint(void* self_arg, dttasker_handle tasker_handle)
{
    dterr_t* dterr = NULL;

    dtnetportal_coap_t* self = (dtnetportal_coap_t*)self_arg;

    dtlog_info(TAG,
      "%s(): business logic started listening on %s:%d and publishing to %s:%d",
      __func__,
      self->config.listen_host,
      self->config.listen_port,
      self->config.publish_to_host,
      self->config.publish_to_port);

    int coap_fd = coap_context_get_coap_fd(self->coap_context);
    if (coap_fd == -1)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "coap_context_get_coap_fd failed");
        goto cleanup;
    }

    int nfds = self->wake_pipe[0];
    if (coap_fd > nfds)
        nfds = coap_fd;

    dtlog_debug(TAG,
      "%s(): FD_SETSIZE is %d coap_fd is %d and wake_pipe[0] is %d so max_fd is %d",
      __func__,
      FD_SETSIZE,
      coap_fd,
      self->wake_pipe[0],
      nfds);

    // let launching process know we are ready
    DTERR_C(dttasker_ready(tasker_handle));

    // keep looping until asked to stop
    while (!self->ioloop_should_stop)
    {
        fd_set read_fd_set;
        FD_ZERO(&read_fd_set);
        FD_SET(self->wake_pipe[0], &read_fd_set);
        FD_SET(coap_fd, &read_fd_set);

        // block until something happens (libcoap sockets or our pipe)
        int result;
        result = select(nfds + 1, &read_fd_set, NULL, NULL, NULL);
        if (result == -1)
        {
            if (errno != EAGAIN)
            {
                dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "select error: %s", strerror(errno));
                goto cleanup;
            }
        }

        if (self->ioloop_should_stop)
            break;

        if (FD_ISSET(self->wake_pipe[0], &read_fd_set))
        {
            dtlog_debug(TAG, "%s(): woke on wake pipe", __func__);
            drain_pipe(self->wake_pipe[0]);
        }

        if (FD_ISSET(coap_fd, &read_fd_set))
        {
            dtlog_debug(TAG, "%s(): woke on libcoap activity", __func__);
            result = coap_io_process(self->coap_context, COAP_IO_NO_WAIT);
            if (result < 0)
            {
                dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "coap_io_process error: %d", result);
                goto cleanup;
            }
        }

        // publish all queued messages
        DTERR_C(dtnetportal_coap__ioloop_publish_all(self));
    }

cleanup:
    if (dterr != NULL)
    {

        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "ioloop exiting due to error");
    }
    else
        dtlog_info(TAG, "%s(): business logic exiting", __func__);

    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtnetportal_coap__ioloop_publish_all(dtnetportal_coap_t* self)
{
    dterr_t* dterr = NULL;

    while (true)
    {
        dtbuffer_t* pubwork_buffer = NULL;
        bool was_timeout = false;
        DTERR_C(dtbufferqueue_get(self->bufferqueue, &pubwork_buffer, DTTIMEOUT_NOWAIT, &was_timeout));

        if (was_timeout)
        {
            // we are done publishing all we have right now
            goto cleanup;
        }

        // we have a buffer to publish so split into topic and payload
        char* topic = NULL;
        void* data = NULL;
        int32_t data_len = 0;
        DTERR_C(dtnetportal_coap__pubwork_debuf(pubwork_buffer, &topic, &data, &data_len));

        dterr = dtnetportal_coap__ioloop_publish_one(self, topic, data, data_len);

        // free the buffer now that we have published its contents
        dtbuffer_dispose(pubwork_buffer);

        if (dterr)
        {
            dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "unable to publish on topic \"%s\"", topic);
            goto cleanup;
        }
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtnetportal_coap__ioloop_publish_one(dtnetportal_coap_t* self, const char* topic, void* data, int32_t data_len)
{
    dterr_t* dterr = NULL;
    coap_pdu_t* pdu = NULL;
    coap_optlist_t* optlist = NULL;

    if (!self || !self->coap_context || !self->coap_session || !self->publish_uri)
    {
        return dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "invalid args");
    }

    // Build PDU
    pdu = coap_pdu_init(COAP_MESSAGE_CON,
      COAP_REQUEST_CODE_POST,
      coap_new_message_id(self->coap_session),
      coap_session_max_pdu_size(self->coap_session));
    if (!pdu)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "coap_pdu_init failed");
        goto cleanup;
    }

    // Token
    uint8_t token[8];
    size_t tkl = sizeof(token);
    coap_session_new_token(self->coap_session, &tkl, token);
    coap_add_token(pdu, tkl, token);

    // ===================

    // ... same setup as before ...

    // Base URI -> optlist
    // if (!coap_uri_into_optlist(self->publish_uri, &self->listen_address.addr, &optlist, 1))
    // {
    //     dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "coap_uri_into_options failed: %s", strerror(errno));
    //     goto cleanup;
    // }

    // Append resource path: /ingress
    if (!coap_insert_optlist(&optlist, coap_new_optlist(COAP_OPTION_URI_PATH, 7, (const uint8_t*)"ingress")))
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "unable to add URI-Path option");
        goto cleanup;
    }

    // If there will be a payload, set Content-Format *before* any Uri-Query
    if (data_len > 0 && data)
    {
        uint8_t cbuf[4];
        size_t clen = coap_encode_var_safe(cbuf, sizeof cbuf, COAP_MEDIATYPE_APPLICATION_OCTET_STREAM);
        if (!coap_insert_optlist(&optlist, coap_new_optlist(COAP_OPTION_CONTENT_FORMAT, (uint16_t)clen, cbuf)))
        {
            dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "unable to add Content-Format option");
            goto cleanup;
        }
    }

    // Append query = <topic>

    if (!coap_insert_optlist(&optlist, coap_new_optlist(COAP_OPTION_URI_QUERY, (uint16_t)strlen(topic), (const uint8_t*)topic)))
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "unable to add URI-Query option");
        goto cleanup;
    }

    // Apply all options in one go (libcoap consumes/frees the list)
    coap_add_optlist_pdu(pdu, &optlist);

    // Payload last
    if (data_len > 0 && data)
    {
        coap_add_data(pdu, (size_t)data_len, (const uint8_t*)data);
    }

    // ===================

    // Send (on success, libcoap owns and will free the PDU; on failure, you must free it)
    coap_mid_t mid = coap_send(self->coap_session, pdu);
    if (mid == COAP_INVALID_MID)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "coap_send failed");
        coap_delete_pdu(pdu); // still ours on failure
        pdu = NULL;
        goto cleanup; // nothing else to do
    }

    // Success: prevent double-free; libcoap owns PDU now.
    pdu = NULL;

    dtlog_debug(TAG, "%s(): POST /ingress?%s (%d bytes)", __func__, topic, (int)data_len);

cleanup:
    // If coap_add_optlist_pdu() was not reached (error before applying), we still own optlist.
    if (optlist)
        coap_delete_optlist(optlist);

    if (dterr)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "unable to publish on /ingress?%s", topic ? topic : "");

    return dterr;
}
