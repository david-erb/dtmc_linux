// dtbufferqueue_linux.c
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dtmc_base/dtmc_base_constants.h>

#include <dtcore/dtbuffer.h>
#include <dtcore/dtbytes.h>
#include <dtcore/dterr.h>
#include <dtcore/dtheaper.h>
#include <dtcore/dtlog.h>
#include <dtcore/dttimeout.h>

#include <dtmc_base/dtbufferqueue.h>

#define TAG "dtbufferqueue"
#define dtlog_debug(TAG, ...)

// --------------------------------------------------------------------------------------
typedef struct dtbufferqueue_t
{
    int32_t max_count;
    bool should_overwrite;

    // POSIX synchronization + monotonic clock for timed waits
    pthread_mutex_t mtx;
    pthread_cond_t cv_not_empty;
    pthread_cond_t cv_not_full;
    pthread_condattr_t cv_attr;

    // Ring buffer of dtbuffer_t* (pointers only)
    dtbuffer_t** ring;
    uint32_t count; // number of items currently in queue
    uint32_t head;  // pop index
    uint32_t tail;  // push index

} dtbufferqueue_t;

// ---- helpers -------------------------------------------------------------------------
static inline void
add_ms_to_timespec_monotonic(struct timespec* ts, int32_t ms)
{
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
    else if (ts->tv_nsec < 0)
    {
        ts->tv_sec -= 1;
        ts->tv_nsec += 1000000000L;
    }
}

// --------------------------------------------------------------------------------------
extern dterr_t*
dtbufferqueue_create(dtbufferqueue_handle* out_handle, int32_t max_count, bool should_overwrite)
{
    dterr_t* dterr = NULL;
    dtbufferqueue_t* self = NULL;
    DTERR_ASSERT_NOT_NULL(out_handle);

    DTERR_C(dtheaper_alloc_and_zero(sizeof(dtbufferqueue_t), "dtbufferqueue_t", (void**)&self));

    self->max_count = max_count;
    self->should_overwrite = should_overwrite;

    if (self->max_count == 0)
        self->max_count = 1;

    int32_t storage_size = (int32_t)(sizeof(dtbuffer_t*) * (size_t)self->max_count);
    DTERR_C(dtheaper_alloc_and_zero(storage_size, "dtbufferqueue_linux ring", (void**)&self->ring));

    int rc;

    rc = pthread_mutex_init(&self->mtx, NULL);
    if (rc != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "pthread_mutex_init: %d", rc);
        goto cleanup;
    }

    rc = pthread_condattr_init(&self->cv_attr);
    if (rc != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "pthread_condattr_init: %d", rc);
        goto cleanup;
    }

    rc = pthread_condattr_setclock(&self->cv_attr, CLOCK_MONOTONIC);
    if (rc != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "pthread_condattr_setclock: %d", rc);
        goto cleanup;
    }

    rc = pthread_cond_init(&self->cv_not_empty, &self->cv_attr);
    if (rc != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "pthread_cond_init(not_empty): %d", rc);
        goto cleanup;
    }

    rc = pthread_cond_init(&self->cv_not_full, &self->cv_attr);
    if (rc != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "pthread_cond_init(not_full): %d", rc);
        goto cleanup;
    }

    *out_handle = (dtbufferqueue_handle)self;

