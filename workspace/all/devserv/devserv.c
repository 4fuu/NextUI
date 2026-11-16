#include "device_backend.h"
#include "device_backend_display_audio.h"
#include "device_backend_leds_platform.h"
#include "device_gate.h"
#include "device_protocol.h"
#include "device_session.h"
#include "device_session_platform.h"
#include "pak_manifest.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_SOCKET_PATH "/tmp/nextui-devserv.sock"
#define REQUEST_TIMEOUT_SECONDS 2

static volatile sig_atomic_t running = 1;
static DeviceSession session_state;
static const DeviceSessionOps* session_ops;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static void usage(FILE* stream)
{
    fprintf(stream,
            "usage: devserv [--socket PATH] --grants PATH\n"
            "       devserv [--socket PATH]   (grants from $NEXTUI_DEVICE_GRANTS\n"
            "                                 or $SHARED_USERDATA_PATH/device_grants.json)\n"
            "\n"
            "Serves newline-delimited JSON device requests on a Unix socket.\n"
            "The socket defaults to $NEXTUI_DEVSERV_SOCKET or %s.\n"
            "Grants must be configured explicitly; there is no implicit /tmp store.\n"
            "Backend: led.control through the existing NextUI LED path; every other\n"
            "hardware capability reports unavailable.\n"
            "Sessions: session-start/stop/status restore only fields the service saw\n"
            "change; haptics is unavailable and LED teardown is delegated to C2.\n",
            DEFAULT_SOCKET_PATH);
}

static int path_join(char* out, size_t out_size, const char* directory, const char* name)
{
    int written = snprintf(out, out_size, "%s/%s", directory, name);
    return written >= 0 && (size_t)written < out_size;
}

/* Returns 0 when no persistent grant store is configured. */
static int resolve_grants_path(char* out, size_t out_size)
{
    const char* environment = getenv("NEXTUI_DEVICE_GRANTS");

    if (environment != NULL && environment[0] != '\0')
    {
        snprintf(out, out_size, "%s", environment);
        return 1;
    }

    environment = getenv("SHARED_USERDATA_PATH");
    if (environment != NULL && environment[0] != '\0'
        && path_join(out, out_size, environment, "device_grants.json"))
        return 1;

    out[0] = '\0';
    return 0;
}

static int socket_is_live(const char* path)
{
    struct sockaddr_un address;
    int descriptor;
    int connected;

    descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) return 0;

    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    connected = connect(descriptor, (struct sockaddr*)&address, sizeof address) == 0;
    close(descriptor);
    return connected;
}

static int create_listener(const char* path)
{
    struct sockaddr_un address;
    struct stat existing;
    int listener;

    if (strlen(path) >= sizeof address.sun_path)
    {
        fprintf(stderr, "devserv: socket path is too long: %s\n", path);
        return -1;
    }

    if (lstat(path, &existing) == 0)
    {
        if (!S_ISSOCK(existing.st_mode))
        {
            fprintf(stderr, "devserv: refusing to replace non-socket %s\n", path);
            return -1;
        }
        if (socket_is_live(path))
        {
            fprintf(stderr, "devserv: another instance is already listening on %s\n", path);
            return -1;
        }
        if (unlink(path) != 0)
        {
            fprintf(stderr, "devserv: could not remove stale socket %s: %s\n", path,
                    strerror(errno));
            return -1;
        }
    }
    else if (errno != ENOENT)
    {
        fprintf(stderr, "devserv: could not inspect %s: %s\n", path, strerror(errno));
        return -1;
    }

    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0)
    {
        fprintf(stderr, "devserv: socket: %s\n", strerror(errno));
        return -1;
    }

    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);

    if (bind(listener, (struct sockaddr*)&address, sizeof address) != 0)
    {
        fprintf(stderr, "devserv: bind %s: %s\n", path, strerror(errno));
        close(listener);
        return -1;
    }
    if (chmod(path, 0600) != 0)
    {
        fprintf(stderr, "devserv: chmod %s: %s\n", path, strerror(errno));
        close(listener);
        unlink(path);
        return -1;
    }
    if (listen(listener, 8) != 0)
    {
        fprintf(stderr, "devserv: listen: %s\n", strerror(errno));
        close(listener);
        unlink(path);
        return -1;
    }
    return listener;
}

