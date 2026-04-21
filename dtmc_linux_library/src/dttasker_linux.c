// dttasker.c
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dtmc_base/dtmc_base_constants.h>

#include <dtcore/dterr.h>
#include <dtcore/dtguid.h>
#include <dtcore/dtguidable.h>
#include <dtcore/dtheaper.h>
#include <dtcore/dtlog.h>

#include <dtmc/dtmc_linux.h>

#include <dtmc_base/dtlock.h>
#include <dtmc_base/dtruntime.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>
#include <dtmc_base/dttasker_registry.h>

DTGUIDABLE_INIT_VTABLE(dttasker)

// comment out the logging here
#define dtlog_debug(TAG, ...)

#define TAG "dttasker"

static dterr_t*
dttasker_priority_enum_to_native_number(dttasker_priority_t p, int32_t* native_priority);

static dterr_t*
dttasker__post_ready_once(dttasker_handle self_handle);

// --------------------------------------------------------------------------------------
// private structure for dttasker implementation

typedef struct dttasker_t
{
    int32_t model_number;
    dttasker_config_t config;

    char _name[32]; // storage for name

    // protects mutable shared state such as info/config flags below
    dtlock_handle state_lock;

    // "event" that start() waits on until ready() (or error) signals it
    dtsemaphore_handle ready_semaphore;
    bool ready_posted;

    dttasker_info_t info;

    pthread_t thread;
    int thread_created;

    // stop/join state
    atomic_bool stop_requested;
    atomic_bool thread_exited;
    bool thread_joined;

    dtguid_t guid; // 16 bytes, does not need to be aligned

} dttasker_t;

// --------------------------------------------------------------------------------------
static dterr_t*
dttasker__post_ready_once(dttasker_handle self_handle)
{
    dterr_t* dterr = NULL;
    bool should_post = false;
    dttasker_t* self = (dttasker_t*)self_handle;
    bool lock_needs_cleanup_release = false;
    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dtlock_acquire(self->state_lock));
    lock_needs_cleanup_release = true;

    if (!self->ready_posted)
    {
        self->ready_posted = true;
        should_post = true;
    }

    lock_needs_cleanup_release = false;
    DTERR_C(dtlock_release(self->state_lock));

    if (should_post)
    {
        DTERR_C(dtsemaphore_post(self->ready_semaphore));
    }

cleanup:
    if (lock_needs_cleanup_release)
    {
        dterr = dterr_append(dterr, dtlock_release(self->state_lock));
    }
    return dterr;
}

// --------------------------------------------------------------------------------------
// create a new dttasker instance, allocating memory as needed and access system resources
dterr_t*
dttasker_create(dttasker_handle* self_handle, dttasker_config_t* config)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = NULL;
    DTERR_ASSERT_NOT_NULL(self_handle);
    DTERR_ASSERT_NOT_NULL(config);
    DTERR_ASSERT_NOT_NULL(config->name);

    *self_handle = NULL;

    DTERR_C(dttasker_validate_priority_enum(config->priority));

    DTERR_C(dtheaper_alloc_and_zero(sizeof(dttasker_t), "dttasker_t", (void**)&self));

    self->model_number = DTMC_BASE_CONSTANTS_TASKER_MODEL_LINUX;

    self->config = *config;

    atomic_init(&self->stop_requested, false);
    atomic_init(&self->thread_exited, false);
    self->thread_joined = false;

    // put the config name into internal storage
    strncpy(self->_name, config->name, sizeof(self->_name) - 1);
    self->_name[sizeof(self->_name) - 1] = '\0';

    if (self->config.stack_size != 0 && self->config.stack_size < PTHREAD_STACK_MIN)
    {
        self->config.stack_size = PTHREAD_STACK_MIN;
    }

    DTERR_C(dtguidable_set_vtable(self->model_number, &dttasker_guidable_vt));
    dtguid_generate_from_string(&self->guid, self->config.name);

    DTERR_C(dtlock_create(&self->state_lock));
    DTERR_C(dtsemaphore_create(&self->ready_semaphore, 0, 0));

    {
        dttasker_info_t info = {
            .status = INITIALIZED,
        };
        DTERR_C(dttasker_set_info((dttasker_handle)self, &info));
    }

    *self_handle = (dttasker_handle)self;

    if (!dttasker_registry_global_instance.is_initialized)
        DTERR_C(dttasker_registry_init(&dttasker_registry_global_instance));
    DTERR_C(dttasker_registry_insert(&dttasker_registry_global_instance, *self_handle));

cleanup:
    if (dterr != NULL)
    {
        dttasker_dispose((dttasker_handle)self);
        *self_handle = NULL;
    }

    return dterr;
}

