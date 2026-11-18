#include "device_protocol.h"

#include "device_status.h"
#include "json_lite.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    char* out;
    size_t size;
    size_t length;
    int ok;
} DeviceJsonWriter;

static void device_json_init(DeviceJsonWriter* writer, char* out, size_t out_size)
{
    writer->out = out;
    writer->size = out_size;
    writer->length = 0;
    writer->ok = out != NULL && out_size > 0;
    if (writer->ok) out[0] = '\0';
}

static void device_json_raw(DeviceJsonWriter* writer, const char* text)
{
    size_t length;

    if (!writer->ok || text == NULL) return;
    length = strlen(text);
    if (writer->length + length >= writer->size)
    {
        writer->ok = 0;
        return;
    }
    memcpy(writer->out + writer->length, text, length);
    writer->length += length;
    writer->out[writer->length] = '\0';
}

static void device_json_char(DeviceJsonWriter* writer, char ch)
{
    if (!writer->ok) return;
    if (writer->length + 1 >= writer->size)
    {
        writer->ok = 0;
        return;
    }
    writer->out[writer->length++] = ch;
    writer->out[writer->length] = '\0';
}

static void device_json_string(DeviceJsonWriter* writer, const char* text)
{
    char escaped[DEVICE_PROTOCOL_LINE_MAX];

    if (!writer->ok) return;
    if (!json_lite_escape(text != NULL ? text : "", escaped, sizeof escaped))
    {
        writer->ok = 0;
        return;
    }
    device_json_char(writer, '"');
    device_json_raw(writer, escaped);
    device_json_char(writer, '"');
}

static void device_json_bool(DeviceJsonWriter* writer, int value)
{
    device_json_raw(writer, value ? "true" : "false");
}

static void device_json_int(DeviceJsonWriter* writer, int value)
{
    char text[32];

    snprintf(text, sizeof text, "%d", value);
    device_json_raw(writer, text);
}

static void device_json_long(DeviceJsonWriter* writer, long value)
{
    char text[32];

    snprintf(text, sizeof text, "%ld", value);
    device_json_raw(writer, text);
}

static void device_json_session_id(DeviceJsonWriter* writer, unsigned long long value)
{
    char text[32];

    snprintf(text, sizeof text, "%016llx", value);
    device_json_raw(writer, text);
}

static void device_json_ull(DeviceJsonWriter* writer, unsigned long long value)
{
    char text[32];

    snprintf(text, sizeof text, "%llu", value);
    device_json_raw(writer, text);
}

static void device_json_key(DeviceJsonWriter* writer, const char* key)
{
    device_json_string(writer, key);
    device_json_char(writer, ':');
}

static void device_json_prefix(DeviceJsonWriter* writer, int* first)
{
    if (!*first) device_json_char(writer, ',');
    *first = 0;
}

static void device_json_field_string(DeviceJsonWriter* writer, const char* key, const char* value)
{
    device_json_key(writer, key);
    device_json_string(writer, value);
}

static void device_json_field_bool(DeviceJsonWriter* writer, const char* key, int value)
{
    device_json_key(writer, key);
    device_json_bool(writer, value);
}

static int device_json_finish(DeviceJsonWriter* writer)
{
    return writer->ok;
}

void device_request_message_init(DeviceRequestMessage* message)
{
    if (message != NULL) memset(message, 0, sizeof *message);
}

static int parse_string_into(JsonLiteCursor* cursor, char* out, size_t out_size)
{
    size_t length = 0;

    if (!json_lite_parse_string(cursor, out, out_size, &length)) return 0;
    return length < out_size;
}

/* Strict true/false. Anything else is a malformed request. */
static int parse_bool_into(JsonLiteCursor* cursor, int* out)
{
    if (cursor->p + 4 <= cursor->end && strncmp(cursor->p, "true", 4) == 0)
    {
        *out = 1;
        cursor->p += 4;
        return 1;
    }
    if (cursor->p + 5 <= cursor->end && strncmp(cursor->p, "false", 5) == 0)
    {
        *out = 0;
        cursor->p += 5;
        return 1;
    }
    return 0;
}

/* Non-negative integer that fits an int; rejects fractions and exponents. */
static int parse_int_into(JsonLiteCursor* cursor, int* out)
{
    long value = 0;

    if (!json_lite_parse_integer(cursor, &value)) return 0;
    if (value < 0 || value > 2147483647L) return 0;
    *out = (int)value;
    return 1;
}

/*
 * Parse an array of short strings into fixed slots. Unknown names are the
 * caller's business (CONTRACT.md §3.3 says clients and servers ignore unknown
 * feature names); `overflow` reports that the array did not fit, which only
 * matters for names the caller must validate.
 */
static int parse_string_array(JsonLiteCursor* cursor, char names[][DEVICE_PROTOCOL_FEATURE_NAME_MAX],
                              int capacity, int* count, int* overflow)
{
    int used = 0;
    int dropped = 0;

    *count = 0;
    if (overflow != NULL) *overflow = 0;
    json_lite_skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p != '[') return 0;
    cursor->p++;
    json_lite_skip_ws(cursor);
    if (cursor->p < cursor->end && *cursor->p == ']')
    {
        cursor->p++;
        return 1;
    }

    for (;;)
    {
        char name[DEVICE_PROTOCOL_FEATURE_NAME_MAX];
        size_t length = 0;

        if (!json_lite_parse_string(cursor, name, sizeof name, &length)) return 0;
        if (length >= sizeof name)
            dropped = 1; /* too long to be a known name: ignore, do not fail */
        else if (used < capacity)
            snprintf(names[used++], DEVICE_PROTOCOL_FEATURE_NAME_MAX, "%s", name);
        else
            dropped = 1;

        json_lite_skip_ws(cursor);
        if (cursor->p >= cursor->end) return 0;
        if (*cursor->p == ']')
        {
            cursor->p++;
            break;
        }
        if (*cursor->p != ',') return 0;
        cursor->p++;
        json_lite_skip_ws(cursor);
    }

    *count = used;
    if (overflow != NULL) *overflow = dropped;
    return 1;
}

int device_protocol_feature_is_known(const char* name)
{
    static const char* const FEATURES[] = {"events", "sleep", "status", "hud", "overlay",
                                           "battery"};
    size_t i;

    if (name == NULL) return 0;
    for (i = 0; i < sizeof FEATURES / sizeof FEATURES[0]; i++)
        if (strcmp(FEATURES[i], name) == 0) return 1;
    return 0;
}

int device_protocol_hud_kind_is_known(const char* name)
{
    return name != NULL
           && (strcmp(name, "volume") == 0 || strcmp(name, "brightness") == 0
               || strcmp(name, "colortemp") == 0 || strcmp(name, "mute") == 0);
}

int device_protocol_sleep_reason_is_valid(const char* reason)
{
    return reason != NULL
           && (strcmp(reason, "user") == 0 || strcmp(reason, "idle") == 0
               || strcmp(reason, "low_battery") == 0 || strcmp(reason, "lid") == 0);
}

int device_protocol_woke_by_is_valid(const char* woke_by)
{
    return woke_by != NULL
           && (strcmp(woke_by, "power") == 0 || strcmp(woke_by, "lid") == 0
               || strcmp(woke_by, "timer") == 0 || strcmp(woke_by, "unknown") == 0);
}

int device_protocol_sleep_result_is_valid(const char* result)
{
    return result != NULL && (strcmp(result, "ok") == 0 || strcmp(result, "failed") == 0);
}

