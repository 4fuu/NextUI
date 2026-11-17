/*
 * EVIOCGRAB lifecycle for the resident HOME overlay.
 *
 * While the system HOME panel is open, the app must not receive navigation
 * keys. keymon already owns the evdev fds it polls; this module grabs the
 * selected nodes and, crucially, exposes a single release path used by close,
 * session-end, HUD switch, normal exit, and SIGTERM cleanup. Release is
 * idempotent, so an error path can call it without leaving input locked.
 *
 * The ioctl is injectable so the host test can assert the acquire/release
 * sequence without a device.
 */
#ifndef NEXTUI_SYSTEM_GRAB_H
#define NEXTUI_SYSTEM_GRAB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*SystemGrabIoctl)(int fd, unsigned long request, unsigned long argument);

/* NULL restores the real ioctl. Tests install a fake before acquire(). */
void SystemGrab_setIoctl(SystemGrabIoctl fn);

/* Grab every fd selected by mask. Re-acquiring releases the previous set
 * first. Returns the number of successfully grabbed descriptors. */
int SystemGrab_acquire(const int* fds, uint32_t mask, int count);

/* Release every grabbed descriptor; safe to call repeatedly or when idle. */
void SystemGrab_release(void);

int SystemGrab_active(void);
uint32_t SystemGrab_mask(void);

#ifdef __cplusplus
}
#endif

#endif
