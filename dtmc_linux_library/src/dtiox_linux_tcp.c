// Linux TCP backend for dtiox — POSIX TCP socket
//

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <dtmc_base/dtmc_base_constants.h>

#include <dtcore/dtcore_helper.h>
#include <dtcore/dterr.h>
#include <dtcore/dtheaper.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtobject.h>
#include <dtcore/dtringfifo.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>

#include <dtmc/dtiox_linux_tcp.h>

#define TAG "dtiox_linux_tcp"
#define dtlog_debug(...)

DTIOX_INIT_VTABLE(dtiox_linux_tcp);
DTOBJECT_INIT_VTABLE(dtiox_linux_tcp);

// -----------------------------------------------------------------------------
// Concrete backend type (private)
// -----------------------------------------------------------------------------

typedef struct dtiox_linux_tcp_t
{
    DTIOX_COMMON_MEMBERS
    bool _is_malloced;

    dtiox_linux_tcp_config_t config;

    // connected socket
    int fd;

    // server mode only
    int listen_fd;

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

    // connection state
    bool is_connected;

    dtsemaphore_handle rx_semaphore;

} dtiox_linux_tcp_t;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static void
_close_if_open(int* fd_ptr)
{
    if (fd_ptr && *fd_ptr >= 0)
    {
        close(*fd_ptr);
        *fd_ptr = -1;
    }
}

static dterr_t*
_set_socket_common_options(int fd, const dtiox_linux_tcp_config_t* cfg)
{
    dterr_t* dterr = NULL;
    int optval = 0;

    if (cfg->keepalive)
    {
        optval = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(optval)) < 0)
        {
            return dterr_new(DTERR_IO, DTERR_LOC, NULL, "setsockopt(SO_KEEPALIVE) failed: %s", strerror(errno));
        }
    }

    if (cfg->tcp_nodelay)
    {
        optval = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval)) < 0)
        {
            return dterr_new(DTERR_IO, DTERR_LOC, NULL, "setsockopt(TCP_NODELAY) failed: %s", strerror(errno));
        }
    }

cleanup:
    return dterr;
}

static dterr_t*
_connect_client_socket(dtiox_linux_tcp_t* self, int* out_fd)
{
    dterr_t* dterr = NULL;
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    struct addrinfo* rp = NULL;
    char port_str[32];
    int fd = -1;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_fd);

    if (!self->config.remote_host || self->config.remote_port <= 0 || self->config.remote_port > 65535)
    {
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "invalid TCP client remote endpoint");
    }

    snprintf(port_str, sizeof(port_str), "%d", self->config.remote_port);
    port_str[sizeof(port_str) - 1] = '\0';

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // force IPv4
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(self->config.remote_host, port_str, &hints, &result) != 0)
    {
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "getaddrinfo(%s:%s) failed", self->config.remote_host, port_str);
    }

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        char hostbuf[NI_MAXHOST];
        char servbuf[NI_MAXSERV];

        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
        {
            dtlog_debug(TAG,
              "socket() failed for %s:%s family=%d socktype=%d protocol=%d: errno=%d (%s)",
              self->config.remote_host,
              port_str,
              rp->ai_family,
              rp->ai_socktype,
              rp->ai_protocol,
              errno,
              strerror(errno));
            continue;
        }

        DTERR_C(_set_socket_common_options(fd, &self->config));

        hostbuf[0] = '\0';
        servbuf[0] = '\0';

        if (getnameinfo(rp->ai_addr,
              rp->ai_addrlen,
              hostbuf,
              sizeof(hostbuf),
              servbuf,
              sizeof(servbuf),
              NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        {
            snprintf(hostbuf, sizeof(hostbuf), "<unknown>");
            snprintf(servbuf, sizeof(servbuf), "<unknown>");
        }

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
        {
            dtlog_debug(TAG,
              "connect() succeeded to %s:%s family=%d socktype=%d protocol=%d",
              hostbuf,
              servbuf,
              rp->ai_family,
              rp->ai_socktype,
              rp->ai_protocol);
            *out_fd = fd;
            fd = -1;
            goto cleanup;
        }
        {
            int saved_errno = errno;

            dterr = dterr_new(DTERR_IO,
              DTERR_LOC,
              NULL,
              "connect() failed to %s:%s family=%d socktype=%d protocol=%d: errno=%d (%s)",
              hostbuf,
              servbuf,
              rp->ai_family,
              rp->ai_socktype,
              rp->ai_protocol,
              saved_errno,
              strerror(saved_errno));
        }
        _close_if_open(&fd);

        goto cleanup;
    }

cleanup:
    if (result)
        freeaddrinfo(result);

    _close_if_open(&fd);
    return dterr;
}

