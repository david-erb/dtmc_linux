/*
 * dtiox_linux_modbus_rtu_master -- Modbus RTU master backend for the dtiox interface.
 *
 * Wraps libmodbus in RTU mode behind the dtiox vtable, communicating with a
 * serial Modbus slave over a Linux tty device. Configuration covers baud rate
 * and framing via dtuart_helper_config_t, slave ID, poll interval, per-call
 * response and inter-byte timeouts, RS-485 mode, and RTS toggling for hardware
 * direction control. Large payloads can be sent in chunks when partial-write
 * retries are enabled.
 *
 * cdox v1.0.2
 */
#pragma once

#include <stdint.h>

#include <dtcore/dterr.h>
#include <dtcore/dtobject.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtuart_helpers.h>

// forward-declare concrete type
typedef struct dtiox_linux_modbus_rtu_master_t dtiox_linux_modbus_rtu_master_t;

typedef struct dtiox_linux_modbus_rtu_master_config_t
{
    // Required: serial device path, e.g. "/dev/ttyUSB0" or "/dev/serial0"
    const char* device;

    dtuart_helper_config_t uart_config;

    // Modbus RTU slave id (unit id)
    int32_t slave_id; // 1..247 ; 0 => default 1

    // Polling period for RX thread
    int32_t poll_interval_ms; // 0 => default 25

    // Per-modbus-call response timeout
    int32_t response_timeout_ms; // 0 => default 250

    // Optional byte timeout (inter-byte) for RTU framing robustness
    int32_t byte_timeout_ms; // 0 => leave libmodbus default

    // Max blob size accepted for write/read buffering (clamped to DTIOX_MODBUS_MAX_BLOB_BYTES).
    int32_t max_blob_bytes; // 0 => DTIOX_MODBUS_MAX_BLOB_BYTES

    // permit retries on partial writes, so that large blobs will be sent in chunks
    bool permit_retry_partial_writes; // default false

    // Internal RX FIFO capacity in bytes (not wire). Must be >= max_blob_bytes.
    int32_t rx_ring_capacity; // 0 => default (e.g. 1024)

    // If true, try to configure libmodbus for RS485 mode (where supported).
    // This is helpful when using USB-RS485 adapters or UARTs with RTS-based DE control.
    bool rs485_mode; // default true

    // If true, enable libmodbus RTS toggling (where supported) for RS485 direction control.
    // If your hardware auto-controls DE/RE, set false.
    bool rts_toggle; // default true

} dtiox_linux_modbus_rtu_master_config_t;

extern dterr_t*
dtiox_linux_modbus_rtu_master_create(dtiox_linux_modbus_rtu_master_t** self_ptr);

extern dterr_t*
dtiox_linux_modbus_rtu_master_init(dtiox_linux_modbus_rtu_master_t* self);

extern dterr_t*
dtiox_linux_modbus_rtu_master_configure(dtiox_linux_modbus_rtu_master_t* self,
  const dtiox_linux_modbus_rtu_master_config_t* cfg);

// -----------------------------------------------------------------------------
// Interface plumbing.

DTIOX_DECLARE_API(dtiox_linux_modbus_rtu_master);
DTOBJECT_DECLARE_API(dtiox_linux_modbus_rtu_master);
