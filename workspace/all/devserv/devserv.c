#include "device_backend.h"
#include "device_backend_display_audio.h"
#include "device_backend_leds_platform.h"
#include "device_backend_rumble.h"
#include "device_coop.h"
#include "device_gate.h"
#include "device_protocol.h"
#include "device_session.h"
#include "device_session_platform.h"
#include "device_status.h"
#include "device_status_platform.h"
#include "device_subscription.h"
#include "pak_manifest.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_SOCKET_PATH "/tmp/nextui-devserv.sock"
/* A request connection must deliver its line within the C1 window. */
#define REQUEST_TIMEOUT_MS 2000
/* A sleeping request connection gets the ack deadline plus this grace before
 * the server gives up on it (CONTRACT.md §13.1 item 2). */
#define REQUEST_SLEEP_GRACE_MS 500
/* Upper bound on the poll timeout: the status/feedback sampling tick. */
#define LOOP_TICK_MS 250
#define MAX_CONNECTIONS 40

/* ping/hello advertise these; every one is implemented by this build. */
#define SERVER_FEATURES 0x3Fu /* events, sleep, status, hud, overlay, battery */

typedef enum
{
    CONNECTION_FREE = 0,
    CONNECTION_REQUEST,    /* one line in, one line out, close */
    CONNECTION_EVENT,      /* subscription; one subscribe line, then events */
    CONNECTION_SLEEP_WAIT  /* sleep.request waiting for the handshake */
} ConnectionKind;

typedef struct
{
    ConnectionKind kind;
    int fd;
    unsigned long long opened_ms;
    unsigned long long deadline_ms;
    size_t in_length;
    char in[DEVICE_PROTOCOL_LINE_MAX];
    int have_write;
    size_t write_length;
    size_t write_offset;
    char write_line[DEVICE_PROTOCOL_LINE_MAX + 2];
    int close_after_write;
    int have_tail;
    char tail_line[DEVICE_PROTOCOL_LINE_MAX];
    int subscription; /* index into subscriptions[] for CONNECTION_EVENT */
    int sleep_waiting; /* CONNECTION_SLEEP_WAIT owns the pending handshake */
} Connection;

static volatile sig_atomic_t running = 1;
static DeviceSession session_state;
static const DeviceSessionOps* session_ops;
static DeviceCoop coop;
static const DeviceBackend* backend;
static const DeviceStatusOps* status_ops;
static char grants_path_state[DEVICE_GRANT_PATH_MAX];
static DeviceStatusCache status_cache;
static DeviceStatusSnapshot latest_status;
static unsigned latest_have;
static unsigned long long section_sampled_at[7];
static Connection connections[MAX_CONNECTIONS];
static DeviceSubscription subscriptions[DEVICE_PROTOCOL_SUBSCRIBE_MAX];
static int sleep_resolved_by_deadline;

/* Feedback change detection. One baseline per HUD kind, kept in the event's own
 * units so a comparison is exact. */
typedef struct
{
    int available;
    int value;
    int min;
    int max;
    int mute;
    int locked;
    char sink[DEVICE_STATUS_SINK_MAX];
} FeedbackSample;

static FeedbackSample feedback_last[DEVICE_COOP_HUD_KINDS];
static int feedback_have[DEVICE_COOP_HUD_KINDS];
static int feedback_pending[DEVICE_COOP_HUD_KINDS];
static FeedbackSample feedback_pending_value[DEVICE_COOP_HUD_KINDS];
static unsigned long long feedback_last_at[DEVICE_COOP_HUD_KINDS];

static void handle_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static unsigned long long now_ms(void)
{
    return device_session_monotonic_ms();
}

static void set_text(char* out, size_t out_size, const char* text)
{
    if (out != NULL && out_size > 0) snprintf(out, out_size, "%s", text != NULL ? text : "");
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
            "Backend: led.control through the existing NextUI LED path,\n"
            "display.* and audio.* through the existing msettings path, and\n"
            "input.rumble through the existing PLAT_setRumble path; every other\n"
            "hardware capability reports unavailable.\n"
            "Sessions: session-start/stop/status restore only fields the service saw\n"
            "change; app-action records a session-scoped home/overlay request and\n"
            "system-feedback reports who draws a system key's visual feedback;\n"
            "neither accepts a PID; the launch parent performs the bounded TERM/KILL.\n"
            "Cooperation protocol 1: hello/ping negotiation, event subscriptions on\n"
            "a persistent connection (subscribe), the sleep prepare/ack/inhibit\n"
            "handshake, overlay hosting, HUD policy and the status provider are\n"
            "served from one poll loop. Request connections keep the one line in,\n"
            "one line out contract.\n"
            "Policy: installed apps may use every implemented capability without a\n"
            "grant; --require-grants enables the legacy grant/identity gate.\n",
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
    if (fcntl(listener, F_SETFL, fcntl(listener, F_GETFL, 0) | O_NONBLOCK) != 0)
    {
        fprintf(stderr, "devserv: nonblocking listener: %s\n", strerror(errno));
        close(listener);
        unlink(path);
        return -1;
    }
    return listener;
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
                            size_t app_size, char* pak_out, size_t pak_size,
                            int* overlay_host)
{
    char resolved[PATH_MAX];
    PakManifest manifest;
    const char* base;

    if (app_out == NULL || app_size == 0 || pak_out == NULL || pak_size == 0) return -1;
    app_out[0] = '\0';
    pak_out[0] = '\0';
    if (overlay_host != NULL) *overlay_host = 0;

    if (pak != NULL && pak[0] != '\0')
    {
        const char* canonical = pak;

        if (realpath(pak, resolved) != NULL) canonical = resolved;
        if (strlen(canonical) >= pak_size) return -1;
        snprintf(pak_out, pak_size, "%s", canonical);

        if (pak_manifest_load(pak_out, &manifest) == PAK_MANIFEST_OK)
        {
            if (overlay_host != NULL)
                *overlay_host =
                    pak_manifest_has_capability(&manifest, PAK_CAPABILITY_OVERLAY) != 0;
            if (manifest.id[0] != '\0')
            {
                if (strlen(manifest.id) >= app_size) return -1;
                snprintf(app_out, app_size, "%s", manifest.id);
            }
        }
        if (app_out[0] == '\0')
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

/* ------------------------------------------------------------------------- */
/* Status provider                                                            */
/* ------------------------------------------------------------------------- */

static void copy_sections(DeviceStatusSnapshot* destination, const DeviceStatusSnapshot* source,
                          unsigned sections)
{
    if ((sections & DEVICE_STATUS_SECTION_BATTERY) != 0) destination->battery = source->battery;
    if ((sections & DEVICE_STATUS_SECTION_WIFI) != 0) destination->wifi = source->wifi;
    if ((sections & DEVICE_STATUS_SECTION_BLUETOOTH) != 0)
        destination->bluetooth = source->bluetooth;
    if ((sections & DEVICE_STATUS_SECTION_VOLUME) != 0) destination->volume = source->volume;
    if ((sections & DEVICE_STATUS_SECTION_DISPLAY) != 0) destination->display = source->display;
    if ((sections & DEVICE_STATUS_SECTION_THEME) != 0) destination->theme = source->theme;
    if ((sections & DEVICE_STATUS_SECTION_CLOCK) != 0) destination->clock = source->clock;
}

static void mark_section_times(unsigned sections, unsigned long long now)
{
    int bit;

    for (bit = 0; bit < 7; bit++)
        if ((sections & (1u << bit)) != 0) section_sampled_at[bit] = now;
}

/* Read `sections` now and merge them into the latest snapshot. The cache still
 * applies its TTLs on request paths; the sampler forces the read because the
 * cadence itself is the TTL here. */
static void sample_status(unsigned sections, unsigned long long now)
{
    DeviceStatusSnapshot fresh;

    if (sections == 0) return;
    sections &= DEVICE_STATUS_SECTION_ALL;
    device_status_read(&status_cache, status_ops, sections, now, 1, &fresh);
    copy_sections(&latest_status, &fresh, sections);
    latest_have |= sections;
    mark_section_times(sections, now);
}

/* Desktop/no-backend rule: a section the platform cannot read is reported
 * available:false with a detail, never as a fabricated value. */
static const DeviceStatusBattery* current_battery(unsigned long long now)
{
    if ((latest_have & DEVICE_STATUS_SECTION_BATTERY) == 0)
        sample_status(DEVICE_STATUS_SECTION_BATTERY, now);
    return &latest_status.battery;
}

static int effective_overlay_open(void)
{
    return coop.active && coop.overlay_open;
}

/* ------------------------------------------------------------------------- */
/* Connection plumbing                                                        */
/* ------------------------------------------------------------------------- */

static void close_connection(Connection* connection);

static void conn_queue(Connection* connection, const char* line)
{
    size_t length = strlen(line);

    if (length > DEVICE_PROTOCOL_LINE_MAX) length = DEVICE_PROTOCOL_LINE_MAX;
    memcpy(connection->write_line, line, length);
    connection->write_line[length] = '\n';
    connection->write_length = length + 1;
    connection->write_offset = 0;
    connection->have_write = 1;
}

/* Fills write_line with the next pending line. Returns 1 when one was queued,
 * 0 when the connection is drained (and closes it when it is done). */
static int conn_next_line(Connection* connection)
{
    if (connection->have_tail)
    {
        conn_queue(connection, connection->tail_line);
        connection->have_tail = 0;
        connection->close_after_write = 1;
        return 1;
    }

    if (connection->kind == CONNECTION_EVENT)
    {
        DeviceSubscription* subscription = &subscriptions[connection->subscription];
        const DeviceSubscriptionEvent* event = device_subscription_peek(subscription);

        if (event != NULL)
        {
            conn_queue(connection, event->line);
            device_subscription_pop(subscription);
            subscription->last_send_ms = now_ms();
            return 1;
        }
        if (subscription->overflowed == 1)
        {
            char overflow[DEVICE_PROTOCOL_LINE_MAX];
            unsigned seq = device_subscription_next_seq(subscription);

            subscription->overflowed = 2;
            if (device_protocol_write_overflow_event(
                    seq, now_ms(), subscription->dropped,
                    subscription->has_session ? subscription->session_id : "", overflow,
                    sizeof overflow))
            {
                conn_queue(connection, overflow);
                connection->close_after_write = 1;
                return 1;
            }
        }
        if (subscription->close_after_flush)
        {
            close_connection(connection);
            return 0;
        }
    }

    if (connection->close_after_write)
    {
        close_connection(connection);
        return 0;
    }
    return 0;
}

static void conn_flush(Connection* connection)
{
    if (connection->kind == CONNECTION_FREE) return;

    for (;;)
    {
        ssize_t written;

        if (!connection->have_write && !conn_next_line(connection)) return;

        written = write(connection->fd, connection->write_line + connection->write_offset,
                        connection->write_length - connection->write_offset);
        if (written < 0)
        {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            close_connection(connection);
            return;
        }
        if (written == 0) return;
        connection->write_offset += (size_t)written;
        if (connection->write_offset < connection->write_length) return;
        connection->have_write = 0;
        connection->write_length = 0;
        connection->write_offset = 0;
    }
}

static void close_connection(Connection* connection)
{
    if (connection->kind == CONNECTION_FREE) return;
    if (connection->kind == CONNECTION_EVENT) {
        int slot = connection->subscription;
        if (slot >= 0 && slot < DEVICE_PROTOCOL_SUBSCRIBE_MAX) {
            if (coop.phase == DEVICE_COOP_SLEEP_PREPARING && (coop.counted & (1u << slot)) != 0)
            {
                coop.counted &= ~(1u << slot);
                if (coop.counted == 0 && coop.decision == DEVICE_COOP_DECISION_NONE)
                    device_coop_sleep_deadline(&coop, now_ms());
            }
            device_subscription_detach(&subscriptions[slot]);
        }
    }
    if (connection->fd >= 0) close(connection->fd);
    memset(connection, 0, sizeof *connection);
    connection->fd = -1;
}

static Connection* allocate_connection(int descriptor)
{
    int index;

    for (index = 0; index < MAX_CONNECTIONS; index++)
    {
        Connection* connection = &connections[index];
        if (connection->kind == CONNECTION_FREE)
        {
            memset(connection, 0, sizeof *connection);
            connection->kind = CONNECTION_REQUEST;
            connection->fd = descriptor;
            connection->opened_ms = now_ms();
            connection->deadline_ms = connection->opened_ms + REQUEST_TIMEOUT_MS;
            connection->subscription = -1;
            return connection;
        }
    }
    return NULL;
}

static void respond_error(Connection* connection, const char* error, const char* detail)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];

    if (!device_protocol_write_error_response(error, detail, response, sizeof response))
        snprintf(response, sizeof response, "{\"ok\":false,\"error\":\"bad_request\"}");
    connection->kind = CONNECTION_REQUEST;
    connection->close_after_write = 1;
    conn_queue(connection, response);
    conn_flush(connection);
}

