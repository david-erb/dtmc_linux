/*
 * dtiox_linux_websocket -- Linux WebSocket server backend for the dtiox interface.
 *
 * Implements the dtiox vtable over an HTTP-upgrade WebSocket listener. The
 * server accepts a single client connection per attach cycle, performs the
 * WebSocket handshake within a configurable timeout, then exposes the framed
 * stream through the standard dtiox read/write path. TCP keepalive and Nagle
 * options are configurable. A minimal dtobject-style identity API supports
 * logging and equality comparisons.
 *
 * cdox v1.0.2
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dtcore/dterr.h>
#include <dtmc_base/dtiox.h>
#include <dtmc_base/dttasker.h>

typedef struct dtiox_linux_websocket_t dtiox_linux_websocket_t;

typedef struct dtiox_linux_websocket_config_t
{
    const char* local_bind_host; // e.g. "0.0.0.0"
    int32_t local_bind_port;     // e.g. 14090

    bool keepalive;
    bool tcp_nodelay;

    // How long attach() will wait for the HTTP upgrade request after accept().
    // 0 => no timeout.
    int32_t handshake_timeout_ms;

    dttasker_info_callback_t tasker_info_callback_fn;
    void* tasker_info_callback_context;

} dtiox_linux_websocket_config_t;

dterr_t*
dtiox_linux_websocket_create(dtiox_linux_websocket_t** out);
dterr_t*
dtiox_linux_websocket_init(dtiox_linux_websocket_t* self);
dterr_t*
dtiox_linux_websocket_configure(dtiox_linux_websocket_t* self, const dtiox_linux_websocket_config_t* cfg);

dterr_t*
dtiox_linux_websocket_attach(dtiox_linux_websocket_t* self DTIOX_ATTACH_ARGS);
dterr_t*
dtiox_linux_websocket_detach(dtiox_linux_websocket_t* self DTIOX_DETACH_ARGS);

dterr_t*
dtiox_linux_websocket_enable(dtiox_linux_websocket_t* self DTIOX_ENABLE_ARGS);
dterr_t*
dtiox_linux_websocket_read(dtiox_linux_websocket_t* self DTIOX_READ_ARGS);
dterr_t*
dtiox_linux_websocket_write(dtiox_linux_websocket_t* self DTIOX_WRITE_ARGS);
dterr_t*
dtiox_linux_websocket_set_rx_semaphore(dtiox_linux_websocket_t* self DTIOX_SET_RX_SEMAPHORE_ARGS);
dterr_t*
dtiox_linux_websocket_concat_format(dtiox_linux_websocket_t* self DTIOX_CONCAT_FORMAT_ARGS);

void
dtiox_linux_websocket_dispose(dtiox_linux_websocket_t* self);

// dtobject-ish helpers
void
dtiox_linux_websocket_copy(dtiox_linux_websocket_t* this, dtiox_linux_websocket_t* that);
bool
dtiox_linux_websocket_equals(dtiox_linux_websocket_t* a, dtiox_linux_websocket_t* b);
const char*
dtiox_linux_websocket_get_class(dtiox_linux_websocket_t* self);
bool
dtiox_linux_websocket_is_iface(dtiox_linux_websocket_t* self, const char* iface_name);
void
dtiox_linux_websocket_to_string(dtiox_linux_websocket_t* self, char* buffer, size_t buffer_size);
