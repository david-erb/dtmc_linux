// POSIX dtnetportal over MQTT using libmosquitto, with dtmanifold fanout.

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <mosquitto.h>

#include <dtmc_base/dtmc_base_constants.h>

#include <dtcore/dtbuffer.h>
#include <dtcore/dterr.h>
#include <dtcore/dtguid.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtobject.h>

#include <dtmc_base/dtcpu.h>
#include <dtmc_base/dtmanifold.h>
#include <dtmc_base/dtnetportal.h>
#include <dtmc_base/dtsemaphore.h>

#include <dtmc/dtnetportal_mosquitto.h>

DTNETPORTAL_INIT_VTABLE(dtnetportal_mosquitto);
DTOBJECT_INIT_VTABLE(dtnetportal_mosquitto);

#define TAG "dtnetportal_mosquitto"
#define dtlog_debug(...)

// --------------------------------------------------------------------------------------
// private structure used internally
typedef struct dtnetportal_mosquitto_t
{
    DTNETPORTAL_COMMON_MEMBERS;

    dtnetportal_mosquitto_config_t config;
    char client_id[DTGUID_STRING_SIZE];

    dtsemaphore_handle manifold_semaphore;
    dtmanifold_t _manifold, *manifold;

    // MQTT
    struct mosquitto* mqt;

    // in dtnetportal_mosquitto_t
    pthread_mutex_t conn_mutex;
    pthread_cond_t conn_cv;
    bool connected;

    // SUBACK tracking (kept; useful if you later decide to wait for SUBACK)
    pthread_mutex_t suback_mutex;
    pthread_cond_t suback_cv;
    int suback_mid_waiting; // -1 if none
    int suback_result;      // -1=waiting, 0=ok, >0=error

    bool _is_malloced;

} dtnetportal_mosquitto_t;

extern dtnetportal_mosquitto_t* static_self; // if you keep a singleton elsewhere

// Forward decls
static void
on_connect(struct mosquitto* m, void* userdata, int rc);
static void
on_disconnect(struct mosquitto* m, void* userdata, int rc);
static void
on_message(struct mosquitto* m, void* userdata, const struct mosquitto_message* msg);
static void
on_log(struct mosquitto* m, void* userdata, int level, const char* str);
static void
on_subscribe(struct mosquitto* m, void* userdata, int mid, int qos_count, const int* granted_qos);
// Iterator used on reconnect to resubscribe topics with recipients.
static bool
resubscribe_iter(void* ctx, const char* subject_name, int recipient_count);
static void
add_ms_to_timespec(struct timespec* ts, int ms)
{
    ts->tv_sec += ms / 1000;
    long nsec = ts->tv_nsec + (long)(ms % 1000) * 1000000L;
    if (nsec >= 1000000000L)
    {
        ts->tv_sec += 1;
        nsec -= 1000000000L;
    }
    ts->tv_nsec = nsec;
}

// --------------------------------------------------------------------------------------
static int
wait_for_connected(dtnetportal_mosquitto_t* self, int timeout_ms)
/* returns 0 when connected, ETIMEDOUT on timeout, ENOTCONN if disconnected */
{
    int rc = 0;

    pthread_mutex_lock(&self->conn_mutex);
    if (!self->connected)
    {
        struct timespec abs;
        clock_gettime(CLOCK_REALTIME, &abs);
        add_ms_to_timespec(&abs, timeout_ms > 0 ? timeout_ms : 2000);

        while (!self->connected)
        {
            int tw = pthread_cond_timedwait(&self->conn_cv, &self->conn_mutex, &abs);
            if (tw == ETIMEDOUT)
            {
                rc = ETIMEDOUT;
                break;
            }
        }
    }
    if (!self->connected && rc == 0)
        rc = ENOTCONN;
    pthread_mutex_unlock(&self->conn_mutex);
    return rc;
}

// --------------------------------------------------------------------------------------
/* Waits for on_subscribe() to confirm the given mid.
 * Returns 0 on success, ETIMEDOUT on timeout, or EIO on broker refusal.
 * Resets suback_mid_waiting to -1 before returning.
 */
