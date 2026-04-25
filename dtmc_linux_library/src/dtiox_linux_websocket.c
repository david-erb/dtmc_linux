// Linux WebSocket backend for dtiox — POSIX TCP + HTTP upgrade + WS binary frames
//
// Server-only, one client only, write-oriented first cut.
//
// Link with:
//   -lcrypto
//

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> // strncasecmp
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <dtmc_base/dtmc_base_constants.h>

#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtobject.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtiox.h>

#include <dtmc/dtiox_linux_websocket.h>
#include <dtmc/dtsha1.h>

#define TAG "dtiox_linux_websocket"
// #define dtlog_debug(...)

DTIOX_INIT_VTABLE(dtiox_linux_websocket);
DTOBJECT_INIT_VTABLE(dtiox_linux_websocket);

// --------------------------------------------------------------------------------------------
// private type
// --------------------------------------------------------------------------------------------

struct dtiox_linux_websocket_t
{
    DTIOX_COMMON_MEMBERS
    bool _is_malloced;

    dtiox_linux_websocket_config_t config;

    int listen_fd;
    int client_fd;

    bool is_connected;
    bool write_enabled;
};

// --------------------------------------------------------------------------------------------
// constants
// --------------------------------------------------------------------------------------------

static const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

// --------------------------------------------------------------------------------------------
// helpers
// --------------------------------------------------------------------------------------------

static void
_close_if_open(int* fd_ptr)
{
    if (fd_ptr && *fd_ptr >= 0)
    {
        close(*fd_ptr);
        *fd_ptr = -1;
    }
}

// --------------------------------------------------------------------------------------------

static dterr_t*
_set_socket_common_options(int fd, const dtiox_linux_websocket_config_t* cfg)
{
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

    return NULL;
}

// --------------------------------------------------------------------------------------------
// send all bytes
// --------------------------------------------------------------------------------------------

static dterr_t*
_send_all(int fd, const uint8_t* buf, size_t len)
{
    while (len > 0)
    {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, buf, len, 0);
#endif
        if (n > 0)
        {
            buf += n;
            len -= (size_t)n;
            continue;
        }

        if (n == 0)
        {
            return dterr_new(DTERR_IO, DTERR_LOC, NULL, "send returned 0");
        }

        if (errno == EINTR)
            continue;

        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "send failed: %s", strerror(errno));
    }

    return NULL;
}

// --------------------------------------------------------------------------------------------
// basic base64
// --------------------------------------------------------------------------------------------

static void
_base64_encode_20bytes(const uint8_t in[20], char out[29])
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i = 0;
    int j = 0;
    uint32_t v = 0;

    for (i = 0; i < 18; i += 3)
    {
        v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | ((uint32_t)in[i + 2]);

        out[j++] = tbl[(v >> 18) & 0x3F];
        out[j++] = tbl[(v >> 12) & 0x3F];
        out[j++] = tbl[(v >> 6) & 0x3F];
        out[j++] = tbl[v & 0x3F];
    }

    // remaining 2 bytes (20 total)
    v = ((uint32_t)in[18] << 16) | ((uint32_t)in[19] << 8);

    out[j++] = tbl[(v >> 18) & 0x3F];
    out[j++] = tbl[(v >> 12) & 0x3F];
    out[j++] = tbl[(v >> 6) & 0x3F];
    out[j++] = '=';

    out[j] = '\0';
}

// --------------------------------------------------------------------------------------------

static const char*
_find_header_value(const char* request, const char* header_name)
{
    size_t header_len = strlen(header_name);
    const char* p = request;

    while (*p)
    {
        const char* line_end = strstr(p, "\r\n");
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);

        if (line_len == 0)
            return NULL;

        if (line_len > header_len && strncasecmp(p, header_name, header_len) == 0 && p[header_len] == ':')
        {
            const char* value = p + header_len + 1;
            while (*value == ' ' || *value == '\t')
                value++;
            return value;
        }

        if (!line_end)
            return NULL;

        p = line_end + 2;
    }

    return NULL;
}

// --------------------------------------------------------------------------------------------

static bool
_header_value_contains_token(const char* line_value, const char* token)
{
    if (!line_value || !token)
        return false;

    size_t token_len = strlen(token);
    const char* p = line_value;

    while (*p && !(p[0] == '\r' && p[1] == '\n'))
    {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;

        const char* start = p;
        while (*p && *p != ',' && !(p[0] == '\r' && p[1] == '\n'))
            p++;

        size_t len = (size_t)(p - start);
        while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t'))
            len--;

        if (len == token_len && strncasecmp(start, token, token_len) == 0)
            return true;

        if (*p == ',')
            p++;
    }

    return false;
}

// --------------------------------------------------------------------------------------------

