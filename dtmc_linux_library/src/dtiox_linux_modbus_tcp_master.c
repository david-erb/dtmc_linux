#include <errno.h>
#include <net/if.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <linux/can.h>
#include <linux/can/raw.h>

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

#include <dtmc/dtiox_linux_modbus_tcp_master.h>

#include "dtiox_linux_modbus_tcp_master__private.h"

#define dtiox_linux_modbus_tcp_master_DEFAULT_RX_RING_CAPACITY 1024

#define TAG "dtiox_linux_modbus_tcp_master"

// vtable
DTIOX_INIT_VTABLE(dtiox_linux_modbus_tcp_master);
DTOBJECT_INIT_VTABLE(dtiox_linux_modbus_tcp_master);

// -----------------------------------------------------------------------------
// Creation / initialization

dterr_t*
dtiox_linux_modbus_tcp_master_create(dtiox_linux_modbus_tcp_master_t** self_ptr)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self_ptr);

    *self_ptr = (dtiox_linux_modbus_tcp_master_t*)malloc(sizeof(**self_ptr));
    if (!*self_ptr)
        return dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc %zu", sizeof(**self_ptr));

    DTERR_C(dtiox_linux_modbus_tcp_master_init(*self_ptr));
    (*self_ptr)->_is_malloced = true;

cleanup:
    if (dterr)
    {
        free(*self_ptr);
        *self_ptr = NULL;
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtiox_linux_modbus_tcp_master_create failed");
    }
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_master_init(dtiox_linux_modbus_tcp_master_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    memset(self, 0, sizeof(*self));
    self->model_number = DTMC_BASE_CONSTANTS_IOX_MODEL_LINUX_MODBUS_TCP_MASTER;

    DTERR_C(dtiox_set_vtable(self->model_number, &dtiox_linux_modbus_tcp_master_vt));
    DTERR_C(dtobject_set_vtable(self->model_number, &dtiox_linux_modbus_tcp_master_object_vt));

    DTERR_C(dtringfifo_init(&self->rx_fifo));

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// Configure
dterr_t*
dtiox_linux_modbus_tcp_master_configure(dtiox_linux_modbus_tcp_master_t* self,
  const dtiox_linux_modbus_tcp_master_config_t* config)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(config);
    DTERR_ASSERT_NOT_NULL(config->ip);

    self->cfg = *config;

    if (self->cfg.port == 0)
        self->cfg.port = DTIOX_MODBUS_TCP_DEFAULT_PORT;
    if (self->cfg.unit_id == 0)
        self->cfg.unit_id = 1;

    if (self->cfg.poll_interval_ms <= 0)
        self->cfg.poll_interval_ms = 25;
    if (self->cfg.response_timeout_ms <= 0)
        self->cfg.response_timeout_ms = 250;

    if (self->cfg.max_blob_bytes <= 0 || self->cfg.max_blob_bytes > DTIOX_MODBUS_MAX_BLOB_BYTES)
        self->cfg.max_blob_bytes = DTIOX_MODBUS_MAX_BLOB_BYTES;

    if (config->rx_ring_capacity < 0)
    {
        dterr = dterr_new(DTERR_BADARG,
          DTERR_LOC,
          NULL,
          "rx_ring_capacity must be >= 0 (0 means default, got %" PRId32 ")",
          config->rx_ring_capacity);
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
            dterr =
              dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc rx ring buffer %d bytes failed", self->cfg.rx_ring_capacity);
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
        c.name = "modbus_rx";
        c.tasker_entry_point_fn = dtiox_linux_modbus_tcp_master__rxtask_entry;
        c.tasker_entry_point_arg = self;
        DTERR_C(dttasker_create(&self->rxtasker_handle, &c));
    }

    DTERR_C(dtlock_create(&self->lock_handle));

cleanup:
    return dterr;
}

// ----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_master_attach(dtiox_linux_modbus_tcp_master_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->lock_handle);

    DTERR_C(dtlock_acquire(self->lock_handle));

    self->mb = modbus_new_tcp(self->cfg.ip, self->cfg.port);
    if (self->mb == NULL)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "modbus_new_tcp failed");
        goto release_lock_and_cleanup;
    }

    // dtiox_linux_modbus_tcp_master_ERRNO_C(modbus_set_debug(self->mb, true));

    dtiox_linux_modbus_tcp_master_ERRNO_C(modbus_set_slave(self->mb, self->cfg.unit_id));

    // libmodbus timeout config is in seconds + microseconds.
    dtiox_linux_modbus_tcp_master_ERRNO_C(modbus_set_response_timeout(
      self->mb, self->cfg.response_timeout_ms / 1000, (self->cfg.response_timeout_ms % 1000) * 1000));

    self->rx_overflow_pending = false;
    self->dead_slave_pending = false;

    self->enabled = true;
    self->stop_requested = false;

    // connect the socket before starting RX task
    dterr = dtiox_linux_modbus_tcp_master__ensure_connected(self);
    if (dterr != NULL)
    {
        goto release_lock_and_cleanup;
    }

    // Start RX task
    DTERR_C(dttasker_start(self->rxtasker_handle));

release_lock_and_cleanup:
    dterr = dtiox_linux_modbus_tcp_master__release_lock(self, dterr);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_master_detach(dtiox_linux_modbus_tcp_master_t* self)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dtlock_acquire(self->lock_handle));

    self->stop_requested = true;
    self->enabled = false;

    if (self->mb != NULL)
    {
        modbus_close(self->mb);
    }