static int
wait_for_suback(dtnetportal_mosquitto_t* self, int mid, int timeout_ms)
{
    int rc = 0;

    pthread_mutex_lock(&self->suback_mutex);
    self->suback_mid_waiting = mid;
    self->suback_result = -1; // -1 = still waiting

    struct timespec abstime;
    clock_gettime(CLOCK_REALTIME, &abstime);
    add_ms_to_timespec(&abstime, timeout_ms > 0 ? timeout_ms : 1000);

    while (self->suback_result == -1 && self->connected)
    {
        int tw = pthread_cond_timedwait(&self->suback_cv, &self->suback_mutex, &abstime);
        if (tw == ETIMEDOUT)
        {
            rc = ETIMEDOUT;
            break;
        }
    }

    if (self->suback_result == 0)
    {
        rc = 0; // success
    }
    else if (self->suback_result > 0)
    {
        rc = EIO; // broker refused (QoS 0x80)
    }
    else if (!self->connected)
    {
        rc = ENOTCONN; // disconnected while waiting
    }

    self->suback_mid_waiting = -1; // clear
    pthread_mutex_unlock(&self->suback_mutex);
    return rc;
}

// --------------------------------------------------------------------------------------
extern dterr_t*
dtnetportal_mosquitto_create(dtnetportal_mosquitto_t** self_ptr)
{
    dterr_t* dterr = NULL;

    *self_ptr = (dtnetportal_mosquitto_t*)malloc(sizeof(dtnetportal_mosquitto_t));
    if (*self_ptr == NULL)
    {
        dterr = dterr_new(DTERR_NOMEM,
          DTERR_LOC,
          NULL,
          "failed to allocate %zu bytes for dtnetportal_mosquitto_t",
          sizeof(dtnetportal_mosquitto_t));
        goto cleanup;
    }

    DTERR_C(dtnetportal_mosquitto_init(*self_ptr));
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
dtnetportal_mosquitto_init(dtnetportal_mosquitto_t* self)
{
    dterr_t* dterr = NULL;

    memset(self, 0, sizeof(*self));
    self->model_number = DTMC_BASE_CONSTANTS_NETPORTAL_MODEL_MOSQUITTO;

    DTERR_C(dtnetportal_set_vtable(self->model_number, &dtnetportal_mosquitto_vt));
    DTERR_C(dtobject_set_vtable(self->model_number, &dtnetportal_mosquitto_object_vt));

    self->manifold = &self->_manifold;
    DTERR_C(dtmanifold_init(self->manifold));

    // create a semaphore for the manifold so it self-serializes
    {
        DTERR_C(dtsemaphore_create(&self->manifold_semaphore, 1, 1));
        DTERR_C(dtmanifold_set_threadsafe_semaphore(self->manifold, self->manifold_semaphore, 10));
    }

    pthread_mutex_init(&self->conn_mutex, NULL);
    pthread_cond_init(&self->conn_cv, NULL);
    self->connected = false;

    pthread_mutex_init(&self->suback_mutex, NULL);
    pthread_cond_init(&self->suback_cv, NULL);
    self->suback_mid_waiting = -1;
    self->suback_result = -1;

    mosquitto_lib_init();

cleanup:
    if (dterr != NULL)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "%s(): failed", __func__);
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtnetportal_mosquitto_configure(dtnetportal_mosquitto_t* self, dtnetportal_mosquitto_config_t* config)
{
    dterr_t* dterr = NULL;
    if (config == NULL)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "%s(): called with NULL config", __func__);
        goto cleanup;
    }

    if (config->host == NULL)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "%s(): config.host is required", __func__);
        goto cleanup;
    }

    self->config = *config;

    if (self->config.port <= 0)
    {
        self->config.port = 1883; // default MQTT port
    }

    // caller doesn't provide a client-id?
    if (self->config.client_id == NULL)
    {
        // generate a random guid for client id
        int32_t random = dtcpu_random_int32();
        dtguid_t o;
        dtguid_generate_from_int32(&o, random);
        dtguid_to_string(&o, self->client_id, DTGUID_STRING_SIZE);
        self->config.client_id = self->client_id;
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// establish connection to the network
dterr_t*
dtnetportal_mosquitto_activate(dtnetportal_mosquitto_t* self DTNETPORTAL_ACTIVATE_ARGS)
{
    dterr_t* dterr = NULL;

    const char* cid = self->config.client_id ? self->config.client_id : NULL;
    bool clean = cid == NULL || self->config.clean_start;

    self->mqt = mosquitto_new(cid, clean, self);
    if (!self->mqt)
    {
        dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "mosquitto_new() failed");
        goto cleanup;
    }

    if (self->config.username && self->config.password)
    {
        int rc = mosquitto_username_pw_set(self->mqt, self->config.username, self->config.password);
        if (rc != MOSQ_ERR_SUCCESS)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "mosquitto_username_pw_set(): %s", mosquitto_strerror(rc));
            goto cleanup;
        }
        dtlog_debug(TAG, "MQTT activate: using username \"%s\"", self->config.username);
    }

    if (self->config.use_tls)
    {
        int rc = mosquitto_tls_set(
          self->mqt, self->config.tls_ca_file, "/etc/ssl/certs", self->config.tls_certfile, self->config.tls_keyfile, NULL);
        if (rc != MOSQ_ERR_SUCCESS)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "mosquitto_tls_set(): %s", mosquitto_strerror(rc));
            goto cleanup;
        }
        rc = mosquitto_tls_insecure_set(self->mqt, true); // DEBUG ONLY
        if (rc != MOSQ_ERR_SUCCESS)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "mosquitto_tls_insecure_set(): %s", mosquitto_strerror(rc));
            goto cleanup;
        }
    }

    mosquitto_connect_callback_set(self->mqt, on_connect);
    mosquitto_disconnect_callback_set(self->mqt, on_disconnect);
    mosquitto_message_callback_set(self->mqt, on_message);
    mosquitto_log_callback_set(self->mqt, on_log);
    mosquitto_subscribe_callback_set(self->mqt, on_subscribe);

    dtlog_debug(TAG,
      "MQTT activate: connecting to %s:%d (clean=%s, tls=%s)",
      self->config.host ? self->config.host : "localhost",
      self->config.port,
      clean ? "true" : "false",
      self->config.use_tls ? "true" : "false");

    {
        int rc = mosquitto_connect(self->mqt,
          self->config.host ? self->config.host : "localhost",
          self->config.port,
          self->config.keepalive > 0 ? self->config.keepalive : 30);
        if (rc != MOSQ_ERR_SUCCESS)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "mosquitto_connect(): %s", mosquitto_strerror(rc));
            goto cleanup;
        }
    }

    {
        int rc = mosquitto_loop_start(self->mqt);
        if (rc != MOSQ_ERR_SUCCESS)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "mosquitto_loop_start(): %s", mosquitto_strerror(rc));
            goto cleanup;
        }
    }

    {
        int rc = wait_for_connected(self, /*ms*/ 5000);
        if (rc != 0)
        {
            dterr = dterr_new(DTERR_TIMEOUT, DTERR_LOC, NULL, "activate(): connect wait failed (%d)", rc);
            goto cleanup;
        }
    }

