#include "device_protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SOCKET_PATH "/tmp/nextui-devserv.sock"
#define DEFAULT_CONNECT_WAIT_MS 2000
#define CONNECT_DELAY_MS 50

static long connect_wait_ms = DEFAULT_CONNECT_WAIT_MS;

static void usage(FILE* stream)
{
    fprintf(stream,
            "usage: nextdev [--socket PATH] [--pak DIR] [--app ID] [--session-id ID]\n"
            "               [--wait-ms N] <command> [arguments]\n"
            "\n"
            "commands:\n"
            "  ping                         check that devserv is reachable\n"
            "  status                       report declared/granted/available capabilities\n"
            "  call <cap> [action] [args]   request a capability (args is a JSON value)\n"
            "  session-start                snapshot a device session (--pak recommended)\n"
            "  session-stop                 restore a session; --session-id is the guard\n"
            "  session-status               report the current or last session\n"
            "  app-action <home|overlay>    request an app exit (at_ms is the press time)\n"
            "  app-exit-result              report the launch parent's bounded wait\n"
            "                               (--forced when the grace expired)\n"
            "  setsid [--] <cmd> [args]     run a command as its own process-group leader\n"
            "\n"
            "options:\n"
            "  --socket PATH      devserv socket (default $NEXTUI_DEVSERV_SOCKET or %s)\n"
            "  --pak DIR          pak directory (default $NEXTUI_PAK_PATH)\n"
            "  --app ID           expected app id; devserv rejects a mismatch\n"
            "  --session-id ID    hex generation id for session-stop/app-action\n"
            "  --at-ms N          monotonic press time for app-action (default now)\n"
            "  --forced           app-exit-result was a SIGKILL after the grace\n"
            "  --wait-ms N        bounded connect wait, 0..10000 (default %d)\n"
            "\n"
            "Prints the raw JSON response. Exit codes: 0 allowed/served, 1 denied or\n"
            "unavailable, 2 usage error, 3 communication error.\n",
            DEFAULT_SOCKET_PATH, DEFAULT_CONNECT_WAIT_MS);
}

static void sleep_milliseconds(long milliseconds)
{
    struct timespec request;
    request.tv_sec = milliseconds / 1000;
    request.tv_nsec = (milliseconds % 1000) * 1000000L;
    (void)nanosleep(&request, NULL);
}

static int connect_to(const char* path)
{
    struct sockaddr_un address;
    int attempts;
    int attempt;

    if (strlen(path) >= sizeof address.sun_path)
    {
        fprintf(stderr, "nextdev: socket path is too long: %s\n", path);
        return -1;
    }

    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);

    attempts = (int)((connect_wait_ms + CONNECT_DELAY_MS - 1) / CONNECT_DELAY_MS);
    if (attempts < 1) attempts = 1;

    for (attempt = 0; attempt < attempts; attempt++)
    {
        int descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
        int saved;

        if (descriptor < 0)
        {
            fprintf(stderr, "nextdev: socket: %s\n", strerror(errno));
            return -1;
        }
        if (connect(descriptor, (struct sockaddr*)&address, sizeof address) == 0)
            return descriptor;

        saved = errno;
        close(descriptor);
        if (saved != ENOENT && saved != ECONNREFUSED && saved != ENOTSOCK) break;
        sleep_milliseconds(CONNECT_DELAY_MS);
    }

    fprintf(stderr, "nextdev: cannot connect to %s\n", path);
    return -1;
}

static int write_all(int descriptor, const char* data, size_t length)
{
    size_t written = 0;

    while (written < length)
    {
        ssize_t count = write(descriptor, data + written, length - written);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            return 0;
        }
        written += (size_t)count;
    }
    return 1;
}

static int read_response(int descriptor, char* out, size_t out_size)
{
    size_t total = 0;

    while (total + 1 < out_size)
    {
        ssize_t count = read(descriptor, out + total, out_size - 1 - total);

        if (count == 0) break;
        if (count < 0)
        {
            if (errno == EINTR) continue;
            return 0;
        }
        total += (size_t)count;
        if (memchr(out + total - (size_t)count, '\n', (size_t)count) != NULL) break;
    }

    out[total] = '\0';
    while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r')) out[--total] = '\0';
    return total > 0;
}

