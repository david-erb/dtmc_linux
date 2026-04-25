// Linux CAN backend for dtiox — Raspberry Pi 5 / SocketCAN

#include <errno.h>
#include <net/if.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
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
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>

#include <dtmc/dtiox_linux_canbus.h>

#define dtiox_linux_canbus_DEFAULT_RX_RING_CAPACITY 1024

#define TAG "dtiox_linux_canbus"

// vtable
DTIOX_INIT_VTABLE(dtiox_linux_canbus);
DTOBJECT_INIT_VTABLE(dtiox_linux_canbus);

typedef struct dtiox_linux_canbus_t
{
    DTIOX_COMMON_MEMBERS
    bool _is_malloced;

    dtiox_linux_canbus_config_t config;

    int sock_fd;
    struct sockaddr_can addr;

    dtsemaphore_handle rx_semaphore;

    // RX thread plumbing
    dttasker_handle rx_tasker_handle;
    bool rx_thread_running;

    // Whether the RX thread should actually feed the FIFO.
    // When false, frames are still received and discarded.
    bool rx_enabled;

    // Overflow handling: set when FIFO write fails; reported on next read.
    bool rx_overflow_flag;

    // RX FIFO: thread producer, foreground consumer
    dtringfifo_t rx_fifo;
    uint8_t* rx_fifo_storage;
    pthread_mutex_t rx_fifo_mutex;

    // Diagnostics
    int64_t rx_frame_count;
    int64_t rx_byte_count;
    int32_t _rx_monitor_count;
    int32_t fifo_dropped_bytes;

    // Error propagated from RX thread, consumed on next read()
    dterr_t* thread_dterr;

    // (No shutdown/join coordination: thread is allowed to live with the process.)
} dtiox_linux_canbus_t;

// -----------------------------------------------------------------------------
// Internal helpers

static inline int32_t
_rx_fifo_capacity_from_config(const dtiox_linux_canbus_t* self)
{
    int32_t capacity = self->config.rx_ring_capacity;
    if (capacity <= 0)
        capacity = dtiox_linux_canbus_DEFAULT_RX_RING_CAPACITY;
    return capacity;
}

static inline int32_t
_rx_fifo_push_locked(dtiox_linux_canbus_t* self, const uint8_t* src, int32_t len)
{
    if (!self || !src || len <= 0)
        return 0;

    pthread_mutex_lock(&self->rx_fifo_mutex);
    int32_t written = dtringfifo_push(&self->rx_fifo, src, len);
    int32_t dropped = len - written;
    if (dropped > 0)
    {
        self->rx_overflow_flag = true;
        if (INT32_MAX - self->fifo_dropped_bytes >= dropped)
            self->fifo_dropped_bytes += dropped;
        else
            self->fifo_dropped_bytes = INT32_MAX;
    }
    pthread_mutex_unlock(&self->rx_fifo_mutex);

    return written;
}

// -----------------------------------------------------------------------------
static inline int32_t
_rx_fifo_pop_locked(dtiox_linux_canbus_t* self, uint8_t* dest, int32_t len)
{
    if (!self || !dest || len <= 0)
        return 0;

    pthread_mutex_lock(&self->rx_fifo_mutex);
    int32_t read = dtringfifo_pop(&self->rx_fifo, dest, len);
    pthread_mutex_unlock(&self->rx_fifo_mutex);

    return read;
}

// -----------------------------------------------------------------------------
// RX thread
//
// Note: this thread is started in attach() and is never joined or explicitly
// shut down. It will normally live for the life of the process. If a fatal
// error occurs on recv(), it records an error and exits.

static dterr_t*
_rx_task_entry(void* arg, dttasker_handle tasker_handle)
{
    dterr_t* dterr = NULL;
    dtiox_linux_canbus_t* self = (dtiox_linux_canbus_t*)arg;

    DTERR_ASSERT_NOT_NULL(self);

    // signal we are ready for business to the process who started us
    DTERR_C(dttasker_ready(tasker_handle));

    for (;;)
    {
        struct can_frame frame;
        ssize_t n = recv(self->sock_fd, &frame, sizeof(frame), 0);
        if (n < 0)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "SocketCAN recv failed: %d (%s)", errno, strerror(errno));
            goto cleanup;
        }

        if (n != (ssize_t)sizeof(struct can_frame))
        {
            // Should not happen on SocketCAN; treat as fatal
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "SocketCAN recv returned unexpected size: %zd", n);
            goto cleanup;
        }

        int32_t payload_len = (int32_t)frame.can_dlc;
        if (payload_len <= 0)
            continue;
        if (payload_len > 8)
            payload_len = 8; // classic CAN safety clamp

        // Count frames/bytes regardless of whether they are enqueued.
        self->rx_frame_count++;
        self->rx_byte_count += payload_len;
        self->_rx_monitor_count++;

        // If disabled, just discard the payload.
        if (!self->rx_enabled)
            continue;

        int32_t written = _rx_fifo_push_locked(self, frame.data, payload_len);
        if (written > 0)
        {
            if (self->rx_semaphore)
            {
                DTERR_C(dtsemaphore_post(self->rx_semaphore));
            }
        }
    }

