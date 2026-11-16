#ifndef DEVICE_BACKEND_LEDS_H
#define DEVICE_BACKEND_LEDS_H

#include "device_backend.h"
#include "led_lease.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generic adapter used by tests and by the production constructor. The backend
 * is a process-global singleton because devserv is single-threaded and only one
 * app runs in the foreground at a time. */
const DeviceBackend* device_backend_leds_with_ops(const LedLeaseOps* ops, void* context);
/* Explicit teardown hook: drop any app lease and restore the user default.
 * Returns 1 on success (including no active lease), 0 when the restore failed. */
int device_backend_leds_restore(void);
void device_backend_leds_reset(void);

#ifdef __cplusplus
}
#endif

#endif
