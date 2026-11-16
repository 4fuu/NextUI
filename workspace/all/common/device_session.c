#include "device_session.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void set_text(char* out, size_t out_size, const char* text)
{
    if (out != NULL && out_size > 0) snprintf(out, out_size, "%s", text != NULL ? text : "");
}

static const char* const FIELD_NAMES[DEVICE_SESSION_FIELD_COUNT] = {
    "brightness",
    "colortemp",
    "volume",
    "mute",
    "audio_sink",
    "haptics",
};

static const char* const RESULT_NAMES[] = {
    "ok",
    "not_active",
    "already_active",
    "stale",
    "partial",
    "bad_argument",
};

static const char* const LED_STATE_NAMES[] = {
    "idle",
    "unavailable",
    "released",
    "failed",
};

const char* device_session_field_name(DeviceSessionField field)
{
    if (field < 0 || field >= DEVICE_SESSION_FIELD_COUNT) return "unknown";
    return FIELD_NAMES[field];
}

const char* device_session_result_name(DeviceSessionResult result)
{
    if (result < DEVICE_SESSION_OK || result > DEVICE_SESSION_BAD_ARGUMENT) return "unknown";
    return RESULT_NAMES[result];
}

const char* device_session_led_state_name(DeviceSessionLedState state)
{
    if (state < DEVICE_SESSION_LED_IDLE || state > DEVICE_SESSION_LED_FAILED) return "unknown";
    return LED_STATE_NAMES[state];
}

void device_session_init(DeviceSession* session)
{
    if (session == NULL) return;
    memset(session, 0, sizeof *session);
    session->last_result = DEVICE_SESSION_OK;
    session->led_state = DEVICE_SESSION_LED_IDLE;
}

int device_session_is_active(const DeviceSession* session)
{
    return session != NULL && session->active;
}

static unsigned long long next_session_id(void)
{
    static unsigned long long counter;
    unsigned long long now = (unsigned long long)time(NULL);
    unsigned long long pid = (unsigned long long)getpid();
    unsigned long long sequence = ++counter;

    /* Generation token, not a capability: time + pid + counter keeps a stale
     * launch stop from matching a session created by a restarted service. */
    return (now * 1000003ULL) ^ (pid << 24) ^ (sequence & 0xffffULL);
}

static int field_is_volume_like(DeviceSessionField field)
{
    return field == DEVICE_SESSION_BRIGHTNESS || field == DEVICE_SESSION_COLORTEMP
           || field == DEVICE_SESSION_VOLUME;
}

static int release_led(DeviceSession* session, const DeviceSessionOps* ops)
{
    char detail[DEVICE_SESSION_DETAIL_MAX];

    if (!session->led_available || ops == NULL || ops->led_release == NULL)
    {
        session->led_state = DEVICE_SESSION_LED_UNAVAILABLE;
        set_text(session->led_detail, sizeof session->led_detail, "led lease is not available");
        return 0;
    }
    if (session->led_state == DEVICE_SESSION_LED_RELEASED) return 1;

    detail[0] = '\0';
    if (ops->led_release(ops->context, detail, sizeof detail))
    {
        session->led_state = DEVICE_SESSION_LED_RELEASED;
        set_text(session->led_detail, sizeof session->led_detail,
                 detail[0] != '\0' ? detail : "led lease released");
        return 1;
    }

    session->led_state = DEVICE_SESSION_LED_FAILED;
    set_text(session->led_detail, sizeof session->led_detail,
             detail[0] != '\0' ? detail : "led lease release failed");
    return 0;
}

static void recompute_last_result(DeviceSession* session)
{
    if (session->failed_count > 0 || session->led_state == DEVICE_SESSION_LED_FAILED)
        session->last_result = DEVICE_SESSION_PARTIAL;
    else
        session->last_result = DEVICE_SESSION_OK;
}

static int probe_led(DeviceSession* session, const DeviceSessionOps* ops)
{
    char detail[DEVICE_SESSION_DETAIL_MAX];

    detail[0] = '\0';
    if (ops->led_available != NULL)
        session->led_available = ops->led_available(ops->context, detail, sizeof detail);
    else
        session->led_available = ops->led_release != NULL;

    if (session->led_available && ops->led_release == NULL)
    {
        session->led_available = 0;
        set_text(session->led_detail, sizeof session->led_detail,
                 "led release is not implemented by this backend");
    }
    else if (!session->led_available)
    {
        set_text(session->led_detail, sizeof session->led_detail,
                 detail[0] != '\0' ? detail : "led lease is not available");
    }
    else
    {
        set_text(session->led_detail, sizeof session->led_detail, "");
    }

    session->led_state = session->led_available ? DEVICE_SESSION_LED_IDLE
                                                : DEVICE_SESSION_LED_UNAVAILABLE;
    return session->led_available;
}

