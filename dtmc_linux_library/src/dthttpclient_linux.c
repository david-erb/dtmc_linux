// dthttpclient_linux.c
#include <curl/curl.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dtcore/dtbuffer.h>
#include <dtcore/dterr.h>
#include <dtcore/dtheaper.h>
#include <dtcore/dtkvp.h>
#include <dtcore/dtstr.h>
#include <dtcore/dttimeout.h>

#include <dtmc_base/dthttpclient.h>

#define TAG "dthttpclient_linux"

/*
 * Threading:
 *   dthttpclient handles are not thread-safe. Do not call get/post/dispose
 *   concurrently on the same handle. Use one handle per thread or serialize
 *   access externally.
 */

// --------------------------------------------------------------------------------------
typedef struct dthttpclient_linux_t
{
    CURL* curl;
    void* callback_context;
} dthttpclient_linux_t;

// --------------------------------------------------------------------------------------
typedef struct dthttpclient_accumulator_t
{
    uint8_t* bytes;
    int32_t length;
    int32_t capacity;
    bool failed;
} dthttpclient_accumulator_t;

static bool g_curl_global_initialized = false;

// --------------------------------------------------------------------------------------
static bool
dthttpclient_linux_accumulate(dthttpclient_accumulator_t* acc, const void* bytes, int32_t length)
{
    if (acc == NULL || bytes == NULL || length < 0)
        return false;

    if (length == 0)
        return true;

    if (acc->length > INT32_MAX - length)
        return false;

    int32_t required = acc->length + length;

    if (required > acc->capacity)
    {
        int32_t new_capacity = acc->capacity == 0 ? 4096 : acc->capacity;

        while (new_capacity < required)
        {
            if (new_capacity > INT32_MAX / 2)
            {
                new_capacity = required;
                break;
            }

            new_capacity *= 2;
        }

        uint8_t* new_bytes = realloc(acc->bytes, (size_t)new_capacity);
        if (new_bytes == NULL)
            return false;

        acc->bytes = new_bytes;
        acc->capacity = new_capacity;
    }

    memcpy(acc->bytes + acc->length, bytes, (size_t)length);
    acc->length += length;

    return true;
}

// --------------------------------------------------------------------------------------
static size_t
dthttpclient_linux_write_body(void* contents, size_t size, size_t nmemb, void* userp)
{
    dthttpclient_accumulator_t* acc = (dthttpclient_accumulator_t*)userp;

    if (acc == NULL)
        return 0;

    if (size != 0 && nmemb > SIZE_MAX / size)
    {
        acc->failed = true;
        return 0;
    }

    size_t byte_count = size * nmemb;

    if (byte_count > INT32_MAX)
    {
        acc->failed = true;
        return 0;
    }

    if (!dthttpclient_linux_accumulate(acc, contents, (int32_t)byte_count))
    {
        acc->failed = true;
        return 0;
    }

    return byte_count;
}

// --------------------------------------------------------------------------------------
static dterr_t*
dthttpclient_linux_build_url(const char* url, dtkvp_list_t* query_params, char** out_url)
{
    dterr_t* dterr = NULL;
    char* encoded = NULL;
    char* final_url = NULL;

    DTERR_ASSERT_NOT_NULL(url);
    DTERR_ASSERT_NOT_NULL(out_url);

    *out_url = NULL;

    if (query_params != NULL)
        DTERR_C(dtkvp_list_urlencode(query_params, &encoded));

    if (encoded == NULL || encoded[0] == '\0')
    {
        final_url = strdup(url);
        if (final_url == NULL)
        {
            dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "strdup failed building HTTP URL");
            goto cleanup;
        }

        *out_url = final_url;
        final_url = NULL;
        goto cleanup;
    }

    const char* separator = strchr(url, '?') == NULL ? "?" : "&";

    size_t url_len = strlen(url);
    size_t separator_len = strlen(separator);
    size_t encoded_len = strlen(encoded);

    if (url_len > SIZE_MAX - separator_len || url_len + separator_len > SIZE_MAX - encoded_len ||
        url_len + separator_len + encoded_len > SIZE_MAX - 1)
    {
        dterr = dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "HTTP URL length overflow");
        goto cleanup;
    }

    size_t final_len = url_len + separator_len + encoded_len + 1;

    final_url = malloc(final_len);
    if (final_url == NULL)
    {
        dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "malloc failed building HTTP URL");
        goto cleanup;
    }

    snprintf(final_url, final_len, "%s%s%s", url, separator, encoded);

    *out_url = final_url;
    final_url = NULL;

