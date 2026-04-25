#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dtcore/dterr.h>
#include <dtmc_base/dtmc_base_constants.h>

#include <dtmc/dtnvblob_linux_file.h>
#include <dtmc_base/dtnvblob.h>

DTNVBLOB_INIT_VTABLE(dtnvblob_linux_file)

/*------------------------------------------------------------------------*/
// Concrete instance layout (private to this TU)
typedef struct dtnvblob_linux_file_t
{
    DTNVBLOB_COMMON_MEMBERS;
    dtnvblob_linux_file_config_t config;
    bool _is_malloced;
} dtnvblob_linux_file_t;

/*------------------------------------------------------------------------*/
dterr_t*
dtnvblob_linux_file_create(dtnvblob_linux_file_t** self_ptr)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self_ptr);

    *self_ptr = (dtnvblob_linux_file_t*)malloc(sizeof(dtnvblob_linux_file_t));
    if (*self_ptr == NULL)
    {
        dterr = dterr_new(DTERR_NOMEM,
          DTERR_LOC,
          NULL,
          "failed to allocate %zu bytes for dtnvblob_linux_file_t",
          sizeof(dtnvblob_linux_file_t));
        goto cleanup;
    }

    DTERR_C(dtnvblob_linux_file_init(*self_ptr));
    (*self_ptr)->_is_malloced = true;

cleanup:
    if (dterr != NULL)
    {
        if (self_ptr != NULL && *self_ptr != NULL)
        {
            free(*self_ptr);
            *self_ptr = NULL;
        }
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtnvblob_linux_file_create failed");
    }
    return dterr;
}

/*------------------------------------------------------------------------*/
dterr_t*
dtnvblob_linux_file_init(dtnvblob_linux_file_t* self)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);

    memset(self, 0, sizeof(*self));
    self->model_number = DTMC_BASE_CONSTANTS_NVBLOB_MODEL_LINUX_FILE;

    // Publish vtable for this model
    DTERR_C(dtnvblob_set_vtable(self->model_number, &dtnvblob_linux_file_vt));

cleanup:
    if (dterr != NULL)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtnvblob_linux_file_init failed");
    return dterr;
}

/*------------------------------------------------------------------------*/
dterr_t*
dtnvblob_linux_file_configure(dtnvblob_linux_file_t* self, dtnvblob_linux_file_config_t* config)
{
    dterr_t* dterr = NULL;
    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(config);

    self->config = *config;

cleanup:
    return dterr;
}

