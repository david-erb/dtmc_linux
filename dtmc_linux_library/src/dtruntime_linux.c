#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <dtcore/dterr.h>
#include <dtcore/dtstr.h>
#include <dtmc_base/dttasker_registry.h>

#include <dtmc_base/dtcpu.h>
#include <dtmc_base/dtruntime.h>

#include <dtmc/dtmc_linux.h>

static bool g_clock_initialized = false;

// Choose a monotonic clock (RAW if available, else MONOTONIC)
#ifndef DTTICKER_CLOCK_ID
#ifdef CLOCK_MONOTONIC_RAW
#define DTTICKER_CLOCK_ID CLOCK_MONOTONIC_RAW
#else
#define DTTICKER_CLOCK_ID CLOCK_MONOTONIC
#endif
#endif

// --------------------------------------------------------------------------------------
const char*
dtruntime_flavor(void)
{
    return DTMC_LINUX_FLAVOR;
}

// --------------------------------------------------------------------------------------
const char*
dtruntime_version(void)
{
    return DTMC_LINUX_VERSION;
}

// --------------------------------------------------------------------------------------
bool
dtruntime_is_qemu()
{
    return false;
}

// --------------------------------------------------------------------------------------
// Initialize microsecond ticker for POSIX/Linux.
// No special hardware setup is required; we just sanity-check the clock.
static dterr_t*
dtruntime_init_microseconds(void)
{
    if (g_clock_initialized)
    {
        return NULL;
    }

    struct timespec res;
    if (clock_getres(DTTICKER_CLOCK_ID, &res) != 0)
    {
        return dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "clock_getres failed: %d", errno);
    }

    // Not strictly required, but nice to know we have decent resolution.
    // We don't fail if resolution is coarse; we just proceed.
    g_clock_initialized = true;
    return NULL;
}

// --------------------------------------------------------------------------------------
extern dtruntime_milliseconds_t
dtruntime_now_milliseconds()
{
    if (!g_clock_initialized)
    {
        (void)dtruntime_init_microseconds();
    }

    struct timespec ts;
    (void)clock_gettime(DTTICKER_CLOCK_ID, &ts);

    uint64_t ms = (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
    return (dtruntime_milliseconds_t)ms;
}

// --------------------------------------------------------------------------------------
extern void
dtruntime_sleep_milliseconds(dtruntime_milliseconds_t milliseconds)
{
    if (milliseconds <= 0)
    {
        // Let other threads run
        sched_yield();
        return;
    }

    struct timespec req;
    req.tv_sec = (time_t)(milliseconds / 1000);
    req.tv_nsec = (long)((milliseconds % 1000) * 1000000L);

    // Loop in case of EINTR
    while (nanosleep(&req, &req) == -1 && errno == EINTR)
    {
        // continue sleeping the remaining time
    }
}

// --------------------------------------------------------------------------------------
dterr_t*
dtruntime_format_environment_as_table(char** out_string)
{
    dterr_t* dterr = NULL;
    *out_string = NULL;

    char* s = NULL;
    char* t = "\n";
    char* p = "    ";

    const char* item_format_str = "%s%-40s %-24s";
    const char* item_format_int = "%s%-40s %" PRIu64;
    const char* item_format_double = "%s%-40s %g";

    s = dtstr_concat_format(s, t, item_format_str, p, "Flavor", dtruntime_flavor());
    s = dtstr_concat_format(s, t, item_format_str, p, "Version", dtruntime_version());

    s = dtstr_concat_format(s, t, item_format_str, p, "Is QEMU", dtruntime_is_qemu() ? "Yes" : "No");
    s = dtstr_concat_format(s, t, item_format_int, p, "Clock ID", (uint64_t)DTTICKER_CLOCK_ID);

    *out_string = s;

    return dterr;
}

// --------------------------------------------------------------------------------------
// On Linux there is no OS-level task enumeration, so we copy from the global singleton
// that dttasker_linux populates automatically on create/dispose.
dterr_t*
dtruntime_register_tasks(dttasker_registry_t* registry)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(registry);

    if (!registry->is_initialized)
    {
        DTERR_C(dttasker_registry_init(registry));
    }

    dttasker_registry_t* global = &dttasker_registry_global_instance;
    if (!global->is_initialized)
        goto cleanup;

    for (int i = 0; i < global->pool.max_items; i++)
    {
        dtguidable_handle item = global->pool.items[i];
        if (item == NULL)
            continue;
        DTERR_C(dttasker_registry_insert(registry, (dttasker_handle)item));
    }

cleanup:
    return dterr;
}