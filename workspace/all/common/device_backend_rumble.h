#ifndef DEVICE_BACKEND_RUMBLE_H
#define DEVICE_BACKEND_RUMBLE_H

#include "device_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Rumble backend (R3, input.rumble).
 *
 * The adapter is split from the platform calls on purpose: the generic code
 * owns argument validation, capability dispatch, JSON responses and read-back
 * checks, while the platform implementation calls the existing PLAT_setRumble
 * path. Tests use fake ops and an isolated sysfs tree, which is host
 * verification only and never evidence that a motor turned.
 *
 * The strength domain is the existing one: 0..0xFFFF, exactly the range
 * PLAT_setRumble already accepts (MAX_STRENGTH in every platform). The adapter
 * never maps a strength to a voltage or a driver level of its own; the platform
 * keeps that mapping, so the Brick Pro 2.5V ceiling stays where it is instead of
 * being re-derived (and possibly re-guessed) here.
 */

#define RUMBLE_STRENGTH_MIN 0
#define RUMBLE_STRENGTH_MAX 0xFFFF
#define RUMBLE_UNIT_MAX 16

typedef struct
{
    /* -1 when the enable node exists but could not be read. */
    int enabled;
    /* Raw value of the level node, meaningful only when level_read is 1. */
    long level;
    int level_read;
} RumbleState;

typedef struct
{
    const char* name;
    void* context;
    /*
     * available: this platform implements a rumble path and the nodes it writes
     * are present and writable. It must be free of side effects; devserv asks
     * for status before any app has run. A platform whose PLAT_setRumble is a
     * no-op, or that can only switch the motor on and off, must report 0 here:
     * advertising a strength the platform cannot set would fake a write.
     * read: the nodes as they are now. 1 when the enable node was read, 0 with
     * a detail when it could not be.
     * write: 1 on platform success, 0 with a detail on failure. PLAT_setRumble
     * returns void and ignores a failed write, so write() success means "the
     * platform accepted the call"; the adapter then reads the nodes back.
     */
    int (*available)(void* context, char* detail, size_t detail_size);
    int (*read)(void* context, RumbleState* state, char* detail, size_t detail_size);
    int (*write)(void* context, int strength, char* detail, size_t detail_size);
    /* Unit of the level node value: "uv" (microvolts), "level" (raw strength),
     * or NULL when the platform has no level node. */
    const char* level_unit;
} RumbleOps;

/* Generic adapter used by the tests and by the production constructor. */
const DeviceBackend* device_backend_rumble_with_ops(const RumbleOps* ops, void* context);

/* Production constructor: the existing PLAT_setRumble path, no second sysfs
 * controller. A platform whose PLAT_setRumble is a no-op, or that can only
 * switch the motor on and off, reports input.rumble unavailable. */
const DeviceBackend* device_backend_rumble(void);

/*
 * Compose the rumble backend with an existing backend so devserv still exposes
 * exactly one DeviceBackend. A capability the base already reports available
 * stays with the base; otherwise input.rumble goes to this module and every
 * other capability goes to the base. A NULL base keeps capabilities this module
 * does not serve unavailable. This is the only routing change devserv needs.
 */
const DeviceBackend* device_backend_rumble_compose(const DeviceBackend* base);

void device_backend_rumble_reset(void);

#ifdef __cplusplus
}
#endif

#endif