static void respond(Connection* connection, const char* response)
{
    connection->close_after_write = 1;
    conn_queue(connection, response);
    conn_flush(connection);
}

/* ------------------------------------------------------------------------- */
/* Event fan-out                                                              */
/* ------------------------------------------------------------------------- */

static int sleep_participates(const DeviceSubscription* subscription)
{
    if ((subscription->families & DEVICE_EVENT_FAMILY_SLEEP) == 0) return 0;
    if (device_session_is_active(&session_state))
        return subscription->has_session && strcmp(subscription->session_id, coop.session_id) == 0;
    return !subscription->has_session;
}

static void subscription_send(DeviceSubscription* subscription, const char* line,
                              DeviceEventPriority priority)
{
    (void)device_subscription_push(subscription, line, priority);
}

static void emit_sleep_prepare(DeviceSubscription* subscription, int counted)
{
    char line[DEVICE_PROTOCOL_LINE_MAX];
    unsigned seq = device_subscription_next_seq(subscription);
    unsigned long long now = now_ms();
    unsigned long long deadline_at = coop.sleep_deadline_at_ms;
    int remaining = 0;
    const DeviceStatusBattery* battery = current_battery(now);

    if (deadline_at > now) remaining = (int)(deadline_at - now);
    if (!device_protocol_write_sleep_prepare_event(
            seq, now, coop.active ? coop.session_id : "", coop.sleep_id, coop.sleep_reason,
            remaining, deadline_at, battery->available ? battery : NULL, line, sizeof line))
        return;
    subscription_send(subscription, line, DEVICE_EVENT_PRIORITY_PROTECTED);
    if (counted) coop.counted |= 1u << (unsigned)(subscription - subscriptions);
}

static void fanout_sleep_prepare(int count_new)
{
    int index;

    for (index = 0; index < DEVICE_PROTOCOL_SUBSCRIBE_MAX; index++)
    {
        DeviceSubscription* subscription = &subscriptions[index];

        if (!subscription->used) continue;
        if (!sleep_participates(subscription)) continue;
        emit_sleep_prepare(subscription, count_new);
    }
}

static void fanout_wake_done(const DeviceCoopSleepCycle* cycle)
{
    int index;
    unsigned long long now = now_ms();
    const DeviceStatusBattery* battery = current_battery(now);

    for (index = 0; index < DEVICE_PROTOCOL_SUBSCRIBE_MAX; index++)
    {
        DeviceSubscription* subscription = &subscriptions[index];
        char line[DEVICE_PROTOCOL_LINE_MAX];
        unsigned seq;

        if (!subscription->used) continue;
        if ((subscription->families & DEVICE_EVENT_FAMILY_SLEEP) == 0) continue;
        if (subscription->has_session)
        {
            if (cycle->session_id[0] == '\0'
                || strcmp(subscription->session_id, cycle->session_id) != 0)
                continue;
        }
        else if (cycle->session_id[0] != '\0')
        {
            /* A system-scope subscription only receives cycles that started
             * with no session; a session's wake belongs to that session. */
            continue;
        }
        seq = device_subscription_next_seq(subscription);
        if (!device_protocol_write_wake_done_event(
                seq, now, cycle->session_id, cycle->sleep_id, cycle->slept_ms, cycle->reason,
                cycle->woke_by, cycle->result, battery->available ? battery : NULL, line,
                sizeof line))
            continue;
        subscription_send(subscription, line, DEVICE_EVENT_PRIORITY_PROTECTED);
    }
}

static void fanout_session_ended(const char* session_id, const char* reason)
{
    int index;

    for (index = 0; index < DEVICE_PROTOCOL_SUBSCRIBE_MAX; index++)
    {
        DeviceSubscription* subscription = &subscriptions[index];
        char line[DEVICE_PROTOCOL_LINE_MAX];
        unsigned seq;

        if (!subscription->used || !subscription->has_session) continue;
        if (strcmp(subscription->session_id, session_id) != 0) continue;
        seq = device_subscription_next_seq(subscription);
        if (device_protocol_write_session_ended_event(seq, now_ms(), session_id, reason, line,
                                                      sizeof line))
            subscription_send(subscription, line, DEVICE_EVENT_PRIORITY_PROTECTED);
        /* Nothing session-scoped may follow it. */
        subscription->families = 0;
        subscription->close_after_flush = 1;
    }
}

static void emit_status_snapshot(DeviceSubscription* subscription, int initial)
{
    char line[DEVICE_PROTOCOL_LINE_MAX];
    unsigned seq = device_subscription_next_seq(subscription);
    unsigned long long now = now_ms();
    unsigned sections = subscription->status_sections;

    if (!device_protocol_write_status_snapshot_event(
            seq, now, device_session_is_active(&session_state) ? coop.session_id : "", initial,
            &latest_status, sections, effective_overlay_open(), line, sizeof line))
        return;
    subscription_send(subscription, line, DEVICE_EVENT_PRIORITY_DROPPABLE);
    copy_sections(&subscription->status, &latest_status, sections);
    subscription->have_status = 1;
    subscription->pending_sections = 0;
    subscription->last_status_ms = now;
    subscription->last_snapshot_ms = now;
}

static void fanout_feedback(int kind, const FeedbackSample* sample, const char* source)
{
    int index;
    unsigned long long now = now_ms();

    for (index = 0; index < DEVICE_PROTOCOL_SUBSCRIBE_MAX; index++)
    {
        DeviceSubscription* subscription = &subscriptions[index];
        char line[DEVICE_PROTOCOL_LINE_MAX];
        unsigned seq;

        if (!subscription->used) continue;
        if ((subscription->families & DEVICE_EVENT_FAMILY_HUD) == 0) continue;
        if (!subscription->has_session
            || strcmp(subscription->session_id, coop.session_id) != 0)
            continue;
        if (!device_coop_effective_hud(&coop, kind)) continue;
        seq = device_subscription_next_seq(subscription);
        if (!device_protocol_write_feedback_event(
                seq, now, coop.session_id, device_coop_hud_kind_name(kind), sample->value,
                sample->min, sample->max, sample->mute, sample->locked,
                effective_overlay_open(), sample->sink, source, line, sizeof line))
            continue;
        subscription_send(subscription, line, DEVICE_EVENT_PRIORITY_DROPPABLE);
    }
}

