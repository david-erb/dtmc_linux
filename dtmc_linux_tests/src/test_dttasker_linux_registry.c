#include <stdlib.h>

#include <dtcore/dterr.h>
#include <dtcore/dtguid.h>
#include <dtcore/dtguidable.h>
#include <dtcore/dtguidable_pool.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtstr.h>
#include <dtcore/dttimeout.h>
#include <dtcore/dtunittest.h>

#include <dtmc_base/dtcpu.h>
#include <dtmc_base/dtruntime.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>
#include <dtmc_base/dttasker_registry.h>

#define TAG "test_dtmc_base_dttasker_registry"

// comment out the logging here
// #define dtlog_debug(TAG, ...)

// --------------------------------------------------------------------------------------------
// Verify tasks appear in the global registry on create and vanish on dispose.
static dterr_t*
test_dtmc_linux_dttasker_registry_auto_register(void)
{
    dterr_t* dterr = NULL;
    dttasker_handle h1 = NULL;
    dttasker_handle h2 = NULL;
    int32_t count = 0;

    // start from a clean global registry
    dttasker_registry_dispose(&dttasker_registry_global_instance);

    dttasker_config_t c = { 0 };
    c.name = "auto_reg_1";
    c.priority = DTTASKER_PRIORITY_NORMAL_MEDIUM;
    c.stack_size = 4096;
    DTERR_C(dttasker_create(&h1, &c));

    DTERR_C(dtguidable_pool_count(&dttasker_registry_global_instance.pool, &count));
    DTUNITTEST_ASSERT_INT(count, ==, 1);

    c.name = "auto_reg_2";
    c.stack_size = 4096;
    DTERR_C(dttasker_create(&h2, &c));

    DTERR_C(dtguidable_pool_count(&dttasker_registry_global_instance.pool, &count));
    DTUNITTEST_ASSERT_INT(count, ==, 2);

    dttasker_dispose(h1);
    h1 = NULL;

    DTERR_C(dtguidable_pool_count(&dttasker_registry_global_instance.pool, &count));
    DTUNITTEST_ASSERT_INT(count, ==, 1);

    dttasker_dispose(h2);
    h2 = NULL;

    DTERR_C(dtguidable_pool_count(&dttasker_registry_global_instance.pool, &count));
    DTUNITTEST_ASSERT_INT(count, ==, 0);

cleanup:
    dttasker_dispose(h1);
    dttasker_dispose(h2);
    dttasker_registry_dispose(&dttasker_registry_global_instance);
    return dterr;
}

// --------------------------------------------------------------------------------------------
// Verify dttasker_registry_remove works and is idempotent.
static dterr_t*
test_dtmc_linux_dttasker_registry_remove(void)
{
    dterr_t* dterr = NULL;
    dttasker_handle h = NULL;
    dttasker_registry_t reg = { 0 };
    int32_t count = 0;

    DTERR_C(dttasker_registry_init(&reg));

    dttasker_config_t c = { 0 };
    c.name = "remove_test_task";
    c.priority = DTTASKER_PRIORITY_NORMAL_MEDIUM;
    DTERR_C(dttasker_create(&h, &c));

    DTERR_C(dttasker_registry_insert(&reg, h));
    DTERR_C(dtguidable_pool_count(&reg.pool, &count));
    DTUNITTEST_ASSERT_INT(count, ==, 1);

    DTERR_C(dttasker_registry_remove(&reg, h));
    DTERR_C(dtguidable_pool_count(&reg.pool, &count));
    DTUNITTEST_ASSERT_INT(count, ==, 0);

    // second remove is a silent no-op
    DTERR_C(dttasker_registry_remove(&reg, h));

cleanup:
    dttasker_dispose(h);
    dttasker_registry_dispose(&reg);
    return dterr;
}

// --------------------------------------------------------------------------------------------

void
test_dtmc_linux_dttasker_registry(DTUNITTEST_SUITE_ARGS)
{
    DTUNITTEST_RUN_TEST(test_dtmc_linux_dttasker_registry_auto_register);
    DTUNITTEST_RUN_TEST(test_dtmc_linux_dttasker_registry_remove);
}