int main(int argc, char* argv[])
{
    char socket_path[DEVICE_GRANT_PATH_MAX];
    const char* pak = NULL;
    const char* app = NULL;
    const char* command;
    const char* capability = NULL;
    const char* action = NULL;
    const char* args = NULL;
    const char* session_id = NULL;
    const char* op;
    unsigned long long at_ms = 0;
    int forced = 0;
    char request[DEVICE_PROTOCOL_LINE_MAX];
    char response[DEVICE_PROTOCOL_LINE_MAX];
    int descriptor;
    int argument = 1;
    int socket_overridden = 0;
    int session_command = 0;
    int app_action_command = 0;
    int exit_result_command = 0;
    int ok;

    snprintf(socket_path, sizeof socket_path, "%s", DEFAULT_SOCKET_PATH);

    while (argument < argc && argv[argument][0] == '-')
    {
        if (strcmp(argv[argument], "--socket") == 0 && argument + 1 < argc)
        {
            if (argv[argument + 1][0] == '\0'
                || strlen(argv[argument + 1]) >= sizeof socket_path)
            {
                fprintf(stderr, "nextdev: invalid socket path\n");
                return 2;
            }
            snprintf(socket_path, sizeof socket_path, "%s", argv[argument + 1]);
            socket_overridden = 1;
            argument += 2;
        }
        else if (strcmp(argv[argument], "--pak") == 0 && argument + 1 < argc)
        {
            pak = argv[argument + 1];
            argument += 2;
        }
        else if (strcmp(argv[argument], "--app") == 0 && argument + 1 < argc)
        {
            app = argv[argument + 1];
            argument += 2;
        }
        else if (strcmp(argv[argument], "--session-id") == 0 && argument + 1 < argc)
        {
            session_id = argv[argument + 1];
            argument += 2;
        }
        else if (strcmp(argv[argument], "--at-ms") == 0 && argument + 1 < argc)
        {
            char* end = NULL;

            at_ms = strtoull(argv[argument + 1], &end, 10);
            if (end == argv[argument + 1] || *end != '\0')
            {
                fprintf(stderr, "nextdev: --at-ms must be a non-negative integer\n");
                return 2;
            }
            argument += 2;
        }
        else if (strcmp(argv[argument], "--forced") == 0)
        {
            forced = 1;
            argument += 1;
        }
        else if (strcmp(argv[argument], "--wait-ms") == 0 && argument + 1 < argc)
        {
            char* end = NULL;
            long value = strtol(argv[argument + 1], &end, 10);

            if (end == argv[argument + 1] || *end != '\0' || value < 0 || value > 10000)
            {
                fprintf(stderr, "nextdev: --wait-ms must be 0..10000\n");
                return 2;
            }
            connect_wait_ms = value;
            argument += 2;
        }
        else if (strcmp(argv[argument], "--help") == 0 || strcmp(argv[argument], "-h") == 0)
        {
            usage(stdout);
            return 0;
        }
        else
        {
            fprintf(stderr, "nextdev: unknown option: %s\n", argv[argument]);
            usage(stderr);
            return 2;
        }
    }

    {
        const char* environment = getenv("NEXTUI_DEVSERV_SOCKET");
        if (!socket_overridden && environment != NULL && environment[0] != '\0')
            snprintf(socket_path, sizeof socket_path, "%s", environment);
    }
    if (pak == NULL || pak[0] == '\0') pak = getenv("NEXTUI_PAK_PATH");
    if (app == NULL || app[0] == '\0') app = getenv("NEXTUI_APP_ID");

    if (argument >= argc)
    {
        usage(stderr);
        return 2;
    }
    command = argv[argument++];

    if (strcmp(command, "setsid") == 0)
    {
        /* Launch the app as the leader of a new process group so the launch
         * parent can signal the whole app, descendants included, through
         * -$pid. This is the fallback for devices whose rootfs ships neither
         * setsid nor busybox; the pid stays the same through exec, so the
         * parent's $! is the process-group id it supervises. */
        if (argument < argc && strcmp(argv[argument], "--") == 0) argument++;
        if (argument >= argc)
        {
            fprintf(stderr, "nextdev: setsid needs a command\n");
            return 2;
        }
        /* EPERM means this process already leads its own group, which is the
         * isolation the caller needs. Any other state is unsafe to signal. */
        if (setsid() < 0 && getpgrp() != getpid())
        {
            fprintf(stderr, "nextdev: setsid: %s\n", strerror(errno));
            return 3;
        }
        execvp(argv[argument], &argv[argument]);
        fprintf(stderr, "nextdev: exec %s: %s\n", argv[argument], strerror(errno));
        return 127;
    }

    if (strcmp(command, "ping") == 0)
    {
        op = "ping";
        if (argument != argc)
        {
            usage(stderr);
            return 2;
        }
    }
    else if (strcmp(command, "status") == 0)
    {
        op = "status";
        if (argument != argc || pak == NULL || pak[0] == '\0')
        {
            fprintf(stderr, "nextdev: status needs --pak DIR\n");
            return 2;
        }
    }
    else if (strcmp(command, "call") == 0)
    {
        op = "call";
        if (argument >= argc || pak == NULL || pak[0] == '\0')
        {
            fprintf(stderr, "nextdev: call needs --pak DIR and a capability\n");
            return 2;
        }
        capability = argv[argument++];
        if (argument < argc) action = argv[argument++];
        if (argument < argc) args = argv[argument++];
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
    }
    else if (strcmp(command, "session-start") == 0)
    {
        op = "session-start";
        session_command = 1;
        if (argument != argc)
        {
            usage(stderr);
            return 2;
        }
    }
    else if (strcmp(command, "session-stop") == 0)
    {
        op = "session-stop";
        session_command = 1;
        if (argument != argc)
        {
            usage(stderr);
            return 2;
        }
    }
    else if (strcmp(command, "session-status") == 0)
    {
        op = "session-status";
        session_command = 1;
        if (argument != argc)
        {
            usage(stderr);
            return 2;
        }
    }
    else if (strcmp(command, "app-action") == 0)
    {
        op = "app-action";
        app_action_command = 1;
        if (argument >= argc)
        {
            fprintf(stderr, "nextdev: app-action needs home or overlay\n");
            return 2;
        }
        action = argv[argument++];
        if (strcmp(action, "home") != 0 && strcmp(action, "overlay") != 0)
        {
            fprintf(stderr, "nextdev: app-action must be home or overlay\n");
            return 2;
        }
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
    }
    else if (strcmp(command, "app-exit-result") == 0)
    {
        op = "app-exit-result";
        exit_result_command = 1;
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
    }
    else
    {
        fprintf(stderr, "nextdev: unknown command: %s\n", command);
        usage(stderr);
        return 2;
    }

    if (app_action_command)
    {
        if (!device_protocol_build_app_action_request(action, at_ms, session_id, request,
                                                      sizeof request))
        {
            fprintf(stderr, "nextdev: request is too long\n");
            return 2;
        }
    }
    else if (exit_result_command)
    {
        if (!device_protocol_build_app_exit_result_request(session_id, forced, request,
                                                           sizeof request))
        {
            fprintf(stderr, "nextdev: request is too long\n");
            return 2;
        }
    }
    else if (session_command)
    {
        if (!device_protocol_build_session_request(op, app, pak, session_id, request,
                                                   sizeof request))
        {
            fprintf(stderr, "nextdev: request is too long\n");
            return 2;
        }
    }
    else if (!device_protocol_build_request(op, app, pak, capability, action, args, request,
                                            sizeof request))
    {
        fprintf(stderr, "nextdev: request is too long\n");
        return 2;
    }

    descriptor = connect_to(socket_path);
    if (descriptor < 0) return 3;

    if (!write_all(descriptor, request, strlen(request)) || !write_all(descriptor, "\n", 1))
    {
        fprintf(stderr, "nextdev: could not send request\n");
        close(descriptor);
        return 3;
    }
    (void)shutdown(descriptor, SHUT_WR);

    if (!read_response(descriptor, response, sizeof response))
    {
        fprintf(stderr, "nextdev: no response from devserv\n");
        close(descriptor);
        return 3;
    }
    close(descriptor);

    printf("%s\n", response);
    ok = device_protocol_response_ok(response);
    if (ok == 1) return 0;
    if (ok == 0) return 1;
    fprintf(stderr, "nextdev: malformed response\n");
    return 3;
}
