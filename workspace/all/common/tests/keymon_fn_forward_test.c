/*
 * Host-only keymon C4 forwarding test.
 *
 * Including keymon.c is deliberate: this exercises the real settings reader,
 * action parser and socket writer, not a copy. main() is renamed so the test
 * can call the static forward_fn_slot() without starting the input daemon.
 */
#define main keymon_main
#include "../../../tg5040/keymon/keymon.c"
#undef main

#include <sys/socket.h>
#include <sys/un.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_REQUESTS 32
#define REQUEST_MAX 256

static int server_descriptor = -1;
static char requests[MAX_REQUESTS][REQUEST_MAX];
static volatile int request_count;
static volatile int reply_overlay_system;
static volatile int reply_feedback_system;
static volatile int reply_sleep_defer;

static int grab_calls;
static int release_calls;

static int fake_grab_ioctl(int fd, unsigned long request, unsigned long argument)
{
    (void)fd;
    (void)request;
    if (argument) grab_calls++;
    else release_calls++;
    return 0;
}

/* keymon's tg5040 header declares SetMute; the desktop test library does not
 * implement it because the host has no mute switch. The test never starts the
 * mute watcher, so a no-op keeps the link honest without testing hardware. */
void SetMute(int value)
{
    (void)value;
}

/* The forwarding test does not exercise rendering. Keep the real overlay out
 * of the link so this binary stays a routing test, not a second render test. */
void PWR_setBatteryStatus(int charging, int percent)
{
    (void)charging;
    (void)percent;
}

int SystemOverlay_init(int screen_width, int screen_height)
{
    (void)screen_width;
    (void)screen_height;
    return -1;
}

int SystemOverlay_available(void) { return 0; }
int SystemOverlay_visible(void) { return 0; }
SystemOverlayMode SystemOverlay_mode(void) { return SYSTEM_OVERLAY_HIDDEN; }
int SystemOverlay_showHome(const char* app_name) { (void)app_name; return 0; }
int SystemOverlay_showIndicator(SystemOverlayMode mode) { (void)mode; return -1; }
void SystemOverlay_hide(void) {}
int SystemOverlay_input(SystemOverlayInput input) { (void)input; return 0; }
void SystemOverlay_shutdown(void) {}

static void* request_server(void* unused)
{
    (void)unused;
    for (;;)
    {
        int client = accept(server_descriptor, NULL, NULL);
        size_t length = 0;
        char ch;

        if (client < 0) continue;
        while (length + 1 < REQUEST_MAX && read(client, &ch, 1) == 1)
        {
            if (ch == '\n') break;
            requests[request_count][length++] = ch;
        }
        requests[request_count][length] = '\0';
        if (request_count + 1 < MAX_REQUESTS) request_count++;
        if (strstr(requests[request_count - 1], "\"action\":\"overlay\"")
            && reply_overlay_system)
        {
            const char* reply = "{\"ok\":true,\"session\":\"active\","
                                "\"session_id\":\"0123456789abcdef\","
                                "\"app\":\"Moonlight.pak\","
                                "\"handled_by_app\":false,\"handled_by_system\":true,"
                                "\"exit_requested\":false}\n";
            (void)write(client, reply, strlen(reply));
        }
        else if (strstr(requests[request_count - 1], "\"op\":\"sleep.request\""))
        {
            const char* reply = reply_sleep_defer
                ? "{\"ok\":true,\"decision\":\"defer\",\"sleep_id\":\"9f3c1a2b4d5e6f70\","
                  "\"subscribers\":1,\"acks\":0,\"inhibit\":{\"reason\":\"playing_video\","
                  "\"until_ms\":360000},\"defer_until_ms\":360000,"
                  "\"detail\":\"deferred by the application\"}\n"
                : "{\"ok\":true,\"decision\":\"proceed\",\"sleep_id\":\"9f3c1a2b4d5e6f70\","
                  "\"subscribers\":0,\"acks\":0,\"inhibit\":null,\"defer_until_ms\":0,"
                  "\"detail\":\"no subscribers\"}\n";
            (void)write(client, reply, strlen(reply));
        }
        else if (strstr(requests[request_count - 1], "\"op\":\"sleep.finished\""))
        {
            const char* reply = "{\"ok\":true,\"sleep_id\":\"9f3c1a2b4d5e6f70\","
                                "\"slept_ms\":1000}\n";
            (void)write(client, reply, strlen(reply));
        }
        else if (strstr(requests[request_count - 1], "\"op\":\"system-feedback\"")
                 && reply_feedback_system)
        {
            const char* reply = "{\"ok\":true,\"session\":\"active\","
                                "\"session_id\":\"0123456789abcdef\","
                                "\"handled_by_app\":false,\"handled_by_system\":true,"
                                "\"exit_requested\":false}\n";
            (void)write(client, reply, strlen(reply));
        }
        else if (strstr(requests[request_count - 1], "\"action\":\"overlay\""))
        {
            const char* reply = "{\"ok\":true,\"session\":\"active\","
                                "\"session_id\":\"0123456789abcdef\","
                                "\"handled_by_app\":true,\"handled_by_system\":false,"
                                "\"exit_requested\":false}\n";
            (void)write(client, reply, strlen(reply));
        }
        else
        {
            const char* reply = "{\"ok\":true,\"session\":\"active\","
                                "\"session_id\":\"0123456789abcdef\","
                                "\"handled_by_app\":false,\"handled_by_system\":false,"
                                "\"exit_requested\":true}\n";
            (void)write(client, reply, strlen(reply));
        }
        close(client);
    }
    return NULL;
}