// --------------------------------------------------------------------------------------
static void*
dttasker__thread_inception_function(void* arg)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = (dttasker_t*)arg;

    // Run the user entry point (blocking)
    dterr = self->config.tasker_entry_point_fn(self->config.tasker_entry_point_arg, (dttasker_handle)self);

    // Mark STOPPED and stash dterr
    {
        dttasker_info_t info = { .status = STOPPED, .dterr = dterr };
        dterr = dterr_append(dterr, dttasker_set_info((dttasker_handle)self, &info));
    }

    // If we exited with an error and start() is still waiting, wake it
    if (dterr != NULL)
    {
        dtlog_debug(TAG, "%s(): posting ready (with error) for task \"%s\"", __func__, self->config.name);
        dterr = dterr_append(dterr, dttasker__post_ready_once((dttasker_handle)self));
    }

    atomic_store_explicit(&self->thread_exited, true, memory_order_release);

    return NULL;
}

// --------------------------------------------------------------------------------------
// set the entry point function and argument to be called when the task starts
dterr_t*
dttasker_set_entry_point(dttasker_handle self_handle, dttasker_entry_point_fn entry_point_function, void* entry_point_arg)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = (dttasker_t*)self_handle;
    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dtlock_acquire(self->state_lock));
    self->config.tasker_entry_point_fn = entry_point_function;
    self->config.tasker_entry_point_arg = entry_point_arg;
    DTERR_C(dtlock_release(self->state_lock));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// start the task running, creating the thread
dterr_t*
dttasker_start(dttasker_handle self_handle)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = (dttasker_t*)self_handle;
    pthread_attr_t attr;
    bool attr_initialized = false;
    bool lock_needs_cleanup_release = false;
    DTERR_ASSERT_NOT_NULL(self);

    // Refuse to start again if task is already running / not yet join-complete.
    if (self->thread_created && !atomic_load_explicit(&self->thread_exited, memory_order_acquire))
    {
        dterr = dterr_new(DTERR_STATE, DTERR_LOC, NULL, "task \"%s\" is already started", self->config.name);
        goto cleanup;
    }

    dtlog_debug(TAG, "%s(): called for task %s", __func__, self->config.name);

    DTERR_POSERROR_C(pthread_attr_init(&attr));
    attr_initialized = true;

    // Stack size (no manual stack buffer allocation)
    if (self->config.stack_size)
    {
        // pthreads requires a minimum & alignment; let the call adjust if needed
        DTERR_POSERROR_C(pthread_attr_setstacksize(&attr, (size_t)self->config.stack_size));
    }

    // Optional: priority (best-effort). Requires appropriate permissions to take effect.
    // TODO: Add SCHED_FIFO/SCHED_RR handling to Linux dttasker_start.
    int32_t native_priority = 0;
    DTERR_C(dttasker_priority_enum_to_native_number(self->config.priority, &native_priority));

    DTERR_C(dtlock_acquire(self->state_lock));
    lock_needs_cleanup_release = true;
    self->ready_posted = false;

    atomic_store_explicit(&self->stop_requested, false, memory_order_release);
    atomic_store_explicit(&self->thread_exited, false, memory_order_release);

    self->thread_joined = false;
    self->ready_posted = false;
    lock_needs_cleanup_release = false;
    DTERR_C(dtlock_release(self->state_lock));

    DTERR_POSERROR_C(pthread_create(&self->thread, &attr, dttasker__thread_inception_function, self));
    self->thread_created = 1;

    dtlog_debug(TAG,
      "%s(): created thread %p for task %s nominal priority %s (native %" PRId32 ")",
      __func__,
      (void*)self->thread,
      self->config.name,
      dttasker_priority_enum_to_string(self->config.priority),
      native_priority);

    // Wait until task signals it started (ready()) or errored out
    DTERR_C(dtsemaphore_wait(self->ready_semaphore, DTTIMEOUT_FOREVER, NULL));

    dtlog_debug(TAG, "%s(): sees ready event for task %s", __func__, self->config.name);

cleanup:
    if (attr_initialized)
    {
        pthread_attr_destroy(&attr);
    }
    if (lock_needs_cleanup_release)
    {
        dterr = dterr_append(dterr, dtlock_release(self->state_lock));
    }
    return dterr;
}

// --------------------------------------------------------------------------------------
// signal that the task is ready (called from within the task)
dterr_t*
dttasker_ready(dttasker_handle self_handle)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = (dttasker_t*)self_handle;
    DTERR_ASSERT_NOT_NULL(self);

    {
        dttasker_info_t info = { .status = RUNNING };
        DTERR_C(dttasker_set_info((dttasker_handle)self, &info));
    }

    dtlog_debug(TAG, "%s(): posting ready for task %s", __func__, self->config.name);

    DTERR_C(dttasker__post_ready_once((dttasker_handle)self));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// request stop of the task (called from outside the task)
