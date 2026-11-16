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

#define MAX_REQUESTS 8
#define REQUEST_MAX 256

static int server_descriptor = -1;
static char requests[MAX_REQUESTS][REQUEST_MAX];
static volatile int request_count;

/* keymon's tg5040 header declares SetMute; the desktop test library does not
 * implement it because the host has no mute switch. The test never starts the
 * mute watcher, so a no-op keeps the link honest without testing hardware. */
void SetMute(int value)
{
    (void)value;
}

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

    close(server_descriptor);
    unlink(socket_path);
    printf("keymon FN forwarding tests passed\n");
    return 0;
}
