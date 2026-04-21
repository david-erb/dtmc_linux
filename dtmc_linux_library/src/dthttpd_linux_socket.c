#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <dtcore/dterr.h>
#include <dtcore/dtheaper.h>
#include <dtcore/dtlog.h>

#include <dtmc_base/dtlock.h>
#include <dtmc_base/dtmc_base_constants.h>
#include <dtmc_base/dttasker.h>

#include <dtmc/dthttpd_linux_socket.h>

#define TAG "dthttpd_linux_socket"
// #define dtlog_debug(...)

// vtable
DTHTTPD_INIT_VTABLE(dthttpd_linux_socket);

// --------------------------------------------------------------------------------------
// private structs
// --------------------------------------------------------------------------------------

typedef struct _client_slot_t
{
    bool is_active;
    bool is_joined;
    int client_fd;
    dttasker_handle tasker_handle;
    struct dthttpd_linux_socket_t* server;
} _client_slot_t;

struct dthttpd_linux_socket_t
{
    DTHTTPD_COMMON_MEMBERS

    bool _is_malloced;
    dthttpd_linux_socket_config_t config;

    int listen_fd;
    bool is_started;
    bool stop_requested;

    dttasker_handle accept_tasker_handle;

    dtlock_handle lock;

    _client_slot_t* client_slots;
    int32_t client_slot_count;
    int32_t active_client_count;
};

// --------------------------------------------------------------------------------------
// helpers
// --------------------------------------------------------------------------------------

static void
_client_slot_init(_client_slot_t* slot)
{
    if (!slot)
        return;

    memset(slot, 0, sizeof(*slot));
    slot->client_fd = -1;
    slot->is_joined = true;
}

// --------------------------------------------------------------------------------------

static void
_close_if_open(int* fd_ptr)
{
    if (fd_ptr == NULL || *fd_ptr < 0)
        return;

    dtlog_debug(TAG, "Closing socket %d", *fd_ptr);

    close(*fd_ptr);
    *fd_ptr = -1;
}

// --------------------------------------------------------------------------------------

static const char*
_reason_phrase_for_status(int32_t status_code)
{
    switch (status_code)
    {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 411:
            return "Length Required";
        case 413:
            return "Payload Too Large";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "Unknown";
    }
}

// --------------------------------------------------------------------------------------

static dterr_t*
_send_all(int fd, const void* buffer, int32_t length)
{
    dterr_t* dterr = NULL;
    const uint8_t* p = (const uint8_t*)buffer;
    int32_t remaining = length;

    DTERR_ASSERT_NOT_NULL(buffer);

    while (remaining > 0)
    {
#ifdef MSG_NOSIGNAL
        ssize_t n = send(fd, p, (size_t)remaining, MSG_NOSIGNAL);
#else
        ssize_t n = send(fd, p, (size_t)remaining, 0);
#endif
        if (n > 0)
        {
            p += n;
            remaining -= (int32_t)n;
            continue;
        }

        if (n == 0)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "send returned 0");
            goto cleanup;
        }

        if (errno == EINTR)
            continue;

        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "send failed: %s", strerror(errno));
        goto cleanup;
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_send_response(int fd,
  int32_t status_code,
  const char* content_type,
  const void* body,
  int32_t body_length,
  bool close_connection)
{
    dterr_t* dterr = NULL;
    char header[1024];
    const char* reason = _reason_phrase_for_status(status_code);
    const char* ct = content_type ? content_type : "application/octet-stream";
    int n = 0;

    if (body_length < 0)
    {
        dterr = dterr_new(DTERR_RANGE, DTERR_LOC, NULL, "negative body_length");
        goto cleanup;
    }

    n = snprintf(header,
      sizeof(header),
      "HTTP/1.1 %d %s\r\n"
      "Content-Length: %d\r\n"
      "Content-Type: %s\r\n"
      "Connection: %s\r\n"
      "\r\n",
      status_code,
      reason,
      body_length,
      ct,
      close_connection ? "close" : "keep-alive");

    if (n < 0 || n >= (int)sizeof(header))
    {
        dterr = dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "HTTP header buffer too small");
        goto cleanup;
    }

    DTERR_C(_send_all(fd, header, (int32_t)n));

    if (body != NULL && body_length > 0)
    {
        DTERR_C(_send_all(fd, body, body_length));
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_send_text_response(int fd, int32_t status_code, const char* text)
{
    dterr_t* dterr = NULL;
    const char* body = text ? text : "";

    DTERR_C(_send_response(fd, status_code, "text/plain; charset=utf-8", body, (int32_t)strlen(body), true));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

static bool
_is_safe_url_path(const char* path)
{
    if (path == NULL || path[0] != '/')
        return false;

    if (strstr(path, "..") != NULL)
        return false;

    if (strchr(path, '\\') != NULL)
        return false;

    return true;
}

// --------------------------------------------------------------------------------------

static const char*
_guess_mime_type(const char* path)
{
    const char* ext = strrchr(path, '.');
    if (!ext)
        return "application/octet-stream";

    if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcasecmp(ext, ".css") == 0)
        return "text/css; charset=utf-8";
    if (strcasecmp(ext, ".js") == 0)
        return "application/javascript; charset=utf-8";
    if (strcasecmp(ext, ".json") == 0)
        return "application/json; charset=utf-8";
    if (strcasecmp(ext, ".txt") == 0)
        return "text/plain; charset=utf-8";
    if (strcasecmp(ext, ".svg") == 0)
        return "image/svg+xml";
    if (strcasecmp(ext, ".png") == 0)
        return "image/png";
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, ".gif") == 0)
        return "image/gif";
    if (strcasecmp(ext, ".ico") == 0)
        return "image/x-icon";

    return "application/octet-stream";
}

