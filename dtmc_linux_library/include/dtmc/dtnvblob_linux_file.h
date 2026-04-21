/*
 * dtnvblob_linux_file -- Linux file-based backend for the dtnvblob persistent storage interface.
 *
 * Implements the dtnvblob vtable by reading and writing an opaque binary blob
 * to a named file on the local filesystem. The single configuration field is
 * the file path. The backend satisfies the same read/write/dispose contract
 * as flash or NVS backends on embedded targets, making it useful for
 * development and testing on Linux without dedicated hardware.
 *
 * cdox v1.0.2
 */
#pragma once
// See markdown documentation at the end of this file.

// Stand-in dtnvblob provider for environments without real GPIO hardware.

#include <stdbool.h>
#include <stdint.h>

#include <dtcore/dterr.h>
#include <dtmc_base/dtnvblob.h>

// back-end private plumbing config structure
typedef struct
{
    const char* filename;
} dtnvblob_linux_file_config_t;

// forward declare the concrete linux_file type
typedef struct dtnvblob_linux_file_t dtnvblob_linux_file_t;

extern dterr_t*
dtnvblob_linux_file_create(dtnvblob_linux_file_t** self_ptr);

extern dterr_t*
dtnvblob_linux_file_init(dtnvblob_linux_file_t* self);

extern dterr_t*
dtnvblob_linux_file_configure(dtnvblob_linux_file_t* self, dtnvblob_linux_file_config_t* config);

// The linux_file is a concrete implementation of the ::dtnvblob vtable-based interface.
// The macro below declares the public facade entry points (`attach`, `enable`, `read`, `write`, `dispose`)
// that down-cast the opaque handle and dispatch via this model's registered vtable.

// --------------------------------------------------------------------------------------

DTNVBLOB_DECLARE_API(dtnvblob_linux_file) // < Declare facade glue for the linux_file model.

#if MARKDOWN_DOCUMENTATION
// clang-format off
// --8<-- [start:markdown-documentation]
# dtnvblob_linux_file

A linux file NVBLOB implementation that satisfies the **dtnvblob** vtable interface.

## Mini-guide

- Use `dtnvblob_linux_file_create` for heap lifetime; use `dtnvblob_linux_file_init` for stack/static lifetime.
- Apply file configuration with `dtnvblob_linux_file_configure` before first use (sets filename).
- Error contract: functions returning `dterr_t*` yield `NULL` on success; non-NULL on failure (caller owns the error).

## Example

```c
#include <dtmc_base/dtnvblob_linux_file.h>
#include <dtcore/dterr.h>

void example(void)
{
    dterr_t* dterr = NULL;
    dtnvblob_handle nvblob_handle = NULL;

    {
        // Create the implementation instance
        dtnvblob_linux_file_t* o = NULL;
        DTERR_C(dtnvblob_linux_file_create(&o));
        nvblob_handle = (dtnvblob_handle)o;

        // Configure as output (pin 5, no pull, default drive)
        dtnvblob_linux_file_config_t c = { .filename = "nvblob_file.bin" };
        DTERR_C(dtnvblob_linux_file_configure(o, &c)) ;
    }

    int32_t buffer_size = 0;
    uint8_t* buffer = NULL;
    // get the read size
    DTERR_C(dtnvblob_linux_file_read(nvblob_handle, buffer, &bytes_read));

    // allocate buffer
    buffer = (uint8_t*)malloc((size_t)bytes_read);

    // read data
    DTERR_C(dtnvblob_linux_file_read(nvblob_handle, buffer, &bytes_read));

    // write data
    int32_t bytes_written = buffer_size;
    DTERR_C(dtnvblob_linux_file_write(nvblob_handle, 0, buffer, &bytes_written));

    // check it took it all
    if (bytes_written != buffer_size) 
    {
        // give error
    }

cleanup:
    dtnvblob_dispose(nvblob_handle);
}
```

// --8<-- [end:markdown-documentation]
// clang-format on
#endif
