#pragma once

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <modbus/modbus.h>

#include <dtcore/dterr.h>
#include <dtcore/dtringfifo.h>

#include <dtmc_base/dtlock.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtiox_modbus.h>

#include <dtmc/dtiox_linux_modbus_rtu_master.h>

typedef struct dtiox_linux_modbus_rtu_master_stats_t
{
    int32_t future;
} dtiox_linux_modbus_rtu_master_stats_t;

typedef struct dtiox_linux_modbus_rtu_master_t
{
    DTIOX_COMMON_MEMBERS // int32_t model_number

      dtiox_linux_modbus_rtu_master_config_t cfg;

    dtiox_linux_modbus_rtu_master_stats_t stats;

    // libmodbus state
    modbus_t* mb;
    bool _is_connected;

    // facade state
    bool enabled;
    dtsemaphore_handle rx_semaphore;

    // background thread control
    bool stop_requested;

    // to surface on next read()
    bool rx_overflow_pending;
    bool dead_slave_pending;

    // RX FIFO: thread producer, foreground consumer
    dtringfifo_t rx_fifo;
    uint8_t* rx_fifo_storage;

    // RX thread plumbing
    dttasker_handle rxtasker_handle;

    // synchronization between threads
    dtlock_handle lock_handle;

    bool _is_malloced;

} dtiox_linux_modbus_rtu_master_t;

// task entry point
extern dterr_t*
dtiox_linux_modbus_rtu_master__rxtask_entry(void* self_, dttasker_handle tasker_handle);

// ensure connected
extern dterr_t*
dtiox_linux_modbus_rtu_master__ensure_connected(dtiox_linux_modbus_rtu_master_t* self);
void
dtiox_linux_modbus_rtu_master__ensure_disconnected(dtiox_linux_modbus_rtu_master_t* self);
dterr_t*
dtiox_linux_modbus_rtu_master__verify_rxtask_running(dtiox_linux_modbus_rtu_master_t* self, dterr_t* dterr_in);

// lock release helper
dterr_t*
dtiox_linux_modbus_rtu_master__release_lock(dtiox_linux_modbus_rtu_master_t* self, dterr_t* dterr_in);

// errno-based format string tokens
#define DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_FORMAT "error %d (%s)"
#define DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_ARGS() errno, modbus_strerror(errno)

#define DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_C(call)                                                                            \
    do                                                                                                                         \
    {                                                                                                                          \
        int err;                                                                                                               \
        if ((err = (call)) == -1)                                                                                              \
        {                                                                                                                      \
            dterr = dterr_new(DTERR_IO,                                                                                        \
              DTERR_LOC,                                                                                                       \
              NULL,                                                                                                            \
              DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_FORMAT,                                                                      \
              DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_ARGS());                                                                     \
            goto cleanup;                                                                                                      \
        }                                                                                                                      \
    } while (0)