// --------------------------------------------------------------------------------------

static dterr_t*
_read_entire_file(const char* full_path, void** out_bytes, int32_t* out_size)
{
    dterr_t* dterr = NULL;
    FILE* f = NULL;
    long size_long = 0;
    void* bytes = NULL;

    DTERR_ASSERT_NOT_NULL(full_path);
    DTERR_ASSERT_NOT_NULL(out_bytes);
    DTERR_ASSERT_NOT_NULL(out_size);

    *out_bytes = NULL;
    *out_size = 0;

    f = fopen(full_path, "rb");
    if (!f)
    {
        dterr = dterr_new(DTERR_NOTFOUND, DTERR_LOC, NULL, "file not found: %s", full_path);
        goto cleanup;
    }

    if (fseek(f, 0, SEEK_END) != 0)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "fseek end failed for %s", full_path);
        goto cleanup;
    }

    size_long = ftell(f);
    if (size_long < 0 || size_long > INT32_MAX)
    {
        dterr = dterr_new(DTERR_RANGE, DTERR_LOC, NULL, "invalid file size for %s", full_path);
        goto cleanup;
    }

    if (fseek(f, 0, SEEK_SET) != 0)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "fseek set failed for %s", full_path);
        goto cleanup;
    }

    if (size_long > 0)
    {
        bytes = malloc((size_t)size_long);
        if (!bytes)
        {
            dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc failed for file bytes");
            goto cleanup;
        }

        if (fread(bytes, 1, (size_t)size_long, f) != (size_t)size_long)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "fread failed for %s", full_path);
            goto cleanup;
        }
    }

    *out_bytes = bytes;
    *out_size = (int32_t)size_long;
    bytes = NULL;

cleanup:
    if (f)
        fclose(f);
    free(bytes);
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_try_serve_static_file(struct dthttpd_linux_socket_t* self, int client_fd, const char* request_path, bool* was_served)
{
    dterr_t* dterr = NULL;
    int32_t i = 0;
    char candidate[PATH_MAX];
    const char* relative = NULL;
    void* bytes = NULL;
    int32_t size = 0;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(request_path);
    DTERR_ASSERT_NOT_NULL(was_served);

    *was_served = false;

    if (!_is_safe_url_path(request_path))
    {
        DTERR_C(_send_text_response(client_fd, 403, "Forbidden"));
        *was_served = true;
        goto cleanup;
    }

    relative = request_path;
    if (strcmp(relative, "/") == 0)
        relative = "/index.html";

    for (i = 0; i < self->config.static_directory_count; i++)
    {
        const char* root = self->config.static_directories[i];
        if (!root)
            continue;

        if (snprintf(candidate, sizeof(candidate), "%s%s", root, relative) >= (int)sizeof(candidate))
        {
            dterr = dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "static path too long");
            goto cleanup;
        }

        dterr = _read_entire_file(candidate, &bytes, &size);
        if (dterr == NULL)
        {
            DTERR_C(_send_response(client_fd, 200, _guess_mime_type(candidate), bytes, size, true));
            *was_served = true;
            goto cleanup;
        }

        if (dterr->error_code == DTERR_NOTFOUND)
        {
            dterr_dispose(dterr);
            dterr = NULL;
            continue;
        }

        goto cleanup;
    }

    DTERR_C(_send_text_response(client_fd, 404, "Not Found"));
    *was_served = true;

