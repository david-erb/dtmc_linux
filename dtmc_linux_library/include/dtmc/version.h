/*
 * dtmc_linux -- Library flavor and version identification macros.
 *
 * Provides DTMC_LINUX_VERSION string constants that identify the library build at runtime.
 * Used by dtruntime and diagnostic routines to report the active library variant in environment tables and
 * log output.
 *
 * This file is processed by tooling in the automated build system.
 * It's important to maintain the structure and formatting of the version macros for compatibility with version parsing scripts.
 *
 * cdox v1.0.2.1
 */

#pragma once

// @dtack-version-file DTMC_LINUX

#define DTMC_LINUX_VERSION_MAJOR 1
#define DTMC_LINUX_VERSION_MINOR 2
#define DTMC_LINUX_VERSION_PATCH 3

#define DTMC_LINUX_VERSION_STR_(x) #x
#define DTMC_LINUX_VERSION_STR(x) DTMC_LINUX_VERSION_STR_(x)
#define DTMC_LINUX_VERSION                                                                                                    \
    DTMC_LINUX_VERSION_STR(DTMC_LINUX_VERSION_MAJOR)                                                                         \
    "." DTMC_LINUX_VERSION_STR(DTMC_LINUX_VERSION_MINOR) "." DTMC_LINUX_VERSION_STR(DTMC_LINUX_VERSION_PATCH)
