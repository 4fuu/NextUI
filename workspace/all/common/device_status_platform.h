#ifndef DEVICE_STATUS_PLATFORM_H
#define DEVICE_STATUS_PLATFORM_H

#include "device_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Production status ops for devserv. This is the only status code that touches
 * PLAT_* / msettings; it links api.c and the platform layer, exactly like
 * devserv already does for the session adapter.
 *
 * Host tests build devserv without the platform stack and provide their own
 * definition of this function, the same way they replace
 * device_session_platform_ops(). The replacement must report every section
 * available:false, which is the desktop/no-backend contract.
 */
const DeviceStatusOps* device_status_platform_ops(void);

/*
 * Ops for a build with no status backend (host tests, a platform without the
 * stack). Every section reports available:false with a detail, which is the
 * desktop/no-backend contract in CONTRACT.md §8.1.
 */
const DeviceStatusOps* device_status_unavailable_ops(void);

#ifdef __cplusplus
}
#endif

#endif
