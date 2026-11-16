/*
 * Host replay for the real Brick Pro HOME capture.
 *
 * The device delivers KEY_HOMEPAGE (172) on event3, while js0 exposes only
 * buttons 0-10. This test feeds the captured kernel key state through the same
 * edge detector as PLAT_pollInput(), then drives the real FnBridge overlay path.
 * It does not read evdev and does not claim physical-key verification.
 */
#include "defines.h"
#include "api.h"
#include "fn_action.h"
#include "fn_bridge.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define REPLAY_EV_KEY 0x01
#define REPLAY_KEY_HOMEPAGE 172
#define REPLAY_JS_EVENT_BUTTON 0x01

typedef struct
{
	struct timeval time;
	uint16_t type;
	uint16_t code;
	int32_t value;
} ReplayInputEvent;

typedef struct
{
	uint32_t time;
	int16_t value;
	uint8_t type;
	uint8_t number;
} ReplayJoystickEvent;

static int checks;
static int failures;

#define CHECK(condition, message)                                                                  \
	do                                                                                             \
	{                                                                                              \
		checks++;                                                                                  \
		if (!(condition))                                                                          \
		{                                                                                          \
			failures++;                                                                            \
			printf("[FAIL] %s:%d: %s\n", __FILE__, __LINE__, message);                             \
		}                                                                                          \
	} while (0)

static unsigned char* read_file(const char* path, size_t* size_out)
{
	FILE* file = fopen(path, "rb");
	unsigned char* data;
	long size;

	if (!file) return NULL;
	if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		return NULL;
	}
	data = (unsigned char*)malloc((size_t)size);
	if (!data)
	{
		fclose(file);
		return NULL;
	}
	if (fread(data, 1, (size_t)size, file) != (size_t)size)
	{
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);
	*size_out = (size_t)size;
	return data;
}

static void inject_home_state(int pressed)
{
	/* PAD_poll clears these before it feeds the platform state. */
	pad.just_pressed = BTN_NONE;
	pad.just_released = BTN_NONE;
	pad.just_repeated = BTN_NONE;
	PAD_setHomePressed(pressed);
}

static void test_event3_home_replay(const char* path)
{
	size_t size = 0;
	unsigned char* data = read_file(path, &size);
	size_t count = data ? size / sizeof(ReplayInputEvent) : 0;
	int presses = 0;
	int releases = 0;

	CHECK(data != NULL, "event3 fixture must be readable");
	CHECK(size == 384 && count == 16, "event3 fixture must contain 16 input_event records");
	CHECK(sizeof(ReplayInputEvent) == 24, "host input_event ABI must be 24 bytes");

	if (data)
	{
		PAD_reset();
		for (size_t i = 0; i < count; i++)
		{
			ReplayInputEvent* event = &((ReplayInputEvent*)data)[i];
			if (event->type != REPLAY_EV_KEY || event->code != REPLAY_KEY_HOMEPAGE) continue;

			if (event->value == 1)
			{
				inject_home_state(1);
				CHECK(PAD_justPressed(BTN_HOME), "KEY_HOMEPAGE press must reach BTN_HOME");
				CHECK(PAD_isPressed(BTN_HOME), "KEY_HOMEPAGE press must stay held");
				presses++;

				/* A held key must not generate a second justPressed event. */
				inject_home_state(1);
				CHECK(!PAD_justPressed(BTN_HOME), "held KEY_HOMEPAGE must not repeat");
			}
			else if (event->value == 0)
			{
				inject_home_state(0);
				CHECK(PAD_justReleased(BTN_HOME), "KEY_HOMEPAGE release must reach BTN_HOME");
				CHECK(!PAD_isPressed(BTN_HOME), "released KEY_HOMEPAGE must not stay held");
				releases++;
			}
			else
			{
				inject_home_state(1);
				CHECK(!PAD_justPressed(BTN_HOME), "KEY_HOMEPAGE repeat must not create a press");
			}
		}
	}

	CHECK(presses == 3, "fixture must contain three complete HOME presses");
	CHECK(releases == 3, "fixture must contain three complete HOME releases");
	free(data);
}

