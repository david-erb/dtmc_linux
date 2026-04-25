#pragma once

#include <stdbool.h>

#include <dtcore/dterr.h>

// --------------------------------------------------------------------------------------
// MAIN config

typedef enum main_mode_e
{
    MAIN_MODE_NONE = 0,
    MAIN_MODE_RTU_MASTER,
    MAIN_MODE_TCP_MASTER,
    MAIN_MODE_TCP_SLAVE,
} main_mode_t;

typedef struct main_config_s
{
    main_mode_t mode;

    // common
    int port;     // TCP master/slave
    int slave_id; // RTU master

    // RTU
    const char* device;

    // TCP master
    const char* ip;

    // message count to send in the benchmark
    int32_t message_count;

    // message payload size to send in the benchmark
    int32_t payload_size;

} main_config_t;

// --------------------------------------------------------------------------------------
// API

void
main_config_init(main_config_t* cfg);

/**
 * Parse argv into cfg.
 *
 * Returns:
 *   NULL on success.
 *   dterr_t* on failure (DTERR_BADARG, DTERR_RANGE, etc).
 *
 * Notes:
 * - This function prints usage on --help (stdout) and returns NULL.
 * - It does not mutate global state except getopt's optind, which it resets.
 */
dterr_t*
main_parse_args(int argc, char** argv, main_config_t* cfg);

/**
 * Print usage text.
 * Kept separate so main() can call it if desired.
 */
void
main_usage(const char* exe_name);