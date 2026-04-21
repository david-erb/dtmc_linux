// Linux UART backend for dtiox — POSIX /dev/tty*
//

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <dtmc_base/dtmc_base_constants.h>

#include <dtcore/dterr.h>
#include <dtcore/dtheaper.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtobject.h>
#include <dtcore/dtringfifo.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>
#include <dtmc_base/dtuart_helpers.h>

#include <dtmc/dtiox_linux_tty.h>

#define TAG "dtiox_linux_tty"

DTIOX_INIT_VTABLE(dtiox_linux_tty);
DTOBJECT_INIT_VTABLE(dtiox_linux_tty);

// -----------------------------------------------------------------------------
// Concrete backend type (private)
// -----------------------------------------------------------------------------

typedef struct dtiox_linux_tty_t
{
    DTIOX_COMMON_MEMBERS
    bool _is_malloced;

    dtiox_linux_tty_config_t config;

    int fd;

    // RX tasker
    dttasker_handle rx_tasker_handle;
    bool rx_thread_running;

    // RX FIFO and overflow tracking
    dtringfifo_t rx_fifo;
    uint8_t* rx_fifo_storage;
    int32_t fifo_dropped_bytes;
    bool rx_overflow_flag;

    // RX enabled flag (gates FIFO + semaphore)
    bool rx_enabled;

    bool rx_thread_failed;
    bool device_gone;
    int rx_errno;

    // Stats
    int64_t rx_byte_count;
    int32_t rx_read_calls;
    int64_t tx_byte_count;
    int32_t tx_write_calls;

    dtsemaphore_handle rx_semaphore;

} dtiox_linux_tty_t;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static speed_t
_map_baud(int32_t baud)
{
    switch (baud)
    {
        case 9600:
            return B9600;
        case 19200:
            return B19200;
        case 38400:
            return B38400;
        case 57600:
            return B57600;
        case 115200:
            return B115200;
        case 230400:
            return B230400;
        case 460800:
            return B460800;
        case 921600:
            return B921600;
        default:
            return B115200;
    }
}

static void
_apply_data_bits(struct termios* tio, dtuart_data_bits_t bits)
{
    tio->c_cflag &= ~CSIZE;
    tio->c_cflag |= (bits == DTUART_DATA_BITS_7) ? CS7 : CS8;
}

static void
_apply_parity(struct termios* tio, dtuart_parity_t parity)
{
    tio->c_cflag &= ~(PARENB | PARODD);
    if (parity == DTUART_PARITY_EVEN)
        tio->c_cflag |= PARENB;
    else if (parity == DTUART_PARITY_ODD)
        tio->c_cflag |= (PARENB | PARODD);
}

static void
_apply_stop_bits(struct termios* tio, dtuart_stopbits_t sb)
{
    if (sb == DTUART_STOPBITS_2)
        tio->c_cflag |= CSTOPB;
    else
        tio->c_cflag &= ~CSTOPB;
}

static void
_apply_flow(struct termios* tio, dtuart_flow_t f)
{
#ifdef CRTSCTS
    if (f == DTUART_FLOW_RTSCTS)
        tio->c_cflag |= CRTSCTS;
    else
        tio->c_cflag &= ~CRTSCTS;
#endif
}

// Clamped increment helpers
static inline void
_inc32(int32_t* v)
{
    if (*v < INT32_MAX)
        (*v)++;
}
static inline void
_add64(int64_t* v, int64_t delta)
{
    if (delta < 0)
        return;
    if (*v > INT64_MAX - delta)
        *v = INT64_MAX;
    else
        *v += delta;
}

// -----------------------------------------------------------------------------
// RX task entry
// -----------------------------------------------------------------------------

