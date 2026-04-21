#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <dtcore/dtobject.h>

#include <dtmc_base/dtiox.h>

#include <dtmc/dtiox_linux_modbus_rtu_master.h>

#include "dtiox_linux_modbus_rtu_master__private.h"

void
dtiox_linux_modbus_rtu_master_copy(dtiox_linux_modbus_rtu_master_t* this, dtiox_linux_modbus_rtu_master_t* that)
{
    (void)this;
    (void)that;
}

bool
dtiox_linux_modbus_rtu_master_equals(dtiox_linux_modbus_rtu_master_t* a, dtiox_linux_modbus_rtu_master_t* b)
{
    if (a == NULL || b == NULL)
        return false;

    return (a->model_number == b->model_number);
}

const char*
dtiox_linux_modbus_rtu_master_get_class(dtiox_linux_modbus_rtu_master_t* self)
{
    (void)self;
    return "dtiox_linux_modbus_rtu_master_t";
}

bool
dtiox_linux_modbus_rtu_master_is_iface(dtiox_linux_modbus_rtu_master_t* self, const char* iface_name)
{
    (void)self;
    return strcmp(iface_name, DTIOX_IFACE_NAME) == 0 || strcmp(iface_name, "dtobject_iface") == 0;
}

void
dtiox_linux_modbus_rtu_master_to_string(dtiox_linux_modbus_rtu_master_t* self, char* buffer, size_t buffer_size)
{
    if (self == NULL || buffer == NULL || buffer_size == 0)
        return;

    char tmp[128];
    dtuart_helper_to_string(&self->cfg.uart_config, tmp, sizeof(tmp));

    snprintf(buffer, buffer_size, "%s:slave=%" PRId32 " %s", self->cfg.device, self->cfg.slave_id, tmp);
    buffer[buffer_size - 1] = '\0';
}