static dterr_t*
_bind_and_accept_server_socket(dtiox_linux_tcp_t* self, int* out_listen_fd, int* out_client_fd)
{
    dterr_t* dterr = NULL;
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    struct addrinfo* rp = NULL;
    char port_str[32];
    int listen_fd = -1;
    int client_fd = -1;
    int optval = 1;
    const char* bind_host = self->config.local_bind_host ? self->config.local_bind_host : "0.0.0.0";

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_listen_fd);
    DTERR_ASSERT_NOT_NULL(out_client_fd);

    if (self->config.local_bind_port <= 0 || self->config.local_bind_port > 65535)
    {
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "invalid TCP server bind port");
    }

    snprintf(port_str, sizeof(port_str), "%d", self->config.local_bind_port);
    port_str[sizeof(port_str) - 1] = '\0';

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET; // force IPv4
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(bind_host, port_str, &hints, &result) != 0)
    {
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "getaddrinfo(%s:%s) failed", bind_host, port_str);
    }

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        listen_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (listen_fd < 0)
            continue;

        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
            goto cleanup;
        }

        if (bind(listen_fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        _close_if_open(&listen_fd);
    }

    if (listen_fd < 0)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "bind(%s:%d) failed", bind_host, self->config.local_bind_port);
        goto cleanup;
    }

    if (listen(listen_fd, 1) < 0)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "listen() failed: %s", strerror(errno));
        goto cleanup;
    }

    client_fd = accept(listen_fd, NULL, NULL);
    if (client_fd < 0)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "accept() failed: %s", strerror(errno));
        goto cleanup;
    }

    DTERR_C(_set_socket_common_options(client_fd, &self->config));

    *out_listen_fd = listen_fd;
    *out_client_fd = client_fd;
    listen_fd = -1;
    client_fd = -1;

cleanup:
    if (result)
        freeaddrinfo(result);

    _close_if_open(&client_fd);
    _close_if_open(&listen_fd);
    return dterr;
}

// -----------------------------------------------------------------------------
// RX task entry
// -----------------------------------------------------------------------------

static dterr_t*
_rx_task_entry(void* arg, dttasker_handle self_task)
{
    dterr_t* dterr = NULL;
    dtiox_linux_tcp_t* self = (dtiox_linux_tcp_t*)arg;

    DTERR_C(dttasker_ready(self_task));
    self->rx_thread_running = true;

    uint8_t buffer[1024];

    for (;;)
    {
        ssize_t n = recv(self->fd, buffer, sizeof(buffer), 0);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "TCP recv() failed: %d (%s)", errno, strerror(errno));
            goto cleanup;
        }

        if (n == 0)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "TCP peer closed connection");
            goto cleanup;
        }

        if (!self->rx_enabled)
            continue;

        int32_t pushed = dtringfifo_push(&self->rx_fifo, buffer, (int32_t)n);
        int32_t dropped = (int32_t)n - pushed;

        if (dropped > 0)
        {
            dtlog_debug(TAG,
              "RX FIFO %d overflow, only wrote %d bytes out of %d, dropped %d bytes",
              self->rx_fifo.capacity,
              pushed,
              n,
              dropped);
            self->rx_overflow_flag = true;
            DTCORE_HELPER_ADD32(self->fifo_dropped_bytes, dropped);
        }
        else if (self->rx_semaphore)
        {
            DTERR_C(dtsemaphore_post(self->rx_semaphore));
        }
    }

cleanup:
    self->is_connected = false;
    self->rx_thread_running = false;

    if (self->fd >= 0)
    {
        close(self->fd);
        self->fd = -1;
    }
    if (self->listen_fd >= 0)
    {
        close(self->listen_fd);
        self->listen_fd = -1;
    }

    // wake up any readers
    if (self->rx_semaphore)
        dtsemaphore_post(self->rx_semaphore);

    return dterr;
}

