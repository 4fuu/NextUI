#include "device_backend_rumble.h"

#include "defines.h"
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Production ops for the rumble backend.
 *
 * Every write goes through PLAT_setRumble(), the same call the desktop's VIB_
 * thread makes, so the strength semantics and the Brick Pro 2.5V ceiling stay
 * in platform.c instead of being re-derived here. This file adds no voltage,
 * no driver level and no timing of its own.
 *
 * The nodes below are the ones each platform's PLAT_setRumble() already writes.
 * They are only read, to prove a write landed: PLAT_setRumble returns void and
 * ignores a failed fopen, so the nodes are the only evidence available on the
 * host or the device.
 */

/* Host tests override the root so the real node handling can run against an
 * isolated tree; production builds use the absolute sysfs paths. */
#ifndef NEXTUI_RUMBLE_NODE_ROOT
#define NEXTUI_RUMBLE_NODE_ROOT ""
#endif

#define RUMBLE_NODE(path) NEXTUI_RUMBLE_NODE_ROOT path

typedef struct
{
    const char* enable_node;
    /* NULL when the platform has no strength node at all. */
    const char* level_node;
    const char* level_unit;
    /* 0 when PLAT_setRumble ignores the strength and only switches the motor
     * on and off. Such a platform must report unavailable: accepting a
     * strength it cannot set would fake a write. */
    int sets_strength;
} RumblePath;

static void set_text(char* detail, size_t detail_size, const char* text)
{
    if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "%s", text);
}

static int platform_is(const char* name)
{
#if defined(PLATFORM)
    return strcmp(PLATFORM, name) == 0;
#else
    (void)name;
    return 0;
#endif
}

static const RumblePath* rumble_path(void)
{
    if (platform_is("tg5040"))
    {
        /* Brick Pro: the motor driver takes a voltage in microvolts and is
         * capped at 2.5V by platform.c, whatever strength is requested. */
        static const RumblePath path = {
            RUMBLE_NODE("/sys/class/gpio/gpio227/value"),
            RUMBLE_NODE("/sys/class/motor/voltage"),
            "uv",
            1,
        };
        return &path;
    }
    if (platform_is("tg5050"))
    {
        /* tg5050 writes the raw strength to the motor level node. */
        static const RumblePath path = {
            RUMBLE_NODE("/sys/class/gpio/gpio236/value"),
            RUMBLE_NODE("/sys/class/motor/level"),
            "level",
            1,
        };
        return &path;
    }
    if (platform_is("h700"))
    {
        /* h700 has no strength node: PLAT_setRumble switches the motor on and
         * off through the PMIC and drops the strength. */
        static const RumblePath path = {
            RUMBLE_NODE("/sys/class/power_supply/axp2202-battery/moto"),
            NULL,
            NULL,
            0,
        };
        return &path;
    }
    return NULL;
}

static int read_node_long(const char* path, long* value)
{
    FILE* file;
    int scanned;

    file = fopen(path, "r");
    if (file == NULL) return 0;
    scanned = fscanf(file, "%ld", value);
    fclose(file);
    return scanned == 1;
}

/*
 * devserv is headless and never runs GFX_init(), which is where api.c calls
 * PLAT_initPlatform(). Without that call the platform's own model detection has
 * not run, and PLAT_setRumble would apply the non-Brick-Pro 3.3V ceiling on a
 * Brick Pro. platform.c already re-runs this detection lazily where the model
 * matters (PLAT_getNumLeds); the write path does the same, and on tg5040 the
 * detection only reads $DEVICE.
 */
static int platform_ready = 0;

static void ensure_platform(void)
{
    if (platform_ready) return;
    PLAT_initPlatform();
    platform_ready = 1;
}

/* tg5040 is the only platform whose rumble ceiling depends on the model:
 * PLAT_setRumble picks 2.5V when is_brickpro and 3.3V otherwise, and
 * PLAT_initPlatform() derives is_brickpro from $DEVICE alone. launch.sh exports
 * it before nextui_devserv_start, but a devserv started by hand may not have it,
 * and an unknown value would silently fall through to the 3.3V ceiling on
 * hardware that may be a Brick Pro. These are the models tg5040's launch.sh
 * exports; an environment that names anything else has not been identified, so
 * this backend refuses to serve rumble there instead of guessing a ceiling.
 * platform.c's own map is untouched: a recognized model keeps exactly the
 * voltage platform.c chooses for it. */