cleanup:
    free(bytes);
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_find_header_value(const char* headers, const char* name, char* out_value, int32_t out_value_capacity, bool* was_found)
{
    dterr_t* dterr = NULL;
    const char* p = NULL;
    const char* line_end = NULL;
    size_t name_len = 0;
    int32_t copy_len = 0;

    DTERR_ASSERT_NOT_NULL(headers);
    DTERR_ASSERT_NOT_NULL(name);
    DTERR_ASSERT_NOT_NULL(out_value);
    DTERR_ASSERT_NOT_NULL(was_found);

    if (out_value_capacity <= 0)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "out_value_capacity must be > 0");
        goto cleanup;
    }

    *was_found = false;
    out_value[0] = '\0';
    name_len = strlen(name);

    p = headers;

    while (*p)
    {
        line_end = strstr(p, "\r\n");
        if (!line_end)
            break;

        if ((size_t)(line_end - p) > name_len + 1 && strncasecmp(p, name, name_len) == 0 && p[name_len] == ':')
        {
            const char* value_start = p + name_len + 1;
            while (*value_start == ' ' || *value_start == '\t')
                value_start++;

            copy_len = (int32_t)(line_end - value_start);
            if (copy_len >= out_value_capacity)
            {
                dterr = dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "header value too long for %s", name);
                goto cleanup;
            }

            memcpy(out_value, value_start, (size_t)copy_len);
            out_value[copy_len] = '\0';
            *was_found = true;
            goto cleanup;
        }

        p = line_end + 2;
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_normalize_request_target(const char* raw_target, char* out_path, int32_t out_path_capacity)
{
    dterr_t* dterr = NULL;
    const char* end = NULL;
    int32_t len = 0;

    DTERR_ASSERT_NOT_NULL(raw_target);
    DTERR_ASSERT_NOT_NULL(out_path);

    if (out_path_capacity <= 0)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "out_path_capacity must be > 0");
        goto cleanup;
    }

    end = raw_target;
    while (*end && *end != '?' && *end != '#')
        end++;

    len = (int32_t)(end - raw_target);
    if (len <= 0 || len >= out_path_capacity)
    {
        dterr = dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "request path too long");
        goto cleanup;
    }

    memcpy(out_path, raw_target, (size_t)len);
    out_path[len] = '\0';

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_read_http_request(int client_fd,
  char* out_method,
  int32_t out_method_capacity,
  char* out_path,
  int32_t out_path_capacity,
  char** out_body,
  int32_t* out_body_size)
{
    dterr_t* dterr = NULL;
    char* bytes = NULL;
    int32_t capacity = DTHTTPD_MAX_HEADER_BYTES;
    int32_t length = 0;
    char* headers_end = NULL;
    int32_t header_len = 0;
    int32_t content_length = 0;
    char content_length_str[64];
    bool found_content_length = false;

    DTERR_ASSERT_NOT_NULL(out_method);
    DTERR_ASSERT_NOT_NULL(out_path);
    DTERR_ASSERT_NOT_NULL(out_body);
    DTERR_ASSERT_NOT_NULL(out_body_size);

    if (out_method_capacity <= 0 || out_path_capacity <= 0)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "invalid output buffer capacity");
        goto cleanup;
    }

    *out_body = NULL;
    *out_body_size = 0;
    out_method[0] = '\0';
    out_path[0] = '\0';

    bytes = (char*)malloc((size_t)capacity + 1);
    if (!bytes)
    {
        dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc failed");
        goto cleanup;
    }

    for (;;)
    {
        ssize_t n = recv(client_fd, bytes + length, (size_t)(capacity - length), 0);
        if (n > 0)
        {
            length += (int32_t)n;
            bytes[length] = '\0';

            headers_end = strstr(bytes, "\r\n\r\n");
            if (headers_end)
                break;

            if (length >= capacity)
            {
                dterr = dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "HTTP headers too large");
                goto cleanup;
            }

            continue;
        }

        if (n == 0)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "peer closed while reading request");
            goto cleanup;
        }

        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            dterr = dterr_new(DTERR_TIMEOUT,
              DTERR_LOC,
              NULL,
              "timeout while reading request headers on socket %d: %s",
              client_fd,
              strerror(errno));
            goto cleanup;
        }

        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "recv failed on socket %d: %s", client_fd, strerror(errno));
        goto cleanup;
    }

    header_len = (int32_t)((headers_end + 4) - bytes);

    {
        char* line_end = strstr(bytes, "\r\n");
        char request_line[2048];
        char method_local[DTHTTPD_MAX_METHOD_BYTES];
        char target_local[DTHTTPD_MAX_PATH_BYTES];
        char version_local[64];
        int32_t request_line_len = 0;

        if (!line_end)
        {
            dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "bad HTTP request");
            goto cleanup;
        }

        request_line_len = (int32_t)(line_end - bytes);
        if (request_line_len <= 0 || request_line_len >= (int32_t)sizeof(request_line))
        {
            dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "request line too large");
            goto cleanup;
        }

        memcpy(request_line, bytes, (size_t)request_line_len);
        request_line[request_line_len] = '\0';

        memset(method_local, 0, sizeof(method_local));
        memset(target_local, 0, sizeof(target_local));
        memset(version_local, 0, sizeof(version_local));

        if (sscanf(request_line, "%31s %1023s %63s", method_local, target_local, version_local) != 3)
        {
            dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "unable to parse request line");
            goto cleanup;
        }

        if ((int32_t)strlen(method_local) >= out_method_capacity)
        {
            dterr = dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "method buffer too small");
            goto cleanup;
        }

        memcpy(out_method, method_local, strlen(method_local) + 1);

        DTERR_C(_normalize_request_target(target_local, out_path, out_path_capacity));
    }

    DTERR_C(_find_header_value(bytes, "Content-Length", content_length_str, sizeof(content_length_str), &found_content_length));

    if (strcasecmp(out_method, "POST") == 0)
    {
        int32_t body_already_read = length - header_len;
        char* body = NULL;

        if (!found_content_length)
        {
            dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "POST missing Content-Length");
            goto cleanup;
        }

        content_length = atoi(content_length_str);
        if (content_length < 0 || content_length > DTHTTPD_MAX_REQUEST_BYTES)
        {
            dterr = dterr_new(DTERR_RANGE, DTERR_LOC, NULL, "invalid Content-Length");
            goto cleanup;
        }

        if (content_length > 0)
        {
            body = (char*)malloc((size_t)content_length);
            if (!body)
            {
                dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc failed for request body");
                goto cleanup;
            }

            if (body_already_read > 0)
            {
                int32_t copy_len = body_already_read > content_length ? content_length : body_already_read;
                memcpy(body, bytes + header_len, (size_t)copy_len);
                body_already_read = copy_len;
            }

            while (body_already_read < content_length)
            {
                ssize_t n = recv(client_fd, body + body_already_read, (size_t)(content_length - body_already_read), 0);
                if (n > 0)
                {
                    body_already_read += (int32_t)n;
                    continue;
                }

                if (n == 0)
                {
                    free(body);
                    dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "peer closed while reading request body");
                    goto cleanup;
                }

                if (errno == EINTR)
                    continue;

                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    free(body);
                    dterr = dterr_new(DTERR_TIMEOUT, DTERR_LOC, NULL, "timeout while reading request body");
                    goto cleanup;
                }

                free(body);
                dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "recv body failed: %s", strerror(errno));
                goto cleanup;
            }

            *out_body = body;
            *out_body_size = content_length;
        }
    }