// -----------------------------------------------------------------------------
// create/init/config
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tcp_create(dtiox_linux_tcp_t** out)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(out);

    *out = malloc(sizeof(dtiox_linux_tcp_t));
    if (!*out)
        return dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc failed");

    DTERR_C(dtiox_linux_tcp_init(*out));
    (*out)->_is_malloced = true;

cleanup:
    if (dterr)
    {
        free(*out);
        *out = NULL;
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtiox_linux_tcp_create failed");
    }
    return dterr;
}

// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tcp_init(dtiox_linux_tcp_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    memset(self, 0, sizeof(*self));
    self->model_number = DTMC_BASE_CONSTANTS_IOX_MODEL_LINUX_TCP;
    self->fd = -1;
    self->listen_fd = -1;
    self->rx_enabled = true;
    self->is_connected = false;

    DTERR_C(dtringfifo_init(&self->rx_fifo));

    DTERR_C(dtiox_set_vtable(self->model_number, &dtiox_linux_tcp_vt));
    DTERR_C(dtobject_set_vtable(self->model_number, &dtiox_linux_tcp_object_vt));

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tcp_configure(dtiox_linux_tcp_t* self, const dtiox_linux_tcp_config_t* cfg)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(cfg);

    if (cfg->mode == DTIOX_LINUX_TCP_MODE_CLIENT)
    {
        DTERR_ASSERT_NOT_NULL(cfg->remote_host);
        if (cfg->remote_port <= 0 || cfg->remote_port > 65535)
            return dterr_new(DTERR_BADARG,
              DTERR_LOC,
              NULL,
              "invalid TCP client config host %s port %" PRId32,
              cfg->remote_host,
              cfg->remote_port);
    }
    else if (cfg->mode == DTIOX_LINUX_TCP_MODE_SERVER)
    {
        DTERR_ASSERT_NOT_NULL(cfg->local_bind_host);
        if (cfg->local_bind_port <= 0 || cfg->local_bind_port > 65535)
            return dterr_new(DTERR_BADARG,
              DTERR_LOC,
              NULL,
              "invalid TCP server config host %s port %" PRId32,
              cfg->local_bind_host,
              cfg->local_bind_port);
    }
    else
    {
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "unknown TCP mode");
    }

    self->config = *cfg;

    if (self->config.rx_fifo_capacity <= 0)
        self->config.rx_fifo_capacity = 4096;

    DTERR_C(dtheaper_alloc(self->config.rx_fifo_capacity, "TCP RX FIFO", (void**)&self->rx_fifo_storage));

    {
        dtringfifo_config_t c = {
            .buffer = self->rx_fifo_storage,
            .capacity = self->config.rx_fifo_capacity,
        };
        DTERR_C(dtringfifo_configure(&self->rx_fifo, &c));
    }

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// attach(): connect/bind+accept, create FIFO, start dttasker thread
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tcp_attach(dtiox_linux_tcp_t* self DTIOX_ATTACH_ARGS)
{
    dterr_t* dterr = NULL;
    int fd = -1;
    int listen_fd = -1;

    DTERR_ASSERT_NOT_NULL(self);

    if (self->fd >= 0)
        goto cleanup;

    if (self->config.mode == DTIOX_LINUX_TCP_MODE_CLIENT)
    {
        DTERR_C(_connect_client_socket(self, &fd));
    }
    else
    {
        dtlog_debug(TAG,
          "TCP server listening on %s:%" PRId32,
          self->config.local_bind_host ? self->config.local_bind_host : "0.0.0.0",
          self->config.local_bind_port);

        DTERR_C(_bind_and_accept_server_socket(self, &listen_fd, &fd));
    }

    self->fd = fd;
    self->listen_fd = listen_fd;
    self->is_connected = true;

    if (self->config.mode == DTIOX_LINUX_TCP_MODE_CLIENT)
    {
        dtlog_debug(TAG,
          "TCP client connected to %s:%" PRId32 " on fd %d",
          self->config.remote_host ? self->config.remote_host : "?",
          self->config.remote_port,
          self->fd);
    }
    else
    {
        dtlog_debug(TAG,
          "TCP server accepted client on %s:%" PRId32 " on fd %d",
          self->config.local_bind_host ? self->config.local_bind_host : "0.0.0.0",
          self->config.local_bind_port,
          self->fd);
    }

    {
        dttasker_config_t c = { 0 };
        c.name = "dtiox_tcp";
        c.tasker_entry_point_fn = _rx_task_entry;
        c.tasker_entry_point_arg = self;
        c.tasker_info_callback = self->config.tasker_info_callback_fn;
        c.tasker_info_callback_context = self->config.tasker_info_callback_context;

        DTERR_C(dttasker_create(&self->rx_tasker_handle, &c));
        DTERR_C(dttasker_start(self->rx_tasker_handle));
    }

cleanup:

    if (dterr)
    {
        _close_if_open(&fd);
        _close_if_open(&listen_fd);
    }

    return dterr;
}

// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tcp_detach(dtiox_linux_tcp_t* self DTIOX_DETACH_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    self->is_connected = false;

    _close_if_open(&self->fd);
    _close_if_open(&self->listen_fd);

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// enable(): mirror tty semantics
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tcp_enable(dtiox_linux_tcp_t* self DTIOX_ENABLE_ARGS)
{
    if (!self)
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "null self");

    self->rx_enabled = enabled;

    dtringfifo_reset(&self->rx_fifo);

    self->fifo_dropped_bytes = 0;
    self->rx_overflow_flag = false;

    return NULL;
}

// -----------------------------------------------------------------------------
// read(): non-blocking, overflow once
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tcp_read(dtiox_linux_tcp_t* self DTIOX_READ_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(buf);
    DTERR_ASSERT_NOT_NULL(out_read);

    *out_read = 0;

    if (!self->is_connected)
    {
        self->rx_overflow_flag = false;
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "TCP RX disconnected");
        goto cleanup;
    }

    if (self->rx_overflow_flag)
    {
        self->rx_overflow_flag = false;
        dterr =
          dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "TCP RX overflow (dropped %" PRIu32 " bytes)", self->fifo_dropped_bytes);
        goto cleanup;
    }

    {
        int32_t n = dtringfifo_pop(&self->rx_fifo, buf, buf_len);
        if (n > 0)
            *out_read = n;
    }

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// write(): blocking until all sent, no retention of caller buffer
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tcp_write(dtiox_linux_tcp_t* self DTIOX_WRITE_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(buf);
    DTERR_ASSERT_NOT_NULL(out_written);

    *out_written = 0;

    if (self->fd < 0 || !self->is_connected)
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "TCP not attached");

    int32_t remaining = len;
    const uint8_t* p = buf;

    while (remaining > 0)
    {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(self->fd, p, remaining, MSG_NOSIGNAL);
#else
        ssize_t n = send(self->fd, p, remaining, 0);
#endif
        if (n > 0)
        {
            p += n;
            remaining -= (int32_t)n;
        }
        else if (n == 0)
        {
            self->is_connected = false;
            return dterr_new(DTERR_IO, DTERR_LOC, NULL, "TCP send returned 0");
        }
        else if (errno != EINTR)
        {
            self->is_connected = false;
            return dterr_new(DTERR_IO, DTERR_LOC, NULL, "TCP send failed on fd %d: %s", self->fd, strerror(errno));
        }
    }

    *out_written = len;
cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// set_rx_semaphore()
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tcp_set_rx_semaphore(dtiox_linux_tcp_t* self DTIOX_SET_RX_SEMAPHORE_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    self->rx_semaphore = rx_semaphore;
cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// concat_format()
// -----------------------------------------------------------------------------

dterr_t*
dtiox_linux_tcp_concat_format(dtiox_linux_tcp_t* self DTIOX_CONCAT_FORMAT_ARGS)
{
    dterr_t* dterr = NULL;
    const char* mode_str = "unknown";
    const char* host_str = NULL;
    int32_t port = 0;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_str);

    if (self->config.mode == DTIOX_LINUX_TCP_MODE_CLIENT)
    {
        mode_str = "client";
        host_str = self->config.remote_host ? self->config.remote_host : "?";
        port = self->config.remote_port;
    }
    else if (self->config.mode == DTIOX_LINUX_TCP_MODE_SERVER)
    {
        mode_str = "server";
        host_str = self->config.local_bind_host ? self->config.local_bind_host : "0.0.0.0";
        port = self->config.local_bind_port;
    }
    else
    {
        mode_str = "invalid";
        host_str = "?";
        port = 0;
    }

    *out_str = dtstr_concat_format(in_str,
      separator,
      "linux_tcp (%d) mode=%s endpoint=\"%s:%d\" "
      "connected=%s "
      "fifo_capacity=%" PRId32 " "
      "dropped=%" PRId32 " rx_enabled=%s",
      self->model_number,
      mode_str,
      host_str,
      port,
      self->is_connected ? "true" : "false",
      self->config.rx_fifo_capacity,
      self->fifo_dropped_bytes,
      self->rx_enabled ? "true" : "false");

cleanup:
    return dterr;
}

