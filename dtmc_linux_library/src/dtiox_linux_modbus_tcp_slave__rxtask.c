#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dtmc_base/dtmc_base_constants.h>

#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtobject.h>
#include <dtcore/dtringfifo.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtlock.h>
#include <dtmc_base/dtmodbus_helpers.h>
#include <dtmc_base/dtruntime.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>

#include <dtmc/dtiox_linux_modbus_tcp_slave.h>

#include "dtiox_linux_modbus_tcp_slave__private.h"

#define TAG "dtiox_linux_modbus_tcp_slave"

// -----------------------------------------------------------------------------
static dterr_t*
dtiox_linux_modbus_tcp_slave__rxtask_server_loop(dtiox_linux_modbus_tcp_slave_t* self);
static dterr_t*
dtiox_linux_modbus_tcp_slave__rxtask_session_loop(dtiox_linux_modbus_tcp_slave_t* self);

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave__rxtask_entry(void* self_, dttasker_handle tasker_handle)
{
    dtiox_linux_modbus_tcp_slave_t* self = (dtiox_linux_modbus_tcp_slave_t*)self_;
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(tasker_handle);

    DTERR_C(dttasker_ready(tasker_handle));

    DTERR_C(dtiox_linux_modbus_tcp_slave__rxtask_server_loop(self));

cleanup:
    if (dterr)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, dterr, "rx task exiting");
    }

    return dterr;
}

// -----------------------------------------------------------------------------
static dterr_t*
dtiox_linux_modbus_tcp_slave__rxtask_server_loop(dtiox_linux_modbus_tcp_slave_t* self)
{
    dterr_t* dterr = NULL;

    while (1)
    {
        // Snapshot stop/enabled/attached under lock.
        DTERR_C(dtlock_acquire(self->lock_handle));
        bool rxtask_stop_requested = self->rxtask_stop_requested;
        int listening_socket = self->listening_socket;
        DTERR_C(dtlock_release(self->lock_handle));

        if (rxtask_stop_requested)
            break;

        // Accept a client if needed (blocks). We do not hold the lock across blocking calls.
        // modbus_tcp_accept updates the modbus context's socket internally.
        self->session_socket = listening_socket;

        dtlog_debug(TAG, "rx task server loop waiting for client connection on listening socket %d", (int)listening_socket);

        if (modbus_tcp_accept(self->mb, &self->session_socket) == -1)
        {
            dtlog_debug(TAG, "modbus_tcp_accept failed: %s", strerror(errno));
            self->session_socket = -1;
            // Accept failures are typically transient; keep looping unless stop requested.
            dtruntime_sleep_milliseconds(25);
            continue;
        }

        // Keep the listening socket value in self current (some libmodbus patterns do this).
        // DTERR_C(dtlock_acquire(self->lock_handle));
        // self->listening_socket = session_socket;
        // DTERR_C(dtlock_release(self->lock_handle));

        dtlog_debug(TAG, "Modbus TCP client connection accepted on session socket %d", self->session_socket);
        dterr = dtiox_linux_modbus_tcp_slave__rxtask_session_loop(self);

        if (dterr != NULL)
        {
            dtlog_debug(TAG, "rx task session loop exited: %s", dterr->message);
            dterr_dispose(dterr);
            dterr = NULL;
        }

    cleanup:
        if (dterr)
        {
            dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "rx task server loop exiting due to error");
        }
        else
        {
            dtlog_debug(TAG, "rx task server loop exiting normally on rxtask_stop_requested=%d", (int)rxtask_stop_requested);
        }

        return dterr;
    }
}

// -----------------------------------------------------------------------------
static dterr_t*
dtiox_linux_modbus_tcp_slave__rxtask_session_loop(dtiox_linux_modbus_tcp_slave_t* self)
{
    dterr_t* dterr = NULL;

    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];

    // Client session loop: receive requests, optionally modify mapping, reply.
    while (1)
    {
        // Allow detach to break the blocking receive loop: libmodbus receive will fail once socket closes.

        DTERR_C(dtlock_acquire(self->lock_handle));
        bool rxtask_stop_requested = self->rxtask_stop_requested;
        bool session_stop_requested = self->session_stop_requested;
        DTERR_C(dtlock_release(self->lock_handle));

        if (rxtask_stop_requested || session_stop_requested)
            break;

        // block until a query is received
        int rc = modbus_receive(self->mb, query);

        if (rc > 0)
        {
            // Always reply at Modbus protocol level FIRST.
            // For function codes that WRITE registers (0x10/0x06), libmodbus updates mb_map inside modbus_reply().

            dtiox_linux_modbus_tcp_slave_ERRNO_C(modbus_reply(self->mb, query, rc, self->mb_map));

            // Under lock: interpret command (now that mb_map reflects the write),
            // consume PUT_BLOB into rx_fifo, and prepare S2M response for poll cmd.

            DTERR_C(dtlock_acquire(self->lock_handle));

            // If the master did PUT_BLOB, consume it into rx_fifo.
            dtiox_linux_modbus_tcp_slave__consume_put_blob(self);

            // If the master is polling (GIVE_ME_ANY_DATA), prepare the S2M registers.
            uint16_t cmd = self->mb_map->tab_registers[DTIOX_MODBUS_REG_M2S_CMD];
            if (cmd == (uint16_t)DTIOX_MODBUS_CMD_GIVE_ME_ANY_DATA)
            {
                dtiox_linux_modbus_tcp_slave__prepare_poll_response(self);

                // Clear the poll command after preparing response.
                self->mb_map->tab_registers[DTIOX_MODBUS_REG_M2S_CMD] = (uint16_t)DTIOX_MODBUS_CMD_NONE;
                self->mb_map->tab_registers[DTIOX_MODBUS_REG_M2S_LEN] = 0;
            }

            DTERR_C(dtlock_release(self->lock_handle));
        }
        else if (rc == -1)
        {
            dtlog_debug(TAG, "modbus_receive failed: %s", strerror(errno));
            // Connection closed or error; break to accept a new client.
            break;
        }
        else
        {
            // rc == 0 is unusual; treat as no-op
            dtruntime_sleep_milliseconds(1);
        }

        // End client session; libmodbus will reuse accept loop.
    }

cleanup:
    if (dterr)
    {
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "rx task session loop exiting due to error");
    }
    else
    {
        dtlog_debug(TAG,
          "rx task session loop exiting normally on rxtask_stop_requested=%d session_stop_requested=%d",
          (int)self->rxtask_stop_requested,
          (int)self->session_stop_requested);
    }

    return dterr;
}