cleanup:
    free(bytes);
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_handle_get(struct dthttpd_linux_socket_t* self, int client_fd, const char* path)
{
    dterr_t* dterr = NULL;
    bool was_served = false;

    DTERR_C(_try_serve_static_file(self, client_fd, path, &was_served));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_handle_post(struct dthttpd_linux_socket_t* self, int client_fd, const char* path, void* body, int32_t body_size)
{
    dterr_t* dterr = NULL;
    void* response = NULL;
    int32_t response_size = 0;
    const char* content_type = "application/octet-stream";
    int32_t status_code = 200;

    if (!self->config.post_callback)
    {
        DTERR_C(_send_text_response(client_fd, 405, "POST not configured"));
        goto cleanup;
    }

    DTERR_C(self->config.post_callback(
      self->config.post_callback_context, path, body, body_size, &response, &response_size, &content_type, &status_code));

    if (response_size < 0)
    {
        dterr = dterr_new(DTERR_RANGE, DTERR_LOC, NULL, "post callback returned negative response size");
        goto cleanup;
    }

    DTERR_C(_send_response(client_fd, status_code, content_type, response, response_size, true));

cleanup:
    dtheaper_free(response);
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_mark_slot_finished(_client_slot_t* slot)
{
    dterr_t* dterr = NULL;
    dthttpd_linux_socket_t* server = NULL;
    bool lock_held = false;

    DTERR_ASSERT_NOT_NULL(slot);
    server = slot->server;
    DTERR_ASSERT_NOT_NULL(server);

    DTERR_C(dtlock_acquire(server->lock));
    lock_held = true;

    if (slot->is_active)
    {
        slot->is_active = false;
        if (server->active_client_count > 0)
            server->active_client_count--;
    }

cleanup:
    if (lock_held)
    {
        dterr_t* unlock_err = dtlock_release(server->lock);
        if (unlock_err)
        {
            if (dterr == NULL)
                dterr = unlock_err;
            else
                dterr_dispose(unlock_err);
        }
    }

    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_try_reap_finished_slot(_client_slot_t* slot)
{
    dterr_t* dterr = NULL;
    bool was_timeout = false;

    DTERR_ASSERT_NOT_NULL(slot);

    if (slot->is_active)
        goto cleanup;

    if (slot->tasker_handle == NULL)
    {
        slot->is_joined = true;
        goto cleanup;
    }

    if (!slot->is_joined)
    {
        DTERR_C(dttasker_join(slot->tasker_handle, 0, &was_timeout));
        if (was_timeout)
            goto cleanup;

        slot->is_joined = true;
    }

    if (slot->is_joined && slot->tasker_handle != NULL)
    {
        dttasker_dispose(slot->tasker_handle);
        slot->tasker_handle = NULL;
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_reap_finished_slots_locked(dthttpd_linux_socket_t* self)
{
    dterr_t* dterr = NULL;
    int32_t i = 0;

    DTERR_ASSERT_NOT_NULL(self);

    for (i = 0; i < self->client_slot_count; i++)
    {
        DTERR_C(_try_reap_finished_slot(&self->client_slots[i]));
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_find_free_slot(struct dthttpd_linux_socket_t* self, int32_t* out_index)
{
    dterr_t* dterr = NULL;
    int32_t i = 0;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_index);

    *out_index = -1;

    for (i = 0; i < self->client_slot_count; i++)
    {
        if (!self->client_slots[i].is_active && self->client_slots[i].tasker_handle == NULL)
        {
            *out_index = i;
            goto cleanup;
        }
    }

    dterr = dterr_new(DTERR_BUSY, DTERR_LOC, NULL, "no free client slot");

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_client_task_entry(void* arg, dttasker_handle self_task)
{
    dterr_t* dterr = NULL;
    _client_slot_t* slot = (_client_slot_t*)arg;
    dthttpd_linux_socket_t* server = NULL;
    char method[DTHTTPD_MAX_METHOD_BYTES];
    char path[DTHTTPD_MAX_PATH_BYTES];
    char* body = NULL;
    int32_t body_size = 0;
    bool should_stop = false;

    DTERR_ASSERT_NOT_NULL(slot);
    server = slot->server;
    DTERR_ASSERT_NOT_NULL(server);

    DTERR_C(dttasker_ready(self_task));

    {
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;

        if (setsockopt(slot->client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "setsockopt(SO_RCVTIMEO) failed: %s", strerror(errno));
            goto cleanup;
        }
    }

    DTERR_C(dttasker_poll(self_task, &should_stop));
    if (should_stop)
        goto cleanup;

    dterr = _read_http_request(slot->client_fd, method, sizeof(method), path, sizeof(path), &body, &body_size);
    if (dterr)
    {
        dtlog_dterr(TAG, dterr);

        if (dterr->error_code == DTERR_TIMEOUT || dterr->error_code == DTERR_BADARG || dterr->error_code == DTERR_RANGE)
        {
            dterr_t* send_err = _send_text_response(slot->client_fd, 400, "Bad Request");
            if (send_err)
                dterr_dispose(send_err);
        }

        goto cleanup;
    }

    if (strcasecmp(method, "GET") == 0)
    {
        DTERR_C(_handle_get(server, slot->client_fd, path));
    }
    else if (strcasecmp(method, "POST") == 0)
    {
        DTERR_C(_handle_post(server, slot->client_fd, path, body, body_size));
    }
    else
    {
        DTERR_C(_send_text_response(slot->client_fd, 405, "Method Not Allowed"));
    }

cleanup:
    free(body);
    _close_if_open(&slot->client_fd);

    {
        dterr_t* finish_err = _mark_slot_finished(slot);
        if (finish_err)
        {
            if (dterr == NULL)
                dterr = finish_err;
            else
                dterr_dispose(finish_err);
        }
    }

    if (dterr)
    {
        dtlog_dterr(TAG, dterr);
        dterr_dispose(dterr);
        dterr = NULL;
    }

    return NULL;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_open_listen_socket(dthttpd_linux_socket_t* self, int* out_fd)
{
    dterr_t* dterr = NULL;
    struct addrinfo hints;
    struct addrinfo* result = NULL;
    struct addrinfo* rp = NULL;
    char port_str[32];
    const char* host = NULL;
    int fd = -1;
    int optval = 1;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(out_fd);

    *out_fd = -1;
    host = self->config.bind_host ? self->config.bind_host : "0.0.0.0";

    if (self->config.bind_port <= 0 || self->config.bind_port > 65535)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "invalid bind port");
        goto cleanup;
    }

    snprintf(port_str, sizeof(port_str), "%d", self->config.bind_port);
    port_str[sizeof(port_str) - 1] = '\0';

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(host, port_str, &hints, &result) != 0)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "getaddrinfo(%s:%s) failed", host, port_str);
        goto cleanup;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next)
    {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;

        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
        {
            dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "setsockopt(SO_REUSEADDR) failed: %s", strerror(errno));
            goto cleanup;
        }

        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        _close_if_open(&fd);
    }

    if (fd < 0)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "bind(%s:%d) failed", host, self->config.bind_port);
        goto cleanup;
    }

    if (listen(fd, self->config.listen_backlog) < 0)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "listen failed: %s", strerror(errno));
        goto cleanup;
    }

    *out_fd = fd;
    fd = -1;

