/*
 * dtmc_linux -- Linux platform entry point for the dtmc embedded framework.
 *
 * Declares the platform flavor and version strings, a QEMU environment
 * detector, diagnostic helpers that print environment and task state to
 * stdout, an error-log iterator compatible with dterr_each, and the
 * task-registry entry point that wires all Linux-specific tasks into the
 * dttasker_registry at startup.
 *
 * cdox v1.0.2
 */
#pragma once

#include <stdbool.h>

#include <dtcore/dterr.h>
#include <dtmc/version.h>
#include <dtmc_base/dttasker_registry.h>

#define DTMC_LINUX_FLAVOR "dtmc_linux"

const char*
dtmc_flavor(void);

const char*
dtmc_version(void);

void
dtmc_linux_each_error_log(dterr_t* dterr, void* context);

extern dterr_t*
dtmc_linux_is_qemu(bool* is_qemu);
extern dterr_t*
dtmc_linux_printf_environment(void);
extern dterr_t*
dtmc_linux_printf_tasks(void);

extern dterr_t*
dtmc_register_tasks(dttasker_registry_t* registry);