dterr_t*
dttasker_stop(dttasker_handle self_handle)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = (dttasker_t*)self_handle;
    DTERR_ASSERT_NOT_NULL(self);

    atomic_store_explicit(&self->stop_requested, true, memory_order_release);

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// wait for task to stop (called from outside the task)
dterr_t*
dttasker_join(dttasker_handle self_handle, dttimeout_millis_t timeout_millis, bool* was_timeout)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = (dttasker_t*)self_handle;
    bool local_timeout = false;
    dtruntime_milliseconds_t start_time = 0;
    dtruntime_milliseconds_t now_time = 0;
    const dtruntime_milliseconds_t sleep_quantum_ms = 10;
    bool thread_created = false;
    bool thread_joined = false;
    pthread_t thread_to_join;
    bool lock_needs_cleanup_release = false;

    DTERR_ASSERT_NOT_NULL(self);

    if (was_timeout != NULL)
    {
        *was_timeout = false;
    }

    // Snapshot whether there is anything to wait for.
    DTERR_C(dtlock_acquire(self->state_lock));
    lock_needs_cleanup_release = true;

    thread_created = (self->thread_created != 0);
    thread_joined = self->thread_joined;
    thread_to_join = self->thread;

    lock_needs_cleanup_release = false;
    DTERR_C(dtlock_release(self->state_lock));

    // Nothing was ever started, or it was already joined.
    if (!thread_created || thread_joined)
    {
        goto cleanup;
    }

    start_time = dtruntime_now_milliseconds();

    for (;;)
    {
        if (atomic_load_explicit(&self->thread_exited, memory_order_acquire))
        {
            break;
        }

        if (timeout_millis != DTTIMEOUT_FOREVER)
        {
            now_time = dtruntime_now_milliseconds();

            if ((now_time - start_time) >= (dtruntime_milliseconds_t)timeout_millis)
            {
                local_timeout = true;
                goto done_waiting;
            }
        }

        dtruntime_sleep_milliseconds(sleep_quantum_ms);
    }

done_waiting:
    if (local_timeout)
    {
        if (was_timeout != NULL)
        {
            *was_timeout = true;
        }
        else
        {
            dterr = dterr_new(DTERR_TIMEOUT,
              DTERR_LOC,
              NULL,
              "task \"%s\" did not join within timeout %" DTTIMEOUT_MILLIS_PRI " milliseconds",
              self->config.name ? self->config.name : "<unnamed task>",
              timeout_millis);
        }
        goto cleanup;
    }

    // Thread has exited. Join it exactly once.
    DTERR_C(dtlock_acquire(self->state_lock));
    lock_needs_cleanup_release = true;

    if (self->thread_joined)
    {
        goto cleanup;
    }

    if (!self->thread_created)
    {
        goto cleanup;
    }

    thread_to_join = self->thread;

    lock_needs_cleanup_release = false;
    DTERR_C(dtlock_release(self->state_lock));

    DTERR_POSERROR_C(pthread_join(thread_to_join, NULL));

    DTERR_C(dtlock_acquire(self->state_lock));
    lock_needs_cleanup_release = true;

    self->thread_joined = true;
    self->thread_created = 0;

    lock_needs_cleanup_release = false;
    DTERR_C(dtlock_release(self->state_lock));

cleanup:
    if (lock_needs_cleanup_release)
    {
        dterr = dterr_append(dterr, dtlock_release(self->state_lock));
    }
    return dterr;
}

// --------------------------------------------------------------------------------------
// poll if task is supposed to stop (called from client code inside the task)
dterr_t*
dttasker_poll(dttasker_handle self_handle, bool* should_stop)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = (dttasker_t*)self_handle;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(should_stop);

    *should_stop = atomic_load_explicit(&self->stop_requested, memory_order_acquire);

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dttasker_set_priority(dttasker_handle self_handle, dttasker_priority_t priority)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = (dttasker_t*)self_handle;
    int32_t native_priority = 0;

    DTERR_C(dttasker_validate_priority_enum(priority));
    DTERR_C(dttasker_priority_enum_to_native_number(priority, &native_priority));

    // TODO: In dttasker_set_priority(), implement proper SCHED_FIFO/SCHED_RR handling in Linux dttasker and remove this check.
    if (self == NULL)
    {
        goto cleanup;
    }

    DTERR_C(dtlock_acquire(self->state_lock));
    self->config.priority = priority;
    DTERR_C(dtlock_release(self->state_lock));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// get current task info/status
