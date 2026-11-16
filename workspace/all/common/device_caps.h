#ifndef DEVICE_CAPS_H
#define DEVICE_CAPS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Device capability registry for the device service (plan C1).
 *
 * A capability name is a stable protocol identifier. The `grant_required` bit
 * remains as legacy gate metadata: devserv's default-allow policy ignores it
 * and permits every known, implemented capability. With
 * device_gate_set_default_allow(0), the old gate treats nonzero entries as
 * requiring a grant. The bit is not a product permission decision.
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
