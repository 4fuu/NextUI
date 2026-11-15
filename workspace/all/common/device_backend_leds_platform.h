#ifndef DEVICE_BACKEND_LEDS_PLATFORM_H
#define DEVICE_BACKEND_LEDS_PLATFORM_H

#include "device_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Production constructor: wires the generic adapter to the existing LEDS_*
 * app profile. This does not create a second hardware path. */
const DeviceBackend* device_backend_leds(void);
/* C3 session teardown: call on every app exit, including a crash that C3
 * detected. C2 supplies the hook; PID watching and the call itself are C3. */
int device_backend_leds_session_end(void);
void device_backend_leds_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
