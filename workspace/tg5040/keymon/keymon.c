#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>

#include <msettings.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>

#include "defines.h"
#include "platform.h"
#include "fn_action.h"
#include "sdl.h"
#include "system_overlay.h"

// keymon is a separate process from the app, so it owns its own copies of the
// model globals that platform.h's JOY_* macros use.
int is_brick = 0;
int is_brickpro = 0;

#define VOLUME_MIN 		0
#define VOLUME_MAX 		20
#define BRIGHTNESS_MIN 	0
#define BRIGHTNESS_MAX 	10
#define COLORTEMP_MIN 	0
#define COLORTEMP_MAX 	40

#define CODE_MENU0		314
#define CODE_MENU2		316
#define CODE_JACK		2
#define MUTE_STATE_PATH "/sys/class/gpio/gpio243/value"

#define DEVSERV_RESPONSE_MAX 8192
#define SYSTEM_OVERLAY_SESSION_MAX 24

#define INPUT_COUNT 32
static int inputs[INPUT_COUNT] = {};
static struct input_event ev;

static volatile int quit = 0;
static void on_term(int sig) { quit = 1; }

static int getInt(char* path) {
	int i = 0;
	FILE *file = fopen(path, "r");
	if (file!=NULL) {
		fscanf(file, "%i", &i);
		fclose(file);
	}
	return i;
}

int stringEquals(const char* str1, const char* str2) {
	if (!str1 || !str2) return 0; // NULL isn't safe here
	int len1 = strlen(str1);
	if (len1!=strlen(str2)) return 0;
	return (strncmp(str1,str2,len1)==0);
}

// C4: keymon is a read-only observer. It forwards the user-assigned FN action
// to devserv; it never signals or records a PID. The service stamps the request
// with the session generation and the monotonic press time, so a delayed
// request cannot tear down a newer app session.
static unsigned long long monotonic_ms(void) {
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
	return (unsigned long long)now.tv_sec * 1000ULL
		+ (unsigned long long)now.tv_nsec / 1000000ULL;
}

static void read_settings_value(const char* key, char* out, size_t out_size) {
	const char* base = getenv("SHARED_USERDATA_PATH");
	char path[512];
	char line[256];
	FILE* file;
	size_t key_length;

	if (!out || out_size == 0) return;
	out[0] = '\0';
	if (!base || !base[0]) base = "/mnt/SDCARD/.userdata/shared";
	snprintf(path, sizeof path, "%s/minuisettings.txt", base);
	key_length = strlen(key);

	file = fopen(path, "r");
	if (!file) return;
	while (fgets(line, sizeof line, file)) {
		if (strncmp(line, key, key_length) != 0) continue;
		{
			char* value = line + key_length;
			value[strcspn(value, "\r\n")] = '\0';
			snprintf(out, out_size, "%s", value);
		}
		break;
	}
	fclose(file);
}

static int connect_devserv(void) {
	const char* path = getenv("NEXTUI_DEVSERV_SOCKET");
	struct sockaddr_un address;
	struct pollfd wait;
	int descriptor;
	int flags;
	int result;
	socklen_t length;
	int error = 0;

	if (!path || !path[0]) path = "/tmp/nextui-devserv.sock";
	if (strlen(path) >= sizeof address.sun_path) return -1;

	descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
	if (descriptor < 0) return -1;
	flags = fcntl(descriptor, F_GETFL, 0);
	if (flags >= 0) (void)fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);

	memset(&address, 0, sizeof address);
	address.sun_family = AF_UNIX;
	memcpy(address.sun_path, path, strlen(path) + 1);

	result = connect(descriptor, (struct sockaddr*)&address, sizeof address);
	if (result != 0 && errno != EINPROGRESS) {
		close(descriptor);
		return -1;
	}
	if (result != 0) {
		wait.fd = descriptor;
		wait.events = POLLOUT;
		wait.revents = 0;
		if (poll(&wait, 1, 50) != 1) {
			close(descriptor);
			return -1;
		}
		length = sizeof error;
		if (getsockopt(descriptor, SOL_SOCKET, SO_ERROR, &error, &length) != 0
			|| error != 0) {
			close(descriptor);
			return -1;
		}
	}
	return descriptor;
}

