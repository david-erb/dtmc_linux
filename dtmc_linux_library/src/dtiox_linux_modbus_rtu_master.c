#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <modbus/modbus.h>

#include <dtmc_base/dtmc_base_constants.h>

#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtobject.h>
#include <dtcore/dtringfifo.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtlock.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>

#include <dtmc_base/dtmodbus_helpers.h>
#include <dtmc_base/dtuart_helpers.h>

#include <dtmc/dtiox_linux_modbus_rtu_master.h>

#include "dtiox_linux_modbus_rtu_master__private.h"

#define TAG "dtiox_linux_modbus_rtu_master"
#define dtlog_debug(TAG, ...)

// vtable
DTIOX_INIT_VTABLE(dtiox_linux_modbus_rtu_master);
DTOBJECT_INIT_VTABLE(dtiox_linux_modbus_rtu_master);

// -----------------------------------------------------------------------------
// Creation / initialization

dterr_t*
dtiox_linux_modbus_rtu_master_create(dtiox_linux_modbus_rtu_master_t** self_ptr)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self_ptr);

    *self_ptr = (dtiox_linux_modbus_rtu_master_t*)malloc(sizeof(**self_ptr));
    if (!*self_ptr)
        return dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc %zu", sizeof(**self_ptr));

    DTERR_C(dtiox_linux_modbus_rtu_master_init(*self_ptr));
    (*self_ptr)->_is_malloced = true;

cleanup:
    if (dterr)
    {
        free(*self_ptr);
        *self_ptr = NULL;
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtiox_linux_modbus_rtu_master_create failed");
    }
    return dterr;
}

