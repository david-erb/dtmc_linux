// POSIX dtnetportal over MQTT using libcoap, with dtmanifold fanout.

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> // read, write, pipe, close

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <pthread.h>

#include <coap3/coap.h>

#include <dtmc_base/dtmc_base_constants.h>

#include <dtcore/dtbuffer.h>
#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtobject.h>

#include <dtmc_base/dtbufferqueue.h>
#include <dtmc_base/dtmanifold.h>
#include <dtmc_base/dtnetportal.h>
#include <dtmc_base/dtruntime.h>
#include <dtmc_base/dtsemaphore.h>

#include <dtmc/dtnetportal_coap.h>

#include "dtnetportal_coap__private.h"

DTNETPORTAL_INIT_VTABLE(dtnetportal_coap);
DTOBJECT_INIT_VTABLE(dtnetportal_coap);

static coap_context_t* global_context = NULL;
static pthread_mutex_t global_context_lock = PTHREAD_MUTEX_INITIALIZER;

#define TAG "dtnetportal_coap"

#ifdef COAP_EPOLL_SUPPORT
#error "This build of libcoap has epoll support enabled; use the epoll code path or rebuild without epoll."
#endif

// --------------------------------------------------------------------------------------
extern dterr_t*
dtnetportal_coap_create(dtnetportal_coap_t** self_ptr)
{
    dterr_t* dterr = NULL;

    *self_ptr = (dtnetportal_coap_t*)malloc(sizeof(dtnetportal_coap_t));
    if (*self_ptr == NULL)
    {
        dterr = dterr_new(
          DTERR_NOMEM, DTERR_LOC, NULL, "failed to allocate %zu bytes for dtnetportal_coap_t", sizeof(dtnetportal_coap_t));
        goto cleanup;
    }

    DTERR_C(dtnetportal_coap_init(*self_ptr));
    (*self_ptr)->_is_malloced = true;

cleanup:
    if (dterr != NULL)
    {
        if (*self_ptr != NULL)
            free(*self_ptr);
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "%s(): failed", __func__);
    }
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtnetportal_coap_init(dtnetportal_coap_t* self)
{
    dterr_t* dterr = NULL;

    memset(self, 0, sizeof(*self));
    self->model_number = DTMC_BASE_CONSTANTS_NETPORTAL_MODEL_COAP;

    DTERR_C(dtnetportal_set_vtable(self->model_number, &dtnetportal_coap_vt));
    DTERR_C(dtobject_set_vtable(self->model_number, &dtnetportal_coap_object_vt));

    // create a buffer queue for outgoing publish work, arbitrary limit of 10 for now
    DTERR_C(dtbufferqueue_create(&self->bufferqueue, 10, false));

    self->manifold = &self->_manifold;
    DTERR_C(dtmanifold_init(self->manifold));

    // create a semaphore for the manifold so it self-serializes
    DTERR_C(dtsemaphore_create(&self->manifold_semaphore, 1, 1));
    DTERR_C(dtmanifold_set_threadsafe_semaphore(self->manifold, self->manifold_semaphore, 10));

cleanup:
    if (dterr != NULL)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "%s(): failed", __func__);
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtnetportal_coap_configure(dtnetportal_coap_t* self, dtnetportal_coap_config_t* config)
{
    dterr_t* dterr = NULL;
    if (config == NULL)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "called with NULL config");
        goto cleanup;
    }

    if (config->publish_to_host == NULL)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "publish_to_host is required");
        goto cleanup;
    }

    if (config->listen_port == 0)
        config->listen_port = 5683; // default CoAP port

    self->config = *config;

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// establish connection to the network
dterr_t*
dtnetportal_coap_activate(dtnetportal_coap_t* self DTNETPORTAL_ACTIVATE_ARGS)
{
    dterr_t* dterr = NULL;

    coap_startup();

    /* Log levels: COAP_LOG_EMERG, ALERT, CRIT, WARN, NOTICE, INFO, DEBUG */
    coap_set_log_level(LOG_DEBUG);

    // ---- set up the global context
    if (pthread_mutex_trylock(&global_context_lock) == 0)
    {
        if (global_context == NULL)
            global_context = coap_new_context(NULL);
        pthread_mutex_unlock(&global_context_lock);
    }

    if (global_context == NULL)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "coap_new_context failed");
        goto cleanup;
    }

    self->coap_context = global_context;
    coap_set_app_data(self->coap_context, self);

    coap_register_response_handler(self->coap_context, dtnetportal_coap__on_response);
    coap_register_event_handler(self->coap_context, dtnetportal_coap__on_event);

    DTERR_C(dtnetportal_coap__create_sessions(self));
    DTERR_C(dtnetportal_coap__create_endpoints(self));
    DTERR_C(dtnetportal_coap__create_resource(self));

    if (pipe(self->wake_pipe) == -1)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "pipe() failed: %s", strerror(errno));
        goto cleanup;
    }

    // set non-blocking and close-on-exec
    if (fcntl(self->wake_pipe[0], F_SETFL, O_NONBLOCK) == -1 || fcntl(self->wake_pipe[1], F_SETFL, O_NONBLOCK) == -1 ||
        fcntl(self->wake_pipe[0], F_SETFD, FD_CLOEXEC) == -1 || fcntl(self->wake_pipe[1], F_SETFD, FD_CLOEXEC) == -1)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "fcntl() failed: %s", strerror(errno));
        goto cleanup;
    }
    self->wake_pipe_ok = true;

    // ---- configure and start the ioloop task
    {
        dttasker_config_t c = { 0 };

        c.name = "netportal ioloop";                                          // name of the task
        c.tasker_entry_point_fn = dtnetportal_coap__ioloop_tasker_entrypoint; // main function for the task
        c.tasker_entry_point_arg = self;                                      // self pointer
        c.stack_size = 4096;                                                  // pthread may raise this to min
        c.priority = 5;                                                       // best-effort / ignored on SCHED_OTHER

        // for status changes on ioloop tasker
        c.tasker_info_callback = self->config.tasker_info_callback;
        c.tasker_info_callback_context = self->config.tasker_info_callback_context;

        DTERR_C(dttasker_create(&self->ioloop_tasker_handle, &c));
        DTERR_C(dttasker_start(self->ioloop_tasker_handle));
        dtlog_info(TAG, "%s(): task \"%s\" ready", __func__, c.name);
    }

