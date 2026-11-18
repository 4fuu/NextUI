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
            "  hello                        negotiate protocol 1 and report the session\n"
            "  battery                      read the battery section\n"
            "  status-get [field,field]     read overlay row data (default all sections)\n"
            "  sleep-status                 report the handshake phase and leases\n"
            "  sleep-request [reason]       system-side sleep request; waits for the decision\n"
            "  sleep-ack <sleep_id> [paused] [saved]\n"
            "  sleep-inhibit <reason> <timeout_ms> [sleep_id]\n"
            "  sleep-release [sleep_id]     drop a prepare or automatic inhibit\n"
            "  sleep-finished <sleep_id> [ok|failed] [woke_by]\n"
            "  hud-policy [app|system] [kind,kind]\n"
            "  overlay-state <true|false>   report the in-process overlay open state\n"
            "  overlay-host <true|false>    yield/restore in-process overlay hosting\n"
            "  subscribe [feature,feature] [field,field]\n"
            "                               stream events until the server closes\n"
            "  setsid [--] <cmd> [args]     run a command as its own process-group leader\n"
            "\n"
            "options:\n"
            "  --socket PATH      devserv socket (default $NEXTUI_DEVSERV_SOCKET or %s)\n"
            "  --pak DIR          pak directory (default $NEXTUI_PAK_PATH)\n"
            "  --app ID           expected app id; devserv rejects a mismatch\n"
            "  --session-id ID    hex generation id for session-stop/app-action\n"
            "  --at-ms N          monotonic press time for app-action (default now)\n"
            "  --forced           app-exit-result was a SIGKILL after the grace\n"
            "  --initiator NAME   sleep.request initiator (default system)\n"
            "  --timeout-ms N     sleep.request ack deadline (default 1500)\n"
            "  --wait-ms N        bounded connect wait, 0..10000 (default %d)\n"
            "\n"
            "Prints the raw JSON response. Exit codes: 0 allowed/served, 1 denied or\n"
            "unavailable, 2 usage error, 3 communication error.\n",
            DEFAULT_SOCKET_PATH, DEFAULT_CONNECT_WAIT_MS);
}

