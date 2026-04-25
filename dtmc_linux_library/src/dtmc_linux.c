#include <stdlib.h>

#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>

#include <dtmc_base/dttasker_registry.h>

#include <dtmc/dtmc_linux.h>

const char*
dtmc_linux_version(void)
{
    return DTMC_LINUX_VERSION;
}

// --------------------------------------------------------------------------------------
void
dtmc_linux_each_error_log(dterr_t* dterr, void* context)
{
    const char* tag = (const char*)context;
    dtlog_error(tag, "%s@%ld in %s: %s", dterr->source_file, (long)dterr->line_number, dterr->source_function, dterr->message);
}

// --------------------------------------------------------------------------------------
// NOTE: No official "chip info" in Linux native_sim. Use CONFIG_ARCH and
// runtime inspection if possible.
dterr_t*
dtmc_linux_is_qemu(bool* is_qemu)
{
#ifdef CONFIG_QEMU_TARGET
    *is_qemu = true;
#else
    *is_qemu = false;
#endif
    return NULL; // success
}

// --------------------------------------------------------------------------------------
dterr_t*
dtmc_linux_printf_environment(void)
{
    printf("*dtmc_linux_printf_environment: System:\n");
    printf("*    dtmc_linux version: %s\n", dtmc_linux_version());

    return NULL;
}

// --------------------------------------------------------------------------------------

dterr_t*
dtmc_register_tasks(dttasker_registry_t* registry)
{
    dterr_t* dterr = NULL;
    if (registry == NULL)
    {
        dterr = dterr_new(DTERR_BADARG, DTERR_LOC, NULL, "registry is NULL");
        goto cleanup;
    }

cleanup:
    return dterr;
}

// --------------------------------------------------------------------------------------
dterr_t*
dtmc_printf_tasks(void)
{
    dterr_t* dterr = NULL;
    char* registry_table_string = NULL;

    if (!dttasker_registry_global_instance.is_initialized)
    {
        DTERR_C(dttasker_registry_init(&dttasker_registry_global_instance));
    }

    DTERR_C(dtmc_register_tasks(&dttasker_registry_global_instance));

    DTERR_C(dttasker_registry_format_as_table(&dttasker_registry_global_instance, &registry_table_string));

    printf("%s\n", registry_table_string);

cleanup:

    if (dterr != NULL)
    {
        dterr = dterr_new(DTERR_FAIL, DTERR_LOC, dterr, "failed to print tasks");
        goto cleanup;
    }

    free(registry_table_string);

    return dterr;
}
