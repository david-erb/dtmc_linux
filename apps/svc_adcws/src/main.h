#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dtcore/dterr.h>

typedef enum main_mode_e
{
    MAIN_MODE_NONE = 0,
    MAIN_MODE_TCP_MASTER, // TCP client
    MAIN_MODE_TCP_SLAVE,  // TCP server
} main_mode_t;

typedef struct main_config_t
{
    // tcp socket input
    const char* rx_host;
    int rx_port;

    // websocket output
    const char* tx_host;
    int tx_port;

    bool verbose;
    bool help_requested;

} main_config_t;

void
main_config_init(main_config_t* cfg);

dterr_t*
main_parse_args(int argc, char** argv, main_config_t* cfg);

void
main_usage(const char* exe_name);