#include "device_backend.h"
#include "device_gate.h"
#include "device_protocol.h"

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
            "Backend: none in C1; every hardware capability reports unavailable.\n",
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

static void handle_connection(int descriptor, const char* grants_path,
                              const DeviceBackend* backend)
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
            if (!device_protocol_write_call_response(&result, response, sizeof response))
                snprintf(response, sizeof response,
                         "{\"ok\":false,\"error\":\"backend_error\","
                         "\"detail\":\"response too large\"}");
            break;
        }

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
    const DeviceBackend* backend = device_backend_none();
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
        handle_connection(connection, grants_path, backend);
        close(connection);
    }

    close(listener);
    unlink(socket_path);
    printf("devserv: stopped\n");
    fflush(stdout);
    return 0;
}
