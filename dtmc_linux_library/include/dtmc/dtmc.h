/*
 * dtmc -- Canonical top-level include for the Linux platform library.
 *
 * Re-exports the full dtmc_linux interface under the platform-agnostic
 * name <dtmc/dtmc.h>. Application code includes this header to remain
 * decoupled from the underlying platform selection while still gaining
 * access to flavor strings, QEMU detection, diagnostic helpers, and the
 * task-registry entry point provided by dtmc_linux.
 *
 * cdox v1.0.2
 */
#pragma once
#include <dtmc/dtmc_linux.h>