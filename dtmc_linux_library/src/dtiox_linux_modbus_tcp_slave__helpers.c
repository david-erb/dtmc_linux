#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>

#include <dtmc_base/dtlock.h>
#include <dtmc_base/dtmodbus_helpers.h>

#include "dtiox_linux_modbus_tcp_slave__private.h"

#define TAG "dtiox_linux_modbus_tcp_slave"

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave__release_lock(dtiox_linux_modbus_tcp_slave_t* self, dterr_t* dterr_in)
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
dtiox_linux_modbus_tcp_slave__verify_rxtask_running(dtiox_linux_modbus_tcp_slave_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->rxtasker_handle);

    dttasker_info_t task_info = { 0 };
    DTERR_C(dttasker_get_info(self->rxtasker_handle, &task_info));

    if (task_info.status != RUNNING)
    {
        dterr = dterr_new(
          DTERR_STATE, DTERR_LOC, NULL, "rx task not running (status=%s)", dttasker_state_to_string(task_info.status));
        goto cleanup;
    }

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave__verify_rxtask_not_running(dtiox_linux_modbus_tcp_slave_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->rxtasker_handle);

    dttasker_info_t task_info = { 0 };
    DTERR_C(dttasker_get_info(self->rxtasker_handle, &task_info));

    dtlog_debug(TAG, "rx task status=%s", dttasker_state_to_string(task_info.status));

    if (task_info.status == RUNNING)
    {
        dterr = dterr_new(
          DTERR_STATE, DTERR_LOC, NULL, "rx task still running (status=%s)", dttasker_state_to_string(task_info.status));
        goto cleanup;
    }

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
void
dtiox_linux_modbus_tcp_slave__prepare_poll_response(dtiox_linux_modbus_tcp_slave_t* self)
{
    // Called under lock, self->mb_map guaranteed non-null in attached state.

    if (!self->mb_map || !self->mb_map->tab_registers)
        return;

    if (self->tx_pending && self->tx_len > 0)
    {
        int32_t byte_len = self->tx_len;

        if (byte_len > self->cfg.max_blob_bytes)
            byte_len = self->cfg.max_blob_bytes;
        if (byte_len > DTIOX_MODBUS_MAX_BLOB_BYTES)
            byte_len = DTIOX_MODBUS_MAX_BLOB_BYTES;

        self->mb_map->tab_registers[DTIOX_MODBUS_REG_S2M_STATUS] = (uint16_t)DTIOX_MODBUS_STATUS_HAS_DATA;
        self->mb_map->tab_registers[DTIOX_MODBUS_REG_S2M_LEN] = (uint16_t)byte_len;

        int32_t regs = DTIOX_MODBUS_BLOB_TO_REGS(byte_len);
        if (regs > DTIOX_MODBUS_MAX_BLOB_REGS)
            regs = DTIOX_MODBUS_MAX_BLOB_REGS;

        dtmodbus_helpers_pack_bytes_to_regs(self->tx_bytes, byte_len, &self->mb_map->tab_registers[DTIOX_MODBUS_REG_S2M_DATA]);

        // Mark as sent (one-shot behavior).
        self->tx_pending = false;
        self->tx_len = 0;
    }
    else
    {
        self->mb_map->tab_registers[DTIOX_MODBUS_REG_S2M_STATUS] = (uint16_t)DTIOX_MODBUS_STATUS_NO_DATA;
        self->mb_map->tab_registers[DTIOX_MODBUS_REG_S2M_LEN] = 0;
        // No need to clear data regs.
    }
}

// -----------------------------------------------------------------------------
void
dtiox_linux_modbus_tcp_slave__consume_put_blob(dtiox_linux_modbus_tcp_slave_t* self)
{
    // Called under lock; pull cmd/len/data from mapping written by master.

    if (!self->mb_map || !self->mb_map->tab_registers)
        return;

    uint16_t cmd = self->mb_map->tab_registers[DTIOX_MODBUS_REG_M2S_CMD];
    uint16_t byte_len_u16 = self->mb_map->tab_registers[DTIOX_MODBUS_REG_M2S_LEN];

    if (cmd != (uint16_t)DTIOX_MODBUS_CMD_PUT_BLOB)
        return;

    int32_t byte_len = (int32_t)byte_len_u16;

    if (byte_len < 0)
        byte_len = 0;

    if (byte_len > self->cfg.max_blob_bytes)
        byte_len = self->cfg.max_blob_bytes;

    if (byte_len > DTIOX_MODBUS_MAX_BLOB_BYTES)
        byte_len = DTIOX_MODBUS_MAX_BLOB_BYTES;

    if (byte_len > 0)
    {
        uint8_t bytes[DTIOX_MODBUS_MAX_BLOB_BYTES];

        int32_t regs = DTIOX_MODBUS_BLOB_TO_REGS(byte_len);
        if (regs > DTIOX_MODBUS_MAX_BLOB_REGS)
            regs = DTIOX_MODBUS_MAX_BLOB_REGS;

        dtmodbus_helpers_unpack_regs_to_bytes(&self->mb_map->tab_registers[DTIOX_MODBUS_REG_M2S_DATA], byte_len, bytes);

        int32_t written = dtringfifo_push(&self->rx_fifo, bytes, byte_len);
        if (written < byte_len)
            self->rx_overflow_pending = true;

        if (self->rx_semaphore)
        {
            // best-effort
            (void)dtsemaphore_post(self->rx_semaphore);
        }
    }

    // Clear the command so repeated replies don’t re-consume.
    self->mb_map->tab_registers[DTIOX_MODBUS_REG_M2S_CMD] = (uint16_t)DTIOX_MODBUS_CMD_NONE;
    self->mb_map->tab_registers[DTIOX_MODBUS_REG_M2S_LEN] = 0;
}