dterr_t*
dttasker_get_info(dttasker_handle self_handle, dttasker_info_t* info)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = (dttasker_t*)self_handle;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(info);

    DTERR_C(dtlock_acquire(self->state_lock));

    *info = self->info;

    // always fill the info with the task's originally configured name
    info->name = info->_name;
    strncpy(info->name, self->_name, sizeof(info->_name));
    info->name[sizeof(info->_name) - 1] = '\0';

    DTERR_C(dtlock_release(self->state_lock));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// set current task info/status
dterr_t*
dttasker_set_info(dttasker_handle self_handle, dttasker_info_t* info)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = (dttasker_t*)self_handle;
    dttasker_info_t callback_info;
    bool should_callback = false;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(info);

    DTERR_C(dtlock_acquire(self->state_lock));

    self->info = *info;

    if (self->config.tasker_info_callback != NULL)
    {
        callback_info = self->info;
        callback_info.name = callback_info._name;
        strncpy(callback_info.name, self->_name, sizeof(callback_info._name));
        callback_info.name[sizeof(callback_info._name) - 1] = '\0';
        should_callback = true;
    }

    DTERR_C(dtlock_release(self->state_lock));

    // call outside the lock to avoid callback reentrancy/deadlock
    if (should_callback)
    {
        DTERR_C(self->config.tasker_info_callback(self->config.tasker_info_callback_context, &callback_info));
    }

cleanup:
    return dterr;
}

// ------------------------------------------------------------------------
dterr_t*
dttasker_get_guid(dttasker_handle self_handle, dtguid_t* guid)
{
    dterr_t* dterr = NULL;
    dttasker_t* self = (dttasker_t*)self_handle;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(guid);

    dtguid_copy(guid, &self->guid);

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// release resources held by this dttasker instance
// note this DOES NOT join the thread; caller must ensure thread has exited

void
dttasker_dispose(dttasker_handle self_handle)
{
    dttasker_t* self = (dttasker_t*)self_handle;
    if (!self)
        return;

    dttasker_registry_remove(&dttasker_registry_global_instance, self_handle);

    dtsemaphore_dispose(self->ready_semaphore);
    dtlock_dispose(self->state_lock);

    dtheaper_free(self);
}

// --------------------------------------------------------------------------------------
// TODO: Proper mapping to SCHED_FIFO/SCHED_RR priorities for dttasker_linux.

static dterr_t*
dttasker_priority_enum_to_native_number(dttasker_priority_t p, int32_t* native_priority)
{
    dterr_t* dterr;
    DTERR_ASSERT_NOT_NULL(native_priority);

    switch (p)
    {
        // Stable default: pick the middle of NORMAL band.
        case DTTASKER_PRIORITY_DEFAULT_FOR_SITUATION:
            *native_priority = 8; // NORMAL_MEDIUM
            return NULL;

        // BACKGROUND band
        case DTTASKER_PRIORITY_BACKGROUND_LOWEST:
            *native_priority = 14;
            return NULL;
        case DTTASKER_PRIORITY_BACKGROUND_LOW:
            *native_priority = 13;
            return NULL;
        case DTTASKER_PRIORITY_BACKGROUND_MEDIUM:
            *native_priority = 12;
            return NULL;
        case DTTASKER_PRIORITY_BACKGROUND_HIGH:
            *native_priority = 11;
            return NULL;
        case DTTASKER_PRIORITY_BACKGROUND_HIGHEST:
            *native_priority = 10;
            return NULL;

        // NORMAL band
        case DTTASKER_PRIORITY_NORMAL_LOWEST:
            *native_priority = 9;
            return NULL;
        case DTTASKER_PRIORITY_NORMAL_LOW:
            *native_priority = 8;
            return NULL;
        case DTTASKER_PRIORITY_NORMAL_MEDIUM:
            *native_priority = 7;
            return NULL;
        case DTTASKER_PRIORITY_NORMAL_HIGH:
            *native_priority = 6;
            return NULL;
        case DTTASKER_PRIORITY_NORMAL_HIGHEST:
            *native_priority = 5;
            return NULL;

        // URGENT band
        case DTTASKER_PRIORITY_URGENT_LOWEST:
            *native_priority = 4;
            return NULL;
        case DTTASKER_PRIORITY_URGENT_LOW:
            *native_priority = 3;
            return NULL;
        case DTTASKER_PRIORITY_URGENT_MEDIUM:
            *native_priority = 2;
            return NULL;
        case DTTASKER_PRIORITY_URGENT_HIGH:
            *native_priority = 1;
            return NULL;
        case DTTASKER_PRIORITY_URGENT_HIGHEST:
            *native_priority = 0;
            return NULL;

        // markers / invalid
        case DTTASKER_PRIORITY__START:
        case DTTASKER_PRIORITY__COUNT:
        default:
            dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "unknown dttasker_priority_t value %" PRId32, (int32_t)p);
    }
cleanup:
    return dterr;
}