static dterr_t*
_rx_task_entry(void* arg, dttasker_handle self_task)
{
    dtiox_linux_tty_t* self = (dtiox_linux_tty_t*)arg;
    dterr_t* dterr = NULL;

    DTERR_C(dttasker_ready(self_task));
    self->rx_thread_running = true;

    uint8_t buffer[256];

    for (;;)
    {
        struct pollfd pfd;
        pfd.fd = self->fd;
        pfd.events = POLLIN | POLLERR | POLLHUP;
        pfd.revents = 0;

        int pr = poll(&pfd, 1, 100); // 100 ms
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;

            self->rx_errno = errno;
            self->rx_thread_failed = true;
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "UART poll() failed: %d (%s)", errno, strerror(errno));
            goto cleanup;
        }

        if (pr == 0)
        {
            // timeout, nothing happened
            continue;
        }

        if (pfd.revents & POLLNVAL)
        {
            self->rx_errno = EBADF;
            self->rx_thread_failed = true;
            self->device_gone = true;
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "UART fd became invalid");
            goto cleanup;
        }

        if (pfd.revents & (POLLHUP | POLLERR))
        {
            self->rx_errno = EIO;
            self->rx_thread_failed = true;
            self->device_gone = true;
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "UART device hangup/error revents=0x%x", pfd.revents);
            goto cleanup;
        }

        if (pfd.revents & POLLIN)
        {
            ssize_t n = read(self->fd, buffer, sizeof(buffer));
            if (n < 0)
            {
                if (errno == EINTR)
                    continue;

                self->rx_errno = errno;
                self->rx_thread_failed = true;

                if (errno == EIO || errno == ENODEV || errno == EBADF)
                    self->device_gone = true;

                dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "UART read() failed: %d (%s)", errno, strerror(errno));
                goto cleanup;
            }

            if (n == 0)
            {
                // For tty/USB CDC this can happen even on weird disconnect paths.
                // Do not assume it means healthy idle forever.
                continue;
            }

            _add64(&self->rx_byte_count, n);

            if (!self->rx_enabled)
                continue;

            int32_t pushed = dtringfifo_push(&self->rx_fifo, buffer, (int32_t)n);
            int32_t dropped = (int32_t)n - pushed;

            if (dropped > 0)
            {
                self->rx_overflow_flag = true;
                if (INT32_MAX - self->fifo_dropped_bytes >= dropped)
                    self->fifo_dropped_bytes += dropped;
                else
                    self->fifo_dropped_bytes = INT32_MAX;
            }
            else if (self->rx_semaphore)
            {
                DTERR_C(dtsemaphore_post(self->rx_semaphore));
            }
        }
    }

cleanup:
    dtlog_info(TAG,
      "RX thread exiting: %s failed=%d errno=%d device_gone=%d",
      dterr ? dterr->message : "no error",
      self->rx_thread_failed,
      self->rx_errno,
      self->device_gone);

    self->rx_thread_running = false;

    DTERR_C(dtsemaphore_post(self->rx_semaphore));

    return dterr;
}

// -----------------------------------------------------------------------------
// create/init/config
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tty_create(dtiox_linux_tty_t** out)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(out);

    *out = malloc(sizeof(dtiox_linux_tty_t));
    if (!*out)
        return dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc failed");

    DTERR_C(dtiox_linux_tty_init(*out));
    (*out)->_is_malloced = true;

