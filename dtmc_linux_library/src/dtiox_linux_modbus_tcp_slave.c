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

#include <sys/socket.h>

#define dtiox_linux_modbus_tcp_slave_DEFAULT_RX_RING_CAPACITY 1024

#define TAG "dtiox_linux_modbus_tcp_slave"

// vtable
DTIOX_INIT_VTABLE(dtiox_linux_modbus_tcp_slave);
DTOBJECT_INIT_VTABLE(dtiox_linux_modbus_tcp_slave);

// -----------------------------------------------------------------------------

static int
dtiox_linux_modbus_tcp_slave__required_registers(void)
{
    // We need holding registers up through S2M_DATA + payload.
    // +1 for safety / inclusive indexing.
    return (int)(DTIOX_MODBUS_REG_S2M_DATA + DTIOX_MODBUS_MAX_BLOB_REGS + 1);
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave_create(dtiox_linux_modbus_tcp_slave_t** self_ptr)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self_ptr);

    *self_ptr = (dtiox_linux_modbus_tcp_slave_t*)malloc(sizeof(**self_ptr));
    if (!*self_ptr)
        return dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc %zu", sizeof(**self_ptr));

    DTERR_C(dtiox_linux_modbus_tcp_slave_init(*self_ptr));
    (*self_ptr)->_is_malloced = true;

cleanup:
    if (dterr)
    {
        free(*self_ptr);
        *self_ptr = NULL;
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtiox_linux_modbus_tcp_slave_create failed");
    }
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave_init(dtiox_linux_modbus_tcp_slave_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    memset(self, 0, sizeof(*self));
    self->model_number = DTMC_BASE_CONSTANTS_IOX_MODEL_LINUX_MODBUS_TCP_SLAVE;

    DTERR_C(dtiox_set_vtable(self->model_number, &dtiox_linux_modbus_tcp_slave_vt));
    DTERR_C(dtobject_set_vtable(self->model_number, &dtiox_linux_modbus_tcp_slave_object_vt));

    DTERR_C(dtringfifo_init(&self->rx_fifo));

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave_configure(dtiox_linux_modbus_tcp_slave_t* self,
  const dtiox_linux_modbus_tcp_slave_config_t* config)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(config);

    self->cfg = *config;

    if (self->cfg.port == 0)
        self->cfg.port = DTIOX_MODBUS_TCP_DEFAULT_PORT;

    if (self->cfg.max_blob_bytes <= 0 || self->cfg.max_blob_bytes > DTIOX_MODBUS_MAX_BLOB_BYTES)
        self->cfg.max_blob_bytes = DTIOX_MODBUS_MAX_BLOB_BYTES;

    if (self->cfg.rx_ring_capacity < 0)
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
            self->cfg.rx_ring_capacity = dtiox_linux_modbus_tcp_slave_DEFAULT_RX_RING_CAPACITY;

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
        c.tasker_entry_point_fn = dtiox_linux_modbus_tcp_slave__rxtask_entry;
        c.tasker_entry_point_arg = self;
        DTERR_C(dttasker_create(&self->rxtasker_handle, &c));
    }

    DTERR_C(dtlock_create(&self->lock_handle));

cleanup:
    return dterr;
}

