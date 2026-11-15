#ifndef LED_LEASE_PLATFORM_H
#define LED_LEASE_PLATFORM_H

#include "led_lease.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Real ops backed by the existing NextUI LEDS_* app profile. This is the only
 * place C2 touches hardware, and it goes through platform.c like LedControl. */
const LedLeaseOps* led_lease_platform_ops(void);

/* Initializes lightsDefault/lightsApp from the user's settings and applies the
 * current rules. Call once before serving requests. */
void led_lease_platform_init(void);

/* Best-effort restore of the user default on service shutdown. */
void led_lease_platform_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