cleanup:
    if (dterr != NULL)
    {
        if (self->mqt)
        {
            mosquitto_destroy(self->mqt);
            self->mqt = NULL;
        }

        dterr = dterr_new(
          dterr->error_code, DTERR_LOC, dterr, "failed to connect to broker \"%s:%d\"", self->config.host, self->config.port);
    }
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtnetportal_mosquitto_subscribe(dtnetportal_mosquitto_t* self DTNETPORTAL_SUBSCRIBE_ARGS)
{
    dterr_t* dterr = NULL;

    // 1) Always register with manifold
    DTERR_C(dtmanifold_subscribe(self->manifold, topic, recipient_self, receive_callback));

    // 2) If this topic just got its first recipient, subscribe once on the broker.
    int rcnt = 0;
    DTERR_C(dtmanifold_subject_recipient_count(self->manifold, topic, &rcnt));
    int mid = 0;
    if (rcnt == 1 && self->mqt != NULL && self->connected)
    {
        int rc = mosquitto_subscribe(self->mqt, &mid, topic, /*QoS*/ 0);
        if (rc != MOSQ_ERR_SUCCESS)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "mosquitto_subscribe(\"%s\"): %s", topic, mosquitto_strerror(rc));
            goto cleanup;
        }

        // Wait for SUBACK (timeout: choose a sensible default; here 1500 ms)
        int wrc = wait_for_suback(self, mid, /*timeout_ms*/ 1500);
        if (wrc == ETIMEDOUT)
        {
            dterr = dterr_new(DTERR_TIMEOUT, DTERR_LOC, NULL, "subscribe(\"%s\"): SUBACK timed out", topic);
            goto cleanup;
        }
        else if (wrc == EIO)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "subscribe(\"%s\"): broker refused subscription", topic);
            goto cleanup;
        }
        else if (wrc == ENOTCONN)
        {
            dterr = dterr_new(DTERR_STATE, DTERR_LOC, NULL, "subscribe(\"%s\"): disconnected while waiting for SUBACK", topic);
            goto cleanup;
        }
    }

    dtlog_info(TAG, "%s(): subscribed to topic \"%s\"", __func__, topic);