/* "a,b" -> "\"a\",\"b\"" so a command line can name a JSON string array. */
static int join_quoted(const char* csv, char* out, size_t out_size)
{
    size_t used = 0;
    int first = 1;

    if (out == NULL || out_size < 3) return 0;
    out[0] = '\0';
    while (csv != NULL && *csv != '\0')
    {
        const char* end = strchr(csv, ',');
        size_t length = end ? (size_t)(end - csv) : strlen(csv);

        if (length == 0) return 0;
        if (!first && used + 1 < out_size) out[used++] = ',';
        first = 0;
        if (used + 3 + length >= out_size) return 0;
        out[used++] = '"';
        memcpy(out + used, csv, length);
        used += length;
        out[used++] = '"';
        out[used] = '\0';
        if (!end) break;
        csv = end + 1;
    }
    return used != 0;
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
    const char* initiator = "system";
    int sleep_timeout_ms = DEVICE_PROTOCOL_SLEEP_ACK_MS;
    int stream_command = 0;
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
        else if (strcmp(argv[argument], "--initiator") == 0 && argument + 1 < argc)
        {
            initiator = argv[argument + 1];
            argument += 2;
        }
        else if (strcmp(argv[argument], "--timeout-ms") == 0 && argument + 1 < argc)
        {
            char* end = NULL;
            long value = strtol(argv[argument + 1], &end, 10);

            if (end == argv[argument + 1] || *end != '\0' || value < 0 || value > 5000)
            {
                fprintf(stderr, "nextdev: --timeout-ms must be 0..5000\n");
                return 2;
            }
            sleep_timeout_ms = (int)value;
            argument += 2;
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
    else if (strcmp(command, "hello") == 0)
    {
        op = "hello";
        if (argument != argc)
        {
            usage(stderr);
            return 2;
        }
        snprintf(request, sizeof request,
                 "{\"op\":\"hello\",\"client\":\"nextdev\",\"protocol\":%d%s%s%s%s%s%s}",
                 DEVICE_PROTOCOL_GENERATION, app ? ",\"app\":\"" : "", app ? app : "",
                 app ? "\"" : "", pak ? ",\"pak\":\"" : "", pak ? pak : "",
                 pak ? "\"" : "");
        goto send_request;
    }
    else if (strcmp(command, "battery") == 0 || strcmp(command, "sleep-status") == 0)
    {
        op = strcmp(command, "battery") == 0 ? "battery.status" : "sleep.status";
        if (argument != argc)
        {
            usage(stderr);
            return 2;
        }
    }
    else if (strcmp(command, "status-get") == 0)
    {
        char fields[256];

        op = "status.get";
        if (argument < argc) args = argv[argument++];
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
        if (args != NULL && !join_quoted(args, fields, sizeof fields))
        {
            fprintf(stderr, "nextdev: invalid field list\n");
            return 2;
        }
        if (args != NULL)
            snprintf(request, sizeof request, "{\"op\":\"status.get\",\"fields\":[%s]}",
                     fields);
        else
            snprintf(request, sizeof request, "{\"op\":\"status.get\"}");
        goto send_request;
    }
    else if (strcmp(command, "sleep-request") == 0)
    {
        op = "sleep.request";
        action = "user";
        if (argument < argc) action = argv[argument++];
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
        snprintf(request, sizeof request,
                 "{\"op\":\"sleep.request\",\"reason\":\"%s\",\"initiator\":\"%s\","
                 "\"timeout_ms\":%d}",
                 action, initiator, sleep_timeout_ms);
        goto send_request;
    }
    else if (strcmp(command, "sleep-ack") == 0)
    {
        int paused = 0;
        int saved = 0;

        op = "sleep.ack";
        if (argument >= argc)
        {
            fprintf(stderr, "nextdev: sleep-ack needs a sleep_id\n");
            return 2;
        }
        capability = argv[argument++];
        if (argument < argc) paused = atoi(argv[argument++]);
        if (argument < argc) saved = atoi(argv[argument++]);
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
        snprintf(request, sizeof request,
                 "{\"op\":\"sleep.ack\",\"sleep_id\":\"%s\",\"paused\":%s,"
                 "\"saved\":%s%s%s%s}",
                 capability, paused ? "true" : "false", saved ? "true" : "false",
                 session_id ? ",\"session_id\":\"" : "", session_id ? session_id : "",
                 session_id ? "\"" : "");
        goto send_request;
    }
    else if (strcmp(command, "sleep-inhibit") == 0)
    {
        const char* reason;
        int timeout;
        const char* sleep_id = NULL;

        op = "sleep.inhibit";
        if (argument + 1 >= argc || session_id == NULL)
        {
            fprintf(stderr, "nextdev: sleep-inhibit needs --session-id, a reason and timeout_ms\n");
            return 2;
        }
        reason = argv[argument++];
        timeout = atoi(argv[argument++]);
        if (argument < argc) sleep_id = argv[argument++];
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
        snprintf(request, sizeof request,
                 "{\"op\":\"sleep.inhibit\",\"session_id\":\"%s\"%s%s%s,\"reason\":\"%s\","
                 "\"timeout_ms\":%d}",
                 session_id, sleep_id ? ",\"sleep_id\":\"" : "",
                 sleep_id ? sleep_id : "", sleep_id ? "\"" : "", reason, timeout);
        goto send_request;
    }
    else if (strcmp(command, "sleep-release") == 0)
    {
        const char* sleep_id = NULL;

        op = "sleep.release";
        if (session_id == NULL)
        {
            fprintf(stderr, "nextdev: sleep-release needs --session-id\n");
            return 2;
        }
        if (argument < argc) sleep_id = argv[argument++];
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
        snprintf(request, sizeof request,
                 "{\"op\":\"sleep.release\",\"session_id\":\"%s\"%s%s%s}",
                 session_id, sleep_id ? ",\"sleep_id\":\"" : "",
                 sleep_id ? sleep_id : "", sleep_id ? "\"" : "");
        goto send_request;
    }
    else if (strcmp(command, "sleep-finished") == 0)
    {
        const char* sleep_id;
        const char* result = "ok";
        const char* woke_by = "power";

        op = "sleep.finished";
        if (argument >= argc)
        {
            fprintf(stderr, "nextdev: sleep-finished needs a sleep_id\n");
            return 2;
        }
        sleep_id = argv[argument++];
        if (argument < argc) result = argv[argument++];
        if (argument < argc) woke_by = argv[argument++];
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
        snprintf(request, sizeof request,
                 "{\"op\":\"sleep.finished\",\"sleep_id\":\"%s\",\"result\":\"%s\","
                 "\"woke_by\":\"%s\"}",
                 sleep_id, result, woke_by);
        goto send_request;
    }
    else if (strcmp(command, "hud-policy") == 0)
    {
        const char* mode = NULL;
        const char* kinds = NULL;

        op = "hud.policy";
        if (session_id == NULL)
        {
            fprintf(stderr, "nextdev: hud-policy needs --session-id\n");
            return 2;
        }
        if (argument < argc) mode = argv[argument++];
        if (argument < argc) kinds = argv[argument++];
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
        if (mode && strcmp(mode, "app") != 0 && strcmp(mode, "system") != 0)
        {
            fprintf(stderr, "nextdev: hud-policy mode must be app or system\n");
            return 2;
        }
        if (kinds != NULL)
        {
            char quoted[256];

            if (!join_quoted(kinds, quoted, sizeof quoted))
            {
                fprintf(stderr, "nextdev: invalid kind list\n");
                return 2;
            }
            snprintf(request, sizeof request,
                     "{\"op\":\"hud.policy\",\"session_id\":\"%s\"%s%s%s,\"kinds\":[%s]}",
                     session_id, mode ? ",\"mode\":\"" : "", mode ? mode : "",
                     mode ? "\"" : "", quoted);
        }
        else
        {
            snprintf(request, sizeof request,
                     "{\"op\":\"hud.policy\",\"session_id\":\"%s\"%s%s%s}",
                     session_id, mode ? ",\"mode\":\"" : "", mode ? mode : "",
                     mode ? "\"" : "");
        }
        goto send_request;
    }
    else if (strcmp(command, "overlay-state") == 0 || strcmp(command, "overlay-host") == 0)
    {
        int value;

        op = strcmp(command, "overlay-state") == 0 ? "overlay.state" : "overlay.host";
        if (session_id == NULL || argument >= argc)
        {
            fprintf(stderr, "nextdev: %s needs --session-id and true|false\n", command);
            return 2;
        }
        value = strcmp(argv[argument++], "true") == 0;
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
        snprintf(request, sizeof request,
                 "{\"op\":\"%s\",\"session_id\":\"%s\",\"%s\":%s}", op, session_id,
                 strcmp(op, "overlay.state") == 0 ? "open" : "host",
                 value ? "true" : "false");
        goto send_request;
    }
    else if (strcmp(command, "subscribe") == 0)
    {
        const char* features = "sleep,status,hud,overlay";
        const char* fields = NULL;

        op = "subscribe";
        if (argument < argc) features = argv[argument++];
        if (argument < argc) fields = argv[argument++];
        if (argument != argc)
        {
            fprintf(stderr, "nextdev: too many arguments\n");
            return 2;
        }
        {
            char feature_json[256];
            char field_json[256];

            if (!join_quoted(features, feature_json, sizeof feature_json))
            {
                fprintf(stderr, "nextdev: invalid feature list\n");
                return 2;
            }
            if (fields != NULL && !join_quoted(fields, field_json, sizeof field_json))
            {
                fprintf(stderr, "nextdev: invalid field list\n");
                return 2;
            }
            if (fields != NULL)
            {
                snprintf(request, sizeof request,
                         "{\"op\":\"subscribe\"%s%s%s,\"features\":[%s],"
                         "\"status\":{\"fields\":[%s]}}",
                         session_id ? ",\"session_id\":\"" : "",
                         session_id ? session_id : "", session_id ? "\"" : "",
                         feature_json, field_json);
            }
            else
            {
                snprintf(request, sizeof request,
                         "{\"op\":\"subscribe\"%s%s%s,\"features\":[%s]}",
                         session_id ? ",\"session_id\":\"" : "",
                         session_id ? session_id : "", session_id ? "\"" : "",
                         feature_json);
            }
        }
        stream_command = 1;
        goto send_request;
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

send_request:
    descriptor = connect_to(socket_path);
    if (descriptor < 0) return 3;

    if (!write_all(descriptor, request, strlen(request)) || !write_all(descriptor, "\n", 1))
    {
        fprintf(stderr, "nextdev: could not send request\n");
        close(descriptor);
        return 3;
    }
    (void)shutdown(descriptor, SHUT_WR);

    if (stream_command)
    {
        /* Event connections stay open until the server closes them. Print each
         * line as it arrives so the device checklist can watch a live stream. */
        char line[DEVICE_PROTOCOL_LINE_MAX];
        size_t used = 0;
        int received = 0;

        for (;;)
        {
            ssize_t count = read(descriptor, line + used, sizeof line - 1 - used);

            if (count == 0) break;
            if (count < 0)
            {
                if (errno == EINTR) continue;
                break;
            }
            used += (size_t)count;
            line[used] = '\0';
            for (;;)
            {
                char* newline = memchr(line, '\n', used);

                if (newline == NULL) break;
                *newline = '\0';
                printf("%s\n", line);
                fflush(stdout);
                received = 1;
                used -= (size_t)(newline + 1 - line);
                memmove(line, newline + 1, used);
                line[used] = '\0';
            }
            if (used + 1 >= sizeof line) used = 0;
        }
        close(descriptor);
        return received ? 0 : 3;
    }

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
