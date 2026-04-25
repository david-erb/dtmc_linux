#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtringfifo.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtiox_modbus.h>
#include <dtmc_base/dtlock.h>

#include <dtmc/dtiox_linux_modbus_tcp_master.h>

#include "dtiox_linux_modbus_tcp_master__private.h"

#define TAG "dtiox_linux_modbus_tcp_master"

// -----------------------------------------------------------------------------
// lock release helper
dterr_t*
dtiox_linux_modbus_tcp_master__release_lock(dtiox_linux_modbus_tcp_master_t* self, dterr_t* dterr_in)
{
    dterr_t* dterr = dtlock_release(self->lock_handle);

    // let the incoming error take precedence
    if (dterr_in != NULL)
    {
        dterr_dispose(dterr);
        return dterr_in;
    }
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_master__ensure_connected(dtiox_linux_modbus_tcp_master_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    if (self->_is_connected)
    {
        goto cleanup;
    }

    dtiox_linux_modbus_tcp_master_ERRNO_C(modbus_connect(self->mb));
    self->_is_connected = true;

    dtlog_debug(TAG,
      "dtiox_linux_modbus_tcp_master__ensure_connected: connected to %s:%d (unit id %d)",
      self->cfg.ip,
      self->cfg.port,
      self->cfg.unit_id);

cleanup:
    if (dterr != NULL)
    {
        self->_is_connected = false;
    }

    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_master__verify_rxtask_running(dtiox_linux_modbus_tcp_master_t* self, dterr_t* dterr_in)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->rxtasker_handle);

    dttasker_info_t task_info = { 0 };
    DTERR_C(dttasker_get_info(self->rxtasker_handle, &task_info));

    if (task_info.status != RUNNING)
    {
        dterr = dterr_new(DTERR_STATE,
          DTERR_LOC,
          task_info.dterr,
          "rx task not running (status=%s)",
          dttasker_state_to_string(task_info.status));
        goto cleanup;
    }

cleanup:
    if (dterr_in != NULL)
    {
        if (dterr != NULL)
        {
            dterr_append(dterr_in, dterr);
        }
        dterr = dterr_in;
    }
    return dterr;
}

// -----------------------------------------------------------------------------
void
dtiox_linux_modbus_tcp_master__ensure_disconnected(dtiox_linux_modbus_tcp_master_t* self)
{
    dtlog_debug(TAG,
      "dtiox_linux_modbus_tcp_master__ensure_disconnected: disconnecting from %s:%d (unit id %d)",
      self->cfg.ip,
      self->cfg.port,
      self->cfg.unit_id);

    modbus_close(self->mb);
    self->_is_connected = false;
}