static void set_timeout(int descriptor)
{
    struct timeval timeout;
    timeout.tv_sec = REQUEST_TIMEOUT_SECONDS;
    timeout.tv_usec = 0;
    (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    (void)setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
}

/* 1 = line, 0 = clean EOF with no data, -1 = read error, -2 = too long, -3 = timeout */
static int read_request_line(int descriptor, char* buffer, size_t size)
{
    size_t total = 0;

    while (total + 1 < size)
    {
        char ch;
        ssize_t count = read(descriptor, &ch, 1);

        if (count == 0) break;
        if (count < 0)
        {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -3;
            return -1;
        }
        if (ch == '\n')
        {
            buffer[total] = '\0';
            return 1;
        }
        if (ch == '\r') continue;
        buffer[total++] = ch;
    }

    buffer[total] = '\0';
    if (total + 1 >= size) return -2;
    return total > 0 ? 1 : 0;
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

static void send_response(int descriptor, const char* response)
{
    char line[DEVICE_PROTOCOL_LINE_MAX + 2];

    snprintf(line, sizeof line, "%s\n", response);
    (void)write_all(descriptor, line, strlen(line));
}

static void send_error(int descriptor, const char* error, const char* detail)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];

    if (!device_protocol_write_error_response(error, detail, response, sizeof response))
        snprintf(response, sizeof response, "{\"ok\":false,\"error\":\"bad_request\"}");
    send_response(descriptor, response);
}

static int parse_session_id(const char* text, unsigned long long* out)
{
    const char* cursor;
    char* end = NULL;
    unsigned long long value;

    if (out == NULL) return 0;
    if (text == NULL || text[0] == '\0')
    {
        *out = 0;
        return 1;
    }
    if (strlen(text) > 16) return 0;
    for (cursor = text; *cursor != '\0'; cursor++)
        if (!isxdigit((unsigned char)*cursor)) return 0;

    errno = 0;
    value = strtoull(text, &end, 16);
    if (errno != 0 || end == text || *end != '\0') return 0;
    *out = value;
    return 1;
}

/* Session identity is descriptive, not an authorization decision. Reuse the
 * same canonical path and manifest-id rules the gate uses so a session and a
 * grant describe the same installation. An identity that does not fit the
 * session fields is rejected, exactly like the gate, instead of truncated. */
static int session_identity(const char* claimed_app, const char* pak, char* app_out,
                            size_t app_size, char* pak_out, size_t pak_size)
{
    char resolved[DEVICE_GRANT_PATH_MAX];
    PakManifest manifest;
    const char* base;

    if (app_out == NULL || app_size == 0 || pak_out == NULL || pak_size == 0) return -1;
    app_out[0] = '\0';
    pak_out[0] = '\0';

    if (pak != NULL && pak[0] != '\0')
    {
        const char* canonical = pak;

        if (realpath(pak, resolved) != NULL) canonical = resolved;
        if (strlen(canonical) >= pak_size) return -1;
        snprintf(pak_out, pak_size, "%s", canonical);

        if (pak_manifest_load(pak_out, &manifest) == PAK_MANIFEST_OK && manifest.id[0] != '\0')
        {
            if (strlen(manifest.id) >= app_size) return -1;
            snprintf(app_out, app_size, "%s", manifest.id);
        }
        else
        {
            base = strrchr(pak_out, '/');
            if (base != NULL && strlen(base + 1) >= app_size) return -1;
            snprintf(app_out, app_size, "%s", base != NULL ? base + 1 : pak_out);
        }
    }

    if (claimed_app != NULL && claimed_app[0] != '\0')
    {
        if (app_out[0] == '\0')
            snprintf(app_out, app_size, "%s", claimed_app);
        else if (strcmp(app_out, claimed_app) != 0)
            return 0;
    }
    return 1;
}

static void session_shutdown(void)
{
    DeviceSessionResult result;

    if (session_ops == NULL || !device_session_is_active(&session_state)) return;
    result = device_session_end(&session_state, session_ops, 0);
    if (result == DEVICE_SESSION_PARTIAL)
        fprintf(stderr, "devserv: session restore partial: %s\n", session_state.detail);
}