cleanup:
    if (result)
        freeaddrinfo(result);
    _close_if_open(&fd);
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_reject_busy_client(int client_fd)
{
    dterr_t* dterr = NULL;

    DTERR_C(_send_text_response(client_fd, 503, "Service Unavailable"));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

static dterr_t*
_accept_task_entry(void* arg, dttasker_handle self_task)
{
    dterr_t* dterr = NULL;
    dthttpd_linux_socket_t* self = (dthttpd_linux_socket_t*)arg;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_C(dttasker_ready(self_task));

    dtlog_info(
      TAG, "HTTP server started on %s:%d", self->config.bind_host ? self->config.bind_host : "0.0.0.0", self->config.bind_port);

    for (;;)
    {
        int client_fd = -1;
        bool should_stop = false;

        DTERR_C(dttasker_poll(self_task, &should_stop));
        if (should_stop)
            break;

        client_fd = accept(self->listen_fd, NULL, NULL);
        if (client_fd < 0)
        {
            if (errno == EINTR)
                continue;

            if (self->stop_requested)
                break;

            if (errno == EBADF || errno == EINVAL)
                break;

            dtlog_error(TAG, "accept failed: %s", strerror(errno));
            continue;
        }

        {
            bool lock_held = false;
            int32_t slot_index = -1;

            dterr = dtlock_acquire(self->lock);
            if (dterr)
                goto cleanup;
            lock_held = true;

            dterr = _reap_finished_slots_locked(self);
            if (dterr)
                goto accept_iteration_cleanup;

            if (self->active_client_count >= self->config.max_concurrent_connections)
            {
                dterr = dtlock_release(self->lock);
                lock_held = false;
                if (dterr)
                    goto cleanup;

                {
                    dterr_t* reject_err = _reject_busy_client(client_fd);
                    if (reject_err)
                    {
                        dtlog_dterr(TAG, reject_err);
                        dterr_dispose(reject_err);
                    }
                }

                _close_if_open(&client_fd);
                continue;
            }

            dterr = _find_free_slot(self, &slot_index);
            if (dterr)
            {
                if (dterr->error_code == DTERR_BUSY)
                {
                    dterr_dispose(dterr);
                    dterr = NULL;

                    dterr = dtlock_release(self->lock);
                    lock_held = false;
                    if (dterr)
                        goto cleanup;

                    {
                        dterr_t* reject_err = _reject_busy_client(client_fd);
                        if (reject_err)
                        {
                            dtlog_dterr(TAG, reject_err);
                            dterr_dispose(reject_err);
                        }
                    }

                    _close_if_open(&client_fd);
                    continue;
                }

                goto accept_iteration_cleanup;
            }

            {
                dttasker_config_t tasker_config;
                _client_slot_t* slot = &self->client_slots[slot_index];

                memset(&tasker_config, 0, sizeof(tasker_config));
                tasker_config.name = "http_client";
                tasker_config.tasker_entry_point_fn = _client_task_entry;
                tasker_config.tasker_entry_point_arg = slot;
                tasker_config.stack_size = self->config.child_stack_size;
                tasker_config.priority = self->config.child_priority;
                tasker_config.tasker_info_callback = self->config.tasker_info_callback_fn;
                tasker_config.tasker_info_callback_context = self->config.tasker_info_callback_context;

                slot->server = self;
                slot->client_fd = client_fd;
                slot->is_active = true;
                slot->is_joined = false;

                dterr = dttasker_create(&slot->tasker_handle, &tasker_config);
                if (dterr)
                {
                    slot->is_active = false;
                    slot->is_joined = true;
                    slot->client_fd = -1;
                    goto accept_iteration_cleanup;
                }

                dterr = dttasker_start(slot->tasker_handle);
                if (dterr)
                {
                    dttasker_dispose(slot->tasker_handle);
                    slot->tasker_handle = NULL;
                    slot->is_active = false;
                    slot->is_joined = true;
                    slot->client_fd = -1;
                    goto accept_iteration_cleanup;
                }

                self->active_client_count++;
                client_fd = -1;
            }

            dterr = dtlock_release(self->lock);
            lock_held = false;
            if (dterr)
                goto cleanup;

            continue;

        accept_iteration_cleanup:
            if (lock_held)
            {
                dterr_t* unlock_err = dtlock_release(self->lock);
                lock_held = false;
                if (unlock_err)
                {
                    if (dterr == NULL)
                        dterr = unlock_err;
                    else
                        dterr_dispose(unlock_err);
                }
            }

            if (client_fd >= 0)
                _close_if_open(&client_fd);

            if (dterr)
            {
                dtlog_dterr(TAG, dterr);
                dterr_dispose(dterr);
                dterr = NULL;
            }

            continue;
        }
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
// public API
// --------------------------------------------------------------------------------------

dterr_t*
dthttpd_linux_socket_create(dthttpd_linux_socket_t** out)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(out);
    *out = NULL;

    *out = (dthttpd_linux_socket_t*)malloc(sizeof(dthttpd_linux_socket_t));
    if (!*out)
    {
        dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc failed");
        goto cleanup;
    }

    DTERR_C(dthttpd_linux_socket_init(*out));
    (*out)->_is_malloced = true;

cleanup:
    if (dterr)
    {
        free(*out);
        *out = NULL;
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dthttpd_linux_socket_create failed");
    }
    return dterr;
}

// --------------------------------------------------------------------------------------

dterr_t*
dthttpd_linux_socket_init(dthttpd_linux_socket_t* self)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(self);

    memset(self, 0, sizeof(*self));
    self->listen_fd = -1;

    self->model_number = DTMC_BASE_CONSTANTS_HTTPSERVER_MODEL_LINUX_SOCKET;

    DTERR_C(dthttpd_set_vtable(self->model_number, &dthttpd_linux_socket_vt));
    DTERR_C(dtlock_create(&self->lock));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

dterr_t*
dthttpd_linux_socket_configure(dthttpd_linux_socket_t* self, const dthttpd_linux_socket_config_t* config)
{
    dterr_t* dterr = NULL;
    int32_t slot_count = 0;
    int32_t i = 0;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(config);

    if (config->bind_port <= 0 || config->bind_port > 65535)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "invalid bind_port %" PRId32, config->bind_port);
        goto cleanup;
    }

    self->config = *config;

    if (!self->config.bind_host)
        self->config.bind_host = "0.0.0.0";

    if (self->config.listen_backlog <= 0)
        self->config.listen_backlog = 16;

    if (self->config.max_concurrent_connections <= 0)
        self->config.max_concurrent_connections = DTHTTPD_DEFAULT_MAX_CONCURRENT_CONNECTIONS;

    slot_count = self->config.max_concurrent_connections;

    if (self->client_slots)
    {
        for (i = 0; i < self->client_slot_count; i++)
        {
            _close_if_open(&self->client_slots[i].client_fd);

            if (self->client_slots[i].tasker_handle)
            {
                dttasker_dispose(self->client_slots[i].tasker_handle);
                self->client_slots[i].tasker_handle = NULL;
            }
        }

        free(self->client_slots);
        self->client_slots = NULL;
    }

    self->client_slots = (_client_slot_t*)calloc((size_t)slot_count, sizeof(_client_slot_t));
    if (!self->client_slots)
    {
        dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "calloc failed for client slots");
        goto cleanup;
    }

    for (i = 0; i < slot_count; i++)
    {
        _client_slot_init(&self->client_slots[i]);
    }

    self->client_slot_count = slot_count;
    self->active_client_count = 0;

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