dterr_t*
dtiox_linux_modbus_rtu_master_init(dtiox_linux_modbus_rtu_master_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    memset(self, 0, sizeof(*self));
    self->model_number = DTMC_BASE_CONSTANTS_IOX_MODEL_LINUX_MODBUS_RTU_MASTER;

    DTERR_C(dtiox_set_vtable(self->model_number, &dtiox_linux_modbus_rtu_master_vt));
    DTERR_C(dtobject_set_vtable(self->model_number, &dtiox_linux_modbus_rtu_master_object_vt));

    DTERR_C(dtringfifo_init(&self->rx_fifo));

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// Configure

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_rtu_master_configure(dtiox_linux_modbus_rtu_master_t* self,
  const dtiox_linux_modbus_rtu_master_config_t* cfg)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(cfg);
    DTERR_ASSERT_NOT_NULL(cfg->device);

    DTERR_C(dtuart_helper_validate(&cfg->uart_config));

    self->cfg = *cfg;

    if (self->cfg.slave_id == 0)
        self->cfg.slave_id = 1;

    if (self->cfg.poll_interval_ms <= 0)
        self->cfg.poll_interval_ms = 25;

    if (self->cfg.response_timeout_ms <= 0)
        self->cfg.response_timeout_ms = 250;

    if (self->cfg.max_blob_bytes <= 0 || self->cfg.max_blob_bytes > DTIOX_MODBUS_MAX_BLOB_BYTES)
        self->cfg.max_blob_bytes = DTIOX_MODBUS_MAX_BLOB_BYTES;

    // defaults: try to help RS485 users
    if (!cfg->rs485_mode)
        self->cfg.rs485_mode = false;
    else if (cfg->rs485_mode == false)
        self->cfg.rs485_mode = false;
    else
        self->cfg.rs485_mode = true;

    if (!cfg->rts_toggle)
        self->cfg.rts_toggle = false;
    else if (cfg->rts_toggle == false)
        self->cfg.rts_toggle = false;
    else
        self->cfg.rts_toggle = true;

    if (cfg->rx_ring_capacity < 0)
    {
        dterr = dterr_new(DTERR_BADARG,
          DTERR_LOC,
          NULL,
          "rx_ring_capacity must be >= 0 (0 means default, got %" PRId32 ")",
          cfg->rx_ring_capacity);
        goto cleanup;
    }

    // Allocate and configure RX FIFO storage.
    {
        if (self->cfg.rx_ring_capacity == 0)
            self->cfg.rx_ring_capacity = 1024;

        if (self->rx_fifo_storage)
        {
            free(self->rx_fifo_storage);
            self->rx_fifo_storage = NULL;
        }

        self->rx_fifo_storage = (uint8_t*)malloc((size_t)self->cfg.rx_ring_capacity);
        if (!self->rx_fifo_storage)
        {
            dterr = dterr_new(
              DTERR_NOMEM, DTERR_LOC, NULL, "malloc rx ring buffer %" PRId32 " bytes failed", self->cfg.rx_ring_capacity);
            goto cleanup;
        }

        dtringfifo_config_t fifo_cfg = {
            .buffer = self->rx_fifo_storage,
            .capacity = self->cfg.rx_ring_capacity,
        };

        DTERR_C(dtringfifo_configure(&self->rx_fifo, &fifo_cfg));
    }

    // configure RX task
    {
        dttasker_config_t c = { 0 };
        c.name = "modbus_rtu_rx";
        c.tasker_entry_point_fn = dtiox_linux_modbus_rtu_master__rxtask_entry;
        c.tasker_entry_point_arg = self;
        DTERR_C(dttasker_create(&self->rxtasker_handle, &c));
    }

    DTERR_C(dtlock_create(&self->lock_handle));

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// Attach / detach

dterr_t*
dtiox_linux_modbus_rtu_master_attach(dtiox_linux_modbus_rtu_master_t* self)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->lock_handle);

    DTERR_C(dtlock_acquire(self->lock_handle));

    int parity = '?';
    if (self->cfg.uart_config.parity == DTUART_PARITY_EVEN)
        parity = 'E';
    else if (self->cfg.uart_config.parity == DTUART_PARITY_ODD)
        parity = 'O';
    else if (self->cfg.uart_config.parity == DTUART_PARITY_NONE)
        parity = 'N';

    int data_bits = 0;
    if (self->cfg.uart_config.data_bits == DTUART_DATA_BITS_7)
        data_bits = 7;
    else if (self->cfg.uart_config.data_bits == DTUART_DATA_BITS_8)
        data_bits = 8;

    int stop_bits = 0;
    if (self->cfg.uart_config.stop_bits == DTUART_STOPBITS_1)
        stop_bits = 1;
    else if (self->cfg.uart_config.stop_bits == DTUART_STOPBITS_2)
        stop_bits = 2;

    dtlog_info(TAG,
      "attaching device=%s@%" PRId32 ", uart raw settings {baudrate=%" PRId32 ", parity=%c, data_bits=%d, stop_bits=%d}",
      (self->cfg.device != NULL) ? self->cfg.device : "(null)",
      self->cfg.slave_id,
      self->cfg.uart_config.baudrate,
      parity,
      data_bits,
      stop_bits);

    self->mb = modbus_new_rtu(self->cfg.device, (int)self->cfg.uart_config.baudrate, parity, data_bits, stop_bits);
    if (self->mb == NULL)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "modbus_new_rtu failed");
        goto release_lock_and_cleanup;
    }

    DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_C(modbus_set_slave(self->mb, (int)self->cfg.slave_id));

    // response timeout
    DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_C(modbus_set_response_timeout(
      self->mb, self->cfg.response_timeout_ms / 1000, (self->cfg.response_timeout_ms % 1000) * 1000));

    // optional byte timeout
    if (self->cfg.byte_timeout_ms > 0)
    {
        DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_C(
          modbus_set_byte_timeout(self->mb, self->cfg.byte_timeout_ms / 1000, (self->cfg.byte_timeout_ms % 1000) * 1000));
    }

#if defined(MODBUS_RTU_RS485)
    if (self->cfg.rs485_mode)
    {
        // Best effort: not all libmodbus builds/platforms implement this meaningfully.
        (void)modbus_rtu_set_serial_mode(self->mb, MODBUS_RTU_RS485);
    }
#endif

#if defined(MODBUS_RTU_RTS_UP) && defined(MODBUS_RTU_RTS_DOWN)
    if (self->cfg.rts_toggle)
    {
        // Best effort direction control. You may need to tune this depending on adapter/driver.
        (void)modbus_rtu_set_rts(self->mb, MODBUS_RTU_RTS_UP);
    }