cleanup:
    if (dterr)
    {
        free(*out);
        *out = NULL;
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtiox_linux_tty_create failed");
    }
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_tty_init(dtiox_linux_tty_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    memset(self, 0, sizeof(*self));
    self->model_number = DTMC_BASE_CONSTANTS_IOX_MODEL_LINUX_TTY;
    self->fd = -1;

    self->rx_enabled = true;

    DTERR_C(dtringfifo_init(&self->rx_fifo));

    DTERR_C(dtiox_set_vtable(self->model_number, &dtiox_linux_tty_vt));
    DTERR_C(dtobject_set_vtable(self->model_number, &dtiox_linux_tty_object_vt));

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
dterr_t*
dtiox_linux_tty_configure(dtiox_linux_tty_t* self, const dtiox_linux_tty_config_t* cfg)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(cfg);
    DTERR_ASSERT_NOT_NULL(cfg->device_path);

    self->config = *cfg;

    // --- allocate FIFO -------------------------------------------------------
    if (self->config.rx_fifo_capacity <= 0)
        self->config.rx_fifo_capacity = 4096;

    DTERR_C(dtheaper_alloc(self->config.rx_fifo_capacity, "dtiox_linux_tty RX FIFO", (void**)&self->rx_fifo_storage));

    {
        dtringfifo_config_t c = { //
            .buffer = self->rx_fifo_storage,
            .capacity = self->config.rx_fifo_capacity
        };
        DTERR_C(dtringfifo_configure(&self->rx_fifo, &c));
    }

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// attach(): open device, configure termios, create FIFO, start dttasker thread
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tty_attach(dtiox_linux_tty_t* self DTIOX_ATTACH_ARGS)
{
    dterr_t* dterr = NULL;

    if (self->fd >= 0)
        goto cleanup;

    // --- open device ---------------------------------------------------------
    int fd = open(self->config.device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "open(%s) failed: %s", self->config.device_path, strerror(errno));
    }

    // back to blocking
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0)
    {
        close(fd);
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "tcgetattr failed: %s", strerror(errno));
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, _map_baud(self->config.uart_config.baudrate));
    cfsetospeed(&tio, _map_baud(self->config.uart_config.baudrate));

    _apply_data_bits(&tio, self->config.uart_config.data_bits);
    _apply_parity(&tio, self->config.uart_config.parity);
    _apply_stop_bits(&tio, self->config.uart_config.stop_bits);
    _apply_flow(&tio, self->config.uart_config.flow);

    tio.c_cflag |= (CLOCAL | CREAD);

    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1; // 100 ms

    if (tcsetattr(fd, TCSANOW, &tio) < 0)
    {
        close(fd);
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "tcsetattr failed: %s", strerror(errno));
    }

    self->fd = fd;

    // --- start RX task -------------------------------------------------------
    {
        dttasker_config_t c = { 0 };
        c.name = "dtiox_tty";
        c.tasker_entry_point_fn = _rx_task_entry;
        c.tasker_entry_point_arg = self;
        c.tasker_info_callback = self->config.tasker_info_callback_fn;
        c.tasker_info_callback_context = self->config.tasker_info_callback_context;

        DTERR_C(dttasker_create(&self->rx_tasker_handle, &c));
        DTERR_C(dttasker_start(self->rx_tasker_handle));
    }

    dtlog_info(TAG, "attached to %s", self->config.device_path);

cleanup:
    return dterr;
}

// ----------------------------------------------------------------------------
dterr_t*
dtiox_linux_tty_detach(dtiox_linux_tty_t* self DTIOX_DETACH_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    if (self->fd >= 0)
    {
        close(self->fd);
        self->fd = -1;
    }

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// enable(): mirror CAN semantics
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tty_enable(dtiox_linux_tty_t* self DTIOX_ENABLE_ARGS)
{
    if (!self)
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "null self");

    self->rx_enabled = enabled;

    // flush kernel buffer
    if (self->fd >= 0)
        tcflush(self->fd, TCIFLUSH);

    // clear FIFO + statistics
    dtringfifo_reset(&self->rx_fifo);

    self->fifo_dropped_bytes = 0;
    self->rx_overflow_flag = false;
    self->rx_byte_count = 0;
    self->rx_read_calls = 0;
    self->tx_byte_count = 0;
    self->tx_write_calls = 0;

    self->device_gone = false;
    self->rx_thread_failed = false;
    self->rx_errno = 0;

    return NULL;
}

// -----------------------------------------------------------------------------
// read(): non-blocking, overflow once
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tty_read(dtiox_linux_tty_t* self DTIOX_READ_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(buf);
    DTERR_ASSERT_NOT_NULL(out_read);

    *out_read = 0;
    _inc32(&self->rx_read_calls);

    if (self->rx_thread_failed)
    {
        self->rx_thread_failed = false;

        if (self->device_gone)
        {
            self->device_gone = false;
            dterr = dterr_new(
              DTERR_IO, DTERR_LOC, NULL, "UART device disappeared/rebooted: %d (%s)", self->rx_errno, strerror(self->rx_errno));
            goto cleanup;
        }

        dterr =
          dterr_new(DTERR_IO, DTERR_LOC, NULL, "UART RX thread failed: %d (%s)", self->rx_errno, strerror(self->rx_errno));
        goto cleanup;
    }

    if (self->rx_overflow_flag)
    {
        self->rx_overflow_flag = false;
        dterr = dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "UART RX overflow (dropped %d bytes)", self->fifo_dropped_bytes);
        goto cleanup;
    }

    int32_t n = dtringfifo_pop(&self->rx_fifo, buf, buf_len);

    *out_read = n;

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// write(): blocking, no retention of caller buffer
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tty_write(dtiox_linux_tty_t* self DTIOX_WRITE_ARGS)
{
    *out_written = 0;
    _inc32(&self->tx_write_calls);

    if (self->fd < 0)
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "UART not attached");

    int32_t remaining = len;
    const uint8_t* p = buf;

    while (remaining > 0)
    {
        ssize_t n = write(self->fd, p, remaining);
        if (n > 0)
        {
            p += n;
            remaining -= n;
            _add64(&self->tx_byte_count, n);
        }
        else if (n < 0 && errno != EINTR)
        {
            return dterr_new(DTERR_IO, DTERR_LOC, NULL, "UART write failed: %s", strerror(errno));
        }
    }

    *out_written = len;
    return NULL;
}