DeviceSessionResult device_session_begin(DeviceSession* session, const DeviceSessionOps* ops,
                                         const char* app, const char* pak)
{
    int field;
    int captured = 0;

    if (session == NULL) return DEVICE_SESSION_BAD_ARGUMENT;
    if (session->active) return DEVICE_SESSION_ALREADY_ACTIVE;
    if (ops == NULL) return DEVICE_SESSION_BAD_ARGUMENT;
    if (app != NULL && strlen(app) >= DEVICE_SESSION_APP_MAX) return DEVICE_SESSION_BAD_ARGUMENT;
    if (pak != NULL && strlen(pak) >= DEVICE_SESSION_PAK_MAX) return DEVICE_SESSION_BAD_ARGUMENT;

    memset(session, 0, sizeof *session);
    session->active = 1;
    session->id = next_session_id();
    if (session->id == 0) session->id = 1;
    session->started_at = (long)time(NULL);
    if (session->started_at < 0) session->started_at = 0;
    set_text(session->app, sizeof session->app, app);
    set_text(session->pak, sizeof session->pak, pak);
    session->last_result = DEVICE_SESSION_OK;
    probe_led(session, ops);

    for (field = 0; field < DEVICE_SESSION_FIELD_COUNT; field++)
    {
        DeviceSessionFieldState* state = &session->fields[field];
        char detail[DEVICE_SESSION_DETAIL_MAX];

        detail[0] = '\0';
        state->available = ops->available != NULL
                               ? ops->available(ops->context, (DeviceSessionField)field, detail,
                                                sizeof detail)
                               : 0;
        if (!state->available)
        {
            set_text(state->detail, sizeof state->detail,
                     detail[0] != '\0' ? detail : "backend reports this field unavailable");
            continue;
        }
        if (ops->read == NULL)
        {
            set_text(state->detail, sizeof state->detail, "backend cannot read this field");
            continue;
        }

        detail[0] = '\0';
        if (!ops->read(ops->context, (DeviceSessionField)field, &state->value, detail,
                       sizeof detail))
        {
            state->value = 0;
            set_text(state->detail, sizeof state->detail,
                     detail[0] != '\0' ? detail : "read failed");
            continue;
        }

        state->captured = 1;
        captured++;
        set_text(state->detail, sizeof state->detail, "captured");
    }

    snprintf(session->detail, sizeof session->detail, "captured %d of %d session fields", captured,
             DEVICE_SESSION_FIELD_COUNT);
    return DEVICE_SESSION_OK;
}

static void restore_field(DeviceSession* session, const DeviceSessionOps* ops,
                          DeviceSessionField field, int muted)
{
    DeviceSessionFieldState* state = &session->fields[field];
    int attempt;

    if (!state->captured) return;
    if (!state->changed)
    {
        state->skipped = 1;
        session->skipped_count++;
        set_text(state->detail, sizeof state->detail, "not changed by the app");
        return;
    }
    if (muted && field_is_volume_like(field))
    {
        state->skipped = 1;
        session->skipped_count++;
        set_text(state->detail, sizeof state->detail,
                 "session started muted; effective value left untouched");
        return;
    }

    if (ops->write == NULL)
    {
        state->detail[0] = '\0';
        set_text(state->detail, sizeof state->detail, "backend cannot write this field");
        session->failed_count++;
        return;
    }

    state->restored = 0;
    state->skipped = 0;
    for (attempt = 0; attempt < DEVICE_SESSION_RESTORE_ATTEMPTS; attempt++)
    {
        char detail[DEVICE_SESSION_DETAIL_MAX];
        int actual = 0;

        detail[0] = '\0';
        if (!ops->write(ops->context, field, state->value, detail, sizeof detail))
        {
            set_text(state->detail, sizeof state->detail,
                     detail[0] != '\0' ? detail : "restore write failed");
            continue;
        }

        detail[0] = '\0';
        if (!ops->read(ops->context, field, &actual, detail, sizeof detail))
        {
            set_text(state->detail, sizeof state->detail,
                     detail[0] != '\0' ? detail : "restore verification read failed");
            continue;
        }
        if (actual != state->value)
        {
            snprintf(state->detail, sizeof state->detail,
                     "restore did not take effect (wanted %d, got %d)", state->value, actual);
            continue;
        }

        state->restored = 1;
        set_text(state->detail, sizeof state->detail, "restored");
        session->restored_count++;
        return;
    }

    session->failed_count++;
}