#endif

    self->rx_overflow_pending = false;
    self->dead_slave_pending = false;

    self->enabled = true;
    self->stop_requested = false;

    // connect before starting RX task
    dterr = dtiox_linux_modbus_rtu_master__ensure_connected(self);
    if (dterr != NULL)
        goto release_lock_and_cleanup;

    // Start RX task
    DTERR_C(dttasker_start(self->rxtasker_handle));

release_lock_and_cleanup:
    dterr = dtiox_linux_modbus_rtu_master__release_lock(self, dterr);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_rtu_master_detach(dtiox_linux_modbus_rtu_master_t* self)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dtlock_acquire(self->lock_handle));

    self->stop_requested = true;
    self->enabled = false;

    if (self->mb != NULL)
        modbus_close(self->mb);

release_lock_and_cleanup:
    dterr = dtiox_linux_modbus_rtu_master__release_lock(self, dterr);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// Enable

dterr_t*
dtiox_linux_modbus_rtu_master_enable(dtiox_linux_modbus_rtu_master_t* self, bool enabled)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dtlock_acquire(self->lock_handle));

    self->enabled = enabled;

    // Clear buffers + flags for predictable state.
    dtringfifo_reset(&self->rx_fifo);
    self->rx_overflow_pending = false;
    self->dead_slave_pending = false;

    DTERR_C(dtlock_release(self->lock_handle));

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// Write (PUT_BLOB), best effort, absorb as much as possible with no waiting

dterr_t*
dtiox_linux_modbus_rtu_master_write(dtiox_linux_modbus_rtu_master_t* self,
  const uint8_t* buf,
  int32_t len,
  int32_t* out_written)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->lock_handle);
    DTERR_ASSERT_NOT_NULL(buf);
    DTERR_ASSERT_NOT_NULL(out_written);
    DTERR_ASSERT_NOT_NULL(self->mb);

    if (len < 0)
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "len < 0");

    DTERR_C(dtlock_acquire(self->lock_handle));

    if (!self->enabled)
    {
        *out_written = 0;
        goto release_lock_and_cleanup;
    }

    dterr = dtiox_linux_modbus_rtu_master__ensure_connected(self);
    if (dterr != NULL)
        goto release_lock_and_cleanup;

    int32_t remaining = len;
    const uint8_t* p = buf;
    while (true)
    {
        int32_t max_len = DTIOX_MODBUS_MAX_BLOB_BYTES;
        int32_t send_len = (remaining > max_len) ? max_len : remaining;

        uint16_t regs[2 + DTIOX_MODBUS_MAX_BLOB_REGS];
        regs[0] = (uint16_t)DTIOX_MODBUS_CMD_PUT_BLOB;
        regs[1] = (uint16_t)send_len;

        int32_t data_regs = DTIOX_MODBUS_BLOB_TO_REGS(send_len);
        if (data_regs > DTIOX_MODBUS_MAX_BLOB_REGS)
        {
            dterr = dterr_new(DTERR_FAIL,
              DTERR_LOC,
              NULL,
              "calculated data_regs=%" PRId32 " exceeds max %d for send_len=%" PRId32,
              data_regs,
              DTIOX_MODBUS_MAX_BLOB_REGS,
              send_len);
            goto release_lock_and_cleanup;
        }

        dtmodbus_helpers_pack_bytes_to_regs(p, send_len, &regs[2]);

        int total_regs = 2 + data_regs;

        // modbus_write_registers is blocking, but this function is supposed to return immediately
        // TODO: Add background thread so that dtiox_linux_modbus_rtu_master_write() doesn't block.
        int rc = modbus_write_registers(self->mb, DTIOX_MODBUS_REG_M2S_CMD, total_regs, regs);
        if (rc < 0 || rc != total_regs)
        {
            dterr = dterr_new(DTERR_IO,
              DTERR_LOC,
              NULL,
              "modbus_write_registers " DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_FORMAT,
              DTIOX_LINUX_MODBUS_RTU_MASTER_ERRNO_ARGS());
            goto release_lock_and_cleanup;
        }

        if (send_len < remaining)
        {
            dtlog_debug(TAG,
              "partial write of %" PRId32 " bytes (requested %" PRId32 " bytes, remaining %" PRId32 " bytes)",
              send_len,
              len,
              remaining - send_len);
        }

        remaining -= send_len;
        p += send_len;
        if (remaining <= 0)
            break;

        // configuration doesn't allow partial writes?
        if (!self->cfg.permit_retry_partial_writes)
            break;
    }

    *out_written = len - remaining;