static int wait_for_requests(int expected)
{
    int attempts;

    for (attempts = 0; attempts < 100; attempts++)
    {
        if (request_count >= expected) return 1;
        usleep(10000);
    }
    return 0;
}

static void write_settings(const char* directory, const char* fn1, const char* fn2,
                           const char* fn3)
{
    char path[512];
    FILE* file;

    snprintf(path, sizeof path, "%s/minuisettings.txt", directory);
    file = fopen(path, "w");
    if (!file)
    {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    fprintf(file, "fn1action=%s\nfn2action=%s\nfn3action=%s\n", fn1, fn2, fn3);
    fclose(file);
}

static void expect_request(int index, const char* action, unsigned long long at_ms)
{
    char needle[128];

    if (index >= request_count)
    {
        fprintf(stderr, "missing request %d for %s\n", index, action);
        exit(1);
    }
    snprintf(needle, sizeof needle, "\"action\":\"%s\"", action);
    if (!strstr(requests[index], needle))
    {
        fprintf(stderr, "request %d is not %s: %s\n", index, action, requests[index]);
        exit(1);
    }
    snprintf(needle, sizeof needle, "\"at_ms\":%llu", at_ms);
    if (!strstr(requests[index], needle))
    {
        fprintf(stderr, "request %d lost the press time: %s\n", index, requests[index]);
        exit(1);
    }
    if (strstr(requests[index], "pid") || strstr(requests[index], "kill"))
    {
        fprintf(stderr, "keymon leaked a process-control field: %s\n", requests[index]);
        exit(1);
    }
}

int main(void)
{
    char directory[] = "/tmp/nextui-keymon-fn-XXXXXX";
    char socket_path[256];
    struct sockaddr_un address;
    pthread_t thread;

    if (!mkdtemp(directory))
    {
        perror("mkdtemp");
        return 1;
    }
    snprintf(socket_path, sizeof socket_path, "%s/devserv.sock", directory);
    if (strlen(socket_path) >= sizeof address.sun_path)
    {
        fprintf(stderr, "socket path is too long: %s\n", socket_path);
        return 1;
    }

    server_descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_descriptor < 0)
    {
        perror("socket");
        return 1;
    }
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof address.sun_path, "%s", socket_path);
    if (bind(server_descriptor, (struct sockaddr*)&address, sizeof address) != 0
        || listen(server_descriptor, 4) != 0)
    {
        perror("bind/listen");
        return 1;
    }
    if (pthread_create(&thread, NULL, request_server, NULL) != 0)
    {
        fprintf(stderr, "cannot start request thread\n");
        return 1;
    }

    setenv("SHARED_USERDATA_PATH", directory, 1);
    setenv("NEXTUI_DEVSERV_SOCKET", socket_path, 1);

    /* Empty legacy values use the same per-slot defaults as the app bridge. */
    write_settings(directory, "", "", "");
    forward_fn_slot(1, 1001);
    if (!wait_for_requests(1)) return 1;
    expect_request(0, "home", 1001);

    forward_fn_slot(2, 1002);
    if (!wait_for_requests(2)) return 1;
    expect_request(1, "overlay", 1002);

    /* A user override is read on every press, not cached at startup. */
    write_settings(directory, "none", "overlay", "");
    forward_fn_slot(1, 1003);
    if (!wait_for_requests(3)) return 1;
    expect_request(2, "overlay", 1003);

    /* none and pak: are application-scope no-ops. */
    write_settings(directory, "none", "none", "pak:Tool.pak");
    forward_fn_slot(1, 1004);
    forward_fn_slot(2, 1005);
    usleep(150000);
    if (request_count != 3)
    {
        fprintf(stderr, "none/pak action sent a request: %s\n", requests[3]);
        return 1;
    }

    /* Any slot can be rebound to a system action. */
    write_settings(directory, "home", "none", "home");
    forward_fn_slot(0, 1006);
    if (!wait_for_requests(4)) return 1;
    expect_request(3, "home", 1006);
    forward_fn_slot(2, 1007);
    if (!wait_for_requests(5)) return 1;
    expect_request(4, "home", 1007);

    // HOME/volume state polling: edge detection and sane repeat timing without
    // consuming another process's evdev queue.
    {
        SystemKeyState previous = {0};
        SystemKeyState current = {0};
        SystemKeyRepeat repeat = {0};
        SystemKeyEvent events[8];
        int count;

        current.home = 1;
        count = system_key_events(&previous, &current, 1000, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_HOME) {
            fprintf(stderr, "HOME press was not emitted\n");
            return 1;
        }
        previous = current;
        count = system_key_events(&previous, &current, 1016, &repeat, events, 8);
        if (count != 0) {
            fprintf(stderr, "held HOME repeated\n");
            return 1;
        }
        current.home = 0;
        previous = current;

        current.volume_up = 1;
        count = system_key_events(&previous, &current, 2000, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_VOLUME_UP) {
            fprintf(stderr, "volume-up press was not emitted\n");
            return 1;
        }
        previous = current;
        count = system_key_events(&previous, &current, 2299, &repeat, events, 8);
        if (count != 0) {
            fprintf(stderr, "volume repeated before 300ms\n");
            return 1;
        }
        count = system_key_events(&previous, &current, 2300, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_VOLUME_UP) {
            fprintf(stderr, "volume repeat was not emitted at 300ms\n");
            return 1;
        }
        count = system_key_events(&previous, &current, 2400, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_VOLUME_UP) {
            fprintf(stderr, "volume repeat was not emitted at 100ms cadence\n");
            return 1;
        }

        current.volume_up = 0;
        current.menu = 1;
        previous = current;
        current.volume_up = 1;
        count = system_key_events(&previous, &current, 3000, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_BRIGHTNESS_UP) {
            fprintf(stderr, "menu+plus did not route to brightness\n");
            return 1;
        }

        current.menu = 0;
        current.select = 1;
        previous = current;
        current.volume_up = 0;
        previous = current;
        current.volume_up = 1;
        count = system_key_events(&previous, &current, 4000, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_COLORTEMP_UP) {
            fprintf(stderr, "select+plus did not route to colortemp\n");
            return 1;
        }

        current.select = 0;
        current.volume_up = 0;
        previous = current;
        current.brightness_down = 1;
        count = system_key_events(&previous, &current, 5000, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_BRIGHTNESS_DOWN) {
            fprintf(stderr, "dedicated brightness key was not emitted\n");
            return 1;
        }

        /* D-pad navigation and A/B are edge-detected; up/down repeat like the
         * overlay's PAD_justRepeated path, A/B do not repeat. */
        current.brightness_down = 0;
        previous = current;
        current.nav_down = 1;
        count = system_key_events(&previous, &current, 6000, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_NAV_DOWN) {
            fprintf(stderr, "d-pad down was not emitted\n");
            return 1;
        }
        previous = current;
        count = system_key_events(&previous, &current, 6299, &repeat, events, 8);
        if (count != 0) {
            fprintf(stderr, "d-pad repeated before 300ms\n");
            return 1;
        }
        count = system_key_events(&previous, &current, 6300, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_NAV_DOWN) {
            fprintf(stderr, "d-pad repeat was not emitted\n");
            return 1;
        }
        current.nav_down = 0;
        previous = current;
        current.accept = 1;
        count = system_key_events(&previous, &current, 6400, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_ACCEPT) {
            fprintf(stderr, "A/accept was not emitted\n");
            return 1;
        }
        previous = current;
        count = system_key_events(&previous, &current, 6500, &repeat, events, 8);
        if (count != 0) {
            fprintf(stderr, "A/accept repeated while held\n");
            return 1;
        }
        current.accept = 0;
        previous = current;
        current.cancel = 1;
        count = system_key_events(&previous, &current, 6600, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_CANCEL) {
            fprintf(stderr, "B/cancel was not emitted\n");
            return 1;
        }

        /* KEY_POWER is a single edge-detected event with no repeat. */
        current.cancel = 0;
        previous = current;
        current.power = 1;
        count = system_key_events(&previous, &current, 7000, &repeat, events, 8);
        if (count != 1 || events[0] != SYSKEY_EVENT_POWER) {
            fprintf(stderr, "power press was not emitted\n");
            return 1;
        }
        previous = current;
        count = system_key_events(&previous, &current, 7166, &repeat, events, 8);
        if (count != 0) {
            fprintf(stderr, "held power key repeated\n");
            return 1;
        }
    }

    /* Resident sleep handshake. The fake service answers defer first, then
     * proceed. The host has no /mnt/SDCARD/.system/tg5040/bin/suspend, so the
     * proceed path reports result:"failed" without touching real hardware. */
    {
        int before;

        reply_sleep_defer = 1;
        last_sleep_at = 0;
        sleep_in_flight = 0;
        before = request_count;
        handle_power_press();
        if (!wait_for_requests(before + 1)) {
            fprintf(stderr, "power press did not send sleep.request\n");
            return 1;
        }
        if (!strstr(requests[before], "\"op\":\"sleep.request\"")
            || !strstr(requests[before], "\"initiator\":\"keymon\"")
            || !strstr(requests[before], "\"reason\":\"user\"")
            || !strstr(requests[before], "\"timeout_ms\":1500")) {
            fprintf(stderr, "sleep.request shape is wrong: %s\n", requests[before]);
            return 1;
        }
        /* Deferred: no suspend report and the cooldown swallows a second press. */
        usleep(50000);
        if (request_count != before + 1) {
            fprintf(stderr, "deferred request produced another call: %s\n",
                    requests[before + 1]);
            return 1;
        }
        handle_power_press();
        usleep(50000);
        if (request_count != before + 1) {
            fprintf(stderr, "cooldown did not swallow the second power press\n");
            return 1;
        }

        reply_sleep_defer = 0;
        last_sleep_at = 0;
        sleep_in_flight = 0;
        before = request_count;
        handle_power_press();
        if (!wait_for_requests(before + 2)) {
            fprintf(stderr, "proceed did not report sleep.finished: %s\n",
                    requests[before]);
            return 1;
        }
        if (!strstr(requests[before], "\"op\":\"sleep.request\"")) {
            fprintf(stderr, "proceed path did not start with sleep.request: %s\n",
                    requests[before]);
            return 1;
        }
        if (!strstr(requests[before + 1], "\"op\":\"sleep.finished\"")
            || !strstr(requests[before + 1], "\"sleep_id\":\"9f3c1a2b4d5e6f70\"")
            || !strstr(requests[before + 1], "\"result\":\"failed\"")
            || !strstr(requests[before + 1], "\"woke_by\":\"unknown\"")) {
            fprintf(stderr, "sleep.finished shape is wrong: %s\n",
                    requests[before + 1]);
            return 1;
        }
    }

    // app-action/system-feedback response routing.
    {
        char session_id[24];
        char app[64];
        int handled_by_app = 0;
        reply_overlay_system = 1;
        if (request_overlay_open(6001, session_id, sizeof session_id, app, sizeof app,
                                 &handled_by_app) != 1) {
            fprintf(stderr, "system overlay route was not parsed\n");
            return 1;
        }
        if (strcmp(session_id, "0123456789abcdef") != 0 || handled_by_app
            || strcmp(app, "Moonlight.pak") != 0) {
            fprintf(stderr, "system overlay route lost its session id or app\n");
            return 1;
        }

        reply_overlay_system = 0;
        if (request_overlay_open(6002, session_id, sizeof session_id, app, sizeof app,
                                 &handled_by_app) != 0 || !handled_by_app)
        {
            fprintf(stderr, "adapted app overlay route was not parsed\n");
            return 1;
        }

        reply_feedback_system = 1;
        if (request_feedback("volume", session_id, sizeof session_id) != 1) {
            fprintf(stderr, "system feedback route was not parsed\n");
            return 1;
        }
        reply_feedback_system = 0;
        if (request_feedback("volume", session_id, sizeof session_id) == 1) {
            fprintf(stderr, "app feedback route was reported as system\n");
            return 1;
        }
    }

    // EVIOCGRAB lifecycle: close, session-end and abnormal cleanup all use
    // the same idempotent release. No device ioctl is issued here.
    {
        int fds[3] = {3, 4, 5};

        SystemGrab_setIoctl(fake_grab_ioctl);
        if (SystemGrab_acquire(fds, 0x7, 3) != 3 || SystemGrab_active() != 3) {
            fprintf(stderr, "grab acquire did not track every fd\n");
            return 1;
        }
        SystemGrab_release();
        if (SystemGrab_active() != 0 || SystemGrab_mask() != 0) {
            fprintf(stderr, "grab release left input locked\n");
            return 1;
        }

        /* An abnormal cleanup calls release directly, without a UI close. */
        if (SystemGrab_acquire(fds, 0x7, 3) != 3) {
            fprintf(stderr, "grab re-acquire failed\n");
            return 1;
        }
        SystemGrab_release();
        if (SystemGrab_active() != 0) {
            fprintf(stderr, "abnormal release left input locked\n");
            return 1;
        }

        /* The HOME-open path acquires the same nodes it will release. */
        inputs[0] = 3;
        system_key_mask = 1;
        if (!show_home(NULL, NULL) || SystemGrab_active() != 1) {
            fprintf(stderr, "HOME open did not grab its evdev node\n");
            return 1;
        }
        hide_overlay();
        if (SystemGrab_active() != 0) {
            fprintf(stderr, "overlay close did not release input\n");
            return 1;
        }

        /* The normal overlay-close path must release the grab too. */
        if (SystemGrab_acquire(fds, 0x7, 3) != 3) {
            fprintf(stderr, "grab setup for hide failed\n");
            return 1;
        }
        hide_overlay();
        if (SystemGrab_active() != 0) {
            fprintf(stderr, "overlay close did not release input\n");
            return 1;
        }
        if (grab_calls != release_calls) {
            fprintf(stderr, "grab/release ioctl counts differ: %d/%d\n", grab_calls, release_calls);
            return 1;
        }
        SystemGrab_setIoctl(NULL);
    }

    close(server_descriptor);
    unlink(socket_path);
    printf("keymon FN forwarding tests passed\n");
    return 0;
}
