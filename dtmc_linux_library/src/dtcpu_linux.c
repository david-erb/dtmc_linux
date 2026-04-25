// dtcpu_linux.c — simple & fast

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

#include <dtcore/dterr.h>

#include <dtmc_base/dtcpu.h>

static bool g_clock_initialized = false;

// Choose a monotonic clock (RAW if available, else MONOTONIC)
#ifndef DTCPU_LINUX_CLOCK_ID
#ifdef CLOCK_MONOTONIC_RAW
#define DTCPU_LINUX_CLOCK_ID CLOCK_MONOTONIC_RAW
#else
#define DTCPU_LINUX_CLOCK_ID CLOCK_MONOTONIC
#endif
#endif

// --------------------------------------------------------------------------------------
// Initialize microsecond ticker for POSIX/Linux.
// No special hardware setup is required; we just sanity-check the clock.
dterr_t*
dtcpu_sysinit(void)
{
    if (g_clock_initialized)
    {
        return NULL;
    }

    struct timespec res;
    if (clock_getres(DTCPU_LINUX_CLOCK_ID, &res) != 0)
    {
        return dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "clock_getres failed: %d", errno);
    }

    // Not strictly required, but nice to know we have decent resolution.
    // We don't fail if resolution is coarse; we just proceed.
    g_clock_initialized = true;
    return NULL;
}

// ----------------------------------------------------------------------------------
// Returns a monotonically increasing microsecond timestamp (since an
// unspecified epoch). Safe for long intervals on 64-bit; use
// dtcpu_linux_subtract_microseconds() for deltas.
void
dtcpu_mark(dtcpu_t* m)
{
    if (!g_clock_initialized)
    {
        (void)dtcpu_sysinit();
    }

    struct timespec ts;
    (void)clock_gettime(DTCPU_LINUX_CLOCK_ID, &ts);

    m->old = m->new;
    m->new = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

// -----------------------------------------------------------------------------
uint64_t
dtcpu_elapsed_microseconds(const dtcpu_t* m)
{
    // Unsigned subtraction handles single wrap transparently
    return m->new - m->old;
}

// -----------------------------------------------------------------------------
void
dtcpu_busywait_microseconds(uint64_t microseconds)
{
    struct timespec req, rem;
    req.tv_sec = (time_t)(microseconds / 1000000ULL);
    req.tv_nsec = (long)((microseconds % 1000000ULL) * 1000ULL);

    while (nanosleep(&req, &rem) == -1 && errno == EINTR)
    {
        req = rem;
    }
}

// -----------------------------------------------------------------------------
int32_t
dtcpu_random_int32(void)
{
    int32_t value;
    ssize_t rc = getrandom(&value, sizeof(value), 0);

    if (rc != sizeof(value))
    {
        return (int32_t)rand(); // lame fallback
    }

    return value;
}