cleanup:
    if (dterr != NULL)
    {
        pthread_cond_destroy(&self->cv_not_full);
        pthread_cond_destroy(&self->cv_not_empty);
        pthread_condattr_destroy(&self->cv_attr);
        pthread_mutex_destroy(&self->mtx);
        dtbufferqueue_dispose((dtbufferqueue_handle)self);
    }
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtbufferqueue_put(dtbufferqueue_handle handle DTBUFFERQUEUE_PUT_ARGS)
{
    dterr_t* dterr = NULL;
    dtbufferqueue_t* self = (dtbufferqueue_t*)handle;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(buffer);

    if (was_timeout != NULL)
        *was_timeout = false;

#ifndef dtlog_debug
    {
        char tmp[64];
        dtbytes_compose_hex(((char*)buffer->payload), buffer->length, tmp, sizeof(tmp));
        dtlog_debug(TAG, "put pushing buffer %p length %" PRId32 " %s", buffer, buffer->length, tmp);
    }
#endif

    int rc = pthread_mutex_lock(&self->mtx);
    if (rc != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "mutex lock failed: %d", rc);
        goto cleanup;
    }

    if (timeout_millis == DTTIMEOUT_FOREVER)
    {
        while (self->count == self->max_count)
        {
            rc = pthread_cond_wait(&self->cv_not_full, &self->mtx);
            if (rc != 0)
            {
                dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "cond_wait not_full: %d", rc);
                goto unlock;
            }
        }
    }
    else if (timeout_millis == 0)
    {
        if (self->count == self->max_count)
        {
            if (was_timeout != NULL)
                *was_timeout = true;
            goto unlock;
        }
    }
    else
    {
        struct timespec abs_deadline;
        clock_gettime(CLOCK_MONOTONIC, &abs_deadline);
        add_ms_to_timespec_monotonic(&abs_deadline, timeout_millis);

        while (self->count == self->max_count)
        {
            rc = pthread_cond_timedwait(&self->cv_not_full, &self->mtx, &abs_deadline);
            if (rc == ETIMEDOUT)
            {
                if (was_timeout != NULL)
                    *was_timeout = true;
                goto unlock;
            }
            if (rc != 0)
            {
                dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "cond_timedwait not_full: %d", rc);
                goto unlock;
            }
        }
    }

    // enqueue
    self->ring[self->tail] = buffer;
    self->tail = (self->tail + 1) % self->max_count;
    self->count++;

    // wake one getter
    pthread_cond_signal(&self->cv_not_empty);

unlock:
    pthread_mutex_unlock(&self->mtx);

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtbufferqueue_get(dtbufferqueue_handle handle DTBUFFERQUEUE_GET_ARGS)
{
    dterr_t* dterr = NULL;
    dtbufferqueue_t* self = (dtbufferqueue_t*)handle;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(buffer);

    if (was_timeout != NULL)
        *was_timeout = false;

    int rc = pthread_mutex_lock(&self->mtx);
    if (rc != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "mutex lock failed: %d", rc);
        goto cleanup;
    }

    if (self->count > 0)
    {
        goto dequeue;
    }

    if (timeout_millis == DTTIMEOUT_FOREVER)
    {
        while (self->count == 0)
        {
            rc = pthread_cond_wait(&self->cv_not_empty, &self->mtx);
            if (rc != 0)
            {
                dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "cond_wait not_empty: %d", rc);
                goto unlock;
            }
        }
    }
    else if (timeout_millis == 0)
    {
        if (was_timeout != NULL)
            *was_timeout = true;
        goto unlock;
    }
    else
    {
        struct timespec abs_deadline;
        clock_gettime(CLOCK_MONOTONIC, &abs_deadline);
        add_ms_to_timespec_monotonic(&abs_deadline, timeout_millis);

        while (self->count == 0)
        {
            rc = pthread_cond_timedwait(&self->cv_not_empty, &self->mtx, &abs_deadline);
            if (rc == ETIMEDOUT)
            {
                if (was_timeout != NULL)
                    *was_timeout = true;
                goto unlock;
            }
            if (rc != 0)
            {
                dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "cond_timedwait not_empty: %d", rc);
                goto unlock;
            }
        }
    }

dequeue:
    // dequeue
    dtbuffer_t* out = self->ring[self->head];
    self->head = (self->head + 1) % self->max_count;
    self->count--;

#ifndef dtlog_debug
    {
        char tmp[64];
        dtbytes_compose_hex(((char*)out->payload), out->length, tmp, sizeof(tmp));
        dtlog_debug(TAG, "get returned buffer %p length %" PRId32 " %s", out, out->length, tmp);
    }
#endif

    *buffer = out;

    // wake one putter
    pthread_cond_signal(&self->cv_not_full);

unlock:
    pthread_mutex_unlock(&self->mtx);

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
void
dtbufferqueue_dispose(dtbufferqueue_handle handle)
{
    dtbufferqueue_t* self = (dtbufferqueue_t*)handle;

    if (self == NULL)
        return;

    pthread_cond_destroy(&self->cv_not_full);
    pthread_cond_destroy(&self->cv_not_empty);
    pthread_condattr_destroy(&self->cv_attr);
    pthread_mutex_destroy(&self->mtx);

    dtheaper_free((void*)self->ring);
    dtheaper_free((void*)self);
}