// ----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave_attach(dtiox_linux_modbus_tcp_slave_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->lock_handle);

    // if already listening, don't listen again
    if (self->listening_socket > 0)
    {
        goto cleanup;
    }

    DTERR_C(dtiox_linux_modbus_tcp_slave__verify_rxtask_not_running(self));

    DTERR_C(dtlock_acquire(self->lock_handle));

    // Create libmodbus context, bind on all interfaces.
    self->mb = modbus_new_tcp(NULL, (int)self->cfg.port);
    if (self->mb == NULL)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "modbus_new_tcp failed");
        goto release_lock_and_cleanup;
    }

    // dtiox_linux_modbus_tcp_slave_ERRNO_C(modbus_set_debug(self->mb, true));

    // Optional: response timeout (applies to receive/reply in some backends).
    if (self->cfg.response_timeout_ms > 0)
    {
        dtiox_linux_modbus_tcp_slave_ERRNO_C(modbus_set_response_timeout(
          self->mb, self->cfg.response_timeout_ms / 1000, (self->cfg.response_timeout_ms % 1000) * 1000));
    }

    // Allocate mapping holding registers large enough for our fixed map.
    int nb_regs = dtiox_linux_modbus_tcp_slave__required_registers();

    self->mb_map = modbus_mapping_new(0, 0, nb_regs, 0);
    if (self->mb_map == NULL)
    {
        dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "modbus_mapping_new failed");
        goto release_lock_and_cleanup;
    }

    // Initialize S2M area to "no data".
    self->mb_map->tab_registers[DTIOX_MODBUS_REG_S2M_STATUS] = (uint16_t)DTIOX_MODBUS_STATUS_NO_DATA;
    self->mb_map->tab_registers[DTIOX_MODBUS_REG_S2M_LEN] = 0;

    // Listen socket (accept is done in the rx task).
    self->listening_socket = modbus_tcp_listen(self->mb, 1);
    if (self->listening_socket < 0)
    {
        dterr = dterr_new(DTERR_IO,
          DTERR_LOC,
          NULL,
          "modbus_tcp_listen " dtiox_linux_modbus_tcp_slave_ERRNO_FORMAT,
          dtiox_linux_modbus_tcp_slave_ERRNO_ARGS());
        goto release_lock_and_cleanup;
    }
    dtlog_debug(TAG, "listening for Modbus TCP connections on port %d", (int)self->cfg.port);

    self->rx_overflow_pending = false;

    self->tx_pending = false;
    self->tx_len = 0;

    self->enabled = true;
    self->session_stop_requested = false;
    self->rxtask_stop_requested = false;

    // Start RX task
    DTERR_C(dttasker_start(self->rxtasker_handle));

release_lock_and_cleanup:
    dterr = dtiox_linux_modbus_tcp_slave__release_lock(self, dterr);

cleanup:
    if (dterr)
    {
        // best-effort rollback if partially attached
        if (self->listening_socket > 0)
        {
            close(self->listening_socket);
            self->listening_socket = -1;
        }
        if (self->mb_map)
        {
            modbus_mapping_free(self->mb_map);
            self->mb_map = NULL;
        }
        if (self->mb)
        {
            modbus_free(self->mb);
            self->mb = NULL;
        }
    }
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave_detach(dtiox_linux_modbus_tcp_slave_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->lock_handle);

    // stop the session, not the task
    self->session_stop_requested = true;
    self->enabled = false;

    dtlog_debug(TAG, "detaching Modbus slave on port %d", (int)self->cfg.port);

    // Close session socket to break modbus_receive() in rx task.
    if (self->session_socket > 0 && self->session_socket != self->listening_socket)
    {
        dtlog_debug(TAG, "shutting down session socket %d", self->session_socket);
        shutdown(self->session_socket, SHUT_RDWR);
        dtlog_debug(TAG, "closing session socket %d", self->session_socket);
        close(self->session_socket);
        self->session_socket = -1;
    }

cleanup:
    if (dterr == NULL)
    {
        dtlog_debug(TAG, "detached Modbus slave on port %d", (int)self->cfg.port);
    }
    else
    {
        dterr = dterr_new(dterr->error_code,
          DTERR_LOC,
          dterr,
          "dtiox_linux_modbus_tcp_slave_detach failed, rxtask at line %d",
          self->rxtasker_at_line);
    }
    return dterr;
}

