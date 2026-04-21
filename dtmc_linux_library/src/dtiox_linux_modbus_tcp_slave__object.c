#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <dtcore/dtobject.h>

#include "dtiox_linux_modbus_tcp_slave__private.h"

#define TAG "dtiox_linux_modbus_tcp_slave"

void
dtiox_linux_modbus_tcp_slave_copy(dtiox_linux_modbus_tcp_slave_t* this, dtiox_linux_modbus_tcp_slave_t* that)
{
    (void)this;
    (void)that;
}

bool
dtiox_linux_modbus_tcp_slave_equals(dtiox_linux_modbus_tcp_slave_t* a, dtiox_linux_modbus_tcp_slave_t* b)
{
    if (a == NULL || b == NULL)
        return false;
    return (a->model_number == b->model_number);
}

const char*
dtiox_linux_modbus_tcp_slave_get_class(dtiox_linux_modbus_tcp_slave_t* self)
{
    (void)self;
    return "dtiox_linux_modbus_tcp_slave_t";
}

bool
dtiox_linux_modbus_tcp_slave_is_iface(dtiox_linux_modbus_tcp_slave_t* self, const char* iface_name)
{
    (void)self;
    return strcmp(iface_name, DTIOX_IFACE_NAME) == 0 || strcmp(iface_name, "dtobject_iface") == 0;
}

void
dtiox_linux_modbus_tcp_slave_to_string(dtiox_linux_modbus_tcp_slave_t* self, char* buffer, size_t buffer_size)
{
    if (self == NULL || buffer == NULL || buffer_size == 0)
        return;

    snprintf(buffer, buffer_size, "0.0.0.0:%" PRId32, self->cfg.port);
    buffer[buffer_size - 1] = '\0';
}
