#ifndef DEVICE_SESSION_H
#define DEVICE_SESSION_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Application device sessions (plan C3).
 *
 * This module is deliberately independent of SDL, msettings and the LED stack.
 * It owns only the snapshot/restore state machine and the session generation
 * token; the production adapter in device_session_platform.c reads and writes
 * the existing device seams, and devserv owns the single active session.
 *
 * A session snapshots the fields the service can actually observe. Fields that
 * the backend cannot read are marked available=0 and are never replaced with a
 * default. On teardown only fields whose mutation the service saw through a
 * successful gated call are written back. That is what keeps a normal Settings
 * run from being undone when it exits: Settings changes the device directly and
 * is not a service-observed app mutation.
 *
 * C3 does not implement C4 force-quit, PID watching, or app-private saves. A
 * B3 resume slot is a different contract and is never read or written here.
 */

#define DEVICE_SESSION_APP_MAX 128
#define DEVICE_SESSION_PAK_MAX 512
#define DEVICE_SESSION_DETAIL_MAX 160
#define DEVICE_SESSION_ID_MAX 24
#define DEVICE_SESSION_RESTORE_ATTEMPTS 2

typedef enum
{
    DEVICE_SESSION_BRIGHTNESS = 0,
    DEVICE_SESSION_COLORTEMP,
    DEVICE_SESSION_VOLUME,
    DEVICE_SESSION_MUTE,
    DEVICE_SESSION_AUDIO_SINK,
    DEVICE_SESSION_HAPTICS,
    DEVICE_SESSION_FIELD_COUNT
} DeviceSessionField;

typedef enum
{
    DEVICE_SESSION_OK = 0,
    DEVICE_SESSION_NOT_ACTIVE,
    DEVICE_SESSION_ALREADY_ACTIVE,
    DEVICE_SESSION_STALE,
    DEVICE_SESSION_PARTIAL,
    DEVICE_SESSION_BAD_ARGUMENT
} DeviceSessionResult;

typedef struct
{
    int available;   /* backend can read this field on this platform */
    int captured;    /* available and read() succeeded */
    int changed;     /* a successful gated call mutated it during the session */
    int restored;    /* write() and a verification read both succeeded */
    int skipped;     /* captured but intentionally not written */
    int value;       /* original value, meaningful only when captured */
    char detail[DEVICE_SESSION_DETAIL_MAX];
} DeviceSessionFieldState;

typedef enum
{
    DEVICE_SESSION_LED_IDLE = 0, /* no release attempted yet */
    DEVICE_SESSION_LED_UNAVAILABLE,
    DEVICE_SESSION_LED_RELEASED,
    DEVICE_SESSION_LED_FAILED
} DeviceSessionLedState;

typedef struct
{
    int active;
    unsigned long long id;      /* generation token for this session */
    unsigned long long last_id; /* most recent ended session id */
    char app[DEVICE_SESSION_APP_MAX];
    char pak[DEVICE_SESSION_PAK_MAX];
    long started_at;
    DeviceSessionFieldState fields[DEVICE_SESSION_FIELD_COUNT];
    int led_available;
    int led_used; /* a successful led.control call was observed */
    DeviceSessionLedState led_state;
    char led_detail[DEVICE_SESSION_DETAIL_MAX];
    DeviceSessionResult last_result;
    int restored_count;
    int failed_count;
    int skipped_count;
    char detail[DEVICE_SESSION_DETAIL_MAX];
} DeviceSession;

typedef struct DeviceSessionOps
{
    const char* name;
    void* context;
    int (*available)(void* context, DeviceSessionField field, char* detail, size_t detail_size);
    int (*read)(void* context, DeviceSessionField field, int* value, char* detail,
                size_t detail_size);
    int (*write)(void* context, DeviceSessionField field, int value, char* detail,
                 size_t detail_size);
    int (*led_available)(void* context, char* detail, size_t detail_size);
    int (*led_release)(void* context, char* detail, size_t detail_size);
} DeviceSessionOps;

const char* device_session_field_name(DeviceSessionField field);
const char* device_session_result_name(DeviceSessionResult result);
const char* device_session_led_state_name(DeviceSessionLedState state);

void device_session_init(DeviceSession* session);

/*
 * Capture a new session. A second begin while a session is active returns
 * DEVICE_SESSION_ALREADY_ACTIVE and leaves the first snapshot untouched.
 * Backend read failures are reported per field; begin itself still succeeds so
 * a session failure can never block an app launch.
 */
DeviceSessionResult device_session_begin(DeviceSession* session, const DeviceSessionOps* ops,
                                         const char* app, const char* pak);

/*
 * End the session identified by expected_id.
 *   expected_id == 0: end the active session, or perform orphan LED cleanup.
 *   expected_id != 0: only act when it matches the active/last session id.
 * A mismatch returns DEVICE_SESSION_STALE without reading, writing or releasing
 * anything, so a late stop cannot tear down a newer session.
 *
 * Only changed fields are written. Every write is verified by a read; a field
 * that cannot be restored is reported and does not stop the remaining fields.
 * The session becomes inactive even on a partial restore, so the next launch is
 * never blocked; the failed field details remain queryable in this struct.
 */
DeviceSessionResult device_session_end(DeviceSession* session, const DeviceSessionOps* ops,
                                       unsigned long long expected_id);

int device_session_is_active(const DeviceSession* session);

/*
 * Record a successful gated call. Only mutating capabilities are mapped:
 * display.brightness/set, display.colortemp/set, audio.volume/set,
 * audio.sink/set, and any successful led.control action. Unknown capabilities,
 * read-only get calls and failed calls change nothing.
 */
void device_session_note_capability(DeviceSession* session, const char* capability,
                                    const char* action);

#ifdef __cplusplus
}
#endif

#endif