// ----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave_enable(dtiox_linux_modbus_tcp_slave_t* self, bool enabled)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dtlock_acquire(self->lock_handle));

    self->enabled = enabled;

    // Clear buffers + flags whenever enable is called for predictable state.
    dtringfifo_reset(&self->rx_fifo);
    self->rx_overflow_pending = false;

    self->tx_pending = false;
    self->tx_len = 0;

    // Reset S2M response area.
    if (self->mb_map && self->mb_map->tab_registers)
    {
        self->mb_map->tab_registers[DTIOX_MODBUS_REG_S2M_STATUS] = (uint16_t)DTIOX_MODBUS_STATUS_NO_DATA;
        self->mb_map->tab_registers[DTIOX_MODBUS_REG_S2M_LEN] = 0;
    }

    DTERR_C(dtlock_release(self->lock_handle));

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave_read(dtiox_linux_modbus_tcp_slave_t* self DTIOX_READ_ARGS)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(buf);
    DTERR_ASSERT_NOT_NULL(out_read);

    if (buf_len < 0)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "buf_len < 0");
        goto cleanup;
    }

    DTERR_C(dtlock_acquire(self->lock_handle));

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
    dterr = dtiox_linux_modbus_tcp_slave__release_lock(self, dterr);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave_write(dtiox_linux_modbus_tcp_slave_t* self DTIOX_WRITE_ARGS)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(buf);

    if (len < 0)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "len < 0");
        goto cleanup;
    }

    if (len > self->cfg.max_blob_bytes)
    {
        dterr = dterr_new(
          DTERR_BADARG, DTERR_LOC, NULL, "len (%" PRId32 ") > max_blob_bytes (%" PRId32 ")", len, self->cfg.max_blob_bytes);
        goto cleanup;
    }

    DTERR_C(dtlock_acquire(self->lock_handle));

    if (!self->enabled)
        goto release_lock_and_cleanup;

    if (self->tx_pending)
    {
        dterr = dterr_new(DTERR_BUSY, DTERR_LOC, NULL, "publish failed: previous message still unsent");
        goto release_lock_and_cleanup;
    }

    memcpy(self->tx_bytes, buf, (size_t)len);
    self->tx_len = len;
    self->tx_pending = true;
    *out_written = len;

release_lock_and_cleanup:
    dterr = dtiox_linux_modbus_tcp_slave__release_lock(self, dterr);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_modbus_tcp_slave_set_rx_semaphore(dtiox_linux_modbus_tcp_slave_t* self, dtsemaphore_handle rx_semaphore)
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
dtiox_linux_modbus_tcp_slave_concat_format(dtiox_linux_modbus_tcp_slave_t* self, char* in_str, char* separator, char** out_str)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_str);

    const char* sep = (separator != NULL) ? separator : "";
    const char* base = (in_str != NULL) ? in_str : "";

    DTERR_C(dtlock_acquire(self->lock_handle));

    dttasker_info_t task_info = { 0 };
    if (self->rxtasker_handle)
        DTERR_C(dttasker_get_info(self->rxtasker_handle, &task_info));

    char tmp[256];
    int n = snprintf(tmp,
      sizeof(tmp),
      "%s%sslave=libmodbus_tcp port=%d enabled=%d rx_ring_capacity=%" PRId32 " tx_pending=%d,  rxtask_status=%s",
      base,
      sep,
      (int)self->cfg.port,
      (int)self->enabled,
      (int)self->cfg.rx_ring_capacity,
      (int)self->tx_pending,
      dttasker_state_to_string(task_info.status));

    DTERR_C(dtlock_release(self->lock_handle));

    if (n < 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "snprintf failed");
        goto cleanup;
    }

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
void
dtiox_linux_modbus_tcp_slave_dispose(dtiox_linux_modbus_tcp_slave_t* self)
{
    if (self == NULL)
        return;

    dtlog_debug(TAG, "detaching Modbus slave on port %d", (int)self->cfg.port);

    self->rxtask_stop_requested = true;
    self->session_stop_requested = true;

    // Close listening socket to break accept() in rx task.
    if (self->listening_socket >= 0)
    {
        shutdown(self->listening_socket, SHUT_RDWR);
        close(self->listening_socket);
        self->listening_socket = -1;
    }
}
