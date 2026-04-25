// test_dtmc_linux_dtnvblob_linux.c
#include <stdbool.h>

#include <dtcore/dterr.h>
#include <dtcore/dtledger.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtunittest.h>

#include <dtmc_base/dtnvblob.h>

#include <dtmc/dtmc_linux.h>
#include <dtmc/dtnvblob_linux_file.h>

#include <dtmc_base_tests.h>

#include <dtmc_linux_tests.h>

#define TAG "test_dtnvblob_linux_file"

// If you want it silent during CI:
// #define dtlog_info(TAG, ...)

// -------------------------------------------------------------------------------
static dterr_t*
test_dtmc_linux_dtnvblob_linux_file_simple(void)
{
    dterr_t* dterr = NULL;
    dtnvblob_handle nvblob_handle = NULL;

    {
        dtnvblob_linux_file_t* o = NULL;
        DTERR_C(dtnvblob_linux_file_create(&o));
        nvblob_handle = (dtnvblob_handle)o;
        DTERR_C(dtnvblob_linux_file_init(o));
        dtnvblob_linux_file_config_t c = { 0 };
        c.filename = "test_nvblob_file.bin";
        DTERR_C(dtnvblob_linux_file_configure(o, &c));
    }

    DTERR_C(test_dtmc_base_dtnvblob_example_write_read(nvblob_handle));

cleanup:

    dtnvblob_dispose(nvblob_handle);

    return dterr;
}

// --------------------------------------------------------------------------------------------
// Entry point for all dtnvblob unit tests
void
test_dtmc_linux_dtnvblob(DTUNITTEST_SUITE_ARGS)
{
    DTUNITTEST_RUN_TEST(test_dtmc_linux_dtnvblob_linux_file_simple);
}