static int devserv_request(const char* request, char* response, size_t response_size) {
	int descriptor;
	size_t length;
	size_t written = 0;
	size_t total = 0;
	struct pollfd wait;

	if (!request || !request[0] || !response || response_size < 2) return -1;
	response[0] = '\0';

	descriptor = connect_devserv();
	if (descriptor < 0) {
		// The service is optional. Avoid a log storm while it is restarting.
		static time_t last_log;
		time_t now = time(NULL);
		if (now - last_log >= 10) {
			last_log = now;
			fprintf(stderr, "keymon: devserv unavailable; system action dropped\n");
		}
		return -1;
	}

	length = strlen(request);
	while (written < length) {
		ssize_t count = write(descriptor, request + written, length - written);
		if (count < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				wait.fd = descriptor;
				wait.events = POLLOUT;
				wait.revents = 0;
				if (poll(&wait, 1, 50) == 1) continue;
			}
			close(descriptor);
			return -1;
		}
		written += (size_t)count;
	}

	while (total + 1 < response_size) {
		ssize_t count;
		wait.fd = descriptor;
		wait.events = POLLIN;
		wait.revents = 0;
		if (poll(&wait, 1, 100) <= 0) break;
		count = read(descriptor, response + total, response_size - 1 - total);
		if (count < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (count == 0) break;
		total += (size_t)count;
		if (memchr(response, '\n', total)) break;
	}
	response[total] = '\0';
	close(descriptor);
	return total > 0 ? 0 : -1;
}

static int response_field_true(const char* response, const char* field) {
	char needle[64];
	if (!response || !field) return 0;
	snprintf(needle, sizeof needle, "\"%s\":true", field);
	return strstr(response, needle) != NULL;
}

static int response_session_id(const char* response, char* out, size_t out_size) {
	const char* cursor;
	size_t used = 0;

	if (!out || out_size == 0) return 0;
	out[0] = '\0';
	if (!response) return 0;
	cursor = strstr(response, "\"session_id\":\"");
	if (!cursor) return 0;
	cursor += strlen("\"session_id\":\"");
	while (cursor[used] && cursor[used] != '"' && used + 1 < out_size) {
		out[used] = cursor[used];
		used++;
	}
	out[used] = '\0';
	return used > 0;
}

// 1 = the system draws, 0 = the app owns its own indicator, -1 = no answer.
static int request_feedback(const char* action, char* session_id, size_t session_size) {
	char request[128];
	char response[DEVSERV_RESPONSE_MAX];

	if (session_id && session_size) session_id[0] = '\0';
	snprintf(request, sizeof request, "{\"op\":\"system-feedback\",\"action\":\"%s\"}\n",
		action);
	if (devserv_request(request, response, sizeof response) != 0) return -1;
	if (session_id) response_session_id(response, session_id, session_size);
	if (response_field_true(response, "handled_by_system")) return 1;
	if (response_field_true(response, "handled_by_app")) return 0;
	return -1;
}

// 1 = the system overlay must draw, 0 = the app handled it or no session
// exists, 2 = an older devserv turned HOME into an exit, -1 = no service.
static int request_overlay_open(unsigned long long at_ms, char* session_id,
								size_t session_size, int* handled_by_app) {
	char request[192];
	char response[DEVSERV_RESPONSE_MAX];

	if (session_id && session_size) session_id[0] = '\0';
	if (handled_by_app) *handled_by_app = 0;
	if (at_ms == 0) return -1;
	snprintf(request, sizeof request,
		"{\"op\":\"app-action\",\"action\":\"overlay\",\"at_ms\":%llu}\n", at_ms);
	if (devserv_request(request, response, sizeof response) != 0) return -1;
	if (session_id) response_session_id(response, session_id, session_size);
	if (response_field_true(response, "handled_by_app")) {
		if (handled_by_app) *handled_by_app = 1;
		return 0;
	}
	if (response_field_true(response, "handled_by_system")) return 1;
	if (response_field_true(response, "exit_requested")) return 2;
	return 0;
}

