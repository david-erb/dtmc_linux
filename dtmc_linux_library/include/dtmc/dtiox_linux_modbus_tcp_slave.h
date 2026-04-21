/*
 * dtiox_linux_modbus_tcp_slave -- Modbus TCP slave backend for the dtiox interface.
 *
 * Wraps libmodbus in server mode behind the dtiox vtable, listening on a
 * configurable TCP port. Incoming Modbus register traffic is presented as a
 * byte stream through the standard dtiox read/write path, with an internal
 * RX FIFO whose depth and response timeout are set at configuration time.
 *
 * cdox v1.0.2
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dtcore/dterr.h>
#include <dtcore/dtobject.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>

// Public config for passing in settings.
typedef struct dtiox_linux_modbus_tcp_slave_config_t
{
    // TCP listen port. If 0, defaults to DTIOX_MODBUS_TCP_DEFAULT_PORT.
    int32_t port;

    // Max blob bytes to accept/serve (clamped to DTIOX_MODBUS_MAX_BLOB_BYTES).
    int32_t max_blob_bytes;

    // RX fifo storage size (bytes). If 0, choose a default.
    int32_t rx_ring_capacity;

    // Optional: response timeout configuration for libmodbus accept/receive paths (ms).
    // If <= 0, libmodbus defaults are used.
    int32_t response_timeout_ms;

} dtiox_linux_modbus_tcp_slave_config_t;

typedef struct dtiox_linux_modbus_tcp_slave_t dtiox_linux_modbus_tcp_slave_t;

// Creation / init
dterr_t*
dtiox_linux_modbus_tcp_slave_create(dtiox_linux_modbus_tcp_slave_t** self_ptr);

dterr_t*
dtiox_linux_modbus_tcp_slave_init(dtiox_linux_modbus_tcp_slave_t* self);

// Configure
dterr_t*
dtiox_linux_modbus_tcp_slave_configure(dtiox_linux_modbus_tcp_slave_t* self,
  const dtiox_linux_modbus_tcp_slave_config_t* config);

// -----------------------------------------------------------------------------
// Interface plumbing.

DTIOX_DECLARE_API(dtiox_linux_modbus_tcp_slave);
DTOBJECT_DECLARE_API(dtiox_linux_modbus_tcp_slave);