cleanup:
    self->rx_thread_running = false;
    return dterr;
}

// -----------------------------------------------------------------------------
// Creation / initialization

dterr_t*
dtiox_linux_canbus_create(dtiox_linux_canbus_t** self_ptr)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self_ptr);

    *self_ptr = (dtiox_linux_canbus_t*)malloc(sizeof(**self_ptr));
    if (!*self_ptr)
        return dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc %zu", sizeof(**self_ptr));

    DTERR_C(dtiox_linux_canbus_init(*self_ptr));
    (*self_ptr)->_is_malloced = true;

cleanup:
    if (dterr)
    {
        free(*self_ptr);
        *self_ptr = NULL;
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtiox_linux_canbus_create failed");
    }
    return dterr;
}

dterr_t*
dtiox_linux_canbus_init(dtiox_linux_canbus_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    memset(self, 0, sizeof(*self));

    self->model_number = DTMC_BASE_CONSTANTS_IOX_MODEL_LINUX_CAN;
    self->sock_fd = -1;

    // Default: RX enabled once the thread is running.
    self->rx_enabled = true;

    DTERR_C(dtringfifo_init(&self->rx_fifo));

    if (pthread_mutex_init(&self->rx_fifo_mutex, NULL) != 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "pthread_mutex_init failed");
        goto cleanup;
    }

    DTERR_C(dtiox_set_vtable(self->model_number, &dtiox_linux_canbus_vt));
    DTERR_C(dtobject_set_vtable(self->model_number, &dtiox_linux_canbus_object_vt));

cleanup:
    if (dterr)
    {
        dtringfifo_reset(&self->rx_fifo);
        // Best effort destroy; ignore errors.
        pthread_mutex_destroy(&self->rx_fifo_mutex);
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtiox_linux_canbus_init failed");
    }
    return dterr;
}

// -----------------------------------------------------------------------------
// Configure

dterr_t*
dtiox_linux_canbus_configure(dtiox_linux_canbus_t* self, const dtiox_linux_canbus_config_t* config)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(config);
    DTERR_ASSERT_NOT_NULL(config->interface_name);

    if (config->txid == 0)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "txid must be non-zero");
        goto cleanup;
    }

    if (config->rx_ring_capacity < 0)
    {
        dterr = dterr_new(DTERR_BADARG,
          DTERR_LOC,
          NULL,
          "rx_ring_capacity must be >= 0 (0 means default, got %" PRId32 ")",
          config->rx_ring_capacity);
        goto cleanup;
    }

    self->config = *config;

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// Attach — open and bind the CAN socket, allocate RX FIFO storage, and start
//          the RX thread.
//
// Note: On success, the RX thread is created exactly once and runs until it
// hits a fatal error or the process exits. It is never joined or explicitly
// shut down.

