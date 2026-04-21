/*
 * dthttpd_linux_socket -- POSIX socket HTTP server backend for the dthttpd interface.
 *
 * Implements the dthttpd vtable using POSIX sockets and the dttasker thread
 * model. An accept task listens on a configurable host and port; per-connection
 * child tasks serve static files from one or more filesystem roots for GET
 * requests and deliver POST payloads to an application callback that returns
 * a heap-allocated response. Concurrency limits, stack sizes, and scheduler
 * priorities for both accept and child tasks are exposed through
 * dthttpd_linux_socket_config_t.
 *
 * cdox v1.0.2
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dtcore/dterr.h>
#include <dtcore/dttimeout.h>

#include <dtmc_base/dthttpd.h>
#include <dtmc_base/dtlock.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>

// --------------------------------------------------------------------------------------
// forward declarations

typedef struct dthttpd_linux_socket_t dthttpd_linux_socket_t;

// --------------------------------------------------------------------------------------

typedef struct dthttpd_linux_socket_config_t
{
    const char* bind_host;              // default "0.0.0.0"
    int32_t bind_port;                  // required 1..65535
    int32_t listen_backlog;             // default 16
    int32_t max_concurrent_connections; // default 10

    // GET static file roots
    const char** static_directories;
    int32_t static_directory_count;

    // POST callback
    dthttpd_post_callback_t post_callback;
    void* post_callback_context;

    // optional dttasker reporting hook
    dttasker_info_callback_t tasker_info_callback_fn;
    void* tasker_info_callback_context;

    // optional child task tuning
    int32_t child_stack_size;
    dttasker_priority_t child_priority;

    // optional accept task tuning
    int32_t accept_stack_size;
    dttasker_priority_t accept_priority;

} dthttpd_linux_socket_config_t;

// --------------------------------------------------------------------------------------
// class-style API

extern dterr_t*
dthttpd_linux_socket_create(dthttpd_linux_socket_t** out);

extern dterr_t*
dthttpd_linux_socket_init(dthttpd_linux_socket_t* self);

extern dterr_t*
dthttpd_linux_socket_configure(dthttpd_linux_socket_t* self, const dthttpd_linux_socket_config_t* config);

// -----------------------------------------------------------------------------
// Interface plumbing.

DTHTTPD_DECLARE_API(dthttpd_linux_socket);
