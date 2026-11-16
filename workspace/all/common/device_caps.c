#include "device_caps.h"

#include <string.h>

/*
 * Keep this list in protocol order: status responses enumerate it in order, so
 * clients and tests see a stable layout. Names are exactly the plan's device
 * capability identifiers; no aliases are accepted.
 */
static const DeviceCapabilityInfo capabilities[] = {
    {"battery.read", 0},
    {"power.info", 0},
    {"display.get", 0},
    {"display.brightness", 1},
    {"display.colortemp", 1},
    {"audio.volume", 1},
    {"audio.sink", 1},
    {"led.control", 1},
    {"net.wifi", 1},
    {"net.bluetooth", 1},
    {"power.sleep", 1},
    {"input.rumble", 1},
    {"time.set", 1},
};

size_t device_capability_total(void)
{
    return sizeof capabilities / sizeof capabilities[0];
}

const DeviceCapabilityInfo* device_capability_at(size_t index)
{
    if (index >= device_capability_total()) return NULL;
    return &capabilities[index];
}

const DeviceCapabilityInfo* device_capability_find(const char* name)
{
    size_t i;

    if (name == NULL || name[0] == '\0') return NULL;
    for (i = 0; i < device_capability_total(); i++)
    {
        if (strcmp(capabilities[i].name, name) == 0) return &capabilities[i];
    }
    return NULL;
}

int device_capability_is_read_only(const char* name)
{
    const DeviceCapabilityInfo* info = device_capability_find(name);
    return info != NULL && !info->grant_required;
}
