/*
 * dtiox_linux_tcp -- Linux TCP client/server backend for the dtiox interface.
 *
 * Implements the dtiox vtable over a POSIX TCP socket, operating in either
 * client (outbound connect) or server (listen/accept) mode selected at
 * configuration time. Tunable options include Nagle disabling, keepalive,
 * connect and accept timeouts, and RX FIFO capacity, all isolated in
 * dtiox_linux_tcp_config_t so transport code is unchanged when the
 * underlying connection mode changes.
 *
 * cdox v1.0.2
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dtcore/dterr.h>
#include <dtcore/dtobject.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dttasker.h>

typedef struct dtiox_linux_tcp_t dtiox_linux_tcp_t;

typedef enum dtiox_linux_tcp_mode_t
{
    DTIOX_LINUX_TCP_MODE_CLIENT = 0,
    DTIOX_LINUX_TCP_MODE_SERVER = 1,
} dtiox_linux_tcp_mode_t;

typedef struct dtiox_linux_tcp_config_t
{
    dtiox_linux_tcp_mode_t mode;

    const char* remote_host;
    int32_t remote_port;

    const char* local_bind_host;
    int32_t local_bind_port;

    int32_t rx_fifo_capacity;

    bool tcp_nodelay;
    bool keepalive;
    int32_t connect_timeout_ms;
    int32_t accept_timeout_ms;

    dttasker_info_callback_t tasker_info_callback_fn;
    void* tasker_info_callback_context;

} dtiox_linux_tcp_config_t;

extern dterr_t*
dtiox_linux_tcp_create(dtiox_linux_tcp_t** self_ptr);

extern dterr_t*
dtiox_linux_tcp_init(dtiox_linux_tcp_t* self);

extern dterr_t*
dtiox_linux_tcp_configure(dtiox_linux_tcp_t* self, const dtiox_linux_tcp_config_t* cfg);

DTIOX_DECLARE_API(dtiox_linux_tcp);
DTOBJECT_DECLARE_API(dtiox_linux_tcp);