static dterr_t*
_recv_http_upgrade_request(int fd, int32_t timeout_ms, char* out_request, size_t out_request_size)
{
    size_t used = 0;

    if (!out_request || out_request_size < 8)
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "bad request buffer");

    out_request[0] = '\0';

    for (;;)
    {
        if (strstr(out_request, "\r\n\r\n") != NULL)
            return NULL;

        if (used + 1 >= out_request_size)
        {
            return dterr_new(DTERR_IO, DTERR_LOC, NULL, "HTTP upgrade request too large");
        }

        if (timeout_ms > 0)
        {
            fd_set rfds;
            struct timeval tv;
            int rc;

            FD_ZERO(&rfds);
            FD_SET(fd, &rfds);

            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;

            rc = select(fd + 1, &rfds, NULL, NULL, &tv);
            if (rc == 0)
            {
                return dterr_new(DTERR_TIMEOUT, DTERR_LOC, NULL, "timed out waiting for websocket upgrade request");
            }
            if (rc < 0)
            {
                if (errno == EINTR)
                    continue;
                return dterr_new(DTERR_IO, DTERR_LOC, NULL, "select failed: %s", strerror(errno));
            }
        }

        ssize_t n = recv(fd, out_request + used, out_request_size - used - 1, 0);
        if (n > 0)
        {
            used += (size_t)n;
            out_request[used] = '\0';
            continue;
        }
        if (n == 0)
        {
            return dterr_new(DTERR_IO, DTERR_LOC, NULL, "peer closed during websocket handshake");
        }
        if (errno == EINTR)
            continue;

        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "recv failed during websocket handshake: %s", strerror(errno));
    }
}

// --------------------------------------------------------------------------------------------

static dterr_t*
_do_websocket_handshake(int fd, int32_t timeout_ms)
{
    dterr_t* dterr = NULL;
    char request[8192];
    const char* upgrade = NULL;
    const char* connection = NULL;
    const char* sec_key = NULL;
    char sec_key_trimmed[256];
    char sha1_input[512];
    uint8_t sha1_output[20];
    char accept_b64[29];
    char response[512];

    DTERR_C(_recv_http_upgrade_request(fd, timeout_ms, request, sizeof(request)));

    // crude but good enough for browser clients
    if (strncmp(request, "GET ", 4) != 0)
    {
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "websocket handshake failed: request was not GET");
    }

    upgrade = _find_header_value(request, "Upgrade");
    connection = _find_header_value(request, "Connection");
    sec_key = _find_header_value(request, "Sec-WebSocket-Key");

    if (!upgrade || !_header_value_contains_token(upgrade, "websocket"))
    {
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "websocket handshake failed: missing/invalid Upgrade header");
    }

    if (!connection || !_header_value_contains_token(connection, "Upgrade"))
    {
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "websocket handshake failed: missing/invalid Connection header");
    }

    if (!sec_key)
    {
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "websocket handshake failed: missing Sec-WebSocket-Key");
    }

    // trim key until CR/LF
    {
        size_t i = 0;
        while (sec_key[i] != '\0' && !(sec_key[i] == '\r' && sec_key[i + 1] == '\n') && i + 1 < sizeof(sec_key_trimmed))
        {
            sec_key_trimmed[i] = sec_key[i];
            i++;
        }

        while (i > 0 && (sec_key_trimmed[i - 1] == ' ' || sec_key_trimmed[i - 1] == '\t'))
            i--;

        sec_key_trimmed[i] = '\0';
    }

    if (snprintf(sha1_input, sizeof(sha1_input), "%s%s", sec_key_trimmed, WS_GUID) >= (int)sizeof(sha1_input))
    {
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "websocket handshake failed: Sec-WebSocket-Key too long");
    }

    dtsha1_compute(sha1_input, strlen(sha1_input), sha1_output);
    _base64_encode_20bytes(sha1_output, accept_b64);

    snprintf(response,
      sizeof(response),
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: %s\r\n"
      "\r\n",
      accept_b64);
    response[sizeof(response) - 1] = '\0';

    DTERR_C(_send_all(fd, (const uint8_t*)response, strlen(response)));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------

static dterr_t*
_bind_listen_accept(dtiox_linux_websocket_t* self, int* out_listen_fd, int* out_client_fd)
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
        return dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "invalid websocket bind port");
    }

    snprintf(port_str, sizeof(port_str), "%d", self->config.local_bind_port);
    port_str[sizeof(port_str) - 1] = '\0';

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
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

    dtlog_debug(TAG, "websocket server listening on %s:%" PRId32, bind_host, self->config.local_bind_port);

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

// --------------------------------------------------------------------------------------------
// websocket frame send
// --------------------------------------------------------------------------------------------