DeviceSessionResult device_session_end(DeviceSession* session, const DeviceSessionOps* ops,
                                       unsigned long long expected_id)
{
    static const DeviceSessionField RESTORE_ORDER[DEVICE_SESSION_FIELD_COUNT] = {
        DEVICE_SESSION_MUTE,     DEVICE_SESSION_AUDIO_SINK, DEVICE_SESSION_VOLUME,
        DEVICE_SESSION_BRIGHTNESS, DEVICE_SESSION_COLORTEMP, DEVICE_SESSION_HAPTICS,
    };
    int muted;
    int i;

    if (session == NULL || ops == NULL) return DEVICE_SESSION_BAD_ARGUMENT;

    if (!session->active)
    {
        if (expected_id != 0 && expected_id != session->last_id) return DEVICE_SESSION_STALE;

        /* Orphan/retry path: a failed LED restore or a lease acquired outside a
         * C3 session is still released. C2's session_end is idempotent. */
        if (session->led_state == DEVICE_SESSION_LED_IDLE
            || session->led_state == DEVICE_SESSION_LED_FAILED)
        {
            (void)release_led(session, ops);
            recompute_last_result(session);
            if (session->led_state == DEVICE_SESSION_LED_FAILED)
            {
                set_text(session->detail, sizeof session->detail,
                         "no active session; led lease release failed");
                return DEVICE_SESSION_PARTIAL;
            }
        }
        set_text(session->detail, sizeof session->detail, "no active session");
        return DEVICE_SESSION_NOT_ACTIVE;
    }

    if (expected_id != 0 && expected_id != session->id) return DEVICE_SESSION_STALE;

    session->restored_count = 0;
    session->failed_count = 0;
    session->skipped_count = 0;

    muted = session->fields[DEVICE_SESSION_MUTE].captured
            && session->fields[DEVICE_SESSION_MUTE].value != 0;

    for (i = 0; i < DEVICE_SESSION_FIELD_COUNT; i++)
        restore_field(session, ops, RESTORE_ORDER[i], muted);

    (void)release_led(session, ops);

    session->active = 0;
    session->last_id = session->id;
    recompute_last_result(session);

    snprintf(session->detail, sizeof session->detail, "restored %d, skipped %d, failed %d",
             session->restored_count, session->skipped_count, session->failed_count);

    return session->last_result;
}

void device_session_note_capability(DeviceSession* session, const char* capability,
                                    const char* action)
{
    const char* verb = action != NULL ? action : "";

    if (session == NULL || !session->active || capability == NULL) return;

    if (strcmp(capability, "display.brightness") == 0)
    {
        if (strcmp(verb, "set") == 0 || strcmp(verb, "reset") == 0)
            session->fields[DEVICE_SESSION_BRIGHTNESS].changed = 1;
    }
    else if (strcmp(capability, "display.colortemp") == 0)
    {
        if (strcmp(verb, "set") == 0 || strcmp(verb, "reset") == 0)
            session->fields[DEVICE_SESSION_COLORTEMP].changed = 1;
    }
    else if (strcmp(capability, "audio.volume") == 0)
    {
        if (strcmp(verb, "set") == 0 || strcmp(verb, "reset") == 0)
            session->fields[DEVICE_SESSION_VOLUME].changed = 1;
        else if (strcmp(verb, "setmute") == 0)
            session->fields[DEVICE_SESSION_MUTE].changed = 1;
    }
    else if (strcmp(capability, "audio.sink") == 0)
    {
        if (strcmp(verb, "set") == 0 || strcmp(verb, "reset") == 0)
            session->fields[DEVICE_SESSION_AUDIO_SINK].changed = 1;
    }
    else if (strcmp(capability, "led.control") == 0)
    {
        if (strcmp(verb, "acquire") == 0 || strcmp(verb, "set") == 0
            || strcmp(verb, "release") == 0 || strcmp(verb, "restore") == 0)
            session->led_used = 1;
    }
}