dterr_t*
dthttpd_linux_socket_loop(dthttpd_linux_socket_t* self)
{
    dterr_t* dterr = NULL;
    int listen_fd = -1;
    dttasker_config_t tasker_config;

    DTERR_ASSERT_NOT_NULL(self);

    if (self->is_started)
        goto cleanup;

    DTERR_C(_open_listen_socket(self, &listen_fd));

    self->listen_fd = listen_fd;
    listen_fd = -1;
    self->stop_requested = false;

    memset(&tasker_config, 0, sizeof(tasker_config));
    tasker_config.name = "http_accept";
    tasker_config.tasker_entry_point_fn = _accept_task_entry;
    tasker_config.tasker_entry_point_arg = self;
    tasker_config.stack_size = self->config.accept_stack_size;
    tasker_config.priority = self->config.accept_priority;
    tasker_config.tasker_info_callback = self->config.tasker_info_callback_fn;
    tasker_config.tasker_info_callback_context = self->config.tasker_info_callback_context;

    DTERR_C(dttasker_create(&self->accept_tasker_handle, &tasker_config));
    DTERR_C(dttasker_start(self->accept_tasker_handle));

    self->is_started = true;

cleanup:
    if (dterr)
        _close_if_open(&listen_fd);
    return dterr;
}