static int request_exit(unsigned long long at_ms, const char* session_id) {
	char request[256];
	char response[DEVSERV_RESPONSE_MAX];

	if (at_ms == 0) return 0;
	if (session_id && session_id[0]) {
		snprintf(request, sizeof request,
			"{\"op\":\"app-action\",\"action\":\"home\",\"at_ms\":%llu,"
			"\"session_id\":\"%s\"}\n", at_ms, session_id);
	}
	else {
		snprintf(request, sizeof request,
			"{\"op\":\"app-action\",\"action\":\"home\",\"at_ms\":%llu}\n", at_ms);
	}
	if (devserv_request(request, response, sizeof response) != 0) return 0;
	return response_field_true(response, "exit_requested");
}

static int session_matches(const char* session_id) {
	char response[DEVSERV_RESPONSE_MAX];
	char active_id[SYSTEM_OVERLAY_SESSION_MAX];

	if (devserv_request("{\"op\":\"session-status\"}\n", response, sizeof response) != 0)
		return 0;
	if (!strstr(response, "\"session\":\"active\"")) return 0;
	if (!session_id || !session_id[0]) return 1;
	if (!response_session_id(response, active_id, sizeof active_id)) return 0;
	return strcmp(active_id, session_id) == 0;
}

static int read_int_path(const char* path, int* out) {
	FILE* file;
	int value = 0;

	if (!path || !out) return 0;
	file = fopen(path, "r");
	if (!file) return 0;
	if (fscanf(file, "%i", &value) != 1) {
		fclose(file);
		return 0;
	}
	fclose(file);
	*out = value;
	return 1;
}

static void overlay_font_path(char* out, size_t out_size) {
	char font[64] = "font1.ttf";
	const char* system_path;

	read_settings_value("font=", font, sizeof font);
	if (strcmp(font, "1") == 0) snprintf(font, sizeof font, "font1.ttf");
	else if (strcmp(font, "0") == 0) snprintf(font, sizeof font, "font2.ttf");
	else if (!font[0] || !strstr(font, ".ttf") || strchr(font, '/') || strstr(font, "..")) {
		snprintf(font, sizeof font, "font1.ttf");
	}
	system_path = getenv("SYSTEM_PATH");
	if (system_path && system_path[0]) {
		snprintf(out, out_size, "%s/../res/%s", system_path, font);
		if (access(out, R_OK) != 0) {
			snprintf(out, out_size, "%s/../res/font1.ttf", system_path);
		}
	}
	else {
		snprintf(out, out_size, "%s/%s", RES_PATH, font);
		if (access(out, R_OK) != 0) {
			snprintf(out, out_size, "%s/font1.ttf", RES_PATH);
		}
	}
}

static int overlay_selected = 0;
static char overlay_session_id[SYSTEM_OVERLAY_SESSION_MAX] = "";
static unsigned long long overlay_hide_at = 0;
static unsigned long long overlay_session_check_at = 0;

static void overlay_status(SystemOverlayStatus* status, int can_exit) {
	int value = 0;

	memset(status, 0, sizeof *status);
	status->battery_percent = -1;
	if (read_int_path("/sys/class/power_supply/axp2202-battery/capacity", &value)) {
		status->battery_percent = value;
	}
	value = 0;
	if (read_int_path("/sys/class/power_supply/axp2202-usb/online", &value)) {
		status->charging = value == 1
			&& (status->battery_percent < 0 || status->battery_percent < 100);
	}
	status->volume = GetVolume();
	status->volume_max = VOLUME_MAX;
	status->brightness = GetBrightness();
	status->brightness_max = BRIGHTNESS_MAX;
	status->colortemp = GetColortemp();
	status->colortemp_max = COLORTEMP_MAX;
	status->muted = GetMute();
	status->can_exit = can_exit;
}

static int overlay_render(SystemOverlayMode mode, int selected, unsigned int hide_ms,
						  const char* session_id) {
	SystemOverlayStatus status;

	overlay_status(&status, session_id && session_id[0]);
	if (SystemOverlay_render(mode, &status, selected) != 0) return 0;
	if (session_id && session_id != overlay_session_id) {
		snprintf(overlay_session_id, sizeof overlay_session_id, "%s", session_id);
	}
	else if (!session_id) {
		overlay_session_id[0] = '\0';
	}
	overlay_selected = selected;
	overlay_hide_at = monotonic_ms() + hide_ms;
	overlay_session_check_at = monotonic_ms() + 500;
	return 1;
}