static void fanout_overlay_home(void)
{
    int index;
    unsigned long long now = now_ms();

    for (index = 0; index < DEVICE_PROTOCOL_SUBSCRIBE_MAX; index++)
    {
        DeviceSubscription* subscription = &subscriptions[index];
        char line[DEVICE_PROTOCOL_LINE_MAX];
        unsigned seq;

        if (!subscription->used) continue;
        if ((subscription->families & DEVICE_EVENT_FAMILY_OVERLAY) == 0) continue;
        if (!subscription->has_session
            || strcmp(subscription->session_id, coop.session_id) != 0)
            continue;
        seq = device_subscription_next_seq(subscription);
        if (!device_protocol_write_overlay_home_event(seq, now, coop.session_id, "home", line,
                                                      sizeof line))
            continue;
        subscription_send(subscription, line, DEVICE_EVENT_PRIORITY_DROPPABLE);
    }
}

/* ------------------------------------------------------------------------- */
/* Feedback sampling                                                          */
/* ------------------------------------------------------------------------- */

static FeedbackSample feedback_sample(int kind)
{
    FeedbackSample sample;

    memset(&sample, 0, sizeof sample);
    if (kind == DEVICE_COOP_HUD_VOLUME || kind == DEVICE_COOP_HUD_MUTE)
    {
        if (!latest_status.volume.available) return sample;
        sample.available = 1;
        sample.min = latest_status.volume.min;
        sample.max = latest_status.volume.max;
        sample.mute = latest_status.volume.mute;
        sample.locked = latest_status.volume.locked;
        set_text(sample.sink, sizeof sample.sink, latest_status.volume.sink);
        sample.value = kind == DEVICE_COOP_HUD_MUTE ? (latest_status.volume.mute ? 1 : 0)
                                                    : latest_status.volume.level;
        if (kind == DEVICE_COOP_HUD_MUTE)
        {
            sample.min = 0;
            sample.max = 1;
        }
        return sample;
    }
    if (!latest_status.display.available) return sample;
    sample.available = 1;
    if (kind == DEVICE_COOP_HUD_BRIGHTNESS)
    {
        sample.value = latest_status.display.brightness;
        sample.min = latest_status.display.brightness_min;
        sample.max = latest_status.display.brightness_max;
        sample.locked = latest_status.display.brightness_locked;
    }
    else
    {
        sample.value = latest_status.display.colortemp;
        sample.min = latest_status.display.colortemp_min;
        sample.max = latest_status.display.colortemp_max;
        sample.locked = latest_status.display.colortemp_locked;
    }
    return sample;
}

static int feedback_equal(const FeedbackSample* a, const FeedbackSample* b)
{
    return a->value == b->value && a->min == b->min && a->max == b->max
           && a->mute == b->mute && a->locked == b->locked;
}

static void feedback_deliver(int kind, const FeedbackSample* sample, const char* source)
{
    fanout_feedback(kind, sample, source);
    feedback_last[kind] = *sample;
    feedback_have[kind] = 1;
    feedback_last_at[kind] = now_ms();
}

/* Called after a volume/display sample: detect changes and honour the per-kind
 * coalescing floor, with a trailing event when a burst ends. */
static void feedback_consider(unsigned long long now)
{
    int kind;

    for (kind = 0; kind < DEVICE_COOP_HUD_KINDS; kind++)
    {
        FeedbackSample sample = feedback_sample(kind);

        if (!sample.available) continue;
        if (!feedback_have[kind])
        {
            feedback_last[kind] = sample;
            feedback_have[kind] = 1;
            continue;
        }
        if (!feedback_equal(&sample, &feedback_last[kind]))
        {
            feedback_pending[kind] = 1;
            feedback_pending_value[kind] = sample;
        }
        if (!feedback_pending[kind]) continue;
        if (now - feedback_last_at[kind] < DEVICE_PROTOCOL_FEEDBACK_MIN_INTERVAL_MS) continue;
        feedback_deliver(kind, &feedback_pending_value[kind], "change");
        feedback_pending[kind] = 0;
    }
}

/* ------------------------------------------------------------------------- */
/* Session lifecycle                                                         */
/* ------------------------------------------------------------------------- */

static void note_session_started(void)
{
    char id[32];

    snprintf(id, sizeof id, "%016llx", session_state.id);
    device_coop_session_start(&coop, id, session_state.overlay_host, now_ms());
}

/* A session that ends during a pending handshake can no longer be asked.
 * Resolve the attempt instead of waiting for a closed subscription, then let
 * the deadline path answer the caller (the liveness guarantee). */
static void resolve_attempt_after_session_end(void)
{
    if (coop.phase != DEVICE_COOP_SLEEP_PREPARING) return;
    if (coop.decision != DEVICE_COOP_DECISION_NONE) return;
    coop.counted = 0;
    coop.acked = 0;
    device_coop_sleep_deadline(&coop, now_ms());
    sleep_resolved_by_deadline = 1;
}

/* ------------------------------------------------------------------------- */
/* Sleep handshake                                                           */
/* ------------------------------------------------------------------------- */

static Connection* find_sleep_wait(void)
{
    int index;

    for (index = 0; index < MAX_CONNECTIONS; index++)
        if (connections[index].kind == CONNECTION_SLEEP_WAIT && connections[index].sleep_waiting)
            return &connections[index];
    return NULL;
}

static const char* decision_name(DeviceCoopDecision decision)
{
    return decision == DEVICE_COOP_DECISION_DEFER ? "defer" : "proceed";
}

static void build_sleep_decision(DeviceSleepDecisionView* view, const char* detail)
{
    memset(view, 0, sizeof *view);
    view->decision = decision_name(coop.decision);
    set_text(view->sleep_id, sizeof view->sleep_id, coop.sleep_id);
    view->subscribers = (int)__builtin_popcount(coop.counted);
    view->acks = (int)__builtin_popcount(coop.acked & coop.counted);
    view->detail = detail;
    if (coop.decision == DEVICE_COOP_DECISION_DEFER && coop.inhibit_prepare_active)
    {
        view->inhibit = 1;
        set_text(view->inhibit_reason, sizeof view->inhibit_reason,
                 coop.inhibit_prepare_reason);
        set_text(view->inhibit_detail, sizeof view->inhibit_detail,
                 coop.inhibit_prepare_detail);
        view->inhibit_until_ms = coop.inhibit_prepare_until_ms;
        view->defer_until_ms = coop.inhibit_prepare_until_ms;
    }
}

