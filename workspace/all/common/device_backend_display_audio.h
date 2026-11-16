#ifndef DEVICE_BACKEND_DISPLAY_AUDIO_H
#define DEVICE_BACKEND_DISPLAY_AUDIO_H

#include "device_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Display backend (R1).
 *
 * The adapter is split from the msettings calls on purpose: the generic code
 * owns argument validation, capability dispatch, JSON responses and readback
 * checks, while the platform implementation calls the existing msettings path.
 * Tests use fake ops, which is host verification only and never evidence that
 * a panel changed.
 *
 * Ranges are the existing msettings ranges. They are validated here before any
 * platform write because SetBrightness()/SetColortemp() feed a switch without a
 * default case in the platform libraries: an out-of-range value would use an
 * uninitialised raw value.
 */

#define DISPLAY_AUDIO_BRIGHTNESS_MIN 0
#define DISPLAY_AUDIO_BRIGHTNESS_MAX 10
#define DISPLAY_AUDIO_COLORTEMP_MIN 0
#define DISPLAY_AUDIO_COLORTEMP_MAX 40

typedef enum
{
    DISPLAY_AUDIO_BRIGHTNESS = 0,
    DISPLAY_AUDIO_COLORTEMP,
    DISPLAY_AUDIO_FIELD_COUNT
} DisplayAudioField;

typedef struct DisplayAudioOps
{
    const char* name;
    void* context;
    /*
     * available: the platform path for this field exists. It must be free of
     * side effects; devserv asks for status before any app has run.
     * read/write: 1 on platform success, 0 with a detail on failure. msettings
     * setters return void, so write() success means "the platform path
     * accepted the call"; the adapter then reads the value back.
     */
    int (*available)(void* context, DisplayAudioField field, char* detail, size_t detail_size);
    int (*read)(void* context, DisplayAudioField field, int* value, char* detail,
                size_t detail_size);
    int (*write)(void* context, DisplayAudioField field, int value, char* detail,
                 size_t detail_size);
} DisplayAudioOps;

/* Generic adapter used by the tests and by the production constructor. */
const DeviceBackend* device_backend_display_audio_with_ops(const DisplayAudioOps* ops,
                                                           void* context);

/* Production constructor: the msettings display path, no second sysfs
 * controller. Every unsupported field (desktop) reports unavailable. */
const DeviceBackend* device_backend_display_audio(void);

/*
 * Compose the display/audio backend with the existing LED backend so devserv
 * still exposes exactly one DeviceBackend. display.* and audio.* go to this
 * module; every other capability goes to base. A NULL base keeps those
 * capabilities unavailable. This is the only routing change devserv needs.
 */
const DeviceBackend* device_backend_display_audio_compose(const DeviceBackend* base);

void device_backend_display_audio_reset(void);

#ifdef __cplusplus
}
#endif

#endif
