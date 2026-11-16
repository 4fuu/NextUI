#ifndef DEVICE_BACKEND_DISPLAY_AUDIO_H
#define DEVICE_BACKEND_DISPLAY_AUDIO_H

#include "device_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Display and audio backend (R1/R2).
 *
 * The adapter is split from the msettings calls on purpose: the generic code
 * owns argument validation, capability dispatch, JSON responses and readback
 * checks, while the platform implementation calls the existing msettings /
 * PWR path. Tests use fake ops, which is host verification only and never
 * evidence that a panel, mixer or speaker changed.
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
#define DISPLAY_AUDIO_VOLUME_MIN 0
#define DISPLAY_AUDIO_VOLUME_MAX 20
#define DISPLAY_AUDIO_MUTE_MIN 0
#define DISPLAY_AUDIO_MUTE_MAX 1

#define DISPLAY_AUDIO_SINK_DEFAULT 0
#define DISPLAY_AUDIO_SINK_BLUETOOTH 1
#define DISPLAY_AUDIO_SINK_USBDAC 2
#define DISPLAY_AUDIO_SINK_MIN 0
#define DISPLAY_AUDIO_SINK_MAX 2

typedef enum
{
    DISPLAY_AUDIO_BRIGHTNESS = 0,
    DISPLAY_AUDIO_COLORTEMP,
    DISPLAY_AUDIO_VOLUME,
    DISPLAY_AUDIO_MUTE,
    DISPLAY_AUDIO_SINK,
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

/* Production constructor: the msettings display/audio path, no second sysfs
 * controller. Every unsupported field (desktop, h700 mute) reports unavailable. */
const DeviceBackend* device_backend_display_audio(void);

/*
 * Compose the display/audio backend with the existing LED backend so devserv
 * still exposes exactly one DeviceBackend. A capability the base already
 * reports available stays with the base; otherwise display.* and audio.* go
 * to this module and every other capability goes to the base. A NULL base
 * keeps capabilities this module does not serve unavailable. This is the only
 * routing change devserv needs.
 */
const DeviceBackend* device_backend_display_audio_compose(const DeviceBackend* base);

void device_backend_display_audio_reset(void);

#ifdef __cplusplus
}
#endif

#endif