/* Exactly 16 lowercase hex digits (CONTRACT.md §15). */
int device_protocol_sleep_id_is_valid(const char* sleep_id)
{
    size_t i;

    if (sleep_id == NULL || strlen(sleep_id) != 16) return 0;
    for (i = 0; i < 16; i++)
    {
        char ch = sleep_id[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return 0;
    }
    return 1;
}

int device_protocol_session_id_is_valid(const char* session_id)
{
    size_t i;

    if (session_id == NULL || strlen(session_id) != 16) return 0;
    for (i = 0; i < 16; i++)
    {
        char ch = session_id[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return 0;
    }
    return 1;
}

/*
 * `subscribe` carries one nested object: {"status":{"fields":[...],
 * "min_interval_ms":N}}. The flat key loop cannot express that, so this reads
 * the object directly.
 */
static int parse_status_object(JsonLiteCursor* cursor, DeviceRequestMessage* message, int* overflow)
{
    json_lite_skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p != '{') return 0;
    cursor->p++;
    json_lite_skip_ws(cursor);
    if (cursor->p < cursor->end && *cursor->p == '}')
    {
        cursor->p++;
        return 1;
    }

    for (;;)
    {
        char key[32];
        size_t key_length = 0;

        if (!json_lite_parse_string(cursor, key, sizeof key, &key_length)) return 0;
        json_lite_skip_ws(cursor);
        if (cursor->p >= cursor->end || *cursor->p != ':') return 0;
        cursor->p++;
        json_lite_skip_ws(cursor);

        if (key_length < sizeof key && strcmp(key, "fields") == 0)
        {
            if (!parse_string_array(cursor, message->fields, DEVICE_PROTOCOL_SECTION_MAX,
                                    &message->field_count, overflow))
                return 0;
            message->has_fields = 1;
        }
        else if (key_length < sizeof key && strcmp(key, "min_interval_ms") == 0)
        {
            if (!parse_int_into(cursor, &message->min_interval_ms)) return 0;
            message->has_min_interval_ms = 1;
        }
        else if (!json_lite_skip_value(cursor, 2))
        {
            return 0;
        }

        json_lite_skip_ws(cursor);
        if (cursor->p >= cursor->end) return 0;
        if (*cursor->p == '}')
        {
            cursor->p++;
            break;
        }
        if (*cursor->p != ',') return 0;
        cursor->p++;
        json_lite_skip_ws(cursor);
    }
    return 1;
}

static int parse_message(const char* line, DeviceRequestMessage* message, int cooperation)
{
    enum
    {
        SEEN_OP = 1u << 0,
        SEEN_APP = 1u << 1,
        SEEN_PAK = 1u << 2,
        SEEN_CAPABILITY = 1u << 3,
        SEEN_ACTION = 1u << 4,
        SEEN_ARGS = 1u << 5,
        SEEN_SESSION_ID = 1u << 6,
        SEEN_AT_MS = 1u << 7,
        SEEN_FORCED = 1u << 8,
        SEEN_CLIENT = 1u << 9,
        SEEN_PROTOCOL = 1u << 10,
        SEEN_FEATURES = 1u << 11,
        SEEN_KINDS = 1u << 12,
        SEEN_MODE = 1u << 13,
        SEEN_OPEN = 1u << 14,
        SEEN_HOST = 1u << 15,
        SEEN_SLEEP_ID = 1u << 16,
        SEEN_REASON = 1u << 17,
        SEEN_INITIATOR = 1u << 18,
        SEEN_WOKE_BY = 1u << 19,
        SEEN_RESULT = 1u << 20,
        SEEN_TIMEOUT_MS = 1u << 21,
        SEEN_PAUSED = 1u << 22,
        SEEN_SAVED = 1u << 23,
        SEEN_DETAIL = 1u << 24,
        SEEN_STATUS_OBJECT = 1u << 25,
    };
    JsonLiteCursor cursor;
    char op[24];
    size_t op_length = 0;
    unsigned seen = 0;
    size_t length;

    if (message == NULL) return 0;
    device_request_message_init(message);
    if (line == NULL) return 0;

    length = strlen(line);
    if (length == 0 || length >= DEVICE_PROTOCOL_LINE_MAX) return 0;

    json_lite_init(&cursor, line, length);
    json_lite_skip_ws(&cursor);
    if (cursor.p >= cursor.end || *cursor.p != '{') return 0;
    cursor.p++;
    json_lite_skip_ws(&cursor);

    if (cursor.p < cursor.end && *cursor.p == '}')
    {
        return 0;
    }

    for (;;)
    {
        char key[24];
        size_t key_length = 0;

        if (!json_lite_parse_string(&cursor, key, sizeof key, &key_length)) return 0;
        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end || *cursor.p != ':') return 0;
        cursor.p++;
        json_lite_skip_ws(&cursor);

        if (key_length < sizeof key && strcmp(key, "op") == 0)
        {
            if (!(seen & SEEN_OP))
            {
                seen |= SEEN_OP;
                if (!parse_string_into(&cursor, op, sizeof op)) return 0;
                op_length = strlen(op);
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "app") == 0)
        {
            if (!(seen & SEEN_APP))
            {
                seen |= SEEN_APP;
                if (!parse_string_into(&cursor, message->app, sizeof message->app)) return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "pak") == 0)
        {
            if (!(seen & SEEN_PAK))
            {
                seen |= SEEN_PAK;
                if (!parse_string_into(&cursor, message->pak, sizeof message->pak)) return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "capability") == 0)
        {
            if (!(seen & SEEN_CAPABILITY))
            {
                seen |= SEEN_CAPABILITY;
                if (!parse_string_into(&cursor, message->capability,
                                       sizeof message->capability))
                    return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "action") == 0)
        {
            if (!(seen & SEEN_ACTION))
            {
                seen |= SEEN_ACTION;
                if (!parse_string_into(&cursor, message->action, sizeof message->action)) return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "session_id") == 0)
        {
            if (!(seen & SEEN_SESSION_ID))
            {
                seen |= SEEN_SESSION_ID;
                if (!parse_string_into(&cursor, message->session_id,
                                       sizeof message->session_id))
                    return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "args") == 0)
        {
            if (!(seen & SEEN_ARGS))
            {
                const char* start;
                size_t args_length;
                seen |= SEEN_ARGS;
                if (!json_lite_capture_value(&cursor, &start, &args_length)) return 0;
                if (args_length >= sizeof message->args) return 0;
                memcpy(message->args, start, args_length);
                message->args[args_length] = '\0';
                message->has_args = 1;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "at_ms") == 0)
        {
            if (!(seen & SEEN_AT_MS))
            {
                const char* start;
                char number[32];
                char* end = NULL;
                size_t number_length;
                unsigned long long value;

                seen |= SEEN_AT_MS;
                if (!json_lite_capture_value(&cursor, &start, &number_length)) return 0;
                if (number_length == 0 || number_length >= sizeof number) return 0;
                memcpy(number, start, number_length);
                number[number_length] = '\0';
                if (number[0] == '-') return 0;
                errno = 0;
                value = strtoull(number, &end, 10);
                if (errno != 0 || end == number || *end != '\0') return 0;
                message->at_ms = value;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "forced") == 0)
        {
            if (!(seen & SEEN_FORCED))
            {
                seen |= SEEN_FORCED;
                if (!parse_bool_into(&cursor, &message->forced)) return 0;
                message->has_forced = 1;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "client") == 0)
        {
            if (!(seen & SEEN_CLIENT))
            {
                seen |= SEEN_CLIENT;
                if (!parse_string_into(&cursor, message->client, sizeof message->client))
                    return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "protocol") == 0)
        {
            if (!(seen & SEEN_PROTOCOL))
            {
                seen |= SEEN_PROTOCOL;
                if (!parse_int_into(&cursor, &message->protocol)) return 0;
                message->has_protocol = 1;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "features") == 0)
        {
            if (!(seen & SEEN_FEATURES))
            {
                seen |= SEEN_FEATURES;
                if (!parse_string_array(&cursor, message->features, DEVICE_PROTOCOL_FEATURE_MAX,
                                        &message->feature_count, NULL))
                    return 0;
                message->has_features = 1;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "kinds") == 0)
        {
            if (!(seen & SEEN_KINDS))
            {
                seen |= SEEN_KINDS;
                if (!parse_string_array(&cursor, message->kinds, DEVICE_PROTOCOL_KIND_MAX,
                                        &message->kind_count, NULL))
                    return 0;
                message->has_kinds = 1;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "mode") == 0)
        {
            if (!(seen & SEEN_MODE))
            {
                seen |= SEEN_MODE;
                if (!parse_string_into(&cursor, message->mode, sizeof message->mode)) return 0;
                message->has_mode = 1;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "open") == 0)
        {
            if (!(seen & SEEN_OPEN))
            {
                seen |= SEEN_OPEN;
                if (!parse_bool_into(&cursor, &message->open)) return 0;
                message->has_open = 1;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "host") == 0)
        {
            if (!(seen & SEEN_HOST))
            {
                seen |= SEEN_HOST;
                if (!parse_bool_into(&cursor, &message->host)) return 0;
                message->has_host = 1;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "sleep_id") == 0)
        {
            if (!(seen & SEEN_SLEEP_ID))
            {
                seen |= SEEN_SLEEP_ID;
                if (!parse_string_into(&cursor, message->sleep_id, sizeof message->sleep_id))
                    return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "reason") == 0)
        {
            if (!(seen & SEEN_REASON))
            {
                seen |= SEEN_REASON;
                if (!parse_string_into(&cursor, message->reason, sizeof message->reason))
                    return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "initiator") == 0)
        {
            if (!(seen & SEEN_INITIATOR))
            {
                seen |= SEEN_INITIATOR;
                if (!parse_string_into(&cursor, message->initiator, sizeof message->initiator))
                    return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "woke_by") == 0)
        {
            if (!(seen & SEEN_WOKE_BY))
            {
                seen |= SEEN_WOKE_BY;
                if (!parse_string_into(&cursor, message->woke_by, sizeof message->woke_by))
                    return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "result") == 0)
        {
            if (!(seen & SEEN_RESULT))
            {
                seen |= SEEN_RESULT;
                if (!parse_string_into(&cursor, message->result, sizeof message->result))
                    return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "timeout_ms") == 0)
        {
            if (!(seen & SEEN_TIMEOUT_MS))
            {
                seen |= SEEN_TIMEOUT_MS;
                if (!parse_int_into(&cursor, &message->timeout_ms)) return 0;
                message->has_timeout_ms = 1;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "paused") == 0)
        {
            if (!(seen & SEEN_PAUSED))
            {
                seen |= SEEN_PAUSED;
                if (!parse_bool_into(&cursor, &message->paused)) return 0;
                message->has_paused = 1;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "saved") == 0)
        {
            if (!(seen & SEEN_SAVED))
            {
                seen |= SEEN_SAVED;
                if (!parse_bool_into(&cursor, &message->saved)) return 0;
                message->has_saved = 1;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "detail") == 0)
        {
            if (!(seen & SEEN_DETAIL))
            {
                seen |= SEEN_DETAIL;
                if (!parse_string_into(&cursor, message->detail, sizeof message->detail))
                    return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "status") == 0)
        {
            if (!(seen & SEEN_STATUS_OBJECT))
            {
                seen |= SEEN_STATUS_OBJECT;
                if (!parse_status_object(&cursor, message, NULL)) return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (!json_lite_skip_value(&cursor, 2))
        {
            return 0;
        }

        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end) return 0;
        if (*cursor.p == '}')
        {
            cursor.p++;
            break;
        }
        if (*cursor.p != ',') return 0;
        cursor.p++;
        json_lite_skip_ws(&cursor);
    }

    json_lite_skip_ws(&cursor);
    if (cursor.p != cursor.end) return 0;
    if (!(seen & SEEN_OP) || op_length >= sizeof op) return 0;

    if (strcmp(op, "ping") == 0)
    {
        message->op = DEVICE_OP_PING;
        return 1;
    }
    if (strcmp(op, "status") == 0)
    {
        message->op = DEVICE_OP_STATUS;
        return message->pak[0] != '\0';
    }
    if (strcmp(op, "call") == 0)
    {
        message->op = DEVICE_OP_CALL;
        return message->pak[0] != '\0' && message->capability[0] != '\0';
    }
    if (strcmp(op, "session-start") == 0)
    {
        message->op = DEVICE_OP_SESSION_START;
        return 1;
    }
    if (strcmp(op, "session-stop") == 0)
    {
        message->op = DEVICE_OP_SESSION_STOP;
        return 1;
    }
    if (strcmp(op, "session-status") == 0)
    {
        message->op = DEVICE_OP_SESSION_STATUS;
        return 1;
    }
    if (strcmp(op, "app-action") == 0)
    {
        message->op = DEVICE_OP_APP_ACTION;
        return message->action[0] != '\0';
    }
    if (strcmp(op, "system-feedback") == 0)
    {
        message->op = DEVICE_OP_SYSTEM_FEEDBACK;
        return message->action[0] != '\0';
    }
    if (strcmp(op, "app-exit-result") == 0)
    {
        message->op = DEVICE_OP_APP_EXIT_RESULT;
        return 1;
    }
    if (!cooperation) return 0;

    /* --- protocol 1 (CONTRACT.md §10) --- */
    if (strcmp(op, "hello") == 0)
    {
        message->op = DEVICE_OP_HELLO;
        return 1;
    }
    if (strcmp(op, "subscribe") == 0)
    {
        message->op = DEVICE_OP_SUBSCRIBE;
        return 1;
    }
    if (strcmp(op, "battery.status") == 0)
    {
        message->op = DEVICE_OP_BATTERY_STATUS;
        return 1;
    }
    if (strcmp(op, "status.get") == 0)
    {
        message->op = DEVICE_OP_STATUS_GET;
        return 1;
    }
    if (strcmp(op, "sleep.request") == 0)
    {
        message->op = DEVICE_OP_SLEEP_REQUEST;
        return 1;
    }
    if (strcmp(op, "sleep.ack") == 0)
    {
        message->op = DEVICE_OP_SLEEP_ACK;
        return 1;
    }
    if (strcmp(op, "sleep.inhibit") == 0)
    {
        message->op = DEVICE_OP_SLEEP_INHIBIT;
        return 1;
    }
    if (strcmp(op, "sleep.release") == 0)
    {
        message->op = DEVICE_OP_SLEEP_RELEASE;
        return 1;
    }
    if (strcmp(op, "sleep.finished") == 0)
    {
        message->op = DEVICE_OP_SLEEP_FINISHED;
        return 1;
    }
    if (strcmp(op, "sleep.status") == 0)
    {
        message->op = DEVICE_OP_SLEEP_STATUS;
        return 1;
    }
    if (strcmp(op, "hud.policy") == 0)
    {
        message->op = DEVICE_OP_HUD_POLICY;
        return 1;
    }
    if (strcmp(op, "overlay.state") == 0)
    {
        message->op = DEVICE_OP_OVERLAY_STATE;
        return 1;
    }
    if (strcmp(op, "overlay.host") == 0)
    {
        message->op = DEVICE_OP_OVERLAY_HOST;
        return 1;
    }
    return 0;
}

int device_protocol_parse_request(const char* line, DeviceRequestMessage* message)
{
    return parse_message(line, message, 0);
}

int device_protocol_parse_message(const char* line, DeviceRequestMessage* message)
{
    return parse_message(line, message, 1);
}

int device_protocol_build_request(const char* op, const char* app, const char* pak,
                                  const char* capability, const char* action, const char* args,
                                  char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "op", op != NULL ? op : "");
    if (app != NULL && app[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "app", app);
    }
    if (pak != NULL && pak[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "pak", pak);
    }
    if (capability != NULL && capability[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "capability", capability);
    }
    if (action != NULL && action[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "action", action);
    }
    if (args != NULL && args[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_key(&writer, "args");
        device_json_raw(&writer, args);
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_build_session_request(const char* op, const char* app, const char* pak,
                                          const char* session_id, char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "op", op != NULL ? op : "");
    if (app != NULL && app[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "app", app);
    }
    if (pak != NULL && pak[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "pak", pak);
    }
    if (session_id != NULL && session_id[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "session_id", session_id);
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_build_app_action_request(const char* action, unsigned long long at_ms,
                                             const char* session_id, char* out,
                                             size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;
    char timestamp[32];

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "op", "app-action");
    if (action != NULL && action[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "action", action);
    }
    if (at_ms != 0)
    {
        snprintf(timestamp, sizeof timestamp, "%llu", at_ms);
        device_json_prefix(&writer, &first);
        device_json_key(&writer, "at_ms");
        device_json_raw(&writer, timestamp);
    }
    if (session_id != NULL && session_id[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "session_id", session_id);
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_build_app_exit_result_request(const char* session_id, int forced,
                                                  char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "op", "app-exit-result");
    if (session_id != NULL && session_id[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "session_id", session_id);
    }
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "forced", forced);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_ping_response(char* out, size_t out_size)
{
    DeviceJsonWriter writer;

    device_json_init(&writer, out, out_size);
    device_json_raw(&writer, "{\"ok\":true,\"service\":\"devserv\",\"version\":1}");
    return device_json_finish(&writer);
}

/*
 * The `limits` object body. One definition so ping, hello and
 * subscription.ready can never advertise different numbers.
 */
int device_protocol_write_limits(char* out, size_t out_size)
{
    DeviceJsonWriter writer;

    device_json_init(&writer, out, out_size);
    device_json_raw(&writer, "{\"line_max\":8192");
    device_json_raw(&writer, ",\"event_queue\":64");
    device_json_raw(&writer, ",\"subscribe_max\":8");
    device_json_raw(&writer, ",\"sleep_ack_ms\":1500");
    device_json_raw(&writer, ",\"inhibit_max_ms\":300000");
    device_json_raw(&writer, ",\"sleep_cooldown_ms\":1000");
    device_json_raw(&writer, ",\"heartbeat_ms\":15000");
    device_json_raw(&writer, ",\"status_min_interval_ms\":500");
    device_json_raw(&writer, ",\"status_resync_ms\":30000");
    device_json_raw(&writer, ",\"feedback_min_interval_ms\":100}");
    return device_json_finish(&writer);
}

static const char* const FAMILY_NAMES[] = {"sleep", "status", "hud", "overlay"};

unsigned device_protocol_family_for_name(const char* name)
{
    size_t i;

    if (name == NULL) return 0;
    for (i = 0; i < sizeof FAMILY_NAMES / sizeof FAMILY_NAMES[0]; i++)
        if (strcmp(FAMILY_NAMES[i], name) == 0) return 1u << i;
    return 0;
}

const char* device_protocol_family_name(unsigned family)
{
    switch (family)
    {
    case DEVICE_EVENT_FAMILY_SLEEP: return "sleep";
    case DEVICE_EVENT_FAMILY_STATUS: return "status";
    case DEVICE_EVENT_FAMILY_HUD: return "hud";
    case DEVICE_EVENT_FAMILY_OVERLAY: return "overlay";
    }
    return "unknown";
}

int device_protocol_write_ping_response_protocol(char* out, size_t out_size)
{
    DeviceJsonWriter writer;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_raw(&writer, "\"ok\":true,\"service\":\"devserv\",\"version\":1");
    device_json_raw(&writer, ",\"protocol\":1,\"contract\":\"nextui-app/1\"");
    device_json_raw(&writer,
                    ",\"features\":[\"events\",\"sleep\",\"status\",\"hud\",\"overlay\","
                    "\"battery\"]");
    device_json_raw(&writer, ",\"limits\":");
    {
        char limits[512];
        if (!device_protocol_write_limits(limits, sizeof limits))
        {
            writer.ok = 0;
            return 0;
        }
        device_json_raw(&writer, limits);
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_protocol_unsupported_response(char* out, size_t out_size)
{
    DeviceJsonWriter writer;

    device_json_init(&writer, out, out_size);
    device_json_raw(&writer, "{\"ok\":false,\"error\":\"protocol_unsupported\"");
    device_json_raw(&writer, ",\"server_protocol\":1,\"min_protocol\":1");
    device_json_raw(&writer, ",\"detail\":\"this server requires protocol 1\"}");
    return device_json_finish(&writer);
}

/* Shared section writers, used by status.get and both status events. */
static void write_battery_object(DeviceJsonWriter* writer, const DeviceStatusBattery* battery)
{
    device_json_char(writer, '{');
    {
        int first = 1;
        device_json_prefix(writer, &first);
        device_json_field_bool(writer, "available", battery->available);
        if (battery->available)
        {
            device_json_prefix(writer, &first);
            device_json_key(writer, "percent");
            device_json_int(writer, battery->percent);
            device_json_prefix(writer, &first);
            device_json_field_bool(writer, "charging", battery->charging);
            device_json_prefix(writer, &first);
            device_json_field_bool(writer, "usb", battery->usb);
        }
        if (battery->detail[0] != '\0')
        {
            device_json_prefix(writer, &first);
            device_json_field_string(writer, "detail", battery->detail);
        }
    }
    device_json_char(writer, '}');
}

static void write_section_header(DeviceJsonWriter* writer, int* first, const char* key)
{
    device_json_prefix(writer, first);
    device_json_key(writer, key);
}

static void write_unavailable_detail(DeviceJsonWriter* writer, const char* detail)
{
    if (detail != NULL && detail[0] != '\0')
        device_json_field_string(writer, "detail", detail);
}

static void write_wifi_object(DeviceJsonWriter* writer, const DeviceStatusWifi* wifi)
{
    device_json_char(writer, '{');
    {
        int first = 1;
        device_json_prefix(writer, &first);
        device_json_field_bool(writer, "available", wifi->available);
        if (wifi->available)
        {
            device_json_prefix(writer, &first);
            device_json_field_bool(writer, "enabled", wifi->enabled);
            device_json_prefix(writer, &first);
            device_json_field_bool(writer, "connected", wifi->connected);
            device_json_prefix(writer, &first);
            device_json_field_string(writer, "strength",
                                     wifi->strength[0] != '\0' ? wifi->strength : "off");
            device_json_prefix(writer, &first);
            device_json_field_string(writer, "ssid", wifi->ssid);
        }
        if (wifi->detail[0] != '\0')
        {
            device_json_prefix(writer, &first);
            write_unavailable_detail(writer, wifi->detail);
        }
    }
    device_json_char(writer, '}');
}

static void write_bluetooth_object(DeviceJsonWriter* writer,
                                   const DeviceStatusBluetooth* bluetooth)
{
    device_json_char(writer, '{');
    {
        int first = 1;
        device_json_prefix(writer, &first);
        device_json_field_bool(writer, "available", bluetooth->available);
        if (bluetooth->available)
        {
            device_json_prefix(writer, &first);
            device_json_field_bool(writer, "enabled", bluetooth->enabled);
            device_json_prefix(writer, &first);
            device_json_field_bool(writer, "connected", bluetooth->connected);
            device_json_prefix(writer, &first);
            device_json_field_string(writer, "name", bluetooth->name);
        }
        if (bluetooth->detail[0] != '\0')
        {
            device_json_prefix(writer, &first);
            write_unavailable_detail(writer, bluetooth->detail);
        }
    }
    device_json_char(writer, '}');
}

static void write_volume_object(DeviceJsonWriter* writer, const DeviceStatusVolume* volume)
{
    device_json_char(writer, '{');
    {
        int first = 1;
        device_json_prefix(writer, &first);
        device_json_field_bool(writer, "available", volume->available);
        if (volume->available)
        {
            device_json_prefix(writer, &first);
            device_json_key(writer, "level");
            device_json_int(writer, volume->level);
            device_json_prefix(writer, &first);
            device_json_key(writer, "min");
            device_json_int(writer, volume->min);
            device_json_prefix(writer, &first);
            device_json_key(writer, "max");
            device_json_int(writer, volume->max);
            device_json_prefix(writer, &first);
            device_json_field_bool(writer, "mute", volume->mute);
            device_json_prefix(writer, &first);
            device_json_field_bool(writer, "mute_supported", volume->mute_supported);
            device_json_prefix(writer, &first);
            device_json_field_string(writer, "sink",
                                     volume->sink[0] != '\0' ? volume->sink : "default");
            device_json_prefix(writer, &first);
            device_json_field_bool(writer, "locked", volume->locked);
        }
        if (volume->detail[0] != '\0')
        {
            device_json_prefix(writer, &first);
            write_unavailable_detail(writer, volume->detail);
        }
    }
    device_json_char(writer, '}');
}

static void write_display_object(DeviceJsonWriter* writer, const DeviceStatusDisplay* display)
{
    device_json_char(writer, '{');
    {
        int first = 1;
        device_json_prefix(writer, &first);
        device_json_field_bool(writer, "available", display->available);
        if (display->available)
        {
            device_json_prefix(writer, &first);
            device_json_key(writer, "brightness");
            device_json_int(writer, display->brightness);
            device_json_prefix(writer, &first);
            device_json_key(writer, "brightness_min");
            device_json_int(writer, display->brightness_min);
            device_json_prefix(writer, &first);
            device_json_key(writer, "brightness_max");
            device_json_int(writer, display->brightness_max);
            device_json_prefix(writer, &first);
            device_json_field_bool(writer, "brightness_locked",
                                   display->brightness_locked);
            device_json_prefix(writer, &first);
            device_json_key(writer, "colortemp");
            device_json_int(writer, display->colortemp);
            device_json_prefix(writer, &first);
            device_json_key(writer, "colortemp_min");
            device_json_int(writer, display->colortemp_min);
            device_json_prefix(writer, &first);
            device_json_key(writer, "colortemp_max");
            device_json_int(writer, display->colortemp_max);
            device_json_prefix(writer, &first);
            device_json_field_bool(writer, "colortemp_locked", display->colortemp_locked);
        }
        if (display->detail[0] != '\0')
        {
            device_json_prefix(writer, &first);
            write_unavailable_detail(writer, display->detail);
        }
    }
    device_json_char(writer, '}');
}

static void write_color(DeviceJsonWriter* writer, const char* key, uint32_t color)
{
    char text[16];

    snprintf(text, sizeof text, "#%06X", (unsigned)(color & 0xFFFFFFu));
    device_json_key(writer, key);
    device_json_string(writer, text);
}

static void write_theme_object(DeviceJsonWriter* writer, const DeviceStatusTheme* theme)
{
    device_json_char(writer, '{');
    {
        int first = 1;
        device_json_prefix(writer, &first);
        device_json_field_bool(writer, "available", theme->available);
        if (theme->available)
        {
            device_json_prefix(writer, &first);
            write_color(writer, "main", theme->main);
            device_json_prefix(writer, &first);
            write_color(writer, "accent", theme->accent);
            device_json_prefix(writer, &first);
            write_color(writer, "accent2", theme->accent2);
            device_json_prefix(writer, &first);
            write_color(writer, "list_text", theme->list_text);
            device_json_prefix(writer, &first);
            write_color(writer, "list_text_selected", theme->list_text_selected);
            device_json_prefix(writer, &first);
            write_color(writer, "hint", theme->hint);
            device_json_prefix(writer, &first);
            write_color(writer, "background", theme->background);
        }
        if (theme->detail[0] != '\0')
        {
            device_json_prefix(writer, &first);
            write_unavailable_detail(writer, theme->detail);
        }
    }
    device_json_char(writer, '}');
}

static void write_clock_object(DeviceJsonWriter* writer, const DeviceStatusClock* clock)
{
    device_json_char(writer, '{');
    {
        int first = 1;
        device_json_prefix(writer, &first);
        device_json_field_bool(writer, "show", clock->show);
        device_json_prefix(writer, &first);
        device_json_field_bool(writer, "hour24", clock->hour24);
    }
    device_json_char(writer, '}');
}

static void write_status_sections(DeviceJsonWriter* writer, const DeviceStatusSnapshot* status,
                                 unsigned sections, int* first)
{
    if ((sections & DEVICE_STATUS_SECTION_BATTERY) != 0)
    {
        write_section_header(writer, first, "battery");
        write_battery_object(writer, &status->battery);
    }
    if ((sections & DEVICE_STATUS_SECTION_WIFI) != 0)
    {
        write_section_header(writer, first, "wifi");
        write_wifi_object(writer, &status->wifi);
    }
    if ((sections & DEVICE_STATUS_SECTION_BLUETOOTH) != 0)
    {
        write_section_header(writer, first, "bluetooth");
        write_bluetooth_object(writer, &status->bluetooth);
    }
    if ((sections & DEVICE_STATUS_SECTION_VOLUME) != 0)
    {
        write_section_header(writer, first, "volume");
        write_volume_object(writer, &status->volume);
    }
    if ((sections & DEVICE_STATUS_SECTION_DISPLAY) != 0)
    {
        write_section_header(writer, first, "display");
        write_display_object(writer, &status->display);
    }
    if ((sections & DEVICE_STATUS_SECTION_THEME) != 0)
    {
        write_section_header(writer, first, "theme");
        write_theme_object(writer, &status->theme);
    }
    if ((sections & DEVICE_STATUS_SECTION_CLOCK) != 0)
    {
        write_section_header(writer, first, "clock");
        write_clock_object(writer, &status->clock);
    }
}

int device_protocol_write_battery_response(const DeviceStatusBattery* battery, char* out,
                                           size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    if (battery == NULL)
    {
        device_json_raw(&writer, "{\"ok\":false,\"error\":\"bad_request\","
                                 "\"detail\":\"missing battery\"}");
        return device_json_finish(&writer);
    }
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    device_json_prefix(&writer, &first);
    write_section_header(&writer, &first, "battery");
    write_battery_object(&writer, battery);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_status_response_sections(const DeviceStatusSnapshot* status,
                                                   unsigned sections, int overlay_open,
                                                   char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    if (status == NULL)
    {
        device_json_raw(&writer, "{\"ok\":false,\"error\":\"bad_request\","
                                 "\"detail\":\"missing status\"}");
        return device_json_finish(&writer);
    }
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_open", overlay_open);
    write_status_sections(&writer, status, sections, &first);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_sleep_status_response(const DeviceSleepStatusView* status,
                                                unsigned long long at_ms, char* out,
                                                size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    if (status == NULL)
    {
        device_json_raw(&writer, "{\"ok\":false,\"error\":\"bad_request\","
                                 "\"detail\":\"missing sleep status\"}");
        return device_json_finish(&writer);
    }
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "phase",
                             status->phase != NULL ? status->phase : "awake");
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "at_ms");
    device_json_ull(&writer, at_ms);

    device_json_prefix(&writer, &first);
    device_json_key(&writer, "pending");
    if (status->pending)
    {
        int pending_first = 1;
        device_json_char(&writer, '{');
        device_json_prefix(&writer, &pending_first);
        device_json_field_string(&writer, "sleep_id", status->pending_sleep_id);
        device_json_prefix(&writer, &pending_first);
        device_json_field_string(&writer, "reason", status->pending_reason);
        device_json_prefix(&writer, &pending_first);
        device_json_key(&writer, "deadline_ms");
        device_json_int(&writer, status->pending_deadline_ms);
        device_json_prefix(&writer, &pending_first);
        device_json_key(&writer, "deadline_at_ms");
        device_json_ull(&writer, status->pending_deadline_at_ms);
        device_json_prefix(&writer, &pending_first);
        device_json_key(&writer, "subscribers");
        device_json_int(&writer, status->pending_subscribers);
        device_json_prefix(&writer, &pending_first);
        device_json_key(&writer, "acks");
        device_json_int(&writer, status->pending_acks);
        device_json_char(&writer, '}');
    }
    else
    {
        device_json_raw(&writer, "null");
    }

    device_json_prefix(&writer, &first);
    device_json_key(&writer, "inhibit");
    if (status->inhibit_active)
    {
        int inhibit_first = 1;
        device_json_char(&writer, '{');
        device_json_prefix(&writer, &inhibit_first);
        device_json_field_string(&writer, "scope",
                                 status->inhibit_scope != NULL ? status->inhibit_scope
                                                               : "automatic");
        device_json_prefix(&writer, &inhibit_first);
        device_json_field_string(&writer, "reason", status->inhibit_reason);
        device_json_prefix(&writer, &inhibit_first);
        device_json_key(&writer, "expires_at_ms");
        device_json_ull(&writer, status->inhibit_expires_at_ms);
        if (status->inhibit_sleep_id[0] != '\0')
        {
            device_json_prefix(&writer, &inhibit_first);
            device_json_field_string(&writer, "sleep_id", status->inhibit_sleep_id);
        }
        device_json_char(&writer, '}');
    }
    else
    {
        device_json_raw(&writer, "null");
    }

    device_json_prefix(&writer, &first);
    device_json_key(&writer, "last");
    if (status->last_valid)
    {
        int last_first = 1;
        device_json_char(&writer, '{');
        device_json_prefix(&writer, &last_first);
        device_json_field_string(&writer, "sleep_id", status->last_sleep_id);
        device_json_prefix(&writer, &last_first);
        device_json_field_string(&writer, "reason", status->last_reason);
        device_json_prefix(&writer, &last_first);
        device_json_field_string(&writer, "result", status->last_result);
        device_json_prefix(&writer, &last_first);
        device_json_field_string(&writer, "woke_by", status->last_woke_by);
        device_json_prefix(&writer, &last_first);
        device_json_key(&writer, "started_at_ms");
        device_json_ull(&writer, status->last_started_at_ms);
        device_json_prefix(&writer, &last_first);
        device_json_key(&writer, "ended_at_ms");
        device_json_ull(&writer, status->last_ended_at_ms);
        device_json_prefix(&writer, &last_first);
        device_json_key(&writer, "slept_ms");
        device_json_ull(&writer, status->last_slept_ms);
        device_json_char(&writer, '}');
    }
    else
    {
        device_json_raw(&writer, "null");
    }

    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_sleep_request_response(const DeviceSleepDecisionView* view, char* out,
                                                 size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    if (view == NULL)
    {
        device_json_raw(&writer, "{\"ok\":false,\"error\":\"bad_request\","
                                 "\"detail\":\"missing decision\"}");
        return device_json_finish(&writer);
    }
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "decision",
                             view->decision != NULL ? view->decision : "proceed");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "sleep_id", view->sleep_id);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "subscribers");
    device_json_int(&writer, view->subscribers);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "acks");
    device_json_int(&writer, view->acks);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "inhibit");
    if (view->inhibit)
    {
        int inhibit_first = 1;
        device_json_char(&writer, '{');
        device_json_prefix(&writer, &inhibit_first);
        device_json_field_string(&writer, "reason", view->inhibit_reason);
        if (view->inhibit_detail[0] != '\0')
        {
            device_json_prefix(&writer, &inhibit_first);
            device_json_field_string(&writer, "detail", view->inhibit_detail);
        }
        device_json_prefix(&writer, &inhibit_first);
        device_json_key(&writer, "until_ms");
        device_json_ull(&writer, view->inhibit_until_ms);
        device_json_char(&writer, '}');
    }
    else
    {
        device_json_raw(&writer, "null");
    }
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "defer_until_ms");
    device_json_ull(&writer, view->defer_until_ms);
    if (view->detail != NULL && view->detail[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "detail", view->detail);
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_sleep_ack_response(const char* sleep_id, const char* decision,
                                             const char* detail, char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "sleep_id", sleep_id != NULL ? sleep_id : "");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "result", "accepted");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "decision",
                             decision != NULL ? decision : "defer");
    if (detail != NULL && detail[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "detail", detail);
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_sleep_inhibit_response(const char* scope, const char* sleep_id,
                                                 const char* decision, int inhibit_active,
                                                 unsigned long long expires_at_ms,
                                                 const char* reason, char* out,
                                                 size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "scope", scope != NULL ? scope : "automatic");
    device_json_prefix(&writer, &first);
    if (sleep_id != NULL && sleep_id[0] != '\0')
        device_json_field_string(&writer, "sleep_id", sleep_id);
    else
    {
        device_json_key(&writer, "sleep_id");
        device_json_raw(&writer, "null");
    }
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "decision", decision != NULL ? decision : "defer");
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "inhibit_active", inhibit_active);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "expires_at_ms");
    device_json_ull(&writer, expires_at_ms);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "reason", reason != NULL ? reason : "");
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_sleep_release_response(int released, const char* scope, char* out,
                                                 size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "released", released);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "scope", scope != NULL ? scope : "none");
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_sleep_finished_response(const char* sleep_id,
                                                  unsigned long long slept_ms, char* out,
                                                  size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "sleep_id", sleep_id != NULL ? sleep_id : "");
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "slept_ms");
    device_json_ull(&writer, slept_ms);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