cleanup:
    if (dterr != NULL)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "unable to subscribe to topic \"%s\"", topic);
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtnetportal_mosquitto_publish(dtnetportal_mosquitto_t* self DTNETPORTAL_PUBLISH_ARGS)
{
    dterr_t* dterr = NULL;

    // Broker publish only; local delivery will occur via broker echo -> on_message()
    if (!self->mqt)
    {
        dterr = dterr_new(DTERR_STATE, DTERR_LOC, NULL, "publish(): not activated/connected");
        goto cleanup;
    }

    const void* payload = buffer->payload;
    size_t size = (size_t)buffer->length;

    int rc = mosquitto_publish(self->mqt, NULL, topic, (int)size, payload, /*QoS*/ 0, /*retain*/ false);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "mosquitto_publish(\"%s\"): %s", topic, mosquitto_strerror(rc));
        goto cleanup;
    }

    dtlog_debug(TAG, "%s(): published %d bytes on topic \"%s\"", __func__, (int)size, topic);

cleanup:
    if (dterr != NULL)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "unable to publish on topic \"%s\"", topic);
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtnetportal_mosquitto_get_info(dtnetportal_mosquitto_t* self, dtnetportal_info_t* info)
{
    dterr_t* dterr = NULL;
    if (info == NULL)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "called with NULL info");
        goto cleanup;
    }

    memset(info, 0, sizeof(*info));

    snprintf(info->listening_origin, sizeof(info->listening_origin), "mqtt://[%s]:%d", self->config.host, self->config.port);
    info->listening_origin[sizeof(info->listening_origin) - 1] = '\0';

cleanup:
    if (dterr != NULL)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "unable to obtain netportal info");

    return dterr;
}

// --------------------------------------------------------------------------------------
void
dtnetportal_mosquitto_dispose(dtnetportal_mosquitto_t* self)
{
    if (self == NULL)
        return;

    if (self->mqt)
    {
        mosquitto_loop_stop(self->mqt, /*force*/ true);
        mosquitto_disconnect(self->mqt);
        mosquitto_destroy(self->mqt);
        self->mqt = NULL;
    }

    mosquitto_lib_cleanup();

    pthread_cond_destroy(&self->suback_cv);
    pthread_mutex_destroy(&self->suback_mutex);

    pthread_cond_destroy(&self->conn_cv);
    pthread_mutex_destroy(&self->conn_mutex);

    dtmanifold_dispose(self->manifold);
    dtsemaphore_dispose(self->manifold_semaphore);

    if (self->_is_malloced)
        free(self);
    else
        memset(self, 0, sizeof(*self));
}

// --------------------------------------------------------------------------------------
// mosquitto callbacks

static bool
resubscribe_iter(void* ctx, const char* subject_name, int recipient_count)
{
    struct mosquitto* mqt = (struct mosquitto*)ctx;
    if (recipient_count <= 0)
        return true; // skip empty topics

    int rc = mosquitto_subscribe(mqt, NULL, subject_name, /*QoS*/ 0);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        dtlog_error(TAG, "resubscribe(\"%s\") failed: %s", subject_name, mosquitto_strerror(rc));
    }
    else
    {
        dtlog_debug(TAG, "resubscribed to \"%s\"", subject_name);
    }
    return true; // continue
}

// ----------------------------------------------------------------
static void
on_log(struct mosquitto* m, void* userdata, int level, const char* str)
{
    dtlog_debug(TAG, "mosq log[%d]: %s", level, str ? str : "");
}

// ----------------------------------------------------------------
static void
on_connect(struct mosquitto* m, void* userdata, int rc)
{
    dtnetportal_mosquitto_t* self = (dtnetportal_mosquitto_t*)userdata;
    self->connected = (rc == 0);

    pthread_cond_broadcast(&self->conn_cv); // <- wake waiters
    pthread_mutex_unlock(&self->conn_mutex);

    if (rc == 0)
    {
        dtlog_debug(TAG, "MQTT connected");

        // Re-subscribe all known topics that currently have recipients.
        (void)dtmanifold_foreach_topic(self->manifold, resubscribe_iter, (void*)self->mqt);
    }
    else
    {
        dtlog_error(TAG, "MQTT connect failed: rc=%d", rc);
    }
}

