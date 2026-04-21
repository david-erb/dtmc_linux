#pragma once

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <modbus/modbus.h>

#include <dtcore/dterr.h>
#include <dtcore/dtringfifo.h>

#include <dtmc_base/dtlock.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtiox_modbus.h>

// public header (you'll add this in dtmc/ include path)
#include <dtmc/dtiox_linux_modbus_tcp_slave.h>

typedef struct dtiox_linux_modbus_tcp_slave_t
{
    DTIOX_COMMON_MEMBERS // int32_t model_number

      dtiox_linux_modbus_tcp_slave_config_t cfg;

    // libmodbus server state
    modbus_t* mb;
    modbus_mapping_t* mb_map;
    int listening_socket; // listening socket, used by modbus_tcp_accept
    int session_socket;   // current session socket, used by modbus_receive

    // facade state
    bool enabled;
    dtsemaphore_handle rx_semaphore;

    // background thread/task control
    bool rxtask_stop_requested;
    bool session_stop_requested;

    // "surface on next read()"
    bool rx_overflow_pending;

    // RX FIFO: task producer, foreground consumer
    dtringfifo_t rx_fifo;
    uint8_t* rx_fifo_storage;

    // RX thread plumbing
    dttasker_handle rxtasker_handle;
    int rxtasker_at_line;

    // TX "publish" (single outstanding message to return on next poll)
    bool tx_pending;
    int32_t tx_len;
    uint8_t tx_bytes[DTIOX_MODBUS_MAX_BLOB_BYTES];

    // synchronization
    dtlock_handle lock_handle;

    bool _is_malloced;

} dtiox_linux_modbus_tcp_slave_t;

// task entry point (blocking receive + reply)
extern dterr_t*
dtiox_linux_modbus_tcp_slave__rxtask_entry(void* self_, dttasker_handle tasker_handle);

// lock release helper
dterr_t*
dtiox_linux_modbus_tcp_slave__release_lock(dtiox_linux_modbus_tcp_slave_t* self, dterr_t* dterr_in);

// -----------------------------------------------------------------------------
// register map helpers
extern void
dtiox_linux_modbus_tcp_slave__prepare_poll_response(dtiox_linux_modbus_tcp_slave_t* self);

extern void
dtiox_linux_modbus_tcp_slave__consume_put_blob(dtiox_linux_modbus_tcp_slave_t* self);

extern dterr_t*
dtiox_linux_modbus_tcp_slave__verify_rxtask_running(dtiox_linux_modbus_tcp_slave_t* self);
extern dterr_t*
dtiox_linux_modbus_tcp_slave__verify_rxtask_not_running(dtiox_linux_modbus_tcp_slave_t* self);

// -----------------------------------------------------------------------------

// errno-based format string tokens
#define dtiox_linux_modbus_tcp_slave_ERRNO_FORMAT "error %d (%s)"
#define dtiox_linux_modbus_tcp_slave_ERRNO_ARGS() errno, modbus_strerror(errno)

// libmodbus sometimes returns -1 and sets errno; treat that as DTERR_FAIL
#define dtiox_linux_modbus_tcp_slave_ERRNO_C(call)                                                                             \
    do                                                                                                                         \
    {                                                                                                                          \
        int err;                                                                                                               \
        if ((err = (call)) == -1)                                                                                              \
        {                                                                                                                      \
            dterr = dterr_new(DTERR_IO,                                                                                        \
              DTERR_LOC,                                                                                                       \
              NULL,                                                                                                            \
              dtiox_linux_modbus_tcp_slave_ERRNO_FORMAT,                                                                       \
              dtiox_linux_modbus_tcp_slave_ERRNO_ARGS());                                                                      \
            goto cleanup;                                                                                                      \
        }                                                                                                                      \
    } while (0)