cleanup:
    if (dterr != NULL)
    {
        dterr = dterr_new(dterr->error_code,
          DTERR_LOC,
          dterr,
          "unable to activate for failed netportal for URI \"%s:%d\"",
          self->config.publish_to_host,
          self->config.publish_to_port);
    }
    return dterr;
}

// --------------------------------------------------------------------------------------
// subscribe must happen before activate
dterr_t*
dtnetportal_coap_subscribe(dtnetportal_coap_t* self DTNETPORTAL_SUBSCRIBE_ARGS)
{
    dterr_t* dterr = NULL;

    DTERR_C(dtmanifold_subscribe(self->manifold, topic, recipient_self, receive_callback));

    dtlog_info(TAG, "%s(): subscribed to topic \"%s\"", __func__, topic);

cleanup:
    if (dterr != NULL)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "unable to subscribe to topic \"%s\"", topic);
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtnetportal_coap_publish(dtnetportal_coap_t* self DTNETPORTAL_PUBLISH_ARGS)
{
    dterr_t* dterr = NULL;
    dtbuffer_t* pubwork_buffer = NULL;

    // enbuf is an owned copy of the sender's buffer
    DTERR_C(dtnetportal_coap__pubwork_enbuf(&pubwork_buffer, topic, buffer));

    DTERR_C(dtbufferqueue_put(self->bufferqueue, pubwork_buffer, DTTIMEOUT_FOREVER, NULL));

    // wake up ioloop to send it
    uint8_t b = 1;
    write(self->wake_pipe[1], &b, 1);

    dtlog_debug(TAG, "%s(): queued %d bytes for publish on topic \"%s\"", __func__, (int)buffer->length, topic);

cleanup:
    if (dterr)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "unable to publish on topic \"%s\"", topic);
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtnetportal_coap_get_info(dtnetportal_coap_t* self, dtnetportal_info_t* info)
{
    dterr_t* dterr = NULL;
    if (info == NULL)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "called with NULL info");
        goto cleanup;
    }

    memset(info, 0, sizeof(*info));

    info->flavor = DTNETPORTAL_COAP_FLAVOR;
    info->version = DTNETPORTAL_COAP_VERSION;

    snprintf(info->listening_origin,
      sizeof(info->listening_origin),
      "coap://[%s]:%d",
      self->config.listen_host,
      self->config.listen_port);
    info->listening_origin[sizeof(info->listening_origin) - 1] = '\0';