static void hide_overlay(void) {
	SystemOverlay_hide();
	overlay_session_id[0] = '\0';
	overlay_selected = 0;
	overlay_hide_at = 0;
	overlay_session_check_at = 0;
}

static int show_home(const char* session_id) {
	return overlay_render(SYSTEM_OVERLAY_HOME, 0, 5000, session_id);
}

static int show_feedback(const char* action, unsigned int hide_ms) {
	char session_id[SYSTEM_OVERLAY_SESSION_MAX];
	SystemOverlayMode mode;
	int route;

	route = request_feedback(action, session_id, sizeof session_id);
	if (route != 1) {
		if (SystemOverlay_mode() != SYSTEM_OVERLAY_HOME) hide_overlay();
		return 0;
	}

	if (strcmp(action, "volume") == 0) mode = SYSTEM_OVERLAY_VOLUME;
	else if (strcmp(action, "brightness") == 0) mode = SYSTEM_OVERLAY_BRIGHTNESS;
	else if (strcmp(action, "colortemp") == 0) mode = SYSTEM_OVERLAY_COLORTEMP;
	else mode = SYSTEM_OVERLAY_MUTE;

	if (SystemOverlay_mode() == SYSTEM_OVERLAY_HOME) {
		return overlay_render(SYSTEM_OVERLAY_HOME, overlay_selected, 5000, overlay_session_id);
	}
	return overlay_render(mode, 0, hide_ms, session_id);
}

static void activate_home(void) {
	if (overlay_selected == 0 || !overlay_session_id[0]) {
		hide_overlay();
		return;
	}
	(void)request_exit(monotonic_ms(), overlay_session_id);
	hide_overlay();
}

static void move_home_selection(int delta) {
	if (!overlay_session_id[0]) return;
	overlay_selected = (overlay_selected + 2 + delta) % 2;
	(void)overlay_render(SYSTEM_OVERLAY_HOME, overlay_selected, 5000, overlay_session_id);
}

static void handle_home_press(void) {
	unsigned long long at_ms = monotonic_ms();
	char session_id[SYSTEM_OVERLAY_SESSION_MAX];
	int route;

	if (SystemOverlay_visible() && SystemOverlay_mode() == SYSTEM_OVERLAY_HOME) {
		activate_home();
		return;
	}
	if (SystemOverlay_visible()) hide_overlay();
	if (at_ms == 0) return;

	route = request_overlay_open(at_ms, session_id, sizeof session_id, NULL);
	if (route != 1) return;
	if (show_home(session_id)) return;

	// The display layer is not available: keep the old HOME contract rather
	// than leaving the user with a key that does nothing.
	(void)request_exit(at_ms, session_id);
}

static int adjust_volume(int delta) {
	int value = GetVolume();
	if (delta > 0 && value < VOLUME_MAX) value++;
	else if (delta < 0 && value > VOLUME_MIN) value--;
	else return value;
	SetVolume(value);
	return value;
}

static int adjust_brightness(int delta) {
	int value = GetBrightness();
	if (delta > 0 && value < BRIGHTNESS_MAX) value++;
	else if (delta < 0 && value > BRIGHTNESS_MIN) value--;
	else return value;
	SetBrightness(value);
	return value;
}

static int adjust_colortemp(int delta) {
	int value = GetColortemp();
	if (delta > 0 && value < COLORTEMP_MAX) value++;
	else if (delta < 0 && value > COLORTEMP_MIN) value--;
	else return value;
	SetColortemp(value);
	return value;
}

static void handle_mute_change(void) {
	char session_id[SYSTEM_OVERLAY_SESSION_MAX];
	if (SystemOverlay_visible() && SystemOverlay_mode() == SYSTEM_OVERLAY_HOME) {
		(void)overlay_render(SYSTEM_OVERLAY_HOME, overlay_selected, 5000, overlay_session_id);
		return;
	}
	if (request_feedback("mute", session_id, sizeof session_id) == 1) {
		(void)overlay_render(SYSTEM_OVERLAY_MUTE, 0, 1600, session_id);
	}
	else if (SystemOverlay_visible()) {
		hide_overlay();
	}
}