static void write_hud_policy_object(DeviceJsonWriter* writer, const DeviceHudPolicyView* policy)
{
    int first = 1;

    device_json_char(writer, '{');
    device_json_prefix(writer, &first);
    device_json_field_string(writer, "volume", policy->volume ? "app" : "system");
    device_json_prefix(writer, &first);
    device_json_field_string(writer, "brightness", policy->brightness ? "app" : "system");
    device_json_prefix(writer, &first);
    device_json_field_string(writer, "colortemp", policy->colortemp ? "app" : "system");
    device_json_prefix(writer, &first);
    device_json_field_string(writer, "mute", policy->mute ? "app" : "system");
    device_json_char(writer, '}');
}

int device_protocol_write_hud_policy_response(const char* session_id, int overlay_open,
                                              int overlay_host,
                                              const DeviceHudPolicyView* policy,
                                              const DeviceHudPolicyView* effective, char* out,
                                              size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "session_id", session_id != NULL ? session_id : "");
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_open", overlay_open);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_host", overlay_host);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "policy");
    write_hud_policy_object(&writer, policy);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "effective");
    write_hud_policy_object(&writer, effective);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_overlay_state_response(const char* session_id, int overlay_host,
                                                 int overlay_open, char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "session_id", session_id != NULL ? session_id : "");
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_host", overlay_host);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_open", overlay_open);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_overlay_host_response(const char* session_id, int overlay_host,
                                                int overlay_host_declared,
                                                unsigned long long host_changed_at_ms,
                                                int overlay_open, const char* effective_hud,
                                                char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "session_id", session_id != NULL ? session_id : "");
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_host", overlay_host);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_host_declared", overlay_host_declared);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "host_changed_at_ms");
    device_json_ull(&writer, host_changed_at_ms);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_open", overlay_open);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "effective_hud",
                             effective_hud != NULL ? effective_hud : "system");
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