static dterr_t*
_send_ws_binary_frame(int fd, const uint8_t* payload, uint64_t payload_len)
{
    dterr_t* dterr = NULL;
    uint8_t header[10];
    size_t header_len = 0;

    // FIN=1, RSV=0, opcode=2 (binary)
    header[0] = 0x82;

    if (payload_len <= 125)
    {
        header[1] = (uint8_t)payload_len;
        header_len = 2;
    }
    else if (payload_len <= 0xFFFF)
    {
        header[1] = 126;
        header[2] = (uint8_t)((payload_len >> 8) & 0xFF);
        header[3] = (uint8_t)(payload_len & 0xFF);
        header_len = 4;
    }
    else
    {
        header[1] = 127;
        header[2] = (uint8_t)((payload_len >> 56) & 0xFF);
        header[3] = (uint8_t)((payload_len >> 48) & 0xFF);
        header[4] = (uint8_t)((payload_len >> 40) & 0xFF);
        header[5] = (uint8_t)((payload_len >> 32) & 0xFF);
        header[6] = (uint8_t)((payload_len >> 24) & 0xFF);
        header[7] = (uint8_t)((payload_len >> 16) & 0xFF);
        header[8] = (uint8_t)((payload_len >> 8) & 0xFF);
        header[9] = (uint8_t)(payload_len & 0xFF);
        header_len = 10;
    }

    DTERR_C(_send_all(fd, header, header_len));

    if (payload_len > 0)
    {
        DTERR_C(_send_all(fd, payload, (size_t)payload_len));
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
// create/init/configure
// --------------------------------------------------------------------------------------------

dterr_t*
dtiox_linux_websocket_create(dtiox_linux_websocket_t** out)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(out);

    *out = malloc(sizeof(dtiox_linux_websocket_t));
    if (!*out)
        return dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc failed");

    DTERR_C(dtiox_linux_websocket_init(*out));
    (*out)->_is_malloced = true;

cleanup:
    if (dterr)
    {
        free(*out);
        *out = NULL;
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtiox_linux_websocket_create failed");
    }
    return dterr;
}

// --------------------------------------------------------------------------------------------

dterr_t*
dtiox_linux_websocket_init(dtiox_linux_websocket_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    memset(self, 0, sizeof(*self));
    self->model_number = DTMC_BASE_CONSTANTS_IOX_MODEL_LINUX_WEBSOCKET;
    self->listen_fd = -1;
    self->client_fd = -1;
    self->is_connected = false;
    self->write_enabled = true;

    DTERR_C(dtiox_set_vtable(self->model_number, &dtiox_linux_websocket_vt));
    DTERR_C(dtobject_set_vtable(self->model_number, &dtiox_linux_websocket_object_vt));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------

dterr_t*
dtiox_linux_websocket_configure(dtiox_linux_websocket_t* self, const dtiox_linux_websocket_config_t* cfg)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(cfg);

    if (cfg->local_bind_port <= 0 || cfg->local_bind_port > 65535)
    {
        return dterr_new(DTERR_BADARG,
          DTERR_LOC,
          NULL,
          "invalid websocket server config host %s port %" PRId32,
          cfg->local_bind_host ? cfg->local_bind_host : "0.0.0.0",
          cfg->local_bind_port);
    }

    self->config = *cfg;

    if (!self->config.local_bind_host)
        self->config.local_bind_host = "0.0.0.0";

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
// attach
// --------------------------------------------------------------------------------------------

dterr_t*
dtiox_linux_websocket_attach(dtiox_linux_websocket_t* self DTIOX_ATTACH_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    int listen_fd = -1;
    int client_fd = -1;

    DTERR_ASSERT_NOT_NULL(self);

    if (self->client_fd >= 0)
        return NULL;

    DTERR_C(_bind_listen_accept(self, &listen_fd, &client_fd));

    dtlog_debug(TAG,
      "websocket TCP client accepted on %s:%" PRId32 " on fd %d",
      self->config.local_bind_host ? self->config.local_bind_host : "0.0.0.0",
      self->config.local_bind_port,
      client_fd);

    DTERR_C(_do_websocket_handshake(client_fd, self->config.handshake_timeout_ms));

    self->listen_fd = listen_fd;
    self->client_fd = client_fd;
    self->is_connected = true;

    listen_fd = -1;
    client_fd = -1;

    dtlog_debug(TAG,
      "websocket handshake complete on %s:%" PRId32 " client fd %d",
      self->config.local_bind_host ? self->config.local_bind_host : "0.0.0.0",
      self->config.local_bind_port,
      self->client_fd);

cleanup:
    if (dterr)
    {
        _close_if_open(&client_fd);
        _close_if_open(&listen_fd);
    }
    return dterr;
}

// --------------------------------------------------------------------------------------------

dterr_t*
dtiox_linux_websocket_detach(dtiox_linux_websocket_t* self DTIOX_DETACH_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    self->is_connected = false;

    _close_if_open(&self->client_fd);
    _close_if_open(&self->listen_fd);

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------

dterr_t*
dtiox_linux_websocket_enable(dtiox_linux_websocket_t* self DTIOX_ENABLE_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    self->write_enabled = enabled;

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
// read not supported in first cut
// --------------------------------------------------------------------------------------------

dterr_t*
dtiox_linux_websocket_read(dtiox_linux_websocket_t* self DTIOX_READ_ARGS)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    (void)buf;
    (void)buf_len;
    DTERR_ASSERT_NOT_NULL(out_read);

    *out_read = 0;

    dterr = dterr_new(DTERR_NOTIMPL, DTERR_LOC, NULL, "dtiox_linux_websocket_read not implemented");
    goto cleanup;
cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------
// write one complete websocket binary message
// --------------------------------------------------------------------------------------------

dterr_t*
dtiox_linux_websocket_write(dtiox_linux_websocket_t* self DTIOX_WRITE_ARGS)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(buf);
    DTERR_ASSERT_NOT_NULL(out_written);

    *out_written = 0;

    if (!self->write_enabled)
        return NULL;

    if (self->client_fd < 0 || !self->is_connected)
    {
        return dterr_new(DTERR_IO, DTERR_LOC, NULL, "websocket not attached");
    }

    // Important semantic:
    // one write() call => one complete websocket binary message.
    DTERR_C(_send_ws_binary_frame(self->client_fd, buf, (uint64_t)len));

    *out_written = len;

cleanup:
    if (dterr)
    {
        self->is_connected = false;
        _close_if_open(&self->client_fd);
        _close_if_open(&self->listen_fd);
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "websocket write failed");
    }
    return dterr;
}

// --------------------------------------------------------------------------------------------

dterr_t*
dtiox_linux_websocket_set_rx_semaphore(dtiox_linux_websocket_t* self DTIOX_SET_RX_SEMAPHORE_ARGS)
{
    (void)self;
    (void)rx_semaphore;
    return NULL;
}

// --------------------------------------------------------------------------------------------

dterr_t*
dtiox_linux_websocket_concat_format(dtiox_linux_websocket_t* self DTIOX_CONCAT_FORMAT_ARGS)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_str);

    *out_str = dtstr_concat_format(in_str,
      separator,
      "linux_websocket (%d) mode=server endpoint=\"%s:%d\" connected=%s write_enabled=%s",
      self->model_number,
      self->config.local_bind_host ? self->config.local_bind_host : "0.0.0.0",
      self->config.local_bind_port,
      self->is_connected ? "true" : "false",
      self->write_enabled ? "true" : "false");

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------------

void
dtiox_linux_websocket_dispose(dtiox_linux_websocket_t* self)
{
    if (!self)
        return;

    _close_if_open(&self->client_fd);
    _close_if_open(&self->listen_fd);

    if (self->_is_malloced)
    {
        free(self);
    }
}

// --------------------------------------------------------------------------------------------
// dtobject helpers
// --------------------------------------------------------------------------------------------

void
dtiox_linux_websocket_copy(dtiox_linux_websocket_t* this, dtiox_linux_websocket_t* that)
{
    (void)this;
    (void)that;
}

// --------------------------------------------------------------------------------------------

bool
dtiox_linux_websocket_equals(dtiox_linux_websocket_t* a, dtiox_linux_websocket_t* b)
{
    if (a == NULL || b == NULL)
        return false;

    if (a->model_number != b->model_number)
        return false;

    return strcmp(a->config.local_bind_host ? a->config.local_bind_host : "",
             b->config.local_bind_host ? b->config.local_bind_host : "") == 0 &&
           a->config.local_bind_port == b->config.local_bind_port && a->config.keepalive == b->config.keepalive &&
           a->config.tcp_nodelay == b->config.tcp_nodelay;
}

// --------------------------------------------------------------------------------------------

const char*
dtiox_linux_websocket_get_class(dtiox_linux_websocket_t* self)
{
    (void)self;
    return "dtiox_linux_websocket_t";
}

// --------------------------------------------------------------------------------------------

bool
dtiox_linux_websocket_is_iface(dtiox_linux_websocket_t* self, const char* iface_name)
{
    (void)self;
    return strcmp(iface_name, DTIOX_IFACE_NAME) == 0 || strcmp(iface_name, "dtobject_iface") == 0;
}

// --------------------------------------------------------------------------------------------

void
dtiox_linux_websocket_to_string(dtiox_linux_websocket_t* self, char* buffer, size_t buffer_size)
{
    if (self == NULL || buffer == NULL || buffer_size == 0)
        return;

    snprintf(buffer,
      buffer_size,
      "server %s:%d",
      self->config.local_bind_host ? self->config.local_bind_host : "0.0.0.0",
      self->config.local_bind_port);
    buffer[buffer_size - 1] = '\0';
}