release_lock_and_cleanup:
    dterr = dtiox_linux_modbus_tcp_master__release_lock(self, dterr);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_master_enable(dtiox_linux_modbus_tcp_master_t* self, bool enabled)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dtlock_acquire(self->lock_handle));

    self->enabled = enabled;

    // Clear buffers + flags whenever enable is called  for predictable state.
    dtringfifo_reset(&self->rx_fifo);
    self->rx_overflow_pending = false;
    self->dead_slave_pending = false;

    DTERR_C(dtlock_release(self->lock_handle));

cleanup:
    return dterr;
}
// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_master_write( //
  dtiox_linux_modbus_tcp_master_t* self,
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
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "len < 0");
        goto cleanup;
    }

    DTERR_C(dtlock_acquire(self->lock_handle));

    if (!self->enabled)
    {
        *out_written = 0;
        goto release_lock_and_cleanup;
    }

    dterr = dtiox_linux_modbus_tcp_master__ensure_connected(self);
    if (dterr != NULL)
    {
        goto release_lock_and_cleanup;
    }

    int32_t max_len = self->cfg.max_blob_bytes;
    int32_t send_len = (len > max_len) ? max_len : len;

    uint16_t regs[2 + DTIOX_MODBUS_MAX_BLOB_REGS];
    regs[0] = (uint16_t)DTIOX_MODBUS_CMD_PUT_BLOB;
    regs[1] = (uint16_t)send_len;

    int32_t data_regs = DTIOX_MODBUS_BLOB_TO_REGS(send_len);
    if (data_regs > DTIOX_MODBUS_MAX_BLOB_REGS)
        data_regs = DTIOX_MODBUS_MAX_BLOB_REGS;

    dtmodbus_helpers_pack_bytes_to_regs(buf, send_len, &regs[2]);

    // Total registers written: cmd + len + payload_regs
    int total_regs = 2 + data_regs;

    // NOTE: libmodbus is not guaranteed thread-safe; we serialize all modbus calls using self->lock.
    int rc = modbus_write_registers(self->mb, DTIOX_MODBUS_REG_M2S_CMD, total_regs, regs);

    if (rc < 0)
    {
        dterr = dterr_new(DTERR_IO,
          DTERR_LOC,
          NULL,
          "modbus_write_registers " dtiox_linux_modbus_tcp_master_ERRNO_FORMAT,
          dtiox_linux_modbus_tcp_master_ERRNO_ARGS());
        goto release_lock_and_cleanup;
    }

    if (rc != total_regs)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "modbus_write_registers failed");
        goto release_lock_and_cleanup;
    }

    *out_written = send_len;

release_lock_and_cleanup:
    if (dterr != NULL)
        dtiox_linux_modbus_tcp_master__ensure_disconnected(self);

    dterr = dtiox_linux_modbus_tcp_master__release_lock(self, dterr);

cleanup:

    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_master_read( //
  dtiox_linux_modbus_tcp_master_t* self,
  uint8_t* buf,
  int32_t buf_len,
  int32_t* out_read)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->lock_handle);
    DTERR_ASSERT_NOT_NULL(self->mb);
    DTERR_ASSERT_NOT_NULL(buf);
    DTERR_ASSERT_NOT_NULL(out_read);

    if (buf_len < 0)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "buf_len < 0");
        goto cleanup;
    }

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
    dterr = dtiox_linux_modbus_tcp_master__release_lock(self, dterr);

    dterr = dtiox_linux_modbus_tcp_master__verify_rxtask_running(self, dterr);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_master_set_rx_semaphore(dtiox_linux_modbus_tcp_master_t* self, dtsemaphore_handle rx_semaphore)
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
dterr_t*
dtiox_linux_modbus_tcp_master_concat_format( //
  dtiox_linux_modbus_tcp_master_t* self,
  char* in_str,
  char* separator,
  char** out_str)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_str);

    const char* sep = (separator != NULL) ? separator : "";
    const char* base = (in_str != NULL) ? in_str : "";

    DTERR_C(dtlock_acquire(self->lock_handle));

    char tmp[256];
    int n = snprintf(tmp,
      sizeof(tmp),
      "%s%smaster=libmodbus_tcp ip=%s port=%d unit=%d enabled=%d rx_ring_capacity=%" PRId32,
      base,
      sep,
      (self->cfg.ip != NULL) ? self->cfg.ip : "(null)",
      (int)self->cfg.port,
      (int)self->cfg.unit_id,
      (int)self->enabled,
      (int)self->cfg.rx_ring_capacity);

    DTERR_C(dtlock_release(self->lock_handle));

    if (n < 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "snprintf failed");
        goto cleanup;
    }

    // Allocate exact string (truncate if tmp overflowed).
    size_t need = (size_t)((n < (int)sizeof(tmp)) ? n : (int)sizeof(tmp) - 1);
    char* s = (char*)malloc(need + 1);
    if (s == NULL)
    {
        dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "alloc failed");
        goto cleanup;
    }
    memcpy(s, tmp, need);
    s[need] = '\0';

    *out_str = s;

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// dispose
//
// Intentionally does *not* free or clear `self` or its resources. This backend
// is meant for simple, process-lifetime usage where the RX thread, socket, and
// buffers can safely die with the process. Calling dispose() is effectively a
// no-op to avoid use-after-free issues if other code still holds pointers.

void
dtiox_linux_modbus_tcp_master_dispose(dtiox_linux_modbus_tcp_master_t* self)
{
    if (self == NULL)
        return;

    // Best effort cleanup.
    dtlock_dispose(self->lock_handle);
}
