/*
 * dtiox_linux_tty -- Linux TTY serial backend for the dtiox interface.
 *
 * Implements the dtiox vtable for a POSIX serial port, applying baud rate,
 * parity, data bits, stop bits, and flow control through the shared
 * dtuart_helper_config_t. A configurable RX FIFO and an optional tasker
 * info callback provide buffering and runtime diagnostics. The same
 * dtiox_handle abstraction lets callers swap the TTY backend for Modbus,
 * CAN, or TCP without modifying surrounding I/O logic.
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
typedef struct dtiox_linux_tty_t dtiox_linux_tty_t;

// -----------------------------------------------------------------------------
// Backend-specific config
// -----------------------------------------------------------------------------

typedef struct dtiox_linux_tty_config_t
{
    const char* device_path; // e.g. "/dev/ttyUSB0"

    dtuart_helper_config_t uart_config;

    int32_t rx_fifo_capacity;

    dttasker_info_callback_t tasker_info_callback_fn;
    void* tasker_info_callback_context;

} dtiox_linux_tty_config_t;

// -----------------------------------------------------------------------------
extern dterr_t*
dtiox_linux_tty_create(dtiox_linux_tty_t** self_ptr);

extern dterr_t*
dtiox_linux_tty_init(dtiox_linux_tty_t* self);

extern dterr_t*
dtiox_linux_tty_configure(dtiox_linux_tty_t* self, const dtiox_linux_tty_config_t* cfg);

// -----------------------------------------------------------------------------
// Interface plumbing.

DTIOX_DECLARE_API(dtiox_linux_tty);
DTOBJECT_DECLARE_API(dtiox_linux_tty);
