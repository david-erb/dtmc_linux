#include <inttypes.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dtcore/dterr.h>
#include <dtcore/dtlog.h>
#include <dtcore/dtobject.h>
#include <dtcore/dtstr.h>

#include <dtmc_base/dtiox.h>
#include <dtmc_base/dtsemaphore.h>
#include <dtmc_base/dttasker.h>

#include <dtmc/dtiox_linux_modbus_tcp_master.h>

#include "dtiox_linux_modbus_tcp_master__private.h"

#define TAG "dtiox_linux_modbus_tcp_master"

// --------------------------------------------------------------------------------------------
// dtobject implementation
// --------------------------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------
// Copy constructor
void
dtiox_linux_modbus_tcp_master_copy(dtiox_linux_modbus_tcp_master_t* this, dtiox_linux_modbus_tcp_master_t* that)
{
    // this object does not support copying
    (void)this;
    (void)that;
}

// --------------------------------------------------------------------------------------------
// Equality check
bool
dtiox_linux_modbus_tcp_master_equals(dtiox_linux_modbus_tcp_master_t* a, dtiox_linux_modbus_tcp_master_t* b)
{
    if (a == NULL || b == NULL)
    {
        return false;
    }

    // TODO: Reconside equality semantics for dtiox_linux_modbus_tcp_master_equals backend.
    return (a->model_number == b->model_number);
}

// --------------------------------------------------------------------------------------------
const char*
dtiox_linux_modbus_tcp_master_get_class(dtiox_linux_modbus_tcp_master_t* self)
{
    return "dtiox_linux_modbus_tcp_master_t";
}

// --------------------------------------------------------------------------------------------

bool
dtiox_linux_modbus_tcp_master_is_iface(dtiox_linux_modbus_tcp_master_t* self, const char* iface_name)
{
    return strcmp(iface_name, DTIOX_IFACE_NAME) == 0 || //
           strcmp(iface_name, "dtobject_iface") == 0;
}

// --------------------------------------------------------------------------------------------
// Convert to string
void
dtiox_linux_modbus_tcp_master_to_string(dtiox_linux_modbus_tcp_master_t* self, char* buffer, size_t buffer_size)
{
    if (self == NULL || buffer == NULL || buffer_size == 0)
        return;

    snprintf(buffer, buffer_size, "%s:%" PRId32, self->cfg.ip, self->cfg.port);
    buffer[buffer_size - 1] = '\0';
}
