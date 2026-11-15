#include "device_backend.h"

#include <stdio.h>
#include <string.h>

static void set_detail(char* detail, size_t detail_size, const char* text)
{
    if (detail == NULL || detail_size == 0) return;
    if (text == NULL) text = "";
    snprintf(detail, detail_size, "%s", text);
}

static int none_available(void* context, const char* capability, char* detail, size_t detail_size)
{
    (void)context;
    (void)capability;
    set_detail(detail, detail_size, "no hardware backend wired");
    return 0;
}

static int none_call(void* context, const char* capability, const char* action, const char* args,
                     char* value, size_t value_size, char* detail, size_t detail_size)
{
    (void)context;
    (void)capability;
    (void)action;
    (void)args;
    (void)value;
    (void)value_size;
    set_detail(detail, detail_size, "no hardware backend wired");
    return 0;
}

static const DeviceBackend backend = {
    "none",
    NULL,
    none_available,
    none_call,
};

const DeviceBackend* device_backend_none(void)
{
    return &backend;
}