static void handle_connection(int descriptor, const char* grants_path,
                              const DeviceBackend* backend, DeviceSession* session,
                              const DeviceSessionOps* ops)
{
    char line[DEVICE_PROTOCOL_LINE_MAX];
    char response[DEVICE_PROTOCOL_LINE_MAX];
    int status;

    set_timeout(descriptor);
    status = read_request_line(descriptor, line, sizeof line);
    if (status == -2)
    {
        send_error(descriptor, "bad_request", "request line is too long");
        return;
    }
    if (status == -3)
    {
        send_error(descriptor, "bad_request", "request timed out");
        return;
    }
    if (status != 1)
    {
        send_error(descriptor, "bad_request", "empty request");
        return;
    }

    {
        DeviceRequestMessage message;

        if (!device_protocol_parse_request(line, &message))
        {
            send_error(descriptor, "bad_request", "invalid JSON request");
            return;
        }

        switch (message.op)
        {
        case DEVICE_OP_PING:
            if (!device_protocol_write_ping_response(response, sizeof response))
                snprintf(response, sizeof response,
                         "{\"ok\":false,\"error\":\"backend_error\"}");
            break;

        case DEVICE_OP_STATUS:
        {
            DeviceStatusResult result;
            if (device_gate_status(message.app, message.pak, grants_path, backend, &result)
                    != DEVICE_GATE_OK
                && result.error == DEVICE_GATE_BAD_REQUEST)
            {
                send_error(descriptor, "bad_request", result.detail);
                return;
            }
            if (!device_protocol_write_status_response(&result, response, sizeof response))
                snprintf(response, sizeof response,
                         "{\"ok\":false,\"error\":\"backend_error\","
                         "\"detail\":\"response too large\"}");
            break;
        }

        case DEVICE_OP_CALL:
        {
            DeviceCallRequest request;
            DeviceCallResult result;

            device_call_request_init(&request);
            snprintf(request.app, sizeof request.app, "%s", message.app);
            snprintf(request.pak, sizeof request.pak, "%s", message.pak);
            snprintf(request.capability, sizeof request.capability, "%s", message.capability);
            snprintf(request.action, sizeof request.action, "%s", message.action);
            request.args = message.has_args ? message.args : NULL;

            (void)device_gate_call(&request, grants_path, backend, &result);
            if (result.error == DEVICE_GATE_OK)
                device_session_note_capability(session, message.capability, message.action);
            if (!device_protocol_write_call_response(&result, response, sizeof response))
                snprintf(response, sizeof response,
                         "{\"ok\":false,\"error\":\"backend_error\","
                         "\"detail\":\"response too large\"}");
            break;
        }

        case DEVICE_OP_SESSION_START:
        {
            char app[DEVICE_SESSION_APP_MAX];
            char pak[DEVICE_SESSION_PAK_MAX];
            DeviceSessionResult result;
            const char* error = NULL;
            const char* detail = NULL;
            int identity;

            identity = session_identity(message.app, message.pak, app, sizeof app, pak,
                                        sizeof pak);
            if (identity < 0)
            {
                send_error(descriptor, "bad_request", "invalid session identity");
                return;
            }
            if (identity == 0)
            {
                send_error(descriptor, "bad_request", "app does not match the pak manifest");
                return;
            }

            result = device_session_begin(session, ops, app, pak);
            if (result == DEVICE_SESSION_ALREADY_ACTIVE)
            {
                error = "session_active";
                detail = "a session is already active";
            }
            else if (result == DEVICE_SESSION_BAD_ARGUMENT)
            {
                error = "bad_request";
                detail = "invalid session arguments";
            }
            if (!device_protocol_write_session_response(session, result, 0, error, detail,
                                                        response, sizeof response))
                snprintf(response, sizeof response,
                         "{\"ok\":false,\"error\":\"backend_error\","
                         "\"detail\":\"response too large\"}");
            break;
        }

        case DEVICE_OP_SESSION_STOP:
        {
            unsigned long long expected = 0;
            DeviceSessionResult result;
            int was_active = device_session_is_active(session);
            int cleaned;
            const char* error = NULL;
            const char* detail = NULL;

            if (!parse_session_id(message.session_id, &expected))
            {
                send_error(descriptor, "bad_request", "invalid session id");
                return;
            }

            result = device_session_end(session, ops, expected);
            if (result == DEVICE_SESSION_STALE)
            {
                error = "session_stale";
                detail = "session id does not match the active session";
            }
            else if (result == DEVICE_SESSION_PARTIAL)
            {
                error = "restore_partial";
                detail = session->detail;
            }
            else if (result == DEVICE_SESSION_BAD_ARGUMENT)
            {
                error = "bad_request";
                detail = "invalid session";
            }
            /* cleaned reports that this stop actually tore a session down; a
             * stale stop leaves the newer session running and cleaned nothing. */
            cleaned = was_active && result != DEVICE_SESSION_STALE;
            if (!device_protocol_write_session_response(session, result, cleaned, error, detail,
                                                        response, sizeof response))
                snprintf(response, sizeof response,
                         "{\"ok\":false,\"error\":\"backend_error\","
                         "\"detail\":\"response too large\"}");
            break;
        }

        case DEVICE_OP_SESSION_STATUS:
            if (!device_protocol_write_session_response(session, DEVICE_SESSION_OK, 0, NULL, NULL,
                                                        response, sizeof response))
                snprintf(response, sizeof response,
                         "{\"ok\":false,\"error\":\"backend_error\","
                         "\"detail\":\"response too large\"}");
            break;

        default:
            send_error(descriptor, "bad_request", "unknown operation");
            return;
        }
    }

    send_response(descriptor, response);
}