static void write_event_header(DeviceJsonWriter* writer, const char* event, unsigned seq,
                               unsigned long long at_ms)
{
    int first = 1;

    device_json_char(writer, '{');
    device_json_prefix(writer, &first);
    device_json_field_string(writer, "event", event);
    device_json_prefix(writer, &first);
    device_json_key(writer, "seq");
    device_json_int(writer, (int)seq);
    device_json_prefix(writer, &first);
    device_json_key(writer, "at_ms");
    device_json_ull(writer, at_ms);
}

static void write_event_session(DeviceJsonWriter* writer, int* first, const char* session_id)
{
    if (session_id == NULL || session_id[0] == '\0') return;
    device_json_prefix(writer, first);
    device_json_field_string(writer, "session_id", session_id);
}

int device_protocol_write_heartbeat_event(unsigned seq, unsigned long long at_ms, char* out,
                                          size_t out_size)
{
    DeviceJsonWriter writer;

    device_json_init(&writer, out, out_size);
    write_event_header(&writer, "heartbeat", seq, at_ms);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_overflow_event(unsigned seq, unsigned long long at_ms, int dropped,
                                         const char* session_id, char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 0;

    device_json_init(&writer, out, out_size);
    write_event_header(&writer, "overflow", seq, at_ms);
    write_event_session(&writer, &first, session_id);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "dropped");
    device_json_int(&writer, dropped);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_session_ended_event(unsigned seq, unsigned long long at_ms,
                                              const char* session_id, const char* reason,
                                              char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 0;

    device_json_init(&writer, out, out_size);
    write_event_header(&writer, "session.ended", seq, at_ms);
    write_event_session(&writer, &first, session_id);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "reason", reason != NULL ? reason : "stopped");
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

static void write_optional_battery(DeviceJsonWriter* writer, int* first,
                                  const DeviceStatusBattery* battery)
{
    if (battery == NULL || !battery->available) return;
    device_json_prefix(writer, first);
    device_json_key(writer, "battery");
    write_battery_object(writer, battery);
}

int device_protocol_write_sleep_prepare_event(unsigned seq, unsigned long long at_ms,
                                              const char* session_id, const char* sleep_id,
                                              const char* reason, int deadline_ms,
                                              unsigned long long deadline_at_ms,
                                              const DeviceStatusBattery* battery, char* out,
                                              size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 0;

    device_json_init(&writer, out, out_size);
    write_event_header(&writer, "sleep.prepare", seq, at_ms);
    write_event_session(&writer, &first, session_id);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "sleep_id", sleep_id != NULL ? sleep_id : "");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "reason", reason != NULL ? reason : "user");
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "deadline_ms");
    device_json_int(&writer, deadline_ms);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "deadline_at_ms");
    device_json_ull(&writer, deadline_at_ms);
    write_optional_battery(&writer, &first, battery);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_wake_done_event(unsigned seq, unsigned long long at_ms,
                                          const char* session_id, const char* sleep_id,
                                          unsigned long long slept_ms, const char* reason,
                                          const char* woke_by, const char* result,
                                          const DeviceStatusBattery* battery, char* out,
                                          size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 0;

    device_json_init(&writer, out, out_size);
    write_event_header(&writer, "wake.done", seq, at_ms);
    write_event_session(&writer, &first, session_id);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "sleep_id", sleep_id != NULL ? sleep_id : "");
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "slept_ms");
    device_json_ull(&writer, slept_ms);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "reason", reason != NULL ? reason : "user");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "woke_by", woke_by != NULL ? woke_by : "unknown");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "result", result != NULL ? result : "ok");
    write_optional_battery(&writer, &first, battery);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_status_snapshot_event(unsigned seq, unsigned long long at_ms,
                                                const char* session_id, int initial,
                                                const DeviceStatusSnapshot* status,
                                                unsigned sections, int overlay_open,
                                                char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 0;

    device_json_init(&writer, out, out_size);
    if (status == NULL)
    {
        writer.ok = 0;
        return 0;
    }
    write_event_header(&writer, "status.snapshot", seq, at_ms);
    write_event_session(&writer, &first, session_id);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "initial", initial);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_open", overlay_open);
    write_status_sections(&writer, status, sections, &first);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_status_changed_event(unsigned seq, unsigned long long at_ms,
                                               const char* session_id,
                                               const DeviceStatusSnapshot* status,
                                               unsigned sections, int overlay_open, char* out,
                                               size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 0;
    unsigned bit;

    device_json_init(&writer, out, out_size);
    if (status == NULL)
    {
        writer.ok = 0;
        return 0;
    }
    write_event_header(&writer, "status.changed", seq, at_ms);
    write_event_session(&writer, &first, session_id);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_open", overlay_open);

    device_json_prefix(&writer, &first);
    device_json_key(&writer, "changed");
    device_json_char(&writer, '[');
    {
        int changed_first = 1;
        for (bit = 0; bit < 7; bit++)
        {
            unsigned section = 1u << bit;
            if ((sections & section) == 0) continue;
            if (!changed_first) device_json_char(&writer, ',');
            changed_first = 0;
            device_json_string(&writer, device_status_section_name((DeviceStatusSection)section));
        }
    }
    device_json_char(&writer, ']');
    write_status_sections(&writer, status, sections, &first);
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_feedback_event(unsigned seq, unsigned long long at_ms,
                                         const char* session_id, const char* kind, int value,
                                         int min, int max, int mute, int locked,
                                         int overlay_open, const char* sink, const char* source,
                                         char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 0;

    device_json_init(&writer, out, out_size);
    write_event_header(&writer, "feedback", seq, at_ms);
    write_event_session(&writer, &first, session_id);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "kind", kind != NULL ? kind : "volume");
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "value");
    device_json_int(&writer, value);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "min");
    device_json_int(&writer, min);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "max");
    device_json_int(&writer, max);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "mute", mute);
    if (kind != NULL && strcmp(kind, "volume") == 0)
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "sink", sink != NULL ? sink : "default");
    }
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "locked", locked);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_open", overlay_open);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "source", source != NULL ? source : "change");
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_overlay_home_event(unsigned seq, unsigned long long at_ms,
                                             const char* session_id, const char* action,
                                             char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 0;

    device_json_init(&writer, out, out_size);
    write_event_header(&writer, "overlay.home", seq, at_ms);
    write_event_session(&writer, &first, session_id);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "action", action != NULL ? action : "home");
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_subscription_ready(unsigned long long at_ms, unsigned families,
                                             const char* session_id, int overlay_host,
                                             char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 0;
    unsigned bit;

    device_json_init(&writer, out, out_size);
    /* seq is always 0 on this acknowledgement (CONTRACT.md §5.1). */
    write_event_header(&writer, "subscription.ready", 0, at_ms);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "protocol");
    device_json_int(&writer, DEVICE_PROTOCOL_GENERATION);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "features");
    device_json_char(&writer, '[');
    {
        int feature_first = 1;
        for (bit = 0; bit < 4; bit++)
        {
            unsigned family = 1u << bit;
            if ((families & family) == 0) continue;
            if (!feature_first) device_json_char(&writer, ',');
            feature_first = 0;
            device_json_string(&writer, device_protocol_family_name(family));
        }
    }
    device_json_char(&writer, ']');
    write_event_session(&writer, &first, session_id);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_host", overlay_host);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "limits");
    {
        char limits[512];
        if (!device_protocol_write_limits(limits, sizeof limits))
        {
            writer.ok = 0;
            return 0;
        }
        device_json_raw(&writer, limits);
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_hello_response(const DeviceSession* session, int overlay_host,
                                         int overlay_host_declared, int overlay_open,
                                         int matches_pak, int battery_available,
                                         const DeviceStatusBattery* battery, int protocol,
                                         unsigned features, char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "version");
    device_json_int(&writer, 1);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "protocol");
    device_json_int(&writer, protocol);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "contract", DEVICE_PROTOCOL_CONTRACT);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "features");
    device_json_char(&writer, '[');
    {
        int feature_first = 1;
        static const struct
        {
            const char* name;
            unsigned bit;
        } FEATURES[] = {{"events", 1u << 0}, {"sleep", 1u << 1}, {"status", 1u << 2},
                        {"hud", 1u << 3},    {"overlay", 1u << 4}, {"battery", 1u << 5}};
        size_t index;
        for (index = 0; index < sizeof FEATURES / sizeof FEATURES[0]; index++)
        {
            if ((features & FEATURES[index].bit) == 0) continue;
            if (!feature_first) device_json_char(&writer, ',');
            feature_first = 0;
            device_json_string(&writer, FEATURES[index].name);
        }
    }
    device_json_char(&writer, ']');
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "limits");
    {
        char limits[512];
        if (!device_protocol_write_limits(limits, sizeof limits))
        {
            writer.ok = 0;
            return 0;
        }
        device_json_raw(&writer, limits);
    }

    device_json_prefix(&writer, &first);
    device_json_key(&writer, "session");
    {
        int session_first = 1;
        device_json_char(&writer, '{');
        device_json_prefix(&writer, &session_first);
        device_json_field_bool(&writer, "active",
                               session != NULL && device_session_is_active(session));
        if (session != NULL && device_session_is_active(session))
        {
            char id[32];
            snprintf(id, sizeof id, "%016llx", session->id);
            device_json_prefix(&writer, &session_first);
            device_json_field_string(&writer, "session_id", id);
            device_json_prefix(&writer, &session_first);
            device_json_field_string(&writer, "app", session->app);
            device_json_prefix(&writer, &session_first);
            device_json_field_string(&writer, "pak", session->pak);
            device_json_prefix(&writer, &session_first);
            device_json_field_bool(&writer, "overlay_host", overlay_host);
            device_json_prefix(&writer, &session_first);
            device_json_field_bool(&writer, "overlay_host_declared", overlay_host_declared);
            device_json_prefix(&writer, &session_first);
            device_json_field_bool(&writer, "overlay_open", overlay_open);
            device_json_prefix(&writer, &session_first);
            device_json_key(&writer, "started_at");
            device_json_long(&writer, session->started_at);
            device_json_prefix(&writer, &session_first);
            device_json_field_bool(&writer, "matches_pak", matches_pak);
        }
        device_json_char(&writer, '}');
    }

    if (battery_available && battery != NULL)
    {
        device_json_prefix(&writer, &first);
        device_json_key(&writer, "battery");
        write_battery_object(&writer, battery);
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_ok_response(const char* extra_json, char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    if (extra_json != NULL && extra_json[0] != '\0')
    {
        device_json_char(&writer, ',');
        device_json_raw(&writer, extra_json);
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_call_response(const DeviceCallResult* result, char* out, size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    if (result == NULL)
    {
        device_json_raw(&writer, "{\"ok\":false,\"error\":\"backend_error\","
                            "\"detail\":\"missing result\"}");
        return device_json_finish(&writer);
    }

    device_json_char(&writer, '{');
    if (result->error == DEVICE_GATE_OK)
    {
        device_json_prefix(&writer, &first);
        device_json_field_bool(&writer, "ok", 1);
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "capability", result->capability);
        device_json_prefix(&writer, &first);
        device_json_field_bool(&writer, "available", result->available);
        device_json_prefix(&writer, &first);
        device_json_field_bool(&writer, "authorized", result->authorized);
        device_json_prefix(&writer, &first);
        device_json_key(&writer, "value");
        device_json_raw(&writer, result->value[0] != '\0' ? result->value : "null");
    }
    else
    {
        device_json_prefix(&writer, &first);
        device_json_field_bool(&writer, "ok", 0);
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "error", device_gate_error_name(result->error));
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "capability", result->capability);
        device_json_prefix(&writer, &first);
        device_json_field_bool(&writer, "supported", result->supported);
        device_json_prefix(&writer, &first);
        device_json_field_bool(&writer, "declared", result->declared);
        device_json_prefix(&writer, &first);
        device_json_field_bool(&writer, "available", result->available);
        device_json_prefix(&writer, &first);
        device_json_field_bool(&writer, "authorized", result->authorized);
        if (result->detail[0] != '\0')
        {
            device_json_prefix(&writer, &first);
            device_json_field_string(&writer, "detail", result->detail);
        }
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

static const char* manifest_status_name(PakManifestStatus status)
{
    switch (status)
    {
    case PAK_MANIFEST_ABSENT:
        return "absent";
    case PAK_MANIFEST_OK:
        return "ok";
    case PAK_MANIFEST_INVALID:
        return "invalid";
    }
    return "invalid";
}

int device_protocol_write_status_response(const DeviceStatusResult* status, char* out,
                                          size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;
    int i;

    device_json_init(&writer, out, out_size);
    if (status == NULL)
    {
        device_json_raw(&writer, "{\"ok\":false,\"error\":\"bad_request\","
                            "\"detail\":\"missing status\"}");
        return device_json_finish(&writer);
    }
    if (status->error != DEVICE_GATE_OK)
    {
        device_json_char(&writer, '{');
        device_json_prefix(&writer, &first);
        device_json_field_bool(&writer, "ok", 0);
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "error", device_gate_error_name(status->error));
        if (status->detail[0] != '\0')
        {
            device_json_prefix(&writer, &first);
            device_json_field_string(&writer, "detail", status->detail);
        }
        device_json_char(&writer, '}');
        return device_json_finish(&writer);
    }

    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 1);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "app", status->app);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "pak", status->pak);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "manifest", manifest_status_name(status->manifest_status));
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "grants", device_grants_status_name(status->grants_status));
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "identity",
                        device_identity_status_name(status->identity_status));
    if (status->identity_detail[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "identity_detail", status->identity_detail);
    }
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "backend", status->backend_name != NULL ? status->backend_name
                                                                         : "none");

    device_json_prefix(&writer, &first);
    device_json_key(&writer, "capabilities");
    device_json_char(&writer, '[');
    for (i = 0; i < status->capability_count; i++)
    {
        const DeviceCapabilityStatus* entry = &status->capabilities[i];
        int entry_first = 1;

        if (i > 0) device_json_char(&writer, ',');
        device_json_char(&writer, '{');
        device_json_prefix(&writer, &entry_first);
        device_json_field_string(&writer, "name", entry->name);
        device_json_prefix(&writer, &entry_first);
        device_json_field_bool(&writer, "supported", entry->supported);
        device_json_prefix(&writer, &entry_first);
        device_json_field_bool(&writer, "grant_required", entry->grant_required);
        device_json_prefix(&writer, &entry_first);
        device_json_field_bool(&writer, "declared", entry->declared);
        device_json_prefix(&writer, &entry_first);
        device_json_field_bool(&writer, "available", entry->available);
        device_json_prefix(&writer, &entry_first);
        device_json_field_bool(&writer, "authorized", entry->authorized);
        device_json_char(&writer, '}');
    }
    device_json_char(&writer, ']');
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_session_response(const DeviceSession* session,
                                           DeviceSessionResult result, int cleaned,
                                           const char* error, const char* detail, char* out,
                                           size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;
    int field;
    unsigned long long id;
    const char* effective_error = error;
    const char* effective_detail = detail;
    int ok;

    device_json_init(&writer, out, out_size);
    if (session == NULL)
    {
        device_json_raw(&writer, "{\"ok\":false,\"error\":\"bad_request\","
                            "\"detail\":\"missing session\"}");
        return device_json_finish(&writer);
    }

    if (effective_error == NULL && result != DEVICE_SESSION_OK
        && result != DEVICE_SESSION_NOT_ACTIVE)
        effective_error = device_session_result_name(result);
    ok = effective_error == NULL;
    if (effective_detail == NULL) effective_detail = session->detail;
    id = session->active ? session->id
                         : (session->last_id != 0 ? session->last_id : session->id);

    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", ok);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "session", session->active ? "active" : "inactive");
    if (id != 0)
    {
        device_json_prefix(&writer, &first);
        device_json_key(&writer, "session_id");
        device_json_char(&writer, '"');
        device_json_session_id(&writer, id);
        device_json_char(&writer, '"');
    }
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "app", session->app);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "pak", session->pak);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "started_at");
    device_json_long(&writer, session->started_at);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "result", device_session_result_name(result));
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "last_result",
                        device_session_result_name(session->last_result));
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "cleaned", cleaned);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "exit_requested", session->exit_requested);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "exit");
    device_json_char(&writer, '{');
    {
        int exit_first = 1;
        device_json_prefix(&writer, &exit_first);
        device_json_field_bool(&writer, "requested", session->exit_requested);
        device_json_prefix(&writer, &exit_first);
        device_json_field_bool(&writer, "completed", session->exit_completed);
        device_json_prefix(&writer, &exit_first);
        device_json_field_bool(&writer, "forced", session->exit_forced);
        if (session->exit_action[0] != '\0')
        {
            device_json_prefix(&writer, &exit_first);
            device_json_field_string(&writer, "action", session->exit_action);
        }
        if (session->exit_requested_at_ms != 0)
        {
            device_json_prefix(&writer, &exit_first);
            device_json_key(&writer, "requested_at_ms");
            device_json_ull(&writer, session->exit_requested_at_ms);
        }
        if (session->exit_deadline_ms != 0)
        {
            device_json_prefix(&writer, &exit_first);
            device_json_key(&writer, "deadline_ms");
            device_json_ull(&writer, session->exit_deadline_ms);
        }
        if (session->exit_result[0] != '\0')
        {
            device_json_prefix(&writer, &exit_first);
            device_json_field_string(&writer, "result", session->exit_result);
        }
        if (session->exit_detail[0] != '\0')
        {
            device_json_prefix(&writer, &exit_first);
            device_json_field_string(&writer, "detail", session->exit_detail);
        }
    }
    device_json_char(&writer, '}');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "overlay_host", session->overlay_host);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "restored");
    device_json_int(&writer, session->restored_count);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "skipped");
    device_json_int(&writer, session->skipped_count);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "failed");
    device_json_int(&writer, session->failed_count);

    device_json_prefix(&writer, &first);
    device_json_key(&writer, "led");
    device_json_char(&writer, '{');
    {
        int led_first = 1;
        device_json_prefix(&writer, &led_first);
        device_json_field_string(&writer, "state", device_session_led_state_name(session->led_state));
        device_json_prefix(&writer, &led_first);
        device_json_field_bool(&writer, "used", session->led_used);
        if (session->led_detail[0] != '\0')
        {
            device_json_prefix(&writer, &led_first);
            device_json_field_string(&writer, "detail", session->led_detail);
        }
    }
    device_json_char(&writer, '}');

    device_json_prefix(&writer, &first);
    device_json_key(&writer, "fields");
    device_json_char(&writer, '[');
    for (field = 0; field < DEVICE_SESSION_FIELD_COUNT; field++)
    {
        const DeviceSessionFieldState* state = &session->fields[field];
        int field_first = 1;

        if (field > 0) device_json_char(&writer, ',');
        device_json_char(&writer, '{');
        device_json_prefix(&writer, &field_first);
        device_json_field_string(&writer, "name", device_session_field_name((DeviceSessionField)field));
        device_json_prefix(&writer, &field_first);
        device_json_field_bool(&writer, "available", state->available);
        device_json_prefix(&writer, &field_first);
        device_json_field_bool(&writer, "captured", state->captured);
        device_json_prefix(&writer, &field_first);
        device_json_field_bool(&writer, "changed", state->changed);
        device_json_prefix(&writer, &field_first);
        device_json_field_bool(&writer, "restored", state->restored);
        device_json_prefix(&writer, &field_first);
        device_json_field_bool(&writer, "skipped", state->skipped);
        if (state->captured)
        {
            device_json_prefix(&writer, &field_first);
            device_json_key(&writer, "value");
            device_json_int(&writer, state->value);
        }
        if (state->detail[0] != '\0')
        {
            device_json_prefix(&writer, &field_first);
            device_json_field_string(&writer, "detail", state->detail);
        }
        device_json_char(&writer, '}');
    }
    device_json_char(&writer, ']');

    if (!ok)
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "error", effective_error);
    }
    if (effective_detail != NULL && effective_detail[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "detail", effective_detail);
    }

    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_app_action_response(const DeviceSession* session,
                                              DeviceSessionResult result, const char* action,
                                              int handled_by_app, int handled_by_system,
                                              const char* error, const char* detail, char* out,
                                              size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;
    int ok;
    unsigned long long id;

    device_json_init(&writer, out, out_size);
    if (session == NULL)
    {
        device_json_raw(&writer, "{\"ok\":false,\"error\":\"bad_request\","
                            "\"detail\":\"missing session\"}");
        return device_json_finish(&writer);
    }

    ok = error == NULL && (result == DEVICE_SESSION_OK
                           || result == DEVICE_SESSION_APP_HANDLED);
    id = session->active ? session->id
                         : (session->last_id != 0 ? session->last_id : session->id);

    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", ok);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "service", "devserv");
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "session", session->active ? "active" : "inactive");
    if (id != 0)
    {
        device_json_prefix(&writer, &first);
        device_json_key(&writer, "session_id");
        device_json_char(&writer, '"');
        device_json_session_id(&writer, id);
        device_json_char(&writer, '"');
    }
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "app", session->app);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "action", action != NULL ? action : "");
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "handled_by_app", handled_by_app);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "handled_by_system", handled_by_system);
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "exit_requested", session->exit_requested);
    device_json_prefix(&writer, &first);
    device_json_key(&writer, "exit");
    device_json_char(&writer, '{');
    {
        int exit_first = 1;
        device_json_prefix(&writer, &exit_first);
        device_json_field_bool(&writer, "requested", session->exit_requested);
        device_json_prefix(&writer, &exit_first);
        device_json_field_bool(&writer, "completed", session->exit_completed);
        device_json_prefix(&writer, &exit_first);
        device_json_field_bool(&writer, "forced", session->exit_forced);
        if (session->exit_action[0] != '\0')
        {
            device_json_prefix(&writer, &exit_first);
            device_json_field_string(&writer, "action", session->exit_action);
        }
        if (session->exit_deadline_ms != 0)
        {
            device_json_prefix(&writer, &exit_first);
            device_json_key(&writer, "deadline_ms");
            device_json_ull(&writer, session->exit_deadline_ms);
        }
        if (session->exit_detail[0] != '\0')
        {
            device_json_prefix(&writer, &exit_first);
            device_json_field_string(&writer, "detail", session->exit_detail);
        }
    }
    device_json_char(&writer, '}');
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "result", device_session_result_name(result));
    if (!ok)
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "error",
                            error != NULL ? error : device_session_result_name(result));
    }
    if (detail != NULL && detail[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "detail", detail);
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_write_error_response(const char* error, const char* detail, char* out,
                                         size_t out_size)
{
    DeviceJsonWriter writer;
    int first = 1;

    device_json_init(&writer, out, out_size);
    device_json_char(&writer, '{');
    device_json_prefix(&writer, &first);
    device_json_field_bool(&writer, "ok", 0);
    device_json_prefix(&writer, &first);
    device_json_field_string(&writer, "error", error != NULL ? error : "bad_request");
    if (detail != NULL && detail[0] != '\0')
    {
        device_json_prefix(&writer, &first);
        device_json_field_string(&writer, "detail", detail);
    }
    device_json_char(&writer, '}');
    return device_json_finish(&writer);
}

int device_protocol_response_ok(const char* line)
{
    JsonLiteCursor cursor;
    int found = 0;
    int value = 0;
    size_t length;

    if (line == NULL) return -1;
    length = strlen(line);
    if (length == 0) return -1;

    json_lite_init(&cursor, line, length);
    json_lite_skip_ws(&cursor);
    if (cursor.p >= cursor.end || *cursor.p != '{') return -1;
    cursor.p++;
    json_lite_skip_ws(&cursor);

    if (cursor.p < cursor.end && *cursor.p == '}')
    {
        return -1;
    }

    for (;;)
    {
        char key[16];
        size_t key_length = 0;

        if (!json_lite_parse_string(&cursor, key, sizeof key, &key_length)) return -1;
        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end || *cursor.p != ':') return -1;
        cursor.p++;
        json_lite_skip_ws(&cursor);

        if (key_length < sizeof key && strcmp(key, "ok") == 0 && !found)
        {
            found = 1;
            if (strncmp(cursor.p, "true", 4) == 0)
            {
                value = 1;
                cursor.p += 4;
            }
            else if (strncmp(cursor.p, "false", 5) == 0)
            {
                value = 0;
                cursor.p += 5;
            }
            else
            {
                return -1;
            }
        }
        else if (!json_lite_skip_value(&cursor, 2))
        {
            return -1;
        }

        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end) return -1;
        if (*cursor.p == '}')
        {
            cursor.p++;
            break;
        }
        if (*cursor.p != ',') return -1;
        cursor.p++;
        json_lite_skip_ws(&cursor);
    }

    json_lite_skip_ws(&cursor);
    if (cursor.p != cursor.end || !found) return -1;
    return value;
}