release_lock_and_cleanup:
    if (dterr != NULL)
        dtiox_linux_modbus_rtu_master__ensure_disconnected(self);

    dterr = dtiox_linux_modbus_rtu_master__release_lock(self, dterr);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// Read (non-blocking from RX FIFO)

dterr_t*
dtiox_linux_modbus_rtu_master_read(dtiox_linux_modbus_rtu_master_t* self, uint8_t* buf, int32_t buf_len, int32_t* out_read)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->lock_handle);
    DTERR_ASSERT_NOT_NULL(self->mb);
    DTERR_ASSERT_NOT_NULL(buf);
    DTERR_ASSERT_NOT_NULL(out_read);

    if (buf_len < 0)
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "buf_len < 0");

    DTERR_C(dtlock_acquire(self->lock_handle));

    // Surface “dead slave” at next read.
    if (self->dead_slave_pending)
    {
        self->dead_slave_pending = false;
        dterr = dterr_new(DTERR_TIMEOUT, DTERR_LOC, NULL, "slave timeout");
        goto release_lock_and_cleanup;
    }

    // Surface overflow at next read.
    if (self->rx_overflow_pending)
    {
        self->rx_overflow_pending = false;
        dterr = dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "rx fifo overflow");
        goto release_lock_and_cleanup;
    }

    if (!self->enabled)
    {
        *out_read = 0;
        goto release_lock_and_cleanup;
    }

    int32_t n = dtringfifo_pop(&self->rx_fifo, buf, buf_len);
    *out_read = n;

release_lock_and_cleanup:
    dterr = dtiox_linux_modbus_rtu_master__release_lock(self, dterr);
    dterr = dtiox_linux_modbus_rtu_master__verify_rxtask_running(self, dterr);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// set_rx_semaphore

dterr_t*
dtiox_linux_modbus_rtu_master_set_rx_semaphore(dtiox_linux_modbus_rtu_master_t* self, dtsemaphore_handle rx_semaphore)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dtlock_acquire(self->lock_handle));
    self->rx_semaphore = rx_semaphore;
    DTERR_C(dtlock_release(self->lock_handle));

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// concat_format

dterr_t*
dtiox_linux_modbus_rtu_master_concat_format(dtiox_linux_modbus_rtu_master_t* self,
  char* in_str,
  char* separator,
  char** out_str)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_str);

    *out_str = in_str;
    const char* sep = (separator != NULL) ? separator : "";

    DTERR_C(dtlock_acquire(self->lock_handle));

    dtuart_helper_config_t uart_cfg = self->cfg.uart_config;
    dtiox_linux_modbus_rtu_master_stats_t stats = self->stats;
    bool enabled = self->enabled;
    int32_t rx_capacity = self->cfg.rx_ring_capacity;

    DTERR_C(dtlock_release(self->lock_handle));

    char uart_string[256];
    dtuart_helper_to_string(&uart_cfg, uart_string, sizeof(uart_string));

    *out_str = dtstr_concat_format(*out_str,
      sep,
      "libmodbus_rtu %s:%" PRId32 " %s enabled=%d rx_ring_capacity=%" PRId32,
      (self->cfg.device != NULL) ? self->cfg.device : "(null)",
      self->cfg.slave_id,
      uart_string,
      (int)enabled,
      rx_capacity);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// dispose

void
dtiox_linux_modbus_rtu_master_dispose(dtiox_linux_modbus_rtu_master_t* self)
{
    if (self == NULL)
        return;

    dtlock_dispose(self->lock_handle);
}
