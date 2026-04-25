/*
 * dtiox_linux_modbus_tcp_master -- Modbus TCP master backend for the dtiox interface.
 *
 * Wraps libmodbus in client mode behind the dtiox vtable, connecting to a
 * remote Modbus TCP slave by hostname and port. A background polling thread
 * handles register reads; configurable unit ID, poll interval, response
 * timeout, and RX FIFO size let the caller tune throughput and reliability
 * without touching the transport-agnostic dtiox call sites.
 *
 * cdox v1.0.2
 */
#pragma once

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dtcore/dtobject.h>

#include <dtmc_base/dtiox.h>

#include <dtcore/dterr.h> // dterr_t, DTERR_* codes :contentReference[oaicite:2]{index=2}

// forward-declare concrete type
typedef struct dtiox_linux_modbus_tcp_master_t dtiox_linux_modbus_tcp_master_t;

typedef struct dtiox_linux_modbus_tcp_master_config_t
{
    // Required: IPv4/IPv6 string or hostname accepted by libmodbus modbus_new_tcp()
    const char* ip;

    // 0 => DTIOX_MODBUS_TCP_DEFAULT_PORT
    int32_t port;

    // Modbus "unit id" (useful with bridges/gateways). For pure Modbus/TCP slaves, 1 is common.
    int32_t unit_id; // 1..247 typically; 0 => default to 1

    // Polling period for the RX thread.
    int32_t poll_interval_ms; // 0 => default (e.g. 25ms)

    // Per-modbus-call response timeout used by libmodbus.
    int32_t response_timeout_ms; // 0 => default (e.g. 250ms)

    // Max blob size accepted for write/read buffering (clamped to DTIOX_MODBUS_MAX_BLOB_BYTES).
    int32_t max_blob_bytes; // 0 => DTIOX_MODBUS_MAX_BLOB_BYTES

    // Internal RX FIFO capacity in bytes (not wire). Must be >= max_blob_bytes.
    int32_t rx_ring_capacity; // 0 => default (e.g. 4096)

} dtiox_linux_modbus_tcp_master_config_t;

extern dterr_t*
dtiox_linux_modbus_tcp_master_create(dtiox_linux_modbus_tcp_master_t** self_ptr);

extern dterr_t*
dtiox_linux_modbus_tcp_master_init(dtiox_linux_modbus_tcp_master_t* self);

extern dterr_t*
dtiox_linux_modbus_tcp_master_configure(dtiox_linux_modbus_tcp_master_t* self,
  const dtiox_linux_modbus_tcp_master_config_t* cfg);

// -----------------------------------------------------------------------------
// Interface plumbing.

DTIOX_DECLARE_API(dtiox_linux_modbus_tcp_master);
DTOBJECT_DECLARE_API(dtiox_linux_modbus_tcp_master);