int main(int argc, char* argv[])
{
    char socket_path[DEVICE_GRANT_PATH_MAX];
    char grants_path[DEVICE_GRANT_PATH_MAX];
    const DeviceBackend* backend;
    const char* environment;
    struct sigaction action;
    int listener;
    int socket_overridden = 0;
    int i;

    snprintf(socket_path, sizeof socket_path, "%s", DEFAULT_SOCKET_PATH);
    grants_path[0] = '\0';

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
        {
            if (argv[i + 1][0] == '\0' || strlen(argv[i + 1]) >= sizeof socket_path)
            {
                fprintf(stderr, "devserv: invalid socket path\n");
                return 2;
            }
            snprintf(socket_path, sizeof socket_path, "%s", argv[++i]);
            socket_overridden = 1;
        }
        else if (strcmp(argv[i], "--grants") == 0 && i + 1 < argc)
        {
            if (argv[i + 1][0] == '\0' || strlen(argv[i + 1]) >= sizeof grants_path)
            {
                fprintf(stderr, "devserv: invalid grant store path\n");
                return 2;
            }
            snprintf(grants_path, sizeof grants_path, "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            usage(stdout);
            return 0;
        }
        else
        {
            fprintf(stderr, "devserv: unknown argument: %s\n", argv[i]);
            usage(stderr);
            return 2;
        }
    }

    environment = getenv("NEXTUI_DEVSERV_SOCKET");
    if (!socket_overridden && environment != NULL && environment[0] != '\0')
        snprintf(socket_path, sizeof socket_path, "%s", environment);

    if (grants_path[0] == '\0' && !resolve_grants_path(grants_path, sizeof grants_path))
    {
        fprintf(stderr,
                "devserv: grants path is not configured; set SHARED_USERDATA_PATH,\n"
                "         NEXTUI_DEVICE_GRANTS, or pass --grants PATH.\n"
                "         Persistent grants are never stored in /tmp implicitly.\n");
        return 2;
    }

#ifdef NEXTUI_DEVSERV_BACKEND_NONE
    /* Host gate tests build this file without the LED stack. Production uses
     * the LED backend below; this keeps the C1 none-backend test meaningful. */
    backend = device_backend_none();
#else
    /* One DeviceBackend: display.* and audio.* come from the msettings
     * backend, every other capability (including led.control) from the
     * existing LED backend. */
    backend = device_backend_display_audio_compose(device_backend_leds());
    (void)atexit(device_backend_leds_shutdown);
#endif

    session_ops = device_session_platform_ops();
    device_session_init(&session_state);
    (void)atexit(session_shutdown);

    umask(0077);
    signal(SIGPIPE, SIG_IGN);
    memset(&action, 0, sizeof action);
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    listener = create_listener(socket_path);
    if (listener < 0) return 1;

    printf("devserv: listening on %s\n", socket_path);
    printf("devserv: grants %s\n", grants_path);
    printf("devserv: backend %s\n", backend->name);
    printf("devserv: session %s\n", session_ops != NULL ? session_ops->name : "none");
    fflush(stdout);

    while (running)
    {
        int connection = accept(listener, NULL, NULL);

        if (connection < 0)
        {
            if (errno == EINTR) continue;
            fprintf(stderr, "devserv: accept: %s\n", strerror(errno));
            break;
        }
        handle_connection(connection, grants_path, backend, &session_state, session_ops);
        close(connection);
    }

    close(listener);
    unlink(socket_path);
    printf("devserv: stopped\n");
    fflush(stdout);
    return 0;
}