static void resolve_sleep_wait(void)
{
    Connection* connection;
    char response[DEVICE_PROTOCOL_LINE_MAX];
    DeviceSleepDecisionView view;
    const char* detail;

    if (coop.phase == DEVICE_COOP_SLEEP_PREPARING && coop.decision == DEVICE_COOP_DECISION_NONE)
        return;

    connection = find_sleep_wait();
    if (connection == NULL) return;

    if (coop.decision == DEVICE_COOP_DECISION_DEFER)
        detail = "deferred by the application";
    else if (sleep_resolved_by_deadline)
        detail = "ack deadline expired";
    else if (__builtin_popcount(coop.counted) == 0)
        detail = "no subscribers";
    else
        detail = "all subscribers ready";
    build_sleep_decision(&view, detail);
    connection->sleep_waiting = 0;
    connection->kind = CONNECTION_REQUEST;
    if (!device_protocol_write_sleep_request_response(&view, response, sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
    sleep_resolved_by_deadline = 0;
}

/* ------------------------------------------------------------------------- */
/* Request handling                                                          */
/* ------------------------------------------------------------------------- */

static const char* validate_session_scope(const char* session_id, int required, int* present)
{
    int has_id = session_id != NULL && session_id[0] != '\0';

    if (present != NULL) *present = has_id;
    if (!coop.active) return "not_active";
    if (!has_id) return required ? "session_stale" : NULL;
    if (strcmp(session_id, coop.session_id) != 0) return "session_stale";
    return NULL;
}

static void handle_subscribe(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    unsigned families;
    unsigned sections;
    int min_interval;
    int slot;
    int present;
    int index;

    if (message->session_id[0] != '\0')
    {
        if (!coop.active || strcmp(message->session_id, coop.session_id) != 0)
        {
            respond_error(connection, "session_stale",
                          "session id does not match the active session");
            return;
        }
        present = 1;
    }
    else
    {
        present = 0;
    }

    for (slot = 0; slot < DEVICE_PROTOCOL_SUBSCRIBE_MAX; slot++)
        if (!subscriptions[slot].used) break;
    if (slot == DEVICE_PROTOCOL_SUBSCRIBE_MAX)
    {
        respond_error(connection, "too_many_subscriptions",
                      "the subscription limit is already reached");
        return;
    }

    families = DEVICE_EVENT_FAMILY_ALL;
    if (message->has_features)
    {
        families = 0;
        for (index = 0; index < message->feature_count; index++)
            families |= device_protocol_family_for_name(message->features[index]);
    }
    if (!present) families &= ~(DEVICE_EVENT_FAMILY_HUD | DEVICE_EVENT_FAMILY_OVERLAY);
    if (present && !coop.overlay_host_declared)
        families &= ~DEVICE_EVENT_FAMILY_OVERLAY;

    sections = DEVICE_STATUS_SECTION_ALL;
    if (message->has_fields)
    {
        sections = 0;
        for (index = 0; index < message->field_count; index++)
            sections |= device_status_section_for_name(message->fields[index]);
    }

    min_interval = DEVICE_PROTOCOL_STATUS_MIN_INTERVAL_MS;
    if (message->has_min_interval_ms)
    {
        min_interval = message->min_interval_ms;
        if (min_interval < 250 || min_interval > 5000)
        {
            respond_error(connection, "bad_request",
                          "status.min_interval_ms must be 250..5000");
            return;
        }
    }

    (void)device_subscription_attach(&subscriptions[slot], connection->fd, present,
                                     present ? coop.session_id : "", families, sections,
                                     min_interval);
    connection->kind = CONNECTION_EVENT;
    connection->subscription = slot;
    subscriptions[slot].last_send_ms = now_ms();
    subscriptions[slot].last_status_ms = now_ms();
    subscriptions[slot].last_snapshot_ms = now_ms();
    (void)device_protocol_write_subscription_ready(
        now_ms(), families, present ? coop.session_id : "",
        present && device_coop_is_hosting(&coop), response, sizeof response);
    conn_queue(connection, response);

    if ((families & DEVICE_EVENT_FAMILY_STATUS) != 0) emit_status_snapshot(&subscriptions[slot], 1);

    /* §5.4: a subscription that arrives while the system waits for acks gets
     * the same sleep.prepare with the remaining deadline. */
    if (coop.phase == DEVICE_COOP_SLEEP_PREPARING
        && coop.decision == DEVICE_COOP_DECISION_NONE && sleep_participates(&subscriptions[slot]))
    {
        emit_sleep_prepare(&subscriptions[slot], 1);
    }
    conn_flush(connection);
}

static void handle_sleep_request(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    DeviceCoopSleepResult result;
    DeviceSleepDecisionView view;
    int was_sleeping;
    int timeout;
    int automatic;
    unsigned long long now = now_ms();

    if (!device_protocol_sleep_reason_is_valid(message->reason))
    {
        respond_error(connection, "bad_request",
                      "reason must be user, idle, low_battery or lid");
        return;
    }
    if (message->initiator[0] == '\0')
    {
        respond_error(connection, "bad_request", "sleep.request needs an initiator");
        return;
    }
    timeout = message->has_timeout_ms ? message->timeout_ms : DEVICE_PROTOCOL_SLEEP_ACK_MS;
    if (timeout < 0 || timeout > 5000)
    {
        respond_error(connection, "bad_request", "timeout_ms must be 0..5000");
        return;
    }
    if (timeout == 0) timeout = DEVICE_PROTOCOL_SLEEP_ACK_MS;
    automatic = strcmp(message->reason, "idle") == 0
                || strcmp(message->reason, "low_battery") == 0;

    was_sleeping = coop.phase == DEVICE_COOP_SLEEP_SLEEPING;
    sleep_resolved_by_deadline = 0;
    {
        int subscribers = 0;
        int index;

        for (index = 0; index < DEVICE_PROTOCOL_SUBSCRIBE_MAX; index++)
            if (subscriptions[index].used && sleep_participates(&subscriptions[index]))
                subscribers++;
        result = device_coop_sleep_request(&coop, message->reason, message->initiator, timeout,
                                           automatic, subscribers, now);
    }
    if (result.error != NULL)
    {
        char detail[DEVICE_SESSION_DETAIL_MAX];
        set_text(detail, sizeof detail, result.detail);
        respond_error(connection, result.error, detail);
        return;
    }

    /* A caller that never reported its resume is finalized as unknown by this
     * request (§6.2); the matching wake.done goes out before the new cycle. */
    if (was_sleeping && coop.last.valid)
    {
        DeviceCoopSleepCycle cycle = coop.last;
        fanout_wake_done(&cycle);
    }

    /* Automatic initiation suppressed by the pre-inhibit lease: never proceed. */
    if (result.sleep_id[0] == '\0')
    {
        memset(&view, 0, sizeof view);
        view.decision = "defer";
        view.detail = result.detail != NULL ? result.detail : "suppressed by the application";
        view.inhibit = coop.inhibit_automatic_active;
        set_text(view.inhibit_reason, sizeof view.inhibit_reason,
                 coop.inhibit_automatic_reason);
        view.inhibit_until_ms = coop.inhibit_automatic_until_ms;
        view.defer_until_ms = coop.inhibit_automatic_until_ms;
        if (!device_protocol_write_sleep_request_response(&view, response, sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
        respond(connection, response);
        return;
    }

    /* Count the subscriptions that are about to receive the prepare, then send
     * it. Counting first makes a zero-subscriber attempt proceed immediately
     * and keeps the response's subscriber count honest. */
    {
        int index;
        for (index = 0; index < DEVICE_PROTOCOL_SUBSCRIBE_MAX; index++)
        {
            int participates = subscriptions[index].used && sleep_participates(&subscriptions[index]);
            if (participates) coop.counted |= 1u << index;
        }
    }

    if (coop.counted == 0)
    {
        /* device_coop_sleep_request treated subscribers=0 as proceed already. */
        build_sleep_decision(&view, "no subscribers");
        if (!device_protocol_write_sleep_request_response(&view, response, sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
        respond(connection, response);
        return;
    }

    fanout_sleep_prepare(0);

    if (coop.decision != DEVICE_COOP_DECISION_NONE)
    {
        build_sleep_decision(&view, "all subscribers ready");
        if (!device_protocol_write_sleep_request_response(&view, response, sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
        respond(connection, response);
        return;
    }

    connection->kind = CONNECTION_SLEEP_WAIT;
    connection->sleep_waiting = 1;
    connection->deadline_ms = coop.sleep_deadline_at_ms + REQUEST_SLEEP_GRACE_MS;
}

static void handle_status_get(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    DeviceStatusSnapshot snapshot;
    unsigned sections = DEVICE_STATUS_SECTION_ALL;
    int index;
    unsigned long long now = now_ms();

    if (message->has_fields)
    {
        sections = 0;
        for (index = 0; index < message->field_count; index++)
            sections |= device_status_section_for_name(message->fields[index]);
    }
    if (message->session_id[0] != '\0')
    {
        const char* error = validate_session_scope(message->session_id, 0, NULL);
        if (error != NULL)
        {
            respond_error(connection, error, "session id does not match the active session");
            return;
        }
    }
    device_status_read(&status_cache, status_ops, sections, now, 1, &snapshot);
    copy_sections(&latest_status, &snapshot, sections);
    latest_have |= sections;
    mark_section_times(sections, now);
    if (!device_protocol_write_status_response_sections(
            &snapshot, sections, effective_overlay_open(), now, response, sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
}

static void handle_battery_status(Connection* connection)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    DeviceStatusBattery battery;
    unsigned long long now = now_ms();

    sample_status(DEVICE_STATUS_SECTION_BATTERY, now);
    battery = latest_status.battery;
    if (!device_protocol_write_battery_response(&battery, now, response, sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
}

static void handle_sleep_ack(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    DeviceCoopDecision decision = DEVICE_COOP_DECISION_NONE;
    const char* error = NULL;
    unsigned long long now = now_ms();
    int index;
    int matched = 0;
    int present;

    if (!device_protocol_sleep_id_is_valid(message->sleep_id))
    {
        respond_error(connection, "bad_request", "sleep.ack needs a 16-digit hex sleep_id");
        return;
    }
    if (message->session_id[0] != '\0')
    {
        if (!coop.active || strcmp(message->session_id, coop.session_id) != 0)
        {
            respond_error(connection, "session_stale",
                          "session id does not match the active session");
            return;
        }
        present = 1;
    }
    else if (coop.active)
    {
        respond_error(connection, "session_stale",
                      "sleep.ack needs session_id while a session is active");
        return;
    }
    else
    {
        present = 0;
    }

    for (index = 0; index < DEVICE_PROTOCOL_SUBSCRIBE_MAX; index++)
    {
        DeviceSubscription* subscription = &subscriptions[index];
        const char* slot_error;
        DeviceCoopDecision slot_decision = DEVICE_COOP_DECISION_NONE;

        if ((coop.counted & (1u << index)) == 0) continue;
        if (present == subscription->has_session)
        {
            if (present && strcmp(subscription->session_id, coop.session_id) != 0) continue;
        }
        else
        {
            continue;
        }
        matched = 1;
        slot_error = device_coop_sleep_ack(&coop, (unsigned)index, message->sleep_id, now,
                                          &slot_decision);
        if (error == NULL && slot_error != NULL) error = slot_error;
        if (slot_decision != DEVICE_COOP_DECISION_NONE) decision = slot_decision;
        if (coop.phase != DEVICE_COOP_SLEEP_PREPARING) break;
    }

    if (error != NULL)
    {
        respond_error(connection, error, "sleep_id does not match the pending attempt");
        return;
    }
    if (decision == DEVICE_COOP_DECISION_NONE) decision = DEVICE_COOP_DECISION_DEFER;
    (void)matched;
    if (!device_protocol_write_sleep_ack_response(
            message->sleep_id, decision_name(decision),
            decision == DEVICE_COOP_DECISION_PROCEED ? "last subscriber"
                                                     : "waiting for more subscribers",
            response, sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
    resolve_sleep_wait();
}

static void handle_sleep_inhibit(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    const char* error;
    unsigned long long expires = 0;
    int scope_prepare;
    char detail[DEVICE_SESSION_DETAIL_MAX];
    unsigned long long now = now_ms();

    error = validate_session_scope(message->session_id, 1, NULL);
    if (error != NULL)
    {
        respond_error(connection, error, "sleep.inhibit is session-scoped");
        return;
    }
    set_text(detail, sizeof detail, message->detail);
    scope_prepare = message->sleep_id[0] != '\0';
    if (scope_prepare && !device_protocol_sleep_id_is_valid(message->sleep_id))
    {
        respond_error(connection, "bad_request", "invalid sleep_id");
        return;
    }
    error = device_coop_sleep_inhibit(&coop, scope_prepare ? message->sleep_id : NULL,
                                      message->reason, message->has_timeout_ms
                                                              ? message->timeout_ms
                                                              : 0,
                                      detail, now, &expires);
    if (error != NULL)
    {
        respond_error(connection, error,
                      scope_prepare ? "sleep_id does not match the pending attempt"
                                    : "timeout_ms must be 1000..300000");
        return;
    }
    if (scope_prepare)
    {
        if (!device_protocol_write_sleep_inhibit_response(
                "prepare", message->sleep_id, "defer", 1, expires, message->reason, response,
                sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
        respond(connection, response);
        resolve_sleep_wait();
        return;
    }
    if (!device_protocol_write_sleep_inhibit_response("automatic", NULL, "suppressed", 1,
                                                      expires, message->reason, response,
                                                      sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
}

static void handle_sleep_release(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    const char* error;
    int released;
    int prepare_scope = 0;
    int automatic_scope = 0;
    const char* scope = "none";

    error = validate_session_scope(message->session_id, 1, NULL);
    if (error != NULL)
    {
        respond_error(connection, error, "sleep.release is session-scoped");
        return;
    }
    if (message->sleep_id[0] != '\0' && !device_protocol_sleep_id_is_valid(message->sleep_id))
    {
        respond_error(connection, "bad_request", "invalid sleep_id");
        return;
    }
    released = device_coop_sleep_release(&coop,
                                         message->sleep_id[0] != '\0' ? message->sleep_id : NULL,
                                         &prepare_scope, &automatic_scope);
    if (prepare_scope) scope = "prepare";
    else if (automatic_scope) scope = "automatic";
    if (!device_protocol_write_sleep_release_response(released, scope, response,
                                                      sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
}

static void handle_sleep_finished(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    DeviceCoopSleepCycle cycle;
    const char* error;
    unsigned long long now = now_ms();

    if (!device_protocol_sleep_id_is_valid(message->sleep_id))
    {
        respond_error(connection, "bad_request", "sleep.finished needs a valid sleep_id");
        return;
    }
    if (!device_protocol_sleep_result_is_valid(message->result))
    {
        respond_error(connection, "bad_request", "result must be ok or failed");
        return;
    }
    if (!device_protocol_woke_by_is_valid(message->woke_by))
    {
        respond_error(connection, "bad_request", "woke_by must be power, lid, timer or unknown");
        return;
    }
    memset(&cycle, 0, sizeof cycle);
    error = device_coop_sleep_finished(&coop, message->sleep_id, message->result,
                                       message->woke_by, now, &cycle);
    if (error != NULL)
    {
        respond_error(connection, error, "sleep_id does not match the sleeping attempt");
        return;
    }
    fanout_wake_done(&cycle);
    if (!device_protocol_write_sleep_finished_response(message->sleep_id, cycle.slept_ms,
                                                       response, sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
}

static void handle_sleep_status(Connection* connection)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    DeviceSleepStatusView view;

    device_coop_sleep_status(&coop, now_ms(), &view);
    if (!device_protocol_write_sleep_status_response(&view, now_ms(), response,
                                                     sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
}

static void handle_hud_policy(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    DeviceHudPolicyView policy;
    DeviceHudPolicyView effective;
    const char* error;
    int kinds[DEVICE_COOP_HUD_KINDS];
    int kind_count = 0;
    int index;
    int mode = -1;

    error = validate_session_scope(message->session_id, 1, NULL);
    if (error != NULL)
    {
        respond_error(connection, error, "hud.policy is session-scoped");
        return;
    }
    if (message->kind_count > 0)
    {
        for (index = 0; index < message->kind_count; index++)
        {
            int kind = device_coop_hud_kind_for_name(message->kinds[index]);
            if (kind < 0)
            {
                respond_error(connection, "bad_request", "unknown HUD kind");
                return;
            }
            kinds[kind_count++] = kind;
        }
    }
    if (kind_count == 0)
    {
        for (index = 0; index < DEVICE_COOP_HUD_KINDS; index++) kinds[kind_count++] = index;
    }
    if (message->has_mode)
    {
        if (strcmp(message->mode, "app") == 0) mode = 1;
        else if (strcmp(message->mode, "system") == 0) mode = 0;
        else
        {
            respond_error(connection, "bad_request", "mode must be app or system");
            return;
        }
        for (index = 0; index < kind_count; index++)
            device_coop_set_hud_policy(&coop, kinds[index], mode);
    }

    policy.volume = device_coop_get_hud_policy(&coop, DEVICE_COOP_HUD_VOLUME);
    policy.brightness = device_coop_get_hud_policy(&coop, DEVICE_COOP_HUD_BRIGHTNESS);
    policy.colortemp = device_coop_get_hud_policy(&coop, DEVICE_COOP_HUD_COLORTEMP);
    policy.mute = device_coop_get_hud_policy(&coop, DEVICE_COOP_HUD_MUTE);
    effective.volume = device_coop_effective_hud(&coop, DEVICE_COOP_HUD_VOLUME);
    effective.brightness = device_coop_effective_hud(&coop, DEVICE_COOP_HUD_BRIGHTNESS);
    effective.colortemp = device_coop_effective_hud(&coop, DEVICE_COOP_HUD_COLORTEMP);
    effective.mute = device_coop_effective_hud(&coop, DEVICE_COOP_HUD_MUTE);
    if (!device_protocol_write_hud_policy_response(coop.session_id, effective_overlay_open(),
                                                   device_coop_is_hosting(&coop), &policy,
                                                   &effective, response, sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
}

static void handle_overlay_state(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    const char* error;

    error = validate_session_scope(message->session_id, 1, NULL);
    if (error != NULL)
    {
        respond_error(connection, error, "overlay.state is session-scoped");
        return;
    }
    if (!message->has_open)
    {
        respond_error(connection, "bad_request", "overlay.state needs a boolean open");
        return;
    }
    if (!coop.overlay_host_declared)
    {
        respond_error(connection, "not_adapted",
                      "the session did not declare the overlay capability");
        return;
    }
    if (message->open && !device_coop_is_hosting(&coop))
    {
        respond_error(connection, "not_hosting", "the session currently yields the overlay");
        return;
    }
    device_coop_set_overlay_open(&coop, message->open);
    if (!device_protocol_write_overlay_state_response(coop.session_id,
                                                       device_coop_is_hosting(&coop),
                                                       coop.overlay_open, response,
                                                       sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
}

static void handle_overlay_host(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    const char* error;
    const char* effective_hud;
    int app_kinds = 0;
    int index;

    error = validate_session_scope(message->session_id, 1, NULL);
    if (error != NULL)
    {
        respond_error(connection, error, "overlay.host is session-scoped");
        return;
    }
    if (!message->has_host)
    {
        respond_error(connection, "bad_request", "overlay.host needs a boolean host");
        return;
    }
    if (message->host && !coop.overlay_host_declared)
    {
        respond_error(connection, "not_adapted",
                      "the session did not declare the overlay capability");
        return;
    }
    (void)device_coop_set_overlay_host(&coop, message->host, now_ms());

    for (index = 0; index < DEVICE_COOP_HUD_KINDS; index++)
        if (device_coop_effective_hud(&coop, index)) app_kinds++;
    effective_hud = app_kinds == DEVICE_COOP_HUD_KINDS ? "app"
                    : app_kinds == 0                  ? "system"
                                                      : "mixed";
    if (!device_protocol_write_overlay_host_response(
            coop.session_id, device_coop_is_hosting(&coop), coop.overlay_host_declared,
            coop.host_changed_at_ms, coop.overlay_open, effective_hud, response,
            sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
}

static void handle_hello(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];
    unsigned features = SERVER_FEATURES;
    DeviceStatusBattery battery;
    char app[DEVICE_SESSION_APP_MAX];
    char pak[DEVICE_SESSION_PAK_MAX];
    int matches_pak = 1;
    int index;
    unsigned long long now = now_ms();

    if (message->has_protocol && message->protocol < DEVICE_PROTOCOL_GENERATION)
    {
        if (!device_protocol_write_protocol_unsupported_response(response, sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"protocol_unsupported\"}");
        respond(connection, response);
        return;
    }

    if (message->pak[0] != '\0')
    {
        int identity = session_identity(message->app, message->pak, app, sizeof app, pak,
                                        sizeof pak, NULL);
        if (identity < 0)
        {
            respond_error(connection, "bad_request", "invalid session identity");
            return;
        }
        if (identity == 0)
        {
            respond_error(connection, "bad_request", "app does not match the pak manifest");
            return;
        }
        if (device_session_is_active(&session_state) && strcmp(pak, session_state.pak) != 0)
            matches_pak = 0;
    }

    if (message->has_features)
    {
        static const struct
        {
            const char* name;
            unsigned bit;
        } FEATURES[] = {{"events", 1u << 0}, {"sleep", 1u << 1}, {"status", 1u << 2},
                        {"hud", 1u << 3},    {"overlay", 1u << 4}, {"battery", 1u << 5}};
        features = 0;
        for (index = 0; index < message->feature_count; index++)
            for (size_t feature = 0; feature < sizeof FEATURES / sizeof FEATURES[0]; feature++)
                if (strcmp(message->features[index], FEATURES[feature].name) == 0)
                    features |= FEATURES[feature].bit;
        features &= SERVER_FEATURES;
    }

    sample_status(DEVICE_STATUS_SECTION_BATTERY, now);
    battery = latest_status.battery;

    if (!device_protocol_write_hello_response(
            &session_state, device_coop_is_hosting(&coop), coop.overlay_host_declared,
            effective_overlay_open(), matches_pak, (features & (1u << 5)) != 0, &battery,
            DEVICE_PROTOCOL_GENERATION, features, response, sizeof response))
        snprintf(response, sizeof response,
                 "{\"ok\":false,\"error\":\"backend_error\",\"detail\":\"response too large\"}");
    respond(connection, response);
}

static void handle_request(Connection* connection, const DeviceRequestMessage* message)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];

    switch (message->op)
    {
    case DEVICE_OP_PING:
        if (!device_protocol_write_ping_response_protocol(response, sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\"}");
        respond(connection, response);
        return;

    case DEVICE_OP_HELLO:
        handle_hello(connection, message);
        return;

    case DEVICE_OP_STATUS:
    {
        DeviceStatusResult result;
        if (device_gate_status(message->app, message->pak, grants_path_state, backend, &result)
                != DEVICE_GATE_OK
            && result.error == DEVICE_GATE_BAD_REQUEST)
        {
            respond_error(connection, "bad_request", result.detail);
            return;
        }
        if (!device_protocol_write_status_response(&result, response, sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\","
                     "\"detail\":\"response too large\"}");
        respond(connection, response);
        return;
    }

    case DEVICE_OP_CALL:
    {
        DeviceCallRequest request;
        DeviceCallResult result;

        device_call_request_init(&request);
        snprintf(request.app, sizeof request.app, "%s", message->app);
        snprintf(request.pak, sizeof request.pak, "%s", message->pak);
        snprintf(request.capability, sizeof request.capability, "%s", message->capability);
        snprintf(request.action, sizeof request.action, "%s", message->action);
        request.args = message->has_args ? message->args : NULL;

        (void)device_gate_call(&request, grants_path_state, backend, &result);
        if (result.error == DEVICE_GATE_OK)
            device_session_note_capability(&session_state, message->capability, message->action);
        if (!device_protocol_write_call_response(&result, response, sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\","
                     "\"detail\":\"response too large\"}");
        respond(connection, response);
        return;
    }

    case DEVICE_OP_SESSION_START:
    {
        char app[DEVICE_SESSION_APP_MAX];
        char pak[DEVICE_SESSION_PAK_MAX];
        DeviceSessionResult result;
        const char* error = NULL;
        const char* detail = NULL;
        int identity;
        int overlay_host = 0;

        identity = session_identity(message->app, message->pak, app, sizeof app, pak, sizeof pak,
                                    &overlay_host);
        if (identity < 0)
        {
            respond_error(connection, "bad_request", "invalid session identity");
            return;
        }
        if (identity == 0)
        {
            respond_error(connection, "bad_request", "app does not match the pak manifest");
            return;
        }

        result = device_session_begin(&session_state, session_ops, app, pak);
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
        else
        {
            /* The manifest is authoritative: an app cannot claim the overlay
             * capability through the session request. */
            device_session_note_overlay_host(&session_state, overlay_host);
            note_session_started();
            /* A new session resets theme/clock-sensitive rows (§8.3). */
            sample_status(DEVICE_STATUS_SECTION_THEME | DEVICE_STATUS_SECTION_CLOCK, now_ms());
        }
        if (!device_protocol_write_session_response(
                &session_state, device_coop_is_hosting(&coop), coop.overlay_host_declared,
                effective_overlay_open(), result, 0, error, detail, response,
                sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\","
                     "\"detail\":\"response too large\"}");
        respond(connection, response);
        return;
    }

    case DEVICE_OP_SESSION_STOP:
    {
        unsigned long long expected = 0;
        DeviceSessionResult result;
        int was_active = device_session_is_active(&session_state);
        int cleaned;
        const char* error = NULL;
        const char* detail = NULL;
        char ended_id[32];

        if (!parse_session_id(message->session_id, &expected))
        {
            respond_error(connection, "bad_request", "invalid session id");
            return;
        }
        snprintf(ended_id, sizeof ended_id, "%016llx", session_state.id);

        result = device_session_end(&session_state, session_ops, expected);
        if (result == DEVICE_SESSION_STALE)
        {
            error = "session_stale";
            detail = "session id does not match the active session";
        }
        else if (result == DEVICE_SESSION_PARTIAL)
        {
            error = "restore_partial";
            detail = session_state.detail;
        }
        else if (result == DEVICE_SESSION_BAD_ARGUMENT)
        {
            error = "bad_request";
            detail = "invalid session";
        }
        /* cleaned reports that this stop actually tore a session down; a
         * stale stop leaves the newer session running and cleaned nothing. */
        cleaned = was_active && result != DEVICE_SESSION_STALE;
        if (cleaned)
        {
            if (coop.active) fanout_session_ended(coop.session_id, "stopped");
            resolve_attempt_after_session_end();
            device_coop_session_end(&coop);
        }
        if (!device_protocol_write_session_response(
                &session_state, device_coop_is_hosting(&coop), coop.overlay_host_declared,
                effective_overlay_open(), result, cleaned, error, detail, response,
                sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\","
                     "\"detail\":\"response too large\"}");
        respond(connection, response);
        return;
    }

    case DEVICE_OP_SESSION_STATUS:
        if (!device_protocol_write_session_response(
                &session_state, device_coop_is_hosting(&coop), coop.overlay_host_declared,
                effective_overlay_open(), DEVICE_SESSION_OK, 0, NULL, NULL, response,
                sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\","
                     "\"detail\":\"response too large\"}");
        respond(connection, response);
        return;

    case DEVICE_OP_APP_ACTION:
    {
        unsigned long long expected = 0;
        DeviceSessionResult result;
        const char* error = NULL;
        const char* detail = NULL;
        int handled_by_app = 0;
        int handled_by_system = 0;

        if (message->session_id[0] != '\0'
            && !parse_session_id(message->session_id, &expected))
        {
            respond_error(connection, "bad_request", "invalid session id");
            return;
        }

        result = device_session_validate_event(&session_state, expected, message->at_ms);
        if (result == DEVICE_SESSION_STALE)
        {
            error = "session_stale";
            detail = "session id or event time does not match";
        }
        else if (result == DEVICE_SESSION_NOT_ACTIVE)
        {
            error = "not_active";
            detail = "no application session is active";
        }
        else if (strcmp(message->action, "overlay") == 0)
        {
            if (device_coop_is_hosting(&coop))
            {
                /* The adapted app consumes HOME in-process. Do not translate
                 * it into an exit and do not draw anything from this process.
                 * Emit the signal for apps that do not read KEY_HOMEPAGE. */
                result = DEVICE_SESSION_APP_HANDLED;
                handled_by_app = 1;
                detail = "the application hosts the overlay";
                fanout_overlay_home();
            }
            else
            {
                /* The system fallback draws the overlay: an unadapted app, or
                 * an adapted one that currently yields hosting (§7.9.2). */
                result = DEVICE_SESSION_OK;
                handled_by_system = 1;
                detail = "the system draws the fallback overlay";
            }
        }
        else
        {
            result = device_session_request_exit(&session_state, expected, message->action,
                                                message->at_ms, 1500);
            if (result == DEVICE_SESSION_STALE)
            {
                error = "session_stale";
                detail = session_state.exit_detail[0] != '\0'
                             ? session_state.exit_detail
                             : "session id or event time does not match";
            }
            else if (result == DEVICE_SESSION_NOT_ACTIVE)
            {
                error = "not_active";
                detail = "no application session is active";
            }
            else if (result == DEVICE_SESSION_BAD_ARGUMENT)
            {
                error = "bad_request";
                detail = "app-action needs home or overlay";
            }
        }

        if (!device_protocol_write_app_action_response(&session_state, result, message->action,
                                                       handled_by_app, handled_by_system, error,
                                                       detail, response, sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\","
                     "\"detail\":\"response too large\"}");
        respond(connection, response);
        return;
    }

    case DEVICE_OP_SYSTEM_FEEDBACK:
    {
        int handled_by_app = 0;
        int handled_by_system = 0;
        int kind;
        const char* detail;

        if (strcmp(message->action, "volume") != 0 && strcmp(message->action, "brightness") != 0
            && strcmp(message->action, "colortemp") != 0
            && strcmp(message->action, "mute") != 0)
        {
            respond_error(connection, "bad_request", "unknown feedback action");
            return;
        }
        kind = device_coop_hud_kind_for_name(message->action);

        if (coop.active)
        {
            if (device_coop_effective_hud(&coop, kind))
            {
                handled_by_app = 1;
                detail = "the application owns its own indicator";
                /* keymon applies the change and then asks; the 250 ms sampler
                 * may still hold the previous value, so read the fresh one
                 * before building the event. This also resets the coalescing
                 * baseline, so the sampler does not repeat the same change as
                 * source:"change". */
                sample_status(DEVICE_STATUS_SECTION_VOLUME | DEVICE_STATUS_SECTION_DISPLAY,
                              now_ms());
                {
                    FeedbackSample sample = feedback_sample(kind);
                    if (sample.available)
                    {
                        fanout_feedback(kind, &sample, "key");
                        feedback_last[kind] = sample;
                        feedback_have[kind] = 1;
                        feedback_last_at[kind] = now_ms();
                        feedback_pending[kind] = 0;
                    }
                }
            }
            else
            {
                handled_by_system = 1;
                detail = "the system draws the fallback indicator";
            }
        }
        else
        {
            detail = "no application session is active";
        }

        if (!device_protocol_write_app_action_response(&session_state, DEVICE_SESSION_OK,
                                                       message->action, handled_by_app,
                                                       handled_by_system, NULL, detail, response,
                                                       sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\","
                     "\"detail\":\"response too large\"}");
        respond(connection, response);
        return;
    }

    case DEVICE_OP_APP_EXIT_RESULT:
    {
        unsigned long long expected = 0;
        DeviceSessionResult result;
        const char* error = NULL;
        const char* detail = NULL;
        int forced = message->has_forced ? message->forced : 0;

        if (message->session_id[0] != '\0' && !parse_session_id(message->session_id, &expected))
        {
            respond_error(connection, "bad_request", "invalid session id");
            return;
        }

        result = device_session_note_exit_outcome(&session_state, expected, forced,
                                                  "launch parent reported the outcome");
        if (result == DEVICE_SESSION_STALE)
        {
            error = "session_stale";
            detail = "exit result does not belong to the active session";
        }
        else if (result == DEVICE_SESSION_NOT_ACTIVE)
        {
            error = "not_active";
            detail = "no application session is active";
        }
        else if (result == DEVICE_SESSION_BAD_ARGUMENT)
        {
            error = "bad_request";
            detail = "no exit request is pending for this session";
        }
        if (!device_protocol_write_session_response(
                &session_state, device_coop_is_hosting(&coop), coop.overlay_host_declared,
                effective_overlay_open(), result, 0, error, detail, response,
                sizeof response))
            snprintf(response, sizeof response,
                     "{\"ok\":false,\"error\":\"backend_error\","
                     "\"detail\":\"response too large\"}");
        respond(connection, response);
        return;
    }

    case DEVICE_OP_BATTERY_STATUS:
        handle_battery_status(connection);
        return;

    case DEVICE_OP_STATUS_GET:
        handle_status_get(connection, message);
        return;

    case DEVICE_OP_SLEEP_REQUEST:
        handle_sleep_request(connection, message);
        return;

    case DEVICE_OP_SLEEP_ACK:
        handle_sleep_ack(connection, message);
        return;

    case DEVICE_OP_SLEEP_INHIBIT:
        handle_sleep_inhibit(connection, message);
        return;

    case DEVICE_OP_SLEEP_RELEASE:
        handle_sleep_release(connection, message);
        return;

    case DEVICE_OP_SLEEP_FINISHED:
        handle_sleep_finished(connection, message);
        return;

    case DEVICE_OP_SLEEP_STATUS:
        handle_sleep_status(connection);
        return;

    case DEVICE_OP_HUD_POLICY:
        handle_hud_policy(connection, message);
        return;

    case DEVICE_OP_OVERLAY_STATE:
        handle_overlay_state(connection, message);
        return;

    case DEVICE_OP_OVERLAY_HOST:
        handle_overlay_host(connection, message);
        return;

    default:
        respond_error(connection, "bad_request", "unknown operation");
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* Line reader                                                               */
/* ------------------------------------------------------------------------- */

static void handle_line(Connection* connection, const char* line)
{
    DeviceRequestMessage message;

    if (line[0] == '\0')
    {
        respond_error(connection, "bad_request", "empty request");
        return;
    }
    if (!device_protocol_parse_message(line, &message))
    {
        respond_error(connection, "bad_request", "invalid JSON request");
        return;
    }
    if (message.op == DEVICE_OP_SUBSCRIBE)
    {
        handle_subscribe(connection, &message);
        return;
    }
    connection->kind = CONNECTION_REQUEST;
    handle_request(connection, &message);
}

static void event_violation(Connection* connection)
{
    char response[DEVICE_PROTOCOL_LINE_MAX];

    if (!device_protocol_write_error_response(
            "bad_request",
            connection->kind == CONNECTION_EVENT
                ? "an event connection accepts exactly one subscribe line"
                : "a request connection accepts exactly one request line",
            response, sizeof response))
        snprintf(response, sizeof response, "{\"ok\":false,\"error\":\"bad_request\"}");
    connection->have_tail = 1;
    set_text(connection->tail_line, sizeof connection->tail_line, response);
    connection->close_after_write = 1;
    conn_flush(connection);
}

static void conn_read(Connection* connection)
{
    char buffer[4096];
    ssize_t count;
    ssize_t index;

    count = read(connection->fd, buffer, sizeof buffer);
    if (count == 0)
    {
        /* A request that never finished its line is a protocol error, but the
         * peer is gone; close silently like C1 did. */
        close_connection(connection);
        return;
    }
    if (count < 0)
    {
        if (errno == EINTR) return;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        close_connection(connection);
        return;
    }

    for (index = 0; index < count; index++)
    {
        char ch = buffer[index];

        if (connection->kind == CONNECTION_REQUEST && connection->close_after_write)
            continue; /* already answered; the peer is only being closed */
        if (connection->kind == CONNECTION_EVENT || connection->kind == CONNECTION_SLEEP_WAIT)
        {
            /* The subscribe line was the only one allowed. Anything after it,
             * including a second line in the same read, is a protocol error. */
            event_violation(connection);
            return;
        }
        if (ch == '\r') continue;
        if (ch == '\n')
        {
            char line[DEVICE_PROTOCOL_LINE_MAX];
            size_t length = connection->in_length;

            memcpy(line, connection->in, length);
            line[length] = '\0';
            connection->in_length = 0;
            handle_line(connection, line);
            if (connection->kind == CONNECTION_FREE) return;
            continue;
        }
        if (connection->in_length + 1 >= sizeof connection->in)
        {
            respond_error(connection, "bad_request", "request line is too long");
            return;
        }
        connection->in[connection->in_length++] = ch;
    }
}

/* ------------------------------------------------------------------------- */
/* Timers                                                                    */
/* ------------------------------------------------------------------------- */

static unsigned due_sections(unsigned long long now)
{
    unsigned sections = 0;

    if (now - section_sampled_at[0] >= 5000) sections |= DEVICE_STATUS_SECTION_BATTERY;
    if (now - section_sampled_at[1] >= 2000) sections |= DEVICE_STATUS_SECTION_WIFI;
    if (now - section_sampled_at[2] >= 2000) sections |= DEVICE_STATUS_SECTION_BLUETOOTH;
    if (now - section_sampled_at[3] >= 250) sections |= DEVICE_STATUS_SECTION_VOLUME;
    if (now - section_sampled_at[4] >= 250) sections |= DEVICE_STATUS_SECTION_DISPLAY;
    if (now - section_sampled_at[5] >= 5000) sections |= DEVICE_STATUS_SECTION_THEME;
    if (now - section_sampled_at[6] >= 5000) sections |= DEVICE_STATUS_SECTION_CLOCK;
    return sections;
}

static void queue_status_patches(DeviceSubscription* subscription, unsigned sections,
                                 unsigned long long now)
{
    if (!subscription->have_status)
    {
        emit_status_snapshot(subscription, 0);
        return;
    }
    {
        unsigned changed = 0;
        int bit;

        for (bit = 0; bit < 7; bit++)
        {
            unsigned section = 1u << bit;
            if ((sections & section) == 0) continue;
            if ((subscription->status_sections & section) == 0) continue;
            if (!device_status_equal(&subscription->status, &latest_status, section))
                changed |= section;
        }
        if (changed != 0) subscription->pending_sections |= changed;
    }
    if (subscription->pending_sections != 0
        && now - subscription->last_status_ms >= (unsigned long long)subscription->min_interval_ms)
    {
        unsigned pending = subscription->pending_sections;
        char line[DEVICE_PROTOCOL_LINE_MAX];
        unsigned seq = device_subscription_next_seq(subscription);

        if (device_protocol_write_status_changed_event(
                seq, now, device_session_is_active(&session_state) ? coop.session_id : "",
                &latest_status, pending, effective_overlay_open(), line, sizeof line))
            subscription_send(subscription, line, DEVICE_EVENT_PRIORITY_DROPPABLE);
        copy_sections(&subscription->status, &latest_status, pending);
        subscription->pending_sections = 0;
        subscription->last_status_ms = now;
    }
}

static void run_timers(void)
{
    unsigned long long now = now_ms();
    unsigned due = due_sections(now);
    int index;

    device_coop_tick(&coop, now);

    if (due != 0)
    {
        unsigned sampled = due;
        sample_status(sampled, now);
        if ((sampled & (DEVICE_STATUS_SECTION_VOLUME | DEVICE_STATUS_SECTION_DISPLAY)) != 0)
            feedback_consider(now);
    }
    for (index = 0; index < DEVICE_COOP_HUD_KINDS; index++)
    {
        if (!feedback_pending[index]) continue;
        if (now - feedback_last_at[index] < DEVICE_PROTOCOL_FEEDBACK_MIN_INTERVAL_MS) continue;
        feedback_deliver(index, &feedback_pending_value[index], "change");
        feedback_pending[index] = 0;
    }

    if (coop.phase == DEVICE_COOP_SLEEP_PREPARING && coop.decision == DEVICE_COOP_DECISION_NONE
        && now >= coop.sleep_deadline_at_ms)
    {
        device_coop_sleep_deadline(&coop, now);
        sleep_resolved_by_deadline = 1;
    }
    resolve_sleep_wait();

    for (index = 0; index < DEVICE_PROTOCOL_SUBSCRIBE_MAX; index++)
    {
        DeviceSubscription* subscription = &subscriptions[index];
        Connection* owner = NULL;
        int connection_index;

        if (!subscription->used) continue;
        for (connection_index = 0; connection_index < MAX_CONNECTIONS; connection_index++)
            if (connections[connection_index].kind == CONNECTION_EVENT
                && connections[connection_index].subscription == index)
                owner = &connections[connection_index];
        if (owner == NULL) continue;

        if ((subscription->families & DEVICE_EVENT_FAMILY_STATUS) != 0)
        {
            queue_status_patches(subscription, subscription->status_sections, now);
            if (now - subscription->last_snapshot_ms >= DEVICE_PROTOCOL_STATUS_RESYNC_MS)
                emit_status_snapshot(subscription, 0);
        }
        if (device_subscription_pending(subscription) == 0 && !owner->have_write
            && now - subscription->last_send_ms >= DEVICE_PROTOCOL_HEARTBEAT_MS)
        {
            char line[DEVICE_PROTOCOL_LINE_MAX];
            unsigned seq = device_subscription_next_seq(subscription);

            if (device_protocol_write_heartbeat_event(seq, now, line, sizeof line))
            {
                subscription_send(subscription, line, DEVICE_EVENT_PRIORITY_DROPPABLE);
                subscription->last_send_ms = now;
            }
        }
        conn_flush(owner);
    }

    for (index = 0; index < MAX_CONNECTIONS; index++)
    {
        Connection* connection = &connections[index];

        if (connection->kind == CONNECTION_FREE) continue;
        if (connection->kind == CONNECTION_SLEEP_WAIT && now >= connection->deadline_ms)
        {
            /* The handshake should have resolved by now (its own deadline is
             * earlier). Fail closed for the caller instead of leaving it
             * blocked on a line that will never come. */
            if (coop.phase == DEVICE_COOP_SLEEP_PREPARING
                && coop.decision == DEVICE_COOP_DECISION_NONE)
            {
                device_coop_sleep_deadline(&coop, now);
                sleep_resolved_by_deadline = 1;
                resolve_sleep_wait();
            }
            else
            {
                resolve_sleep_wait();
            }
            if (connection->kind == CONNECTION_SLEEP_WAIT)
                respond_error(connection, "sleep_busy", "the handshake did not resolve in time");
            continue;
        }
        if (connection->kind == CONNECTION_REQUEST && now >= connection->deadline_ms
            && !connection->have_write)
        {
            respond_error(connection, "bad_request", "request timed out");
            continue;
        }
        conn_flush(connection);
    }
}

/* ------------------------------------------------------------------------- */
/* main                                                                      */
/* ------------------------------------------------------------------------- */

int main(int argc, char* argv[])
{
    char socket_path[DEVICE_GRANT_PATH_MAX];
    char grants_path[DEVICE_GRANT_PATH_MAX];
    const char* environment;
    struct sigaction action;
    int listener;
    int socket_overridden = 0;
    int require_grants = 0;
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
        else if (strcmp(argv[i], "--require-grants") == 0)
        {
            require_grants = 1;
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
        if (require_grants)
        {
            fprintf(stderr,
                    "devserv: --require-grants needs SHARED_USERDATA_PATH,\n"
                    "         NEXTUI_DEVICE_GRANTS, or --grants PATH.\n");
            return 2;
        }
        /* Default-allow does not read the legacy store; no path is required. */
    }

    snprintf(grants_path_state, sizeof grants_path_state, "%s", grants_path);
    /* Installed apps get implemented capabilities by default. The grant store
     * is legacy compatibility state, not a prerequisite for this path. */
    device_gate_set_default_allow(require_grants ? 0 : 1);

    /* The status provider never initializes msettings; it reports unavailable
     * until the desktop has created the shared settings segment. */
    status_ops = device_status_platform_ops();
    device_status_cache_init(&status_cache);
    device_status_snapshot_init(&latest_status);
    device_coop_init(&coop);
    for (i = 0; i < MAX_CONNECTIONS; i++) connections[i].fd = -1;
    for (i = 0; i < DEVICE_PROTOCOL_SUBSCRIBE_MAX; i++) device_subscription_init(&subscriptions[i]);
    sample_status(DEVICE_STATUS_SECTION_ALL, now_ms());

#ifdef NEXTUI_DEVSERV_BACKEND_NONE
    /* Host gate tests build this file without the LED stack. Production uses
     * the LED backend below; this keeps the C1 none-backend test meaningful. */
    backend = device_backend_none();
#else
    backend = device_backend_display_audio_compose(device_backend_leds());
    backend = device_backend_rumble_compose(backend);
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
    printf("devserv: protocol %d (%s)\n", DEVICE_PROTOCOL_GENERATION, DEVICE_PROTOCOL_CONTRACT);
    printf("devserv: grants %s\n",
           grants_path[0] != '\0' ? grants_path : "(none; legacy store unused)");
    printf("devserv: policy %s\n",
           device_gate_default_allow() ? "default-allow" : "legacy-grants");
    printf("devserv: backend %s\n", backend->name);
    printf("devserv: session %s\n", session_ops != NULL ? session_ops->name : "none");
    printf("devserv: status %s\n", status_ops != NULL ? status_ops->name : "none");
    fflush(stdout);

    while (running)
    {
        struct pollfd descriptors[MAX_CONNECTIONS + 1];
        int map[MAX_CONNECTIONS + 1];
        int count = 1;
        int timeout = LOOP_TICK_MS;
        unsigned long long now = now_ms();
        int index;
        int ready;

        descriptors[0].fd = listener;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        map[0] = -1;

        for (index = 0; index < MAX_CONNECTIONS; index++)
        {
            Connection* connection = &connections[index];

            if (connection->kind == CONNECTION_FREE) continue;
            descriptors[count].fd = connection->fd;
            descriptors[count].events = POLLIN;
            if (connection->have_write) descriptors[count].events |= POLLOUT;
            descriptors[count].revents = 0;
            map[count] = index;
            count++;
        }

        if (coop.phase == DEVICE_COOP_SLEEP_PREPARING && coop.decision == DEVICE_COOP_DECISION_NONE
            && coop.sleep_deadline_at_ms > now && coop.sleep_deadline_at_ms - now < (unsigned)timeout)
            timeout = (int)(coop.sleep_deadline_at_ms - now);
        for (index = 0; index < MAX_CONNECTIONS; index++)
        {
            Connection* connection = &connections[index];
            if (connection->kind == CONNECTION_SLEEP_WAIT && connection->deadline_ms > now
                && connection->deadline_ms - now < (unsigned)timeout)
                timeout = (int)(connection->deadline_ms - now);
        }
        if (timeout < 0) timeout = 0;

        ready = poll(descriptors, (nfds_t)count, timeout);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            fprintf(stderr, "devserv: poll: %s\n", strerror(errno));
            break;
        }

        if (ready > 0)
        {
            if ((descriptors[0].revents & POLLIN) != 0)
            {
                for (;;)
                {
                    int descriptor = accept(listener, NULL, NULL);
                    Connection* connection;

                    if (descriptor < 0)
                    {
                        if (errno == EINTR) continue;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        fprintf(stderr, "devserv: accept: %s\n", strerror(errno));
                        break;
                    }
                    if (fcntl(descriptor, F_SETFL, fcntl(descriptor, F_GETFL, 0) | O_NONBLOCK)
                        != 0)
                    {
                        close(descriptor);
                        continue;
                    }
                    connection = allocate_connection(descriptor);
                    if (connection == NULL)
                    {
                        close(descriptor);
                        continue;
                    }
                }
            }
            for (index = 1; index < count; index++)
            {
                Connection* connection;
                short events = descriptors[index].revents;

                if (events == 0) continue;
                if (map[index] < 0) continue;
                connection = &connections[map[index]];
                if (connection->kind == CONNECTION_FREE) continue;
                if ((events & POLLIN) != 0) conn_read(connection);
                if (connection->kind == CONNECTION_FREE) continue;
                if ((events & POLLOUT) != 0) conn_flush(connection);
                if (connection->kind == CONNECTION_FREE) continue;
                /* A pending response must not be dropped on a half-close: the
                 * peer may have shut down only its writing side. */
                if ((events & (POLLHUP | POLLERR)) != 0 && !connection->have_write)
                    close_connection(connection);
            }
        }

        run_timers();
    }

    for (i = 0; i < DEVICE_PROTOCOL_SUBSCRIBE_MAX; i++)
    {
        if (!subscriptions[i].used) continue;
        if (coop.active)
        {
            char line[DEVICE_PROTOCOL_LINE_MAX];
            unsigned seq = device_subscription_next_seq(&subscriptions[i]);

            if (device_protocol_write_session_ended_event(seq, now_ms(), coop.session_id,
                                                          "server_shutdown", line, sizeof line))
                subscription_send(&subscriptions[i], line, DEVICE_EVENT_PRIORITY_PROTECTED);
        }
        subscriptions[i].close_after_flush = 1;
    }
    for (i = 0; i < MAX_CONNECTIONS; i++)
    {
        Connection* connection = &connections[i];

        if (connection->kind == CONNECTION_FREE) continue;
        /* Best effort: the peer may be slow, so give each connection a short
         * bounded flush before the descriptor goes away. */
        {
            int attempts = 0;
            while (attempts < 20)
            {
                short revents = 0;
                struct pollfd descriptor;

                conn_flush(connection);
                if (connections[i].kind == CONNECTION_FREE) break;
                descriptor.fd = connections[i].fd;
                descriptor.events = POLLOUT;
                descriptor.revents = 0;
                if (poll(&descriptor, 1, 10) <= 0) break;
                revents = descriptor.revents;
                (void)revents;
                attempts++;
            }
        }
        close_connection(&connections[i]);
    }

    close(listener);
    unlink(socket_path);
    printf("devserv: stopped\n");
    fflush(stdout);
    return 0;
}