static const char* const DEVICE_MODELS[] = {"brick", "brickpro", "smartpro"};

static int platform_model_known(void)
{
    const char* device;
    size_t i;

    /* The ceiling does not depend on the model on the other platforms. */
    if (!platform_is("tg5040")) return 1;

    device = getenv("DEVICE");
    if (device == NULL || device[0] == '\0') return 0;
    for (i = 0; i < sizeof DEVICE_MODELS / sizeof DEVICE_MODELS[0]; i++)
        if (strcmp(device, DEVICE_MODELS[i]) == 0) return 1;
    return 0;
}

static void set_model_detail(char* detail, size_t detail_size)
{
    set_text(detail, detail_size,
             "DEVICE does not name a supported model, so the rumble strength cannot be verified");
}

static int platform_available(void* context, char* detail, size_t detail_size)
{
    const RumblePath* path;
    char text[192];

    (void)context;

    path = rumble_path();
    if (path == NULL || path->enable_node == NULL)
    {
        set_text(detail, detail_size, "no rumble path on this platform");
        return 0;
    }
    if (!path->sets_strength)
    {
        set_text(detail, detail_size,
                 "this platform switches the motor on and off only and cannot set a strength");
        return 0;
    }
    /* Safety before anything else: an unidentified tg5040 model must never be
     * told that a strength can be served. */
    if (!platform_model_known())
    {
        set_model_detail(detail, detail_size);
        return 0;
    }
    /* The enable node is both the write target and the read-back source, so a
     * node that cannot be read back would make every call unverifiable. */
    if (access(path->enable_node, R_OK | W_OK) != 0)
    {
        snprintf(text, sizeof text, "rumble enable node is missing or not readable/writable: %s",
                 path->enable_node);
        set_text(detail, detail_size, text);
        return 0;
    }
    if (path->level_node != NULL && access(path->level_node, W_OK) != 0)
    {
        snprintf(text, sizeof text, "rumble level node is missing or not writable: %s",
                 path->level_node);
        set_text(detail, detail_size, text);
        return 0;
    }
    snprintf(text, sizeof text, "PLAT_setRumble path %s; the motor itself is unobservable",
             path->enable_node);
    set_text(detail, detail_size, text);
    return 1;
}

static int platform_read(void* context, RumbleState* state, char* detail, size_t detail_size)
{
    const RumblePath* path;
    long enabled;

    (void)context;

    if (state == NULL)
    {
        set_text(detail, detail_size, "missing rumble state buffer");
        return 0;
    }
    path = rumble_path();
    if (path == NULL || path->enable_node == NULL)
    {
        set_text(detail, detail_size, "no rumble path on this platform");
        return 0;
    }

    memset(state, 0, sizeof *state);
    state->enabled = -1;
    if (!read_node_long(path->enable_node, &enabled))
    {
        snprintf(detail, detail_size, "rumble enable node could not be read: %s",
                 path->enable_node);
        return 0;
    }
    state->enabled = enabled != 0;

    if (path->level_node != NULL && read_node_long(path->level_node, &state->level))
        state->level_read = 1;
    return 1;
}

static int platform_write(void* context, int strength, char* detail, size_t detail_size)
{
    (void)context;

    /* Write guard, independent of available(): an unidentified tg5040 model
     * must not reach PLAT_setRumble at all, because the ceiling PLAT_setRumble
     * picks depends on the model. Nothing is written and nothing is clamped
     * here; the platform keeps its own map for the models it knows. */
    if (!platform_model_known())
    {
        set_model_detail(detail, detail_size);
        return 0;
    }
    ensure_platform();
    /* The platform owns the mapping, including the Brick Pro 2.5V ceiling. */
    PLAT_setRumble(strength);
    set_text(detail, detail_size,
             "PLAT_setRumble accepted the strength; the motor itself is unobservable");
    return 1;
}

static RumbleOps PLATFORM_OPS = {
    "platform-rumble",
    NULL,
    platform_available,
    platform_read,
    platform_write,
    NULL, /* level_unit: taken from the platform path below */
};

const RumbleOps* device_backend_rumble_platform_ops(void)
{
    const RumblePath* path = rumble_path();

    PLATFORM_OPS.level_unit = path != NULL ? path->level_unit : NULL;
    return &PLATFORM_OPS;
}
