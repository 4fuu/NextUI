#ifndef DEVICE_BACKEND_H
#define DEVICE_BACKEND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Backend seam between the device gate and the existing NextUI device paths.
 *
 * Availability is a separate question from authorization: a backend must
 * report 0 when the hardware or the platform implementation is not present.
 * It must never return success for an unwired capability. On the desktop
 * template (and until C2/C3 plug real calls in) the default backend reports 0
 * for every capability with the detail "no hardware backend wired".
 *
 * A later implementation plugs into the functions already owned by NextUI:
 *   display.brightness  -> PWR_setBrightness / PWR_getBrightness
 *   display.colortemp   -> PWR_setColortemp
 *   display.get         -> PWR_getBrightness / PWR_getColortemp
 *   audio.volume/sink   -> PWR_setVolume / SetRawVolume / SetAudioSink
 *   led.control         -> LEDS_* (C2 adds the app-profile lease)
 *   input.rumble        -> VIB_*
 *   battery.read        -> PLAT_getBatteryStatusFine
 *   power.info          -> PLAT_getModel / PLAT_isUSBConnected
 * Those calls are deliberately not duplicated here: this header only defines
 * where they belong.
 *
 * call() returns 1 on success and must write a JSON value into value ("null"
 * is valid). On failure it returns 0 and may explain in detail.
 * available()/call() must not write outside their buffers.
 */

typedef struct DeviceBackend
{
    const char* name;
    void* context;
    int (*available)(void* context, const char* capability, char* detail, size_t detail_size);
    int (*call)(void* context, const char* capability, const char* action, const char* args,
                char* value, size_t value_size, char* detail, size_t detail_size);
} DeviceBackend;

/* Production default: no hardware backend is wired in C1. */
const DeviceBackend* device_backend_none(void);

#ifdef __cplusplus
}
#endif

#endif