// -----------------------------------------------------------------------------
// dispose(): same spirit as tty version
// -----------------------------------------------------------------------------

void
dtiox_linux_tcp_dispose(dtiox_linux_tcp_t* self)
{
    (void)self;
    // thread, sockets, and storage intentionally leak until process exit
}

// --------------------------------------------------------------------------------------------
// dtobject implementation
// --------------------------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------
// Copy constructor
void
dtiox_linux_tcp_copy(dtiox_linux_tcp_t* this, dtiox_linux_tcp_t* that)
{
    (void)this;
    (void)that;
}

// --------------------------------------------------------------------------------------------
// Equality check
bool
dtiox_linux_tcp_equals(dtiox_linux_tcp_t* a, dtiox_linux_tcp_t* b)
{
    if (a == NULL || b == NULL)
        return false;

    if (a->model_number != b->model_number)
        return false;

    if (a->config.mode != b->config.mode)
        return false;

    if (a->config.mode == DTIOX_LINUX_TCP_MODE_CLIENT)
    {
        return strcmp(a->config.remote_host ? a->config.remote_host : "", b->config.remote_host ? b->config.remote_host : "") ==
                 0 &&
               a->config.remote_port == b->config.remote_port && a->config.tcp_nodelay == b->config.tcp_nodelay &&
               a->config.keepalive == b->config.keepalive;
    }

    if (a->config.mode == DTIOX_LINUX_TCP_MODE_SERVER)
    {
        return strcmp(a->config.local_bind_host ? a->config.local_bind_host : "",
                 b->config.local_bind_host ? b->config.local_bind_host : "") == 0 &&
               a->config.local_bind_port == b->config.local_bind_port && a->config.tcp_nodelay == b->config.tcp_nodelay &&
               a->config.keepalive == b->config.keepalive;
    }

    return false;
}

// --------------------------------------------------------------------------------------------

const char*
dtiox_linux_tcp_get_class(dtiox_linux_tcp_t* self)
{
    (void)self;
    return "dtiox_linux_tcp_t";
}

// --------------------------------------------------------------------------------------------

bool
dtiox_linux_tcp_is_iface(dtiox_linux_tcp_t* self, const char* iface_name)
{
    (void)self;
    return strcmp(iface_name, DTIOX_IFACE_NAME) == 0 || strcmp(iface_name, "dtobject_iface") == 0;
}

// --------------------------------------------------------------------------------------------
// Convert to string
void
dtiox_linux_tcp_to_string(dtiox_linux_tcp_t* self, char* buffer, size_t buffer_size)
{
    const char* mode_str = "unknown";
    const char* host_str = "?";
    int32_t port = 0;

    if (self == NULL || buffer == NULL || buffer_size == 0)
        return;

    if (self->config.mode == DTIOX_LINUX_TCP_MODE_CLIENT)
    {
        mode_str = "client";
        host_str = self->config.remote_host ? self->config.remote_host : "?";
        port = self->config.remote_port;
    }
    else if (self->config.mode == DTIOX_LINUX_TCP_MODE_SERVER)
    {
        mode_str = "server";
        host_str = self->config.local_bind_host ? self->config.local_bind_host : "0.0.0.0";
        port = self->config.local_bind_port;
    }

    snprintf(buffer, buffer_size, "%s %s:%d", mode_str, host_str, port);
    buffer[buffer_size - 1] = '\0';
}