dterr_t*
dtiox_linux_canbus_attach(dtiox_linux_canbus_t* self DTIOX_ATTACH_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    if (self->sock_fd >= 0)
    {
        // Already attached; this is a no-op.
        goto cleanup;
    }

    const char* ifname = self->config.interface_name;

    int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "socket(PF_CAN, SOCK_RAW) failed: %d (%s)", errno, strerror(errno));
        goto cleanup;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    ifr.ifr_name[sizeof(ifr.ifr_name) - 1] = '\0';

    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0)
    {
        dterr = dterr_new(
          DTERR_FAIL, DTERR_LOC, NULL, "ioctl(SIOCGIFINDEX) for \"%s\" failed: %d (%s)", ifname, errno, strerror(errno));
        close(fd);
        goto cleanup;
    }

    memset(&self->addr, 0, sizeof(self->addr));
    self->addr.can_family = AF_CAN;
    self->addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(fd, (struct sockaddr*)&self->addr, sizeof(self->addr)) < 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "bind(can, \"%s\") failed: %d (%s)", ifname, errno, strerror(errno));
        close(fd);
        goto cleanup;
    }

    // (Optional) you could install CAN filters here based on config.

    self->sock_fd = fd;

    // Allocate and configure RX FIFO storage.
    {
        int32_t capacity = _rx_fifo_capacity_from_config(self);
        if (capacity <= 0)
        {
            dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "rx_ring_capacity must be > 0 after defaults applied");
            goto cleanup;
        }

        if (self->rx_fifo_storage)
        {
            free(self->rx_fifo_storage);
            self->rx_fifo_storage = NULL;
        }

        self->rx_fifo_storage = (uint8_t*)malloc((size_t)capacity);
        if (!self->rx_fifo_storage)
        {
            dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc rx ring buffer %d bytes failed", capacity);
            goto cleanup;
        }

        dtringfifo_config_t fifo_cfg = {
            .buffer = self->rx_fifo_storage,
            .capacity = capacity,
        };

        DTERR_C(dtringfifo_configure(&self->rx_fifo, &fifo_cfg));
    }

    // Start RX task
    {
        dttasker_config_t c = { 0 };
        c.name = "can_rx";
        c.tasker_entry_point_fn = _rx_task_entry;
        c.tasker_entry_point_arg = self;
        DTERR_C(dttasker_create(&self->rx_tasker_handle, &c));
        DTERR_C(dttasker_start(self->rx_tasker_handle));
    }

    dtlog_debug(TAG, "attached to SocketCAN interface \"%s\" (fd=%d, txid=0x%X)", ifname, self->sock_fd, self->config.txid);

cleanup:
    if (dterr)
    {
        if (self->sock_fd >= 0)
        {
            close(self->sock_fd);
            self->sock_fd = -1;
        }

        if (self->rx_fifo_storage)
        {
            free(self->rx_fifo_storage);
            self->rx_fifo_storage = NULL;
        }
        dtringfifo_reset(&self->rx_fifo);
    }
    return dterr;
}

// ----------------------------------------------------------------------------
dterr_t*
dtiox_linux_canbus_detach(dtiox_linux_canbus_t* self DTIOX_DETACH_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// Enable — in this simplified model:
//
//   * enable_rx_irq == true:
//       - clears the RX FIFO
//       - sets rx_enabled = true (RX thread keeps running)
//   * enable_rx_irq == false:
//       - sets rx_enabled = false (RX thread still receives, but discards)
//
// No thread start/stop, no shutdown, no join.

dterr_t*
dtiox_linux_canbus_enable(dtiox_linux_canbus_t* self DTIOX_ENABLE_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    if (enabled)
    {
        // Clear FIFO and enable enqueue.
        pthread_mutex_lock(&self->rx_fifo_mutex);
        dtringfifo_reset(&self->rx_fifo);
        pthread_mutex_unlock(&self->rx_fifo_mutex);

        self->rx_enabled = true;
    }
    else
    {
        // Do not stop or join the thread; just stop feeding the FIFO.
        self->rx_enabled = false;
    }

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// Read — non-blocking from FIFO if RX thread is alive; otherwise direct socket.

dterr_t*
dtiox_linux_canbus_read(dtiox_linux_canbus_t* self DTIOX_READ_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(buf);
    DTERR_ASSERT_NOT_NULL(out_read);

    *out_read = 0;

    // If RX task died, surface this as an error on read.
    {
        dttasker_info_t task_info;
        dterr = dttasker_get_info(self->rx_tasker_handle, &task_info);
        if (dterr == NULL)
            dterr = task_info.dterr;
        if (dterr != NULL || task_info.status != RUNNING)
        {
            dterr = dterr_new(DTERR_FAIL,
              DTERR_LOC,
              dterr,
              "CAN RX task state %s error %p",
              dttasker_state_to_string(task_info.status),
              task_info.dterr);
            goto cleanup;
        }
    }

    // Overflow is reported once on the next read.
    if (self->rx_overflow_flag)
    {
        self->rx_overflow_flag = false;
        dterr =
          dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "RX FIFO overflow (dropped %" PRId32 " bytes)", self->fifo_dropped_bytes);
        goto cleanup;
    }

    if (self->sock_fd < 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "socket not attached");
        goto cleanup;
    }

    *out_read = _rx_fifo_pop_locked(self, buf, buf_len);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// Write — chop byte stream into CAN frames and send.

dterr_t*
dtiox_linux_canbus_write(dtiox_linux_canbus_t* self DTIOX_WRITE_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(buf);
    DTERR_ASSERT_NOT_NULL(out_written);

    *out_written = 0;

    if (self->sock_fd < 0)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "socket not attached");
        goto cleanup;
    }

    int32_t total_written = 0;

    while (total_written < len)
    {
        int32_t remaining = len - total_written;
        int32_t chunk_len = (remaining > 8) ? 8 : remaining;

        struct can_frame frame;
        memset(&frame, 0, sizeof(frame));

        frame.can_id = self->config.txid; // no flags yet (standard ID)
        frame.can_dlc = (uint8_t)chunk_len;
        memcpy(frame.data, &buf[total_written], (size_t)chunk_len);

        ssize_t n = send(self->sock_fd, &frame, sizeof(frame), 0);
        if (n < 0)
        {
            dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "send() failed: %d (%s)", errno, strerror(errno));
            goto cleanup;
        }
        if (n != (ssize_t)sizeof(struct can_frame))
        {
            dterr = dterr_new(DTERR_FAIL, DTERR_LOC, NULL, "send() short write: %zd bytes", n);
            goto cleanup;
        }

        total_written += chunk_len;
    }

    *out_written = total_written;

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// RX semaphore