// --------------------------------------------------------------------------------------

dterr_t*
dthttpd_linux_socket_stop(dthttpd_linux_socket_t* self)
{
    dterr_t* dterr = NULL;
    int32_t i = 0;
    bool lock_held = false;

    DTERR_ASSERT_NOT_NULL(self);

    self->stop_requested = true;

    if (self->accept_tasker_handle)
    {
        DTERR_C(dttasker_stop(self->accept_tasker_handle));
    }

    _close_if_open(&self->listen_fd);

    DTERR_C(dtlock_acquire(self->lock));
    lock_held = true;

    for (i = 0; i < self->client_slot_count; i++)
    {
        if (self->client_slots[i].is_active && self->client_slots[i].tasker_handle)
        {
            dterr_t* stop_err = dttasker_stop(self->client_slots[i].tasker_handle);
            if (stop_err)
            {
                dtlog_dterr(TAG, stop_err);
                dterr_dispose(stop_err);
            }

            if (self->client_slots[i].client_fd >= 0)
            {
                shutdown(self->client_slots[i].client_fd, SHUT_RDWR);
            }
        }
    }

cleanup:
    if (lock_held)
    {
        dterr_t* unlock_err = dtlock_release(self->lock);
        if (unlock_err)
        {
            if (dterr == NULL)
                dterr = unlock_err;
            else
                dterr_dispose(unlock_err);
        }
    }

    return dterr;
}

