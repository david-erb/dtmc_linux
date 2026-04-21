/*
 * dtiox_linux_canbus -- Linux SocketCAN backend for the dtiox byte-stream interface.
 *
 * Implements the dtiox vtable over a Linux SocketCAN network interface
 * (e.g. "can0"), providing non-blocking read and write of raw CAN frames
 * via a configurable 11-bit TX identifier and an internal RX ring buffer.
 * Application code accesses the channel through the platform-agnostic
 * dtiox_handle, leaving CAN-specific configuration isolated in the
 * dtiox_linux_canbus_config_t struct.
 *
 * cdox v1.0.2
 */
#pragma once

#include <stdint.h>

#include <dtcore/dterr.h>
#include <dtcore/dtobject.h>

#include <dtmc_base/dtiox.h>

// forward-declare concrete type
typedef struct dtiox_linux_canbus_t dtiox_linux_canbus_t;

typedef struct
{
    dtiox_handle uart_handle;

    // Linux CAN network interface name, e.g. "can0"
    const char* interface_name;

    // CAN identifier used for TX frames.
    // For classic CAN, normally 11-bit (0x000 - 0x7FF).
    // You MAY use extended IDs (set the EFF flag yourself if you later extend).
    uint32_t txid;

    // RX ring buffer capacity in bytes.
    // 0 => use backend default (currently 1024 bytes).
    int32_t rx_ring_capacity;

    // Future extension points (ignored for now, kept for ABI stability).
    // bool use_extended_id;
    // uint32_t rx_filter_id;
} dtiox_linux_canbus_config_t;

extern dterr_t*
dtiox_linux_canbus_create(dtiox_linux_canbus_t** self_ptr);

extern dterr_t*
dtiox_linux_canbus_init(dtiox_linux_canbus_t* self);

extern dterr_t*
dtiox_linux_canbus_configure(dtiox_linux_canbus_t* self, const dtiox_linux_canbus_config_t* cfg);

// -----------------------------------------------------------------------------
// Interface plumbing.

DTIOX_DECLARE_API(dtiox_linux_canbus);
DTOBJECT_DECLARE_API(dtiox_linux_canbus);