static void test_js0_has_no_home_button(const char* path)
{
	size_t size = 0;
	unsigned char* data = read_file(path, &size);
	size_t count = data ? size / sizeof(ReplayJoystickEvent) : 0;
	unsigned char seen[256] = {0};
	int distinct = 0;

	CHECK(data != NULL, "js0 fixture must be readable");
	CHECK(size == 1968 && count == 246, "js0 fixture must contain 246 js_event records");
	CHECK(sizeof(ReplayJoystickEvent) == 8, "host js_event ABI must be 8 bytes");

	if (data)
	{
		for (size_t i = 0; i < count; i++)
		{
			ReplayJoystickEvent* event = &((ReplayJoystickEvent*)data)[i];
			if ((event->type & REPLAY_JS_EVENT_BUTTON) == 0) continue;
			if (!seen[event->number])
			{
				seen[event->number] = 1;
				distinct++;
			}
		}
	}

	CHECK(distinct == 11, "js0 must expose exactly 11 buttons");
	for (int button = 0; button <= 10; button++)
		CHECK(seen[button], "js0 button 0-10 must be present");
	CHECK(!seen[15], "js0 must not contain guessed HOME button 15");
	free(data);
}

static int test_exit_home(void* user)
{
	int* calls = (int*)user;
	(*calls)++;
	return 1;
}

static void test_adapted_app_overlay(void)
{
	int exit_calls = 0;
	OverlayAppHooks hooks;
	FnBridgeConfig config;
	FnBridgeActions actions = { NULL, NULL, FN_ACTION_OVERLAY };
	FnBridgeButtons buttons = { BTN_NONE, BTN_NONE, BTN_HOME };

	memset(&hooks, 0, sizeof hooks);
	hooks.capabilities = UI_OVERLAY_CAP_EXIT;
	hooks.app_name = "Replay";
	hooks.exit_to_desktop = test_exit_home;
	hooks.user = &exit_calls;

	memset(&config, 0, sizeof config);
	config.user = &exit_calls;
	config.exit_to_home = test_exit_home;
	config.overlay_hooks = &hooks;

	FnBridge* bridge = FnBridge_create(&config);
	CHECK(bridge != NULL, "FnBridge must be created");
	if (bridge)
	{
		PAD_reset();
		inject_home_state(1);
		CHECK(FnBridge_poll(bridge, &actions, &buttons) == 1,
			  "HOME press must be consumed by the adapted app");
		CHECK(FnBridge_overlayOpen(bridge), "HOME must open the shared overlay");

		/* Held HOME must not toggle the overlay a second time. */
		inject_home_state(1);
		CHECK(FnBridge_poll(bridge, &actions, &buttons) == 1,
			  "held HOME must stay in the overlay path");
		CHECK(FnBridge_overlayOpen(bridge), "held HOME must leave the overlay open");

		/* Release is a normal key release, not a close action. */
		inject_home_state(0);
		CHECK(FnBridge_poll(bridge, &actions, &buttons) == 1,
			  "HOME release must be consumed while the overlay owns input");
		CHECK(FnBridge_overlayOpen(bridge), "HOME release must leave the overlay open");

		/* The next press is the documented toggle. */
		inject_home_state(1);
		CHECK(FnBridge_poll(bridge, &actions, &buttons) == 1,
			  "second HOME press must be consumed");
		CHECK(!FnBridge_overlayOpen(bridge), "second HOME press must close the overlay");
		CHECK(FnBridge_exitRequested(bridge) == 0, "HOME overlay must not request exit");
		CHECK(exit_calls == 0, "HOME overlay must not call the app exit callback");

		FnBridge_destroy(bridge);
	}
}

int main(int argc, char** argv)
{
	const char* event3_path = argc > 1 ? argv[1]
									  : "workspace/all/common/tests/fixtures/brickpro-home-event3.bin";
	const char* js0_path = argc > 2 ? argv[2]
									: "workspace/all/common/tests/fixtures/brickpro-js0-run3.bin";

	if (SDL_Init(0) != 0)
	{
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	test_event3_home_replay(event3_path);
	test_js0_has_no_home_button(js0_path);
	test_adapted_app_overlay();

	CHECK(strcmp(fnActionEffective(1, ""), FN_ACTION_HOME) == 0,
		  "FN2 empty legacy value must still mean return to desktop");
	CHECK(strcmp(fnActionEffective(2, ""), FN_ACTION_OVERLAY) == 0,
		  "side HOME empty legacy value must still mean overlay");

	SDL_Quit();
	printf("HOME replay: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