// -----------------------------------------------------------------------------
// set_rx_semaphore()
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tty_set_rx_semaphore(dtiox_linux_tty_t* self DTIOX_SET_RX_SEMAPHORE_ARGS)
{
    self->rx_semaphore = rx_semaphore;
    return NULL;
}

// -----------------------------------------------------------------------------
// concat_format()
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tty_concat_format(dtiox_linux_tty_t* self DTIOX_CONCAT_FORMAT_ARGS)
{

    char tmp[32];
    dtuart_helper_to_string(&self->config.uart_config, tmp, sizeof(tmp));

    *out_str = dtstr_concat_format(in_str,
      separator,
      "linux_uart (%d) dev=\"%s\" %s "
      "rx_bytes=%" PRId64 " tx_bytes=%" PRId64 " rx_calls=%d tx_calls=%d "
      "dropped=%d rx_enabled=%s",
      self->model_number,
      self->config.device_path,
      tmp,
      self->rx_byte_count,
      self->tx_byte_count,
      self->rx_read_calls,
      self->tx_write_calls,
      self->fifo_dropped_bytes,
      self->rx_enabled ? "true" : "false");

    return NULL;
}

// -----------------------------------------------------------------------------
// dispose(): like detach
// -----------------------------------------------------------------------------

void
dtiox_linux_tty_dispose(dtiox_linux_tty_t* self)
{
    if (!self)
        return;

    if (self->fd >= 0)
    {
        close(self->fd);
        self->fd = -1;
    }

    // until we can join the RX thread, just leak it and its resources on dispose if it's still running
    // dtheaper_free(self->rx_fifo_storage);
    // memset(self, 0, sizeof(*self));
}

// --------------------------------------------------------------------------------------------
// dtobject implementation
// --------------------------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------
// Copy constructor
void
dtiox_linux_tty_copy(dtiox_linux_tty_t* this, dtiox_linux_tty_t* that)
{
    // this object does not support copying
    (void)this;
    (void)that;
}

// --------------------------------------------------------------------------------------------
// Equality check
bool
dtiox_linux_tty_equals(dtiox_linux_tty_t* a, dtiox_linux_tty_t* b)
{
    if (a == NULL || b == NULL)
    {
        return false;
    }

    // TODO: Reconside equality semantics for dtiox_linux_tty_equals backend.
    return (a->model_number == b->model_number &&                                 //
            strcmp(a->config.device_path, b->config.device_path) == 0 &&          //
            a->config.uart_config.baudrate == b->config.uart_config.baudrate &&   //
            a->config.uart_config.parity == b->config.uart_config.parity &&       //
            a->config.uart_config.data_bits == b->config.uart_config.data_bits && //
            a->config.uart_config.stop_bits == b->config.uart_config.stop_bits && //
            a->config.uart_config.flow == b->config.uart_config.flow);
}

// --------------------------------------------------------------------------------------------
const char*
dtiox_linux_tty_get_class(dtiox_linux_tty_t* self)
{
    return "dtiox_linux_tty_t";
}

// --------------------------------------------------------------------------------------------

bool
dtiox_linux_tty_is_iface(dtiox_linux_tty_t* self, const char* iface_name)
{
    return strcmp(iface_name, DTIOX_IFACE_NAME) == 0 || //
           strcmp(iface_name, "dtobject_iface") == 0;
}

// --------------------------------------------------------------------------------------------
// Convert to string
void
dtiox_linux_tty_to_string(dtiox_linux_tty_t* self, char* buffer, size_t buffer_size)
{
    if (self == NULL || buffer == NULL || buffer_size == 0)
        return;

    char tmp[32];
    dtuart_helper_to_string(&self->config.uart_config, tmp, sizeof(tmp));

    snprintf(buffer, buffer_size, "%s %s", self->config.device_path, tmp);
    buffer[buffer_size - 1] = '\0';
}