cleanup:
    if (encoded != NULL)
        free(encoded);

    if (final_url != NULL)
        free(final_url);

    return dterr;
}

// --------------------------------------------------------------------------------------
static dterr_t*
dthttpclient_linux_make_response_body(dthttpclient_accumulator_t* acc, dthttpclient_response_t* out_response)
{
    dterr_t* dterr = NULL;

    DTERR_ASSERT_NOT_NULL(acc);
    DTERR_ASSERT_NOT_NULL(out_response);

    DTERR_C(dtbuffer_create(&out_response->body, acc->length));

    if (acc->length > 0)
        memcpy(out_response->body->payload, acc->bytes, (size_t)acc->length);

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
static dterr_t*
dthttpclient_linux_fill_response_info(CURL* curl, dthttpclient_response_t* out_response)
{
    dterr_t* dterr = NULL;
    long status_code = 0;
    const char* content_type = NULL;

    DTERR_ASSERT_NOT_NULL(curl);
    DTERR_ASSERT_NOT_NULL(out_response);

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);
    out_response->status_code = (int32_t)status_code;

    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    if (content_type != NULL)
    {
        strncpy(out_response->content_type, content_type, DTHTTPCLIENT_MAX_CONTENT_TYPE_BYTES - 1);
        out_response->content_type[DTHTTPCLIENT_MAX_CONTENT_TYPE_BYTES - 1] = '\0';
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
static dterr_t*
dthttpclient_linux_perform(dthttpclient_linux_t* self,
  const char* method,
  const char* url,
  dtkvp_list_t* query_params,
  dtbuffer_t* payload,
  const char* content_type,
  dttimeout_millis_t timeout_milliseconds,
  dthttpclient_response_t* out_response)
{
    dterr_t* dterr = NULL;
    CURLcode curl_result;
    char* final_url = NULL;
    struct curl_slist* headers = NULL;
    dthttpclient_accumulator_t acc = { 0 };

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(self->curl);
    DTERR_ASSERT_NOT_NULL(method);
    DTERR_ASSERT_NOT_NULL(url);
    DTERR_ASSERT_NOT_NULL(out_response);

    if (timeout_milliseconds == DTTIMEOUT_NOWAIT)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "DTTIMEOUT_NOWAIT is not supported by blocking dthttpclient_linux");
        goto cleanup;
    }

    long curl_timeout_ms = timeout_milliseconds == DTTIMEOUT_FOREVER ? 0L : (long)timeout_milliseconds;

    memset(out_response, 0, sizeof(*out_response));

    DTERR_C(dthttpclient_linux_build_url(url, query_params, &final_url));

    curl_easy_reset(self->curl);

    curl_easy_setopt(self->curl, CURLOPT_URL, final_url);
    curl_easy_setopt(self->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(self->curl, CURLOPT_CONNECTTIMEOUT_MS, curl_timeout_ms);
    curl_easy_setopt(self->curl, CURLOPT_TIMEOUT_MS, curl_timeout_ms);
    curl_easy_setopt(self->curl, CURLOPT_NOSIGNAL, 1L);

    curl_easy_setopt(self->curl, CURLOPT_WRITEFUNCTION, dthttpclient_linux_write_body);
    curl_easy_setopt(self->curl, CURLOPT_WRITEDATA, &acc);

    if (strcmp(method, "POST") == 0)
    {
        curl_easy_setopt(self->curl, CURLOPT_POST, 1L);

        if (payload != NULL && payload->length > 0)
        {
            curl_easy_setopt(self->curl, CURLOPT_POSTFIELDS, payload->payload);
            curl_easy_setopt(self->curl, CURLOPT_POSTFIELDSIZE, (long)payload->length);
        }
        else
        {
            curl_easy_setopt(self->curl, CURLOPT_POSTFIELDS, "");
            curl_easy_setopt(self->curl, CURLOPT_POSTFIELDSIZE, 0L);
        }

        if (content_type != NULL && content_type[0] != '\0')
        {
            char header[DTHTTPCLIENT_MAX_CONTENT_TYPE_BYTES + 16];

            int n = snprintf(header, sizeof(header), "Content-Type: %s", content_type);
            if (n < 0 || n >= (int)sizeof(header))
            {
                dterr = dterr_new(DTERR_OVERFLOW, DTERR_LOC, NULL, "Content-Type header too long");
                goto cleanup;
            }

            headers = curl_slist_append(headers, header);
            if (headers == NULL)
            {
                dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "curl_slist_append failed");
                goto cleanup;
            }

            curl_easy_setopt(self->curl, CURLOPT_HTTPHEADER, headers);
        }
    }
    else
    {
        curl_easy_setopt(self->curl, CURLOPT_HTTPGET, 1L);
    }

    curl_result = curl_easy_perform(self->curl);
    if (curl_result != CURLE_OK)
    {
        int32_t code = curl_result == CURLE_OPERATION_TIMEDOUT ? DTERR_TIMEOUT : DTERR_IO;

        dterr = dterr_new(code, DTERR_LOC, NULL, "curl_easy_perform failed: %s", curl_easy_strerror(curl_result));

        goto cleanup;
    }

    if (acc.failed)
    {
        dterr = dterr_new(DTERR_NOMEM, DTERR_LOC, NULL, "failed accumulating HTTP response body");
        goto cleanup;
    }

    DTERR_C(dthttpclient_linux_fill_response_info(self->curl, out_response));
    DTERR_C(dthttpclient_linux_make_response_body(&acc, out_response));

cleanup:
    if (headers != NULL)
        curl_slist_free_all(headers);

    if (final_url != NULL)
        free(final_url);

    if (acc.bytes != NULL)
        free(acc.bytes);

    if (dterr != NULL && out_response != NULL)
    {
        if (out_response->body != NULL)
        {
            dtbuffer_dispose(out_response->body);
            out_response->body = NULL;
        }

        out_response->status_code = 0;
        out_response->content_type[0] = '\0';
    }

    return dterr;
}