// --------------------------------------------------------------------------------------

dterr_t*
dthttpd_linux_socket_join(dthttpd_linux_socket_t* self, dttimeout_millis_t timeout_millis, bool* was_timeout)
{
    dterr_t* dterr = NULL;
    int32_t i = 0;
    bool local_timeout = false;

    DTERR_ASSERT_NOT_NULL(self);

    if (was_timeout)
        *was_timeout = false;

    if (self->accept_tasker_handle)
    {
        DTERR_C(dttasker_join(self->accept_tasker_handle, timeout_millis, &local_timeout));
        if (local_timeout)
        {
            if (was_timeout)
                *was_timeout = true;
            goto cleanup;
        }

        dttasker_dispose(self->accept_tasker_handle);
        self->accept_tasker_handle = NULL;
    }

    for (i = 0; i < self->client_slot_count; i++)
    {
        if (self->client_slots[i].tasker_handle)
        {
            DTERR_C(dttasker_join(self->client_slots[i].tasker_handle, timeout_millis, &local_timeout));
            if (local_timeout)
            {
                if (was_timeout)
                    *was_timeout = true;
                goto cleanup;
            }

            self->client_slots[i].is_joined = true;
            _close_if_open(&self->client_slots[i].client_fd);
            dttasker_dispose(self->client_slots[i].tasker_handle);
            self->client_slots[i].tasker_handle = NULL;
        }
    }

    self->is_started = false;
    self->active_client_count = 0;

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------

void
dthttpd_linux_socket_dispose(dthttpd_linux_socket_t* self)
{
    int32_t i = 0;

    if (!self)
        return;

    _close_if_open(&self->listen_fd);

    if (self->accept_tasker_handle)
    {
        dttasker_dispose(self->accept_tasker_handle);
        self->accept_tasker_handle = NULL;
    }

    if (self->client_slots)
    {
        for (i = 0; i < self->client_slot_count; i++)
        {
            _close_if_open(&self->client_slots[i].client_fd);

            if (self->client_slots[i].tasker_handle)
            {
                dttasker_dispose(self->client_slots[i].tasker_handle);
                self->client_slots[i].tasker_handle = NULL;
            }
        }

        free(self->client_slots);
        self->client_slots = NULL;
    }

    if (self->lock)
    {
        dtlock_dispose(self->lock);
        self->lock = NULL;
    }

    if (self->_is_malloced)
        free(self);
}