#ifndef DEVICE_CAPS_H
#define DEVICE_CAPS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Device capability registry for the cooperative device service (plan C1).
 *
 * A capability name is a stable protocol identifier. "grant_required" is 0
 * only for read-only values that the plan exposes without a user grant
 * (battery.read, power.info, display.get). Everything else is denied until the
 * user has granted that exact capability to the exact app installation.
 *
 * This table says what the system understands, not what the hardware can do.
 * Availability is answered by the backend for every request.
 */

typedef struct
{
    const char* name;
    int grant_required;
} DeviceCapabilityInfo;

size_t device_capability_total(void);
const DeviceCapabilityInfo* device_capability_at(size_t index);
const DeviceCapabilityInfo* device_capability_find(const char* name);

/* 1 for a known read-only capability, 0 for gated or unknown names. */
int device_capability_is_read_only(const char* name);

#ifdef __cplusplus
}
#endif

#endif