dterr_t*
dtiox_linux_canbus_set_rx_semaphore(dtiox_linux_canbus_t* self DTIOX_SET_RX_SEMAPHORE_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    self->rx_semaphore = rx_semaphore;

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// concat_format — debug string

dterr_t*
dtiox_linux_canbus_concat_format(dtiox_linux_canbus_t* self DTIOX_CONCAT_FORMAT_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_str);

    int32_t capacity = _rx_fifo_capacity_from_config(self);

    dttasker_info_t task_info = { 0 };
    if (self->rx_tasker_handle)
        DTERR_C(dttasker_get_info(self->rx_tasker_handle, &task_info));

    *out_str = dtstr_concat_format(in_str,
      separator,
      "linux_can (%" PRId32 ") iface=\"%s\" tx_id=0x%X rx_ring_capacity=%" PRId32 " frames=%" PRId64 " bytes=%" PRId64
      " dropped=%" PRId32 " rx_enabled=%s rx_task_status=%s",
      self->model_number,
      self->config.interface_name ? self->config.interface_name : "(null)",
      self->config.txid,
      capacity,
      (int64_t)self->rx_frame_count,
      (int64_t)self->rx_byte_count,
      (int32_t)self->fifo_dropped_bytes,
      self->rx_enabled ? "true" : "false",
      dttasker_state_to_string(task_info.status));

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
dtiox_linux_canbus_dispose(dtiox_linux_canbus_t* self)
{
    (void)self;
    // No-op by design.
}

// --------------------------------------------------------------------------------------------
// dtobject implementation
// --------------------------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------
// Copy constructor
void
dtiox_linux_canbus_copy(dtiox_linux_canbus_t* this, dtiox_linux_canbus_t* that)
{
    // this object does not support copying
    (void)this;
    (void)that;
}

// --------------------------------------------------------------------------------------------
// Equality check
bool
dtiox_linux_canbus_equals(dtiox_linux_canbus_t* a, dtiox_linux_canbus_t* b)
{
    if (a == NULL || b == NULL)
    {
        return false;
    }

    // TODO: Reconside equality semantics for dtiox_linux_tty_equals backend.
    return (a->model_number == b->model_number &&                              //
            strcmp(a->config.interface_name, b->config.interface_name) == 0 && //
            a->config.txid == b->config.txid);
}

// --------------------------------------------------------------------------------------------
const char*
dtiox_linux_canbus_get_class(dtiox_linux_canbus_t* self)
{
    return "dtiox_linux_canbus_t";
}

// --------------------------------------------------------------------------------------------

bool
dtiox_linux_canbus_is_iface(dtiox_linux_canbus_t* self, const char* iface_name)
{
    return strcmp(iface_name, DTIOX_IFACE_NAME) == 0 || //
           strcmp(iface_name, "dtobject_iface") == 0;
}

// --------------------------------------------------------------------------------------------
// Convert to string
void
dtiox_linux_canbus_to_string(dtiox_linux_canbus_t* self, char* buffer, size_t buffer_size)
{
    dterr_t* dterr = NULL;

    if (self == NULL || buffer == NULL || buffer_size == 0)
        return;

    snprintf(buffer, buffer_size, "%s@%" PRId32, self->config.interface_name, self->config.txid);
    buffer[buffer_size - 1] = '\0';

cleanup:
    if (dterr)
    {
        snprintf(buffer, buffer_size, "dtiox_linux_tty_to_string failed: %s", dterr->message);
        buffer[buffer_size - 1] = '\0';
        dterr_dispose(dterr);
    }
}