static void check_overlay_timers(void) {
	unsigned long long now;

	if (!SystemOverlay_visible()) return;
	now = monotonic_ms();
	if (now >= overlay_hide_at) {
		hide_overlay();
		return;
	}
	if (now >= overlay_session_check_at) {
		overlay_session_check_at = now + 500;
		if (!session_matches(overlay_session_id)) hide_overlay();
	}
}

// The foreground app owns the framebuffer and may grab its evdev node. Reading
// events would then miss HOME/volume entirely, so keymon polls the kernel key
// bitmap with EVIOCGKEY. The ioctl reads state without consuming the queue and
// works while another process has the device grabbed. platform.c uses the same
// mechanism for Brick Pro HOME.
typedef struct {
	unsigned home;
	unsigned volume_up;
	unsigned volume_down;
	unsigned menu;
	unsigned select;
	unsigned brightness_up;
	unsigned brightness_down;
} SystemKeyState;

typedef struct {
	uint32_t volume_up_at;
	uint32_t volume_down_at;
	uint32_t brightness_up_at;
	uint32_t brightness_down_at;
} SystemKeyRepeat;

typedef enum {
	SYSKEY_EVENT_NONE = 0,
	SYSKEY_EVENT_HOME,
	SYSKEY_EVENT_VOLUME_UP,
	SYSKEY_EVENT_VOLUME_DOWN,
	SYSKEY_EVENT_BRIGHTNESS_UP,
	SYSKEY_EVENT_BRIGHTNESS_DOWN,
	SYSKEY_EVENT_COLORTEMP_UP,
	SYSKEY_EVENT_COLORTEMP_DOWN,
} SystemKeyEvent;

static int key_bit(const unsigned char* bits, int code) {
	return (bits[code / 8] & (1 << (code % 8))) ? 1 : 0;
}

