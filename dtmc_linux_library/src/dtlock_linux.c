#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <dtcore/dterr.h>
#include <dtmc_base/dtlock.h>

#define TAG "dtlock_linux"

typedef struct dtlock_linux_t
{
    pthread_mutex_t mutex;
    bool _is_malloced;
    bool _is_mutex_initialized;

} dtlock_linux_t;

// ----------------------------------------------------------------------------
static dterr_t*
dtlock_linux__pthread_rc(int rc, const char* what)
{
    if (rc == 0)
        return NULL;

    // pthread_* APIs return an error code directly (they typically do not set errno).
    return dterr_new(DTERR_OS, DTERR_LOC, NULL, "%s failed rc=%d", what, rc);
}

// ----------------------------------------------------------------------------
// Create lock.  Allocates memory. Implies init.
dterr_t*
dtlock_create(dtlock_handle* self_handle)
{
    dterr_t* dterr = NULL;
    dtlock_linux_t* self = NULL;
    pthread_mutexattr_t attr;
    int rc = 0;
    DTERR_ASSERT_NOT_NULL(self_handle);

    self = (dtlock_linux_t*)malloc(sizeof(dtlock_linux_t));
    if (!self)
    {
        dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc %zu", sizeof(dtlock_linux_t));
        goto cleanup;
    }

    memset(self, 0, sizeof(*self));
    self->_is_malloced = true;

    *self_handle = (dtlock_handle)self;

    rc = pthread_mutexattr_init(&attr);
    DTERR_C(dtlock_linux__pthread_rc(rc, "pthread_mutexattr_init"));

    // This is the key: detect misuse cheaply in pthreads itself.
    rc = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    DTERR_C(dtlock_linux__pthread_rc(rc, "pthread_mutexattr_settype(PTHREAD_MUTEX_ERRORCHECK)"));

    rc = pthread_mutex_init(&self->mutex, &attr);
    DTERR_C(dtlock_linux__pthread_rc(rc, "pthread_mutex_init"));
    self->_is_mutex_initialized = true;

    (void)pthread_mutexattr_destroy(&attr);

cleanup:
    if (dterr && self && self->_is_mutex_initialized)
    {
        (void)pthread_mutexattr_destroy(&attr);
        dtlock_dispose(*self_handle);
        *self_handle = NULL;
    }
    return dterr;
}

// ----------------------------------------------------------------------------
// Acquire lock owned by thread. Error if already held by this thread.
dterr_t*
dtlock_acquire(dtlock_handle self_handle)
{
    dterr_t* dterr = NULL;
    dtlock_linux_t* self = (dtlock_linux_t*)self_handle;
    DTERR_ASSERT_NOT_NULL(self);

    int rc = pthread_mutex_lock(&self->mutex);
    if (rc == EDEADLK)
        return dterr_new(DTERR_STATE, DTERR_LOC, NULL, "lock already held by this thread");
    DTERR_C(dtlock_linux__pthread_rc(rc, "pthread_mutex_lock"));

cleanup:
    return dterr;
}

// ----------------------------------------------------------------------------
// Release lock owned by thread. Error if not held by this thread (incl double-release).
dterr_t*
dtlock_release(dtlock_handle self_handle)
{
    dterr_t* dterr = NULL;
    dtlock_linux_t* self = (dtlock_linux_t*)self_handle;
    DTERR_ASSERT_NOT_NULL(self);

    int rc = pthread_mutex_unlock(&self->mutex);
    if (rc == EPERM)
        return dterr_new(DTERR_STATE, DTERR_LOC, NULL, "unlock attempted by non-owner (or double-release)");
    DTERR_C(dtlock_linux__pthread_rc(rc, "pthread_mutex_unlock"));

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// Dispose lock resources.  Implies release.  No error if already disposed.
void
dtlock_dispose(dtlock_handle self_handle)
{
    dtlock_linux_t* self = (dtlock_linux_t*)self_handle;
    if (!self)
        return;

    if (self->_is_mutex_initialized)
    {
        (void)pthread_mutex_destroy(&self->mutex);
    }

    bool is_malloced = self->_is_malloced;
    memset(self, 0, sizeof(*self));

    if (is_malloced)
        free(self);
}