/*------------------------------------------------------------------------*/
dterr_t*
dtnvblob_linux_file_read(dtnvblob_linux_file_t* self DTNVBLOB_READ_ARGS)
{
    dterr_t* dterr = NULL;
    FILE* fp = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(size);

    if (self->config.filename == NULL)
    {
        dterr = dterr_new(DTERR_INTERNAL, DTERR_LOC, NULL, "dtnvblob_linux_file_read: filename not configured");
        goto cleanup;
    }

    fp = fopen(self->config.filename, "rb");
    if (fp == NULL)
    {
        // Explicit error if file does not exist
        if (errno == ENOENT)
        {
            dterr = dterr_new(
              DTERR_NOTFOUND, DTERR_LOC, NULL, "dtnvblob_linux_file_read: file '%s' does not exist", self->config.filename);
        }
        else
        {
            dterr = dterr_new(DTERR_IO,
              DTERR_LOC,
              NULL,
              "dtnvblob_linux_file_read: failed to open '%s' for read (errno=%d)",
              self->config.filename,
              errno);
        }
        goto cleanup;
    }

    if (fseek(fp, 0, SEEK_END) != 0)
    {
        dterr =
          dterr_new(DTERR_IO, DTERR_LOC, NULL, "dtnvblob_linux_file_read: fseek end failed for '%s'", self->config.filename);
        goto cleanup;
    }

    long file_len = ftell(fp);
    if (file_len < 0)
    {
        dterr = dterr_new(DTERR_IO, DTERR_LOC, NULL, "dtnvblob_linux_file_read: ftell failed for '%s'", self->config.filename);
        goto cleanup;
    }
    if (file_len > INT32_MAX)
    {
        dterr = dterr_new(DTERR_IO,
          DTERR_LOC,
          NULL,
          "dtnvblob_linux_file_read: file '%s' too large (%ld bytes)",
          self->config.filename,
          file_len);
        goto cleanup;
    }

    if (fseek(fp, 0, SEEK_SET) != 0)
    {
        dterr =
          dterr_new(DTERR_IO, DTERR_LOC, NULL, "dtnvblob_linux_file_read: fseek start failed for '%s'", self->config.filename);
        goto cleanup;
    }

    // If blob is NULL, treat as size query: just return file size
    if (blob == NULL)
    {
        *size = (int32_t)file_len;
        goto cleanup;
    }

    // Otherwise, read up to *size bytes; no error if file is larger than buffer.
    int32_t capacity = *size;
    if (capacity < 0)
        capacity = 0;

    int32_t to_read = (int32_t)file_len;
    if (to_read > capacity)
        to_read = capacity;

    size_t nread = 0;
    if (to_read > 0)
    {
        nread = fread(blob, 1, (size_t)to_read, fp);
        if (nread < (size_t)to_read && ferror(fp))
        {
            dterr =
              dterr_new(DTERR_IO, DTERR_LOC, NULL, "dtnvblob_linux_file_read: fread failed for '%s'", self->config.filename);
            goto cleanup;
        }
    }

    *size = (int32_t)nread;

cleanup:
    if (fp != NULL)
        fclose(fp);
    if (dterr != NULL)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtnvblob_linux_file_read failed");
    return dterr;
}

/*------------------------------------------------------------------------*/
dterr_t*
dtnvblob_linux_file_write(dtnvblob_linux_file_t* self DTNVBLOB_WRITE_ARGS)
{
    dterr_t* dterr = NULL;
    FILE* fp = NULL;

    DTERR_ASSERT_NOT_NULL(self);
    DTERR_ASSERT_NOT_NULL(blob);
    DTERR_ASSERT_NOT_NULL(size);

    if (self->config.filename == NULL)
    {
        dterr = dterr_new(DTERR_INTERNAL, DTERR_LOC, NULL, "dtnvblob_linux_file_write: filename not configured");
        goto cleanup;
    }

    // Silently overwrite existing file; create if it does not exist.
    fp = fopen(self->config.filename, "wb");
    if (fp == NULL)
    {
        dterr = dterr_new(DTERR_IO,
          DTERR_LOC,
          NULL,
          "dtnvblob_linux_file_write: failed to open '%s' for write (errno=%d)",
          self->config.filename,
          errno);
        goto cleanup;
    }

    int32_t to_write = *size;
    if (to_write < 0)
        to_write = 0;

    size_t nwritten = 0;
    if (to_write > 0)
    {
        nwritten = fwrite(blob, 1, (size_t)to_write, fp);
        if (nwritten < (size_t)to_write)
        {
            dterr = dterr_new(DTERR_IO,
              DTERR_LOC,
              NULL,
              "dtnvblob_linux_file_write: fwrite incomplete for '%s' (%zu of %d bytes)",
              self->config.filename,
              nwritten,
              to_write);
            goto cleanup;
        }
    }

    if (fflush(fp) != 0)
    {
        dterr =
          dterr_new(DTERR_IO, DTERR_LOC, NULL, "dtnvblob_linux_file_write: fflush failed for '%s'", self->config.filename);
        goto cleanup;
    }

    *size = (int32_t)nwritten;

cleanup:
    if (fp != NULL)
        fclose(fp);
    if (dterr != NULL)
        dterr = dterr_new(dterr->error_code, DTERR_LOC, dterr, "dtnvblob_linux_file_write failed");
    return dterr;
}

/*------------------------------------------------------------------------*/
void
dtnvblob_linux_file_dispose(dtnvblob_linux_file_t* self)
{
    if (self == NULL)
        return;

    if (self->_is_malloced)
    {
        free(self);
    }
    else
    {
        memset(self, 0, sizeof(*self));
    }
}