static uint32_t discover_system_keys(void) {
	unsigned char bits[(KEY_MAX + 8) / 8];
	uint32_t mask = 0;
	int index;

	for (index = 0; index < INPUT_COUNT; index++) {
		if (inputs[index] < 0) continue;
		memset(bits, 0, sizeof bits);
		if (ioctl(inputs[index], EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) continue;
		if (key_bit(bits, KEY_HOMEPAGE) || key_bit(bits, KEY_VOLUMEUP)
			|| key_bit(bits, KEY_VOLUMEDOWN) || key_bit(bits, CODE_MENU2)
			|| key_bit(bits, CODE_MENU0) || key_bit(bits, KEY_BRIGHTNESSUP)
			|| key_bit(bits, KEY_BRIGHTNESSDOWN)) {
			mask |= (uint32_t)1 << index;
		}
	}
	return mask;
}

static void read_system_key_state(SystemKeyState* state, uint32_t mask) {
	unsigned char bits[(KEY_MAX + 8) / 8];
	int index;

	memset(state, 0, sizeof *state);
	for (index = 0; index < INPUT_COUNT; index++) {
		if (!(mask & ((uint32_t)1 << index)) || inputs[index] < 0) continue;
		memset(bits, 0, sizeof bits);
		if (ioctl(inputs[index], EVIOCGKEY(sizeof bits), bits) < 0) continue;
		state->home |= key_bit(bits, KEY_HOMEPAGE);
		state->volume_up |= key_bit(bits, KEY_VOLUMEUP);
		state->volume_down |= key_bit(bits, KEY_VOLUMEDOWN);
		state->menu |= key_bit(bits, CODE_MENU2);
		state->select |= key_bit(bits, CODE_MENU0);
		state->brightness_up |= key_bit(bits, KEY_BRIGHTNESSUP);
		state->brightness_down |= key_bit(bits, KEY_BRIGHTNESSDOWN);
	}
}

static int push_event(SystemKeyEvent* events, int count, int capacity, SystemKeyEvent event) {
	if (count < capacity) events[count] = event;
	return count < capacity ? count + 1 : count;
}

static SystemKeyEvent volume_event(const SystemKeyState* now, int up) {
	if (now->menu) return up ? SYSKEY_EVENT_BRIGHTNESS_UP : SYSKEY_EVENT_BRIGHTNESS_DOWN;
	if (now->select) return up ? SYSKEY_EVENT_COLORTEMP_UP : SYSKEY_EVENT_COLORTEMP_DOWN;
	return up ? SYSKEY_EVENT_VOLUME_UP : SYSKEY_EVENT_VOLUME_DOWN;
}

static int system_key_events(const SystemKeyState* previous, const SystemKeyState* current,
							 uint32_t now, SystemKeyRepeat* repeat,
							 SystemKeyEvent* events, int capacity) {
	int count = 0;

	if (current->home && !previous->home) count = push_event(events, count, capacity, SYSKEY_EVENT_HOME);

	if (!current->volume_up) repeat->volume_up_at = 0;
	else if (!previous->volume_up) {
		count = push_event(events, count, capacity, volume_event(current, 1));
		repeat->volume_up_at = now + 300;
	}
	else if (repeat->volume_up_at == 0) repeat->volume_up_at = now + 300;
	else if (now >= repeat->volume_up_at) {
		count = push_event(events, count, capacity, volume_event(current, 1));
		repeat->volume_up_at = now + 100;
	}

	if (!current->volume_down) repeat->volume_down_at = 0;
	else if (!previous->volume_down) {
		count = push_event(events, count, capacity, volume_event(current, 0));
		repeat->volume_down_at = now + 300;
	}
	else if (repeat->volume_down_at == 0) repeat->volume_down_at = now + 300;
	else if (now >= repeat->volume_down_at) {
		count = push_event(events, count, capacity, volume_event(current, 0));
		repeat->volume_down_at = now + 100;
	}

	if (!current->brightness_up) repeat->brightness_up_at = 0;
	else if (!previous->brightness_up) {
		count = push_event(events, count, capacity, SYSKEY_EVENT_BRIGHTNESS_UP);
		repeat->brightness_up_at = now + 300;
	}
	else if (repeat->brightness_up_at == 0) repeat->brightness_up_at = now + 300;
	else if (now >= repeat->brightness_up_at) {
		count = push_event(events, count, capacity, SYSKEY_EVENT_BRIGHTNESS_UP);
		repeat->brightness_up_at = now + 100;
	}

	if (!current->brightness_down) repeat->brightness_down_at = 0;
	else if (!previous->brightness_down) {
		count = push_event(events, count, capacity, SYSKEY_EVENT_BRIGHTNESS_DOWN);
		repeat->brightness_down_at = now + 300;
	}
	else if (repeat->brightness_down_at == 0) repeat->brightness_down_at = now + 300;
	else if (now >= repeat->brightness_down_at) {
		count = push_event(events, count, capacity, SYSKEY_EVENT_BRIGHTNESS_DOWN);
		repeat->brightness_down_at = now + 100;
	}

	return count;
}

static void handle_system_event(SystemKeyEvent event) {
	switch (event) {
		case SYSKEY_EVENT_HOME:
			handle_home_press();
			break;
		case SYSKEY_EVENT_VOLUME_UP:
			if (SystemOverlay_visible() && SystemOverlay_mode() == SYSTEM_OVERLAY_HOME) {
				move_home_selection(-1);
			}
			else {
				(void)adjust_volume(1);
				(void)show_feedback("volume", 1600);
			}
			break;
		case SYSKEY_EVENT_VOLUME_DOWN:
			if (SystemOverlay_visible() && SystemOverlay_mode() == SYSTEM_OVERLAY_HOME) {
				move_home_selection(1);
			}
			else {
				(void)adjust_volume(-1);
				(void)show_feedback("volume", 1600);
			}
			break;
		case SYSKEY_EVENT_BRIGHTNESS_UP:
			(void)adjust_brightness(1);
			if (SystemOverlay_visible() && SystemOverlay_mode() == SYSTEM_OVERLAY_HOME) {
				(void)overlay_render(SYSTEM_OVERLAY_HOME, overlay_selected, 5000,
					overlay_session_id);
			}
			else {
				(void)show_feedback("brightness", 1600);
			}
			break;
		case SYSKEY_EVENT_BRIGHTNESS_DOWN:
			(void)adjust_brightness(-1);
			if (SystemOverlay_visible() && SystemOverlay_mode() == SYSTEM_OVERLAY_HOME) {
				(void)overlay_render(SYSTEM_OVERLAY_HOME, overlay_selected, 5000,
					overlay_session_id);
			}
			else {
				(void)show_feedback("brightness", 1600);
			}
			break;
		case SYSKEY_EVENT_COLORTEMP_UP:
			(void)adjust_colortemp(1);
			if (SystemOverlay_visible() && SystemOverlay_mode() == SYSTEM_OVERLAY_HOME) {
				(void)overlay_render(SYSTEM_OVERLAY_HOME, overlay_selected, 5000,
					overlay_session_id);
			}
			else {
				(void)show_feedback("colortemp", 1600);
			}
			break;
		case SYSKEY_EVENT_COLORTEMP_DOWN:
			(void)adjust_colortemp(-1);
			if (SystemOverlay_visible() && SystemOverlay_mode() == SYSTEM_OVERLAY_HOME) {
				(void)overlay_render(SYSTEM_OVERLAY_HOME, overlay_selected, 5000,
					overlay_session_id);
			}
			else {
				(void)show_feedback("colortemp", 1600);
			}
			break;
		default:
			break;
	}
}

// The action string is the user's stored fn[123]action, with A2's legacy-empty
// fallback. pak:/none/unknown are deliberately ignored in application scope.
static void forward_fn_slot(int slot, unsigned long long at_ms) {
	char key[16];
	char stored[256];
	const char* action;
	const char* argument = NULL;
	FnActionKind kind;

	if (slot < 0 || slot >= 3) return;
	snprintf(key, sizeof key, "fn%daction=", slot + 1);
	read_settings_value(key, stored, sizeof stored);
	action = fnActionEffective(slot, stored);
	kind = fnActionParse(action, &argument);

	if (kind == FN_ACTION_KIND_HOME) {
		(void)request_exit(at_ms, NULL);
	}
	else if (kind == FN_ACTION_KIND_OVERLAY) {
		char session_id[SYSTEM_OVERLAY_SESSION_MAX];
		int route;
		if (SystemOverlay_visible()) hide_overlay();
		route = request_overlay_open(at_ms, session_id, sizeof session_id, NULL);
		if (route == 1 && !show_home(session_id)) {
			(void)request_exit(at_ms, session_id);
		}
	}
}

// This is the same SDL joystick index the app's PAD_poll sees for FN1/FN2.
// The real Brick Pro capture has no HOME button in js0 (only 0-10), so keymon
// does not forward a guessed HOME index. The launcher/adapted app gets HOME
// from the platform KEY_HOMEPAGE state query and opens the shared overlay.
static void handle_joystick_button(int button, unsigned long long at_ms) {
	int fn1 = is_brick ? JOY_L3 : JOY_L4;
	int fn2 = is_brick ? JOY_R3 : JOY_R4;

	if (button == fn1) forward_fn_slot(0, at_ms);
	else if (button == fn2) forward_fn_slot(1, at_ms);
}

static pthread_t mute_pt;
static volatile int mute_changed = 0;
static void* watchMute(void *arg) {
	int is_muted,was_muted;
	int is_brickpro = stringEquals(getenv("DEVICE"), "brickpro");
	
	is_muted = was_muted = getInt(MUTE_STATE_PATH);
	SetMute(is_muted);
	
	while(!quit) {
		usleep(200000); // 5 times per second
		
		is_muted = getInt(MUTE_STATE_PATH);
		// swallow mute val -1 on shutdown
		if (is_muted >= 0 && was_muted!=is_muted) {
			was_muted = is_muted;
			SetMute(is_muted);
			if (GetMute()) {
				// tmp solution
				if (is_brickpro)
					system("echo 900000 > /sys/class/motor/voltage");
				else
					system("echo 1500000 > /sys/class/motor/voltage");
				system("echo 1 > /sys/class/gpio/gpio227/value");
				usleep(100000);
				system("echo 0 > /sys/class/gpio/gpio227/value");
				usleep(100000);
				system("echo 1 > /sys/class/gpio/gpio227/value");
				usleep(100000);
				system("echo 0 > /sys/class/gpio/gpio227/value");
			}
			mute_changed = 1;
		}
	}
	
	return NULL;
}

int main (int argc, char *argv[]) {
	struct sigaction sa = {0};
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);

	{
		const char* device = getenv("DEVICE");
		is_brick = stringEquals(device, "brick");
		is_brickpro = stringEquals(device, "brickpro");
	}

	InitSettings();
	pthread_create(&mute_pt, NULL, &watchMute, NULL);

	char path[32];
	for (int i=0; i<INPUT_COUNT; i++) {
		sprintf(path, "/dev/input/event%i", i);
		inputs[i] = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	}
	uint32_t system_key_mask = discover_system_keys();
	SystemKeyState key_state;
	SystemKeyRepeat key_repeat = {0};
	read_system_key_state(&key_state, system_key_mask);

	// Read the FN buttons through the same SDL joystick mapping the app sees.
	// Reading js0 directly would silently assume its button order matches SDL;
	// using SDL here keeps the platform's JOY_* macros as the only source.
	SDL_Joystick* fn_joystick = NULL;
	SDL_JoystickID fn_instance = -1;
	if (SDL_Init(SDL_INIT_JOYSTICK) == 0) {
		int count = SDL_NumJoysticks();
		for (int i = 0; i < count; i++) {
			fn_joystick = SDL_JoystickOpen(i);
			if (fn_joystick) {
				fn_instance = SDL_JoystickInstanceID(fn_joystick);
				break;
			}
		}
		if (fn_joystick) SDL_JoystickEventState(SDL_ENABLE);
		else fprintf(stderr, "keymon: no joystick for FN forwarding\n");
	}
	else {
		fprintf(stderr, "keymon: SDL joystick unavailable: %s\n", SDL_GetError());
	}

	char font_path[512];
	overlay_font_path(font_path, sizeof font_path);
	if (SystemOverlay_init(FIXED_WIDTH, FIXED_HEIGHT, FIXED_SCALE, font_path) != 0) {
		fprintf(stderr, "keymon: system overlay unavailable; HOME falls back to exit\n");
	}

	uint8_t ignore;
	uint32_t then;
	uint32_t now;
	struct timeval tod;

	gettimeofday(&tod, NULL);
	then = tod.tv_sec * 1000 + tod.tv_usec / 1000; // essential SDL_GetTicks()
	ignore = 0;

	while (!quit) {
		gettimeofday(&tod, NULL);
		now = tod.tv_sec * 1000 + tod.tv_usec / 1000;
		if (now-then>1000) ignore = 1; // ignore input that arrived during sleep

		for (int i=0; i<INPUT_COUNT; i++) {
			if (inputs[i] < 0) continue;
			while(read(inputs[i], &ev, sizeof(ev))==sizeof(ev)) {
				if (ignore) continue;
				if (ev.type==EV_SW && ev.code==CODE_JACK) {
					SetJack(ev.value);
				}
			}
		}

		if (fn_joystick) {
			SDL_Event event;
			while (SDL_PollEvent(&event)) {
				if (event.type != SDL_JOYBUTTONDOWN) continue;
				if (event.jbutton.which != fn_instance) continue;
				if (ignore) continue;
				handle_joystick_button(event.jbutton.button, monotonic_ms());
			}
		}

		{
			SystemKeyState next;
			SystemKeyEvent events[8];
			int count;
			read_system_key_state(&next, system_key_mask);
			if (ignore) {
				memset(&key_repeat, 0, sizeof key_repeat);
			}
			else {
				count = system_key_events(&key_state, &next, now, &key_repeat,
					events, (int)(sizeof events / sizeof events[0]));
				for (int e=0; e<count; e++) handle_system_event(events[e]);
			}
			key_state = next;
		}

		if (mute_changed) {
			mute_changed = 0;
			handle_mute_change();
		}
		check_overlay_timers();

		then = now;
		ignore = 0;

		usleep(16666); // 60fps
	}

	SystemOverlay_shutdown();

	for (int i=0; i<INPUT_COUNT; i++)
		close(inputs[i]);

	if (fn_joystick) SDL_JoystickClose(fn_joystick);
	SDL_Quit();
	
	pthread_cancel(mute_pt);
	pthread_join(mute_pt, NULL);
	return 0;
}
