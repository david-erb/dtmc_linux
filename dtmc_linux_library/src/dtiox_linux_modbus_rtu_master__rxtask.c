#include <stdint.h>

#include <dtcore/dtbytes.h>
#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>

#include <dtmc_base/dtiox_modbus.h>
#include <dtmc_base/dtlock.h>
#include <dtmc_base/dtmodbus_helpers.h>
#include <dtmc_base/dtruntime.h>
#include <dtmc_base/dttasker.h>

#include "dtiox_linux_modbus_rtu_master__private.h"

#define TAG "dtiox_linux_modbus_rtu_master"

// comment out the logging here
#define dtlog_debug(TAG, ...)

dterr_t*
dtiox_linux_modbus_rtu_master__rxtask_entry(void* self_, dttasker_handle tasker_handle)
{
    dtiox_linux_modbus_rtu_master_t* self = (dtiox_linux_modbus_rtu_master_t*)self_;
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(tasker_handle);
    DTERR_ASSERT_NOT_NULL(self->lock_handle);
    DTERR_ASSERT_NOT_NULL(self->mb);

    dtlog_debug(TAG, "RTU master RX task started with poll_interval_ms=%" PRId32, self->cfg.poll_interval_ms);

    DTERR_C(dttasker_ready(tasker_handle));

    uint16_t hdr[2];
    uint16_t data_regs[DTIOX_MODBUS_MAX_BLOB_REGS];
    uint8_t data_bytes[DTIOX_MODBUS_MAX_BLOB_BYTES];

    while (1)
    {
        // Snapshot state
        DTERR_C(dtlock_acquire(self->lock_handle));

        bool stop = self->stop_requested;
        bool ok_to_poll = self->enabled && (self->mb != NULL);

        int32_t poll_ms = self->cfg.poll_interval_ms;
        int32_t max_blob = self->cfg.max_blob_bytes;
        if (max_blob > DTIOX_MODBUS_MAX_BLOB_BYTES)
            max_blob = DTIOX_MODBUS_MAX_BLOB_BYTES;

        DTERR_C(dtlock_release(self->lock_handle));

        if (stop)
            break;

        if (!ok_to_poll)
        {
            dtruntime_sleep_milliseconds(poll_ms);
            continue;
        }

        // Serialize all modbus calls
        DTERR_C(dtlock_acquire(self->lock_handle));

        // 1) Post poll command
        uint16_t cmd_regs[2];
        cmd_regs[0] = (uint16_t)DTIOX_MODBUS_CMD_GIVE_ME_ANY_DATA;
        cmd_regs[1] = 0;

        int rc = modbus_write_registers(self->mb, DTIOX_MODBUS_REG_M2S_CMD, 2, cmd_regs);
        if (rc != 2)
        {
            self->dead_slave_pending = true;
            dterr = dterr_new(DTERR_FAIL,
              DTERR_LOC,
              NULL,
              "modbus_write_registers poll command " DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_FORMAT,
              DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_ARGS());
            goto release_lock_and_cleanup;
        }

        // 2) Read response header
        rc = modbus_read_registers(self->mb, DTIOX_MODBUS_REG_S2M_STATUS, 2, hdr);
        if (rc != 2)
        {
            self->dead_slave_pending = true;
            dterr = dterr_new(DTERR_FAIL,
              DTERR_LOC,
              NULL,
              "modbus_read_registers poll header " DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_FORMAT,
              DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_ARGS());
            goto release_lock_and_cleanup;
        }

        uint16_t status = hdr[0];
        uint16_t byte_len = hdr[1];

        dtlog_debug(TAG, "polled slave, got status=0x%04" PRIx16 ", byte_len=%" PRId32, status, byte_len);

        if (status == (uint16_t)DTIOX_MODBUS_STATUS_HAS_DATA && byte_len > 0)
        {
            if (byte_len > (uint16_t)max_blob)
                byte_len = (uint16_t)max_blob;

            int32_t need_regs = DTIOX_MODBUS_BLOB_TO_REGS((int32_t)byte_len);
            if (need_regs > DTIOX_MODBUS_MAX_BLOB_REGS)
                need_regs = DTIOX_MODBUS_MAX_BLOB_REGS;

            // 3) Read payload regs
            rc = modbus_read_registers(self->mb, DTIOX_MODBUS_REG_S2M_DATA, need_regs, data_regs);
            if (rc != need_regs)
            {
                self->dead_slave_pending = true;
                dterr = dterr_new(DTERR_FAIL,
                  DTERR_LOC,
                  NULL,
                  "modbus_read_registers poll data " DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_FORMAT,
                  DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_ARGS());
                goto release_lock_and_cleanup;
            }

#ifdef dtlog_debug
            {
                char hex[64];
                dtbytes_compose_hex((uint8_t*)data_regs, (int32_t)byte_len, hex, sizeof(hex));
                dtlog_debug(TAG, "got regs %s", hex);
            }
#endif

            dtmodbus_helpers_unpack_regs_to_bytes(data_regs, (int32_t)byte_len, data_bytes);

#ifdef dtlog_debug
            {
                char hex[64];
                dtbytes_compose_hex(data_bytes, (int32_t)byte_len, hex, sizeof(hex));
                dtlog_debug(TAG, "to bytes %s", hex);
            }
#endif

            // 4) Push into fifo
            int32_t written = dtringfifo_push(&self->rx_fifo, data_bytes, (int32_t)byte_len);
            if (written < (int32_t)byte_len)
                self->rx_overflow_pending = true;

            // Notify
            if (self->rx_semaphore != NULL && written > 0)
                dtsemaphore_post(self->rx_semaphore);
        }

        DTERR_C(dtlock_release(self->lock_handle));

        dtruntime_sleep_milliseconds(poll_ms);
    }

    goto cleanup;

release_lock_and_cleanup:
    dterr = dtiox_linux_modbus_rtu_master__release_lock(self, dterr);

cleanup:
    return dterr;
}
