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

#include "defines.h"
#include "platform.h"
#include "fn_action.h"
#include "sdl.h"

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
#define CODE_MENU1		315
#define CODE_MENU2		316
#define KEYMON_CODE_PLUS	115
#define KEYMON_CODE_MINUS	114
#define CODE_MUTE		1
#define CODE_JACK		2

// keymon and api might need different codes

//	for ev.value
#define RELEASED	0
#define PRESSED		1
#define REPEAT		2

#define MUTE_STATE_PATH "/sys/class/gpio/gpio243/value"

#define INPUT_COUNT 5
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

static void send_app_action(const char* action, unsigned long long at_ms) {
	char request[192];
	int descriptor;
	size_t length;
	size_t written = 0;
	time_t now;

	if (!action || !action[0]) return;
	/* Without a real monotonic stamp devserv cannot reject a delayed request
	 * from the previous session, so drop it rather than risk a late exit. */
	if (at_ms == 0) return;
	descriptor = connect_devserv();
	if (descriptor < 0) {
		// The service is optional. Avoid a log storm while it is restarting.
		static time_t last_log;
		now = time(NULL);
		if (now - last_log >= 10) {
			last_log = now;
			fprintf(stderr, "keymon: devserv unavailable; FN action dropped\n");
		}
		return;
	}

	length = (size_t)snprintf(request, sizeof request,
		"{\"op\":\"app-action\",\"action\":\"%s\",\"at_ms\":%llu}\n",
		action, at_ms);
	if (length >= sizeof request) {
		close(descriptor);
		return;
	}
	while (written < length) {
		ssize_t count = write(descriptor, request + written, length - written);
		if (count < 0) {
			if (errno == EINTR) continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct pollfd wait = { descriptor, POLLOUT, 0 };
				if (poll(&wait, 1, 50) == 1) continue;
			}
			break;
		}
		written += (size_t)count;
	}
	close(descriptor);
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

	if (kind == FN_ACTION_KIND_HOME) send_app_action(FN_ACTION_HOME, at_ms);
	else if (kind == FN_ACTION_KIND_OVERLAY) send_app_action(FN_ACTION_OVERLAY, at_ms);
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
		}
	}
	
	return NULL;
}

int main (int argc, char *argv[]) {
	struct sigaction sa = {0};
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);

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
	
	uint32_t input;
	uint32_t val;
	uint32_t menu_pressed = 0;
	uint32_t menu2_pressed = 0;
	
	uint32_t up_pressed = 0;
	uint32_t up_just_pressed = 0;
	uint32_t up_repeat_at = 0;
	
	uint32_t down_pressed = 0;
	uint32_t down_just_pressed = 0;
	uint32_t down_repeat_at = 0;
	
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
			input = inputs[i];
			while(read(input, &ev, sizeof(ev))==sizeof(ev)) {
				if (ignore) continue;
				val = ev.value;
				if (ev.type==EV_SW) {
					//printf("switch: %i\n", ev.code);
					if (ev.code==CODE_JACK) {
						//printf("jack: %i\n", val);
						SetJack(val);
					}
				}
				if (( ev.type != EV_KEY ) || ( val > REPEAT )) continue;
				//printf("code: %i (%i)\n", ev.code, val); fflush(stdout);
				switch (ev.code) {
					case CODE_MENU2:
						menu_pressed = val;
					break;
					case CODE_MENU0:
						menu2_pressed = val;
					break;
					case KEYMON_CODE_PLUS:
						up_pressed = up_just_pressed = val;
						if (val) up_repeat_at = now + 300;
					break;
					case KEYMON_CODE_MINUS:
						down_pressed = down_just_pressed = val;
						if (val) down_repeat_at = now + 300;
					break;
					default:
					break;
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
		
		if (ignore) {
			menu_pressed = 0;
			menu2_pressed = 0;
			up_pressed = up_just_pressed = 0;
			down_pressed = down_just_pressed = 0;
			up_repeat_at = 0;
			down_repeat_at = 0;
		}
		
		if (up_just_pressed || (up_pressed && now>=up_repeat_at)) {
			if (menu_pressed) {
				val = GetBrightness();
				if (val<BRIGHTNESS_MAX) SetBrightness(++val);
			}
			else if (menu2_pressed) {
				val = GetColortemp();
				if (val<COLORTEMP_MAX) {
					SetColortemp(++val);
				}
			}
			else {
				val = GetVolume();
				if (val<VOLUME_MAX) SetVolume(++val);
			}
			
			if (up_just_pressed) up_just_pressed = 0;
			else up_repeat_at += 100;
		}
		
		if (down_just_pressed || (down_pressed && now>=down_repeat_at)) {
			if (menu_pressed) {
				val = GetBrightness();
				if (val>BRIGHTNESS_MIN) SetBrightness(--val);
			}
			else if (menu2_pressed) {
				val = GetColortemp();
				if (val>COLORTEMP_MIN) {
					SetColortemp(--val);
				}
			}
			else {
				val = GetVolume();
				if (val>VOLUME_MIN) SetVolume(--val);
			}
			
			if (down_just_pressed) down_just_pressed = 0;
			else down_repeat_at += 100;
		}
		
		then = now;
		ignore = 0;
		
		usleep(16666); // 60fps
	}

	for (int i=0; i<INPUT_COUNT; i++)
		close(inputs[i]);

	if (fn_joystick) SDL_JoystickClose(fn_joystick);
	SDL_Quit();
	
	pthread_cancel(mute_pt);
	pthread_join(mute_pt, NULL);
	return 0;
}
