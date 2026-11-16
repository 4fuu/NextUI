#include "device_backend_leds_platform.h"

#include "device_backend_leds.h"
#include "led_lease_platform.h"

const DeviceBackend* device_backend_leds(void)
{
    led_lease_platform_init();
    return device_backend_leds_with_ops(led_lease_platform_ops(), NULL);
}

int device_backend_leds_session_end(void)
{
    return device_backend_leds_restore();
}

void device_backend_leds_shutdown(void)
{
    led_lease_platform_shutdown();
    device_backend_leds_reset();
}
