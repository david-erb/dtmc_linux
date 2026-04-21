// dtsemaphore_linux.c
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dtcore/dterr.h>
#include <dtcore/dtheaper.h>
#include <dtcore/dtlog.h>
#include <dtcore/dttimeout.h>

#include <dtmc_base/dtsemaphore.h>

#define TAG "dtsemaphore_linux"

// --------------------------------------------------------------------------------------
typedef struct dtsemaphore_linux_t
{
    bool is_initialized;

    // POSIX synchronization
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    pthread_condattr_t cv_attr;

    uint32_t count;     // current semaphore count
    uint32_t max_count; // 0 means "no explicit cap" (use INT32_MAX)

} dtsemaphore_linux_t;

// ---- helpers -------------------------------------------------------------------------
static inline void
add_ms_to_timespec_monotonic(struct timespec* ts, int32_t ms)
{
    // ts is assumed to be CLOCK_MONOTONIC now
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
dtsemaphore_create(dtsemaphore_handle* self_handle, int32_t initial_count, int32_t max_count)
{
    dterr_t* dterr = NULL;
    dtsemaphore_linux_t* self = NULL;
    int err;
    DTERR_ASSERT_NOT_NULL(self_handle);

    DTERR_C(dtheaper_alloc_and_zero(sizeof(dtsemaphore_linux_t), "dtsemaphore_linux_t", (void**)&self));

    self->max_count = (max_count == 0) ? INT32_MAX : max_count;
    self->count = initial_count;

    err = pthread_mutex_init(&self->mtx, NULL);
    if (err != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "pthread_mutex_init failed: %d", err);
        goto cleanup;
    }

    err = pthread_condattr_init(&self->cv_attr);
    if (err != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "pthread_condattr_init failed: %d", err);
        goto cleanup;
    }

    // Use CLOCK_MONOTONIC for timeouts (avoid wall-clock jumps)
    err = pthread_condattr_setclock(&self->cv_attr, CLOCK_MONOTONIC);
    if (err != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "pthread_condattr_setclock failed: %d", err);
        goto cleanup;
    }

    err = pthread_cond_init(&self->cv, &self->cv_attr);
    if (err != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "pthread_cond_init failed: %d", err);
        goto cleanup;
    }

    *self_handle = (dtsemaphore_handle)self;

cleanup:
    if (dterr != NULL)
    {
        // best-effort cleanup
        dtsemaphore_dispose((dtsemaphore_handle)self);
    }
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtsemaphore_post(dtsemaphore_handle self_handle)
{
    dterr_t* dterr = NULL;
    dtsemaphore_linux_t* self = (dtsemaphore_linux_t*)self_handle;
    DTERR_ASSERT_NOT_NULL(self);

    int err = pthread_mutex_lock(&self->mtx);
    if (err != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "mutex lock failed: %d", err);
        goto cleanup;
    }

    // don't exceed max; no error if already at max.
    if (self->count < self->max_count)
    {
        self->count++;
        // Wake one waiter
        pthread_cond_signal(&self->cv);
    }

    pthread_mutex_unlock(&self->mtx);

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtsemaphore_wait(dtsemaphore_handle self_handle, dttimeout_millis_t timeout_milliseconds, bool* was_timeout)
{
    dterr_t* dterr = NULL;
    dtsemaphore_linux_t* self = (dtsemaphore_linux_t*)self_handle;
    DTERR_ASSERT_NOT_NULL(self);

    int err = pthread_mutex_lock(&self->mtx);
    if (err != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "mutex lock failed: %d", err);
        goto cleanup;
    }

    if (timeout_milliseconds == DTTIMEOUT_FOREVER)
    {
        // Wait forever
        while (self->count == 0)
        {
            err = pthread_cond_wait(&self->cv, &self->mtx);
            if (err != 0)
            {
                dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "cond_wait failed: %d", err);
                goto unlock_and_exit;
            }
        }
        self->count--;
    }
    else if (timeout_milliseconds == DTTIMEOUT_NOWAIT)
    {
        // Non-blocking
        if (self->count == 0)
        {
            if (was_timeout != NULL)
                *was_timeout = true;
            else
                dterr = dterr_new(DTERR_TIMEOUT, DTERR_LOC, NULL, "semaphore wait timed out (non-blocking)");
            goto unlock_and_exit;
        }
        self->count--;
    }
    else
    {
        // Timed wait
        struct timespec abs_deadline;
        clock_gettime(CLOCK_MONOTONIC, &abs_deadline);
        add_ms_to_timespec_monotonic(&abs_deadline, timeout_milliseconds);

        while (self->count == 0)
        {
            err = pthread_cond_timedwait(&self->cv, &self->mtx, &abs_deadline);
            if (err == ETIMEDOUT)
            {
                if (was_timeout != NULL)
                    *was_timeout = true;
                else
                    dterr = dterr_new(DTERR_TIMEOUT,
                      DTERR_LOC,
                      NULL,
                      "semaphore wait timed out after %" DTTIMEOUT_MILLIS_PRI " ms",
                      timeout_milliseconds);
                goto unlock_and_exit;
            }
            else if (err != 0)
            {
                dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "cond_timedwait failed: %d", err);
                goto unlock_and_exit;
            }
        }
        self->count--;
    }

    if (was_timeout != NULL)
        *was_timeout = false;

unlock_and_exit:
    pthread_mutex_unlock(&self->mtx);

cleanup:
    if (dterr != NULL)
    {
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "%s() failed", __func__);
    }
    return dterr;
}

// --------------------------------------------------------------------------------------
void
dtsemaphore_dispose(dtsemaphore_handle self_handle)
{
    dtsemaphore_linux_t* self = NULL;
    self = (dtsemaphore_linux_t*)self_handle;
    if (self == NULL)
        return;

    // Best-effort teardown (OK even if never configured)
    pthread_cond_destroy(&self->cv);
    pthread_condattr_destroy(&self->cv_attr);
    pthread_mutex_destroy(&self->mtx);

    dtheaper_free(self);
}