// --------------------------------------------------------------------------------------
extern dterr_t*
dthttpclient_create(dthttpclient_handle* self_handle)
{
    dterr_t* dterr = NULL;
    dthttpclient_linux_t* self = NULL;

    DTERR_ASSERT_NOT_NULL(self_handle);

    *self_handle = NULL;

    if (!g_curl_global_initialized)
    {
        CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (rc != CURLE_OK)
        {
            dterr = dterr_new(DTERR_INFRASTRUCTURE, DTERR_LOC, NULL, "curl_global_init failed: %s", curl_easy_strerror(rc));
            goto cleanup;
        }

        g_curl_global_initialized = true;
    }

    DTERR_C(dtheaper_alloc_and_zero(sizeof(dthttpclient_linux_t), "dthttpclient_linux_t", (void**)&self));

    self->curl = curl_easy_init();
    if (self->curl == NULL)
    {
        dterr = dterr_new(DTERR_INFRASTRUCTURE, DTERR_LOC, NULL, "curl_easy_init failed");
        goto cleanup;
    }

    *self_handle = (dthttpclient_handle)self;
    self = NULL;

cleanup:
    if (self != NULL)
        dthttpclient_dispose((dthttpclient_handle)self);

    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dthttpclient_get(dthttpclient_handle self_handle,
  const char* url,
  dtkvp_list_t* query_params,
  dttimeout_millis_t timeout_milliseconds,
  dthttpclient_response_t* out_response)
{
    dterr_t* dterr = NULL;
    dthttpclient_linux_t* self = (dthttpclient_linux_t*)self_handle;

    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(dthttpclient_linux_perform(self, "GET", url, query_params, NULL, NULL, timeout_milliseconds, out_response));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dthttpclient_post(dthttpclient_handle self_handle,
  const char* url,
  dtkvp_list_t* query_params,
  dtbuffer_t* payload,
  const char* content_type,
  dttimeout_millis_t timeout_milliseconds,
  dthttpclient_response_t* out_response)
{
    dterr_t* dterr = NULL;
    dthttpclient_linux_t* self = (dthttpclient_linux_t*)self_handle;

    DTERR_ASSERT_NOT_NULL(self);

    DTERR_C(
      dthttpclient_linux_perform(self, "POST", url, query_params, payload, content_type, timeout_milliseconds, out_response));

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
void
dthttpclient_dispose(dthttpclient_handle self_handle)
{
    dthttpclient_linux_t* self = (dthttpclient_linux_t*)self_handle;

    if (self == NULL)
        return;

    if (self->curl != NULL)
    {
        curl_easy_cleanup(self->curl);
        self->curl = NULL;
    }

    dtheaper_free(self);
}