// ----------------------------------------------------------------
static void
on_subscribe(struct mosquitto* m, void* userdata, int mid, int qos_count, const int* granted_qos)
{
    dtnetportal_mosquitto_t* self = (dtnetportal_mosquitto_t*)userdata;
    bool ok = false;

    // MQTT 3.1.1: 0,1,2 are valid QoS; 0x80 (128) indicates failure
    if (qos_count > 0 && granted_qos)
    {
        ok = true;
        for (int i = 0; i < qos_count; i++)
        {
            if (granted_qos[i] < 0 || granted_qos[i] == 128)
            {
                ok = false;
                break;
            }
        }
    }

    pthread_mutex_lock(&self->suback_mutex);
    if (self->suback_mid_waiting == mid)
    {
        self->suback_result = ok ? 0 : 1;
        pthread_cond_broadcast(&self->suback_cv);
    }
    pthread_mutex_unlock(&self->suback_mutex);

    dtlog_debug(TAG, "SUBACK mid=%d ok=%s", mid, ok ? "true" : "false");
}

// ----------------------------------------------------------------
static void
on_disconnect(struct mosquitto* m, void* userdata, int rc)
{
    dtnetportal_mosquitto_t* self = (dtnetportal_mosquitto_t*)userdata;
    self->connected = false;
    dtlog_debug(TAG, "MQTT disconnected: rc=%d", rc);
}

// ----------------------------------------------------------------
static void
each_error_callback(dterr_t* dterr, void* context)
{
    printf("    %s() line %ld: %s\n", dterr->source_function, (long)dterr->line_number, dterr->message);
}

// ----------------------------------------------------------------
static void
on_message(struct mosquitto* m, void* userdata, const struct mosquitto_message* msg)
{
    dterr_t* dterr = NULL;
    dtbuffer_t* buffer = NULL;

    dtlog_debug(TAG, "%s(): received %d bytes on topic \"%s\"", __func__, (int)msg->payloadlen, msg->topic);

    dtnetportal_mosquitto_t* self = (dtnetportal_mosquitto_t*)userdata;
    if (!self || !self->manifold || !msg || !msg->topic)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "%s(): invalid arguments", __func__);
        goto cleanup;
    }

    // wrap a buffer around the data that was sent by the central device (not copying it here)
    DTERR_C(dtbuffer_create(&buffer, 0));
    // substitute for the payload
    buffer->payload = msg->payload;
    buffer->length = msg->payloadlen;

    // fan-out to local recipients via manifold
    DTERR_C(dtmanifold_publish(self->manifold, msg->topic, buffer));

    dtlog_debug(TAG, "%s(): forward %d bytes to manifold on topic \"%s\"", __func__, (int)msg->payloadlen, msg->topic);

cleanup:
    dtbuffer_dispose(buffer);

    if (dterr != NULL)
    {
        dtlog_error(TAG, "%s(): unable to publish received mqtt message to manifold for topic \"%s\"", __func__, msg->topic);
        dterr_each(dterr, each_error_callback, NULL);
        dterr_dispose(dterr);
    }
}

// --------------------------------------------------------------------------------------------
// dtobject implementation
// --------------------------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------
// Copy constructor
void
dtnetportal_mosquitto_copy(dtnetportal_mosquitto_t* this, dtnetportal_mosquitto_t* that)
{
    // this object does not support copying
    (void)this;
    (void)that;
}

// --------------------------------------------------------------------------------------------
// Equality check
bool
dtnetportal_mosquitto_equals(dtnetportal_mosquitto_t* a, dtnetportal_mosquitto_t* b)
{
    if (a == NULL || b == NULL)
    {
        return false;
    }

    // TODO: Reconside equality semantics for dtnetportal_mosquitto_equals backend.
    return (a->model_number == b->model_number);
}

// --------------------------------------------------------------------------------------------
const char*
dtnetportal_mosquitto_get_class(dtnetportal_mosquitto_t* self)
{
    return "dtnetportal_mosquitto_t";
}

// --------------------------------------------------------------------------------------------

bool
dtnetportal_mosquitto_is_iface(dtnetportal_mosquitto_t* self, const char* iface_name)
{
    return strcmp(iface_name, DTNETPORTAL_IFACE_NAME) == 0 || //
           strcmp(iface_name, "dtobject_iface") == 0;
}

// --------------------------------------------------------------------------------------------
// Convert to string
void
dtnetportal_mosquitto_to_string(dtnetportal_mosquitto_t* self, char* buffer, size_t buffer_size)
{
    if (self == NULL || buffer == NULL || buffer_size == 0)
        return;

    strncpy(buffer, "dtnetportal_mosquitto_t", buffer_size);
    buffer[buffer_size - 1] = '\0';
}
