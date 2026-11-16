#include "defines.h"
#include "api.h"

#include <stdio.h>

static int failures = 0;
static int checks = 0;

#define CHECK(condition) \
    do { \
        checks++; \
        if (!(condition)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

#define CHECK_INT(actual, expected) \
    do { \
        int a_ = (int)(actual); \
        int e_ = (int)(expected); \
        checks++; \
        if (a_ != e_) { \
            fprintf(stderr, "FAIL %s:%d: %s: got %d, expected %d\n", __FILE__, __LINE__, \
                    #actual, a_, e_); \
            failures++; \
        } \
    } while (0)

int main(void)
{
    CHECK_INT(LEDS_appProfileActive(), 0);
    CHECK_INT(LEDS_pushAppProfile(), 0);
    CHECK_INT(LEDS_getAppLight(0) == NULL, 1);

    LEDS_initLeds();
    CHECK_INT(LEDS_getCount(), MAX_LIGHTS);
    CHECK_INT(LEDS_appProfileActive(), 0);
    CHECK(LEDS_getDefaultLight(0) != NULL);
    CHECK(LEDS_getDefaultLight(MAX_LIGHTS) == NULL);
    CHECK(LEDS_getDefaultLight(-1) == NULL);
    CHECK(LEDS_getAppLight(MAX_LIGHTS - 1) != NULL);
    CHECK(LEDS_getAppLight(MAX_LIGHTS) == NULL);

    lightsDefault[0].color1 = 0x112233;
    lightsDefault[0].brightness = 42;

    CHECK_INT(LEDS_pushAppProfile(), 1);
    CHECK_INT(LEDS_appProfileActive(), 1);
    CHECK_INT(LEDS_getProfileOverride(), LIGHT_PROFILE_APP);
    CHECK_INT(LEDS_getAppLight(0)->color1, 0x112233);
    CHECK_INT(LEDS_getAppLight(0)->brightness, 42);

    LEDS_getAppLight(0)->color1 = 0x445566;
    CHECK_INT(LEDS_updateAppProfile(), 1);
    CHECK_INT(lightsDefault[0].color1, 0x112233);
    CHECK_INT(LEDS_getAppLight(0)->color1, 0x445566);

    CHECK_INT(LEDS_pushAppProfile(), 1);
    CHECK_INT(LEDS_getAppLight(0)->color1, 0x445566);
    CHECK_INT(LEDS_popAppProfile(), 1);
    CHECK_INT(LEDS_appProfileActive(), 1);
    CHECK_INT(LEDS_getProfileOverride(), LIGHT_PROFILE_APP);
    CHECK_INT(LEDS_popAppProfile(), 1);
    CHECK_INT(LEDS_appProfileActive(), 0);
    CHECK_INT(LEDS_getProfileOverride(), LIGHT_PROFILE_DEFAULT);
    CHECK_INT(LEDS_popAppProfile(), 0);
    CHECK_INT(LEDS_updateAppProfile(), 0);

    CHECK_INT(LEDS_restoreDefault(), 0);

    CHECK_INT(LEDS_pushAppProfile(), 1);
    LEDS_getAppLight(0)->color1 = 0xABCDEF;
    CHECK_INT(LEDS_restoreDefault(), 1);
    CHECK_INT(LEDS_appProfileActive(), 0);
    CHECK_INT(LEDS_getProfileOverride(), LIGHT_PROFILE_DEFAULT);
    CHECK_INT(lightsDefault[0].color1, 0x112233);

    /* A higher-priority override above the app profile must survive cleanup. */
    CHECK_INT(LEDS_pushAppProfile(), 1);
    CHECK_INT(LEDS_pushProfileOverride(LIGHT_PROFILE_SLEEP), 1);
    CHECK_INT(LEDS_getProfileOverride(), LIGHT_PROFILE_SLEEP);
    CHECK_INT(LEDS_popAppProfile(), 0);
    CHECK_INT(LEDS_appProfileActive(), 1);
    CHECK_INT(LEDS_restoreDefault(), 1);
    CHECK_INT(LEDS_appProfileActive(), 0);
    CHECK_INT(LEDS_getProfileOverride(), LIGHT_PROFILE_SLEEP);
    CHECK_INT(LEDS_popProfileOverride(LIGHT_PROFILE_SLEEP), 1);
    CHECK_INT(LEDS_getProfileOverride(), LIGHT_PROFILE_DEFAULT);

    if (failures != 0)
    {
        fprintf(stderr, "led profile tests failed: %d failure(s), %d checks\n", failures, checks);
        return 1;
    }
    printf("led profile tests passed (%d checks)\n", checks);
    return 0;
}
