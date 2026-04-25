#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dtcore/dterr.h>
#include <dtmc/dtiox_linux_tcp.h>

typedef enum main_mode_e
{
    MAIN_MODE_NONE = 0,
    MAIN_MODE_TCP_MASTER, // TCP client
    MAIN_MODE_TCP_SLAVE,  // TCP server
} main_mode_t;

typedef struct main_config_s
{
    main_mode_t mode;

    // TCP endpoint
    // master: remote host to connect to
    // slave: optional local bind host (NULL => 0.0.0.0)
    const char* host;
    int port;

    // TCP options
    bool tcp_nodelay;
    bool keepalive;
    int32_t rx_fifo_capacity;
    int32_t connect_timeout_ms;
    int32_t accept_timeout_ms;

    // benchmark options
    int32_t message_count;
    int32_t payload_size;

    // parser state
    bool help_requested;

} main_config_t;

void
main_config_init(main_config_t* cfg);

dterr_t*
main_parse_args(int argc, char** argv, main_config_t* cfg);

dterr_t*
main_build_tcp_config(const main_config_t* cfg, dtiox_linux_tcp_config_t* tcp_cfg);

void
main_usage(const char* exe_name);