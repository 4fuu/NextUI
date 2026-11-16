#ifndef DEVICE_SESSION_PLATFORM_H
#define DEVICE_SESSION_PLATFORM_H

#include "device_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Production session ops for devserv.
 *
 * Display and audio fields use the same libmsettings calls that nextui.elf,
 * keymon and Settings already use. There is no new sysfs or ALSA path here.
 * Haptics is intentionally unavailable: it lives in the desktop config store
 * and has no device capability, so C3 cannot honestly snapshot it from devserv.
 *
 * LED teardown is delegated to C2's device_backend_leds_session_end(). The
 * reference is weak so this file still builds on a tree without C2; in that
 * case LED state is reported unavailable instead of faking a restore.
 */
const DeviceSessionOps* device_session_platform_ops(void);

#ifdef __cplusplus
}
#endif

#endif