cleanup:
    if (dterr != NULL)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "unable to obtain netportal info");

    return dterr;
}

// --------------------------------------------------------------------------------------
void
dtnetportal_coap_dispose(dtnetportal_coap_t* self)
{
    if (self == NULL)
        return;

    if (self->coap_context != NULL && self->ioloop_tasker_handle != NULL)
    {
        // Stop background I/O pump, if running...
        self->ioloop_should_stop = 2;

        // wake up ioloop to let it exit
        uint8_t b = 1;
        write(self->wake_pipe[1], &b, 1);

        dtruntime_sleep_milliseconds(1);
    }

    if (self->wake_pipe_ok)
    {
        close(self->wake_pipe[0]);
        close(self->wake_pipe[1]);
        self->wake_pipe_ok = false;
    }

    dttasker_dispose(self->ioloop_tasker_handle);

    if (self->coap_resource)
        coap_delete_resource(self->coap_context, self->coap_resource);
    if (self->coap_resource_path)
        coap_delete_str_const(self->coap_resource_path);

    if (self->publish_uri != NULL)
        coap_free(self->publish_uri);
    if (self->coap_session != NULL)
        coap_session_release(self->coap_session);

    dtmanifold_dispose(self->manifold);
    dtsemaphore_dispose(self->manifold_semaphore);

    dtbufferqueue_dispose(self->bufferqueue);

    if (self->_is_malloced)
        free(self);
    else
        memset(self, 0, sizeof(*self));
}

// --------------------------------------------------------------------------------------------
// dtobject implementation
// --------------------------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------
// Copy constructor
void
dtnetportal_coap_copy(dtnetportal_coap_t* this, dtnetportal_coap_t* that)
{
    // this object does not support copying
    (void)this;
    (void)that;
}

// --------------------------------------------------------------------------------------------
// Equality check
bool
dtnetportal_coap_equals(dtnetportal_coap_t* a, dtnetportal_coap_t* b)
{
    if (a == NULL || b == NULL)
    {
        return false;
    }

    // TODO: Reconside equality semantics for dtnetportal_coap_equals backend.
    return (a->model_number == b->model_number);
}

// --------------------------------------------------------------------------------------------
const char*
dtnetportal_coap_get_class(dtnetportal_coap_t* self)
{
    return "dtnetportal_coap_t";
}

// --------------------------------------------------------------------------------------------

bool
dtnetportal_coap_is_iface(dtnetportal_coap_t* self, const char* iface_name)
{
    return strcmp(iface_name, DTNETPORTAL_IFACE_NAME) == 0 || //
           strcmp(iface_name, "dtobject_iface") == 0;
}

// --------------------------------------------------------------------------------------------
// Convert to string
void
dtnetportal_coap_to_string(dtnetportal_coap_t* self, char* buffer, size_t buffer_size)
{
    if (self == NULL || buffer == NULL || buffer_size == 0)
        return;

    strncpy(buffer, "dtnetportal_coap_t", buffer_size);
    buffer[buffer_size - 1] = '\0';
}
