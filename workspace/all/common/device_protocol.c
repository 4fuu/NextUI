#include "device_protocol.h"

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
} Writer;

static void writer_init(Writer* writer, char* out, size_t out_size)
{
    writer->out = out;
    writer->size = out_size;
    writer->length = 0;
    writer->ok = out != NULL && out_size > 0;
    if (writer->ok) out[0] = '\0';
}

static void writer_raw(Writer* writer, const char* text)
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

static void writer_char(Writer* writer, char ch)
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

static void writer_string(Writer* writer, const char* text)
{
    char escaped[DEVICE_PROTOCOL_LINE_MAX];

    if (!writer->ok) return;
    if (!json_lite_escape(text != NULL ? text : "", escaped, sizeof escaped))
    {
        writer->ok = 0;
        return;
    }
    writer_char(writer, '"');
    writer_raw(writer, escaped);
    writer_char(writer, '"');
}

static void writer_bool(Writer* writer, int value)
{
    writer_raw(writer, value ? "true" : "false");
}

static void writer_int(Writer* writer, int value)
{
    char text[32];

    snprintf(text, sizeof text, "%d", value);
    writer_raw(writer, text);
}

static void writer_long(Writer* writer, long value)
{
    char text[32];

    snprintf(text, sizeof text, "%ld", value);
    writer_raw(writer, text);
}

static void writer_session_id(Writer* writer, unsigned long long value)
{
    char text[32];

    snprintf(text, sizeof text, "%016llx", value);
    writer_raw(writer, text);
}

static void writer_ull(Writer* writer, unsigned long long value)
{
    char text[32];

    snprintf(text, sizeof text, "%llu", value);
    writer_raw(writer, text);
}

static void writer_key(Writer* writer, const char* key)
{
    writer_string(writer, key);
    writer_char(writer, ':');
}

static void writer_prefix(Writer* writer, int* first)
{
    if (!*first) writer_char(writer, ',');
    *first = 0;
}

static void writer_field_string(Writer* writer, const char* key, const char* value)
{
    writer_key(writer, key);
    writer_string(writer, value);
}

static void writer_field_bool(Writer* writer, const char* key, int value)
{
    writer_key(writer, key);
    writer_bool(writer, value);
}

static int writer_finish(Writer* writer)
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

int device_protocol_parse_request(const char* line, DeviceRequestMessage* message)
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
    };
    JsonLiteCursor cursor;
    char op[16];
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
                if (cursor.p + 4 <= cursor.end && strncmp(cursor.p, "true", 4) == 0)
                {
                    message->forced = 1;
                    message->has_forced = 1;
                    cursor.p += 4;
                }
                else if (cursor.p + 5 <= cursor.end
                         && strncmp(cursor.p, "false", 5) == 0)
                {
                    message->forced = 0;
                    message->has_forced = 1;
                    cursor.p += 5;
                }
                else
                {
                    return 0;
                }
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
    return 0;
}

int device_protocol_build_request(const char* op, const char* app, const char* pak,
                                  const char* capability, const char* action, const char* args,
                                  char* out, size_t out_size)
{
    Writer writer;
    int first = 1;

    writer_init(&writer, out, out_size);
    writer_char(&writer, '{');
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "op", op != NULL ? op : "");
    if (app != NULL && app[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "app", app);
    }
    if (pak != NULL && pak[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "pak", pak);
    }
    if (capability != NULL && capability[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "capability", capability);
    }
    if (action != NULL && action[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "action", action);
    }
    if (args != NULL && args[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_key(&writer, "args");
        writer_raw(&writer, args);
    }
    writer_char(&writer, '}');
    return writer_finish(&writer);
}

int device_protocol_build_session_request(const char* op, const char* app, const char* pak,
                                          const char* session_id, char* out, size_t out_size)
{
    Writer writer;
    int first = 1;

    writer_init(&writer, out, out_size);
    writer_char(&writer, '{');
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "op", op != NULL ? op : "");
    if (app != NULL && app[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "app", app);
    }
    if (pak != NULL && pak[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "pak", pak);
    }
    if (session_id != NULL && session_id[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "session_id", session_id);
    }
    writer_char(&writer, '}');
    return writer_finish(&writer);
}

int device_protocol_build_app_action_request(const char* action, unsigned long long at_ms,
                                             const char* session_id, char* out,
                                             size_t out_size)
{
    Writer writer;
    int first = 1;
    char timestamp[32];

    writer_init(&writer, out, out_size);
    writer_char(&writer, '{');
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "op", "app-action");
    if (action != NULL && action[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "action", action);
    }
    if (at_ms != 0)
    {
        snprintf(timestamp, sizeof timestamp, "%llu", at_ms);
        writer_prefix(&writer, &first);
        writer_key(&writer, "at_ms");
        writer_raw(&writer, timestamp);
    }
    if (session_id != NULL && session_id[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "session_id", session_id);
    }
    writer_char(&writer, '}');
    return writer_finish(&writer);
}

int device_protocol_build_app_exit_result_request(const char* session_id, int forced,
                                                  char* out, size_t out_size)
{
    Writer writer;
    int first = 1;

    writer_init(&writer, out, out_size);
    writer_char(&writer, '{');
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "op", "app-exit-result");
    if (session_id != NULL && session_id[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "session_id", session_id);
    }
    writer_prefix(&writer, &first);
    writer_field_bool(&writer, "forced", forced);
    writer_char(&writer, '}');
    return writer_finish(&writer);
}

int device_protocol_write_ping_response(char* out, size_t out_size)
{
    Writer writer;

    writer_init(&writer, out, out_size);
    writer_raw(&writer, "{\"ok\":true,\"service\":\"devserv\",\"version\":1}");
    return writer_finish(&writer);
}

int device_protocol_write_call_response(const DeviceCallResult* result, char* out, size_t out_size)
{
    Writer writer;
    int first = 1;

    writer_init(&writer, out, out_size);
    if (result == NULL)
    {
        writer_raw(&writer, "{\"ok\":false,\"error\":\"backend_error\","
                            "\"detail\":\"missing result\"}");
        return writer_finish(&writer);
    }

    writer_char(&writer, '{');
    if (result->error == DEVICE_GATE_OK)
    {
        writer_prefix(&writer, &first);
        writer_field_bool(&writer, "ok", 1);
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "capability", result->capability);
        writer_prefix(&writer, &first);
        writer_field_bool(&writer, "available", result->available);
        writer_prefix(&writer, &first);
        writer_field_bool(&writer, "authorized", result->authorized);
        writer_prefix(&writer, &first);
        writer_key(&writer, "value");
        writer_raw(&writer, result->value[0] != '\0' ? result->value : "null");
    }
    else
    {
        writer_prefix(&writer, &first);
        writer_field_bool(&writer, "ok", 0);
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "error", device_gate_error_name(result->error));
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "capability", result->capability);
        writer_prefix(&writer, &first);
        writer_field_bool(&writer, "supported", result->supported);
        writer_prefix(&writer, &first);
        writer_field_bool(&writer, "declared", result->declared);
        writer_prefix(&writer, &first);
        writer_field_bool(&writer, "available", result->available);
        writer_prefix(&writer, &first);
        writer_field_bool(&writer, "authorized", result->authorized);
        if (result->detail[0] != '\0')
        {
            writer_prefix(&writer, &first);
            writer_field_string(&writer, "detail", result->detail);
        }
    }
    writer_char(&writer, '}');
    return writer_finish(&writer);
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
    Writer writer;
    int first = 1;
    int i;

    writer_init(&writer, out, out_size);
    if (status == NULL)
    {
        writer_raw(&writer, "{\"ok\":false,\"error\":\"bad_request\","
                            "\"detail\":\"missing status\"}");
        return writer_finish(&writer);
    }
    if (status->error != DEVICE_GATE_OK)
    {
        writer_char(&writer, '{');
        writer_prefix(&writer, &first);
        writer_field_bool(&writer, "ok", 0);
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "error", device_gate_error_name(status->error));
        if (status->detail[0] != '\0')
        {
            writer_prefix(&writer, &first);
            writer_field_string(&writer, "detail", status->detail);
        }
        writer_char(&writer, '}');
        return writer_finish(&writer);
    }

    writer_char(&writer, '{');
    writer_prefix(&writer, &first);
    writer_field_bool(&writer, "ok", 1);
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "service", "devserv");
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "app", status->app);
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "pak", status->pak);
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "manifest", manifest_status_name(status->manifest_status));
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "grants", device_grants_status_name(status->grants_status));
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "identity",
                        device_identity_status_name(status->identity_status));
    if (status->identity_detail[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "identity_detail", status->identity_detail);
    }
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "backend", status->backend_name != NULL ? status->backend_name
                                                                         : "none");

    writer_prefix(&writer, &first);
    writer_key(&writer, "capabilities");
    writer_char(&writer, '[');
    for (i = 0; i < status->capability_count; i++)
    {
        const DeviceCapabilityStatus* entry = &status->capabilities[i];
        int entry_first = 1;

        if (i > 0) writer_char(&writer, ',');
        writer_char(&writer, '{');
        writer_prefix(&writer, &entry_first);
        writer_field_string(&writer, "name", entry->name);
        writer_prefix(&writer, &entry_first);
        writer_field_bool(&writer, "supported", entry->supported);
        writer_prefix(&writer, &entry_first);
        writer_field_bool(&writer, "grant_required", entry->grant_required);
        writer_prefix(&writer, &entry_first);
        writer_field_bool(&writer, "declared", entry->declared);
        writer_prefix(&writer, &entry_first);
        writer_field_bool(&writer, "available", entry->available);
        writer_prefix(&writer, &entry_first);
        writer_field_bool(&writer, "authorized", entry->authorized);
        writer_char(&writer, '}');
    }
    writer_char(&writer, ']');
    writer_char(&writer, '}');
    return writer_finish(&writer);
}

int device_protocol_write_session_response(const DeviceSession* session,
                                           DeviceSessionResult result, int cleaned,
                                           const char* error, const char* detail, char* out,
                                           size_t out_size)
{
    Writer writer;
    int first = 1;
    int field;
    unsigned long long id;
    const char* effective_error = error;
    const char* effective_detail = detail;
    int ok;

    writer_init(&writer, out, out_size);
    if (session == NULL)
    {
        writer_raw(&writer, "{\"ok\":false,\"error\":\"bad_request\","
                            "\"detail\":\"missing session\"}");
        return writer_finish(&writer);
    }

    if (effective_error == NULL && result != DEVICE_SESSION_OK
        && result != DEVICE_SESSION_NOT_ACTIVE)
        effective_error = device_session_result_name(result);
    ok = effective_error == NULL;
    if (effective_detail == NULL) effective_detail = session->detail;
    id = session->active ? session->id
                         : (session->last_id != 0 ? session->last_id : session->id);

    writer_char(&writer, '{');
    writer_prefix(&writer, &first);
    writer_field_bool(&writer, "ok", ok);
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "service", "devserv");
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "session", session->active ? "active" : "inactive");
    if (id != 0)
    {
        writer_prefix(&writer, &first);
        writer_key(&writer, "session_id");
        writer_char(&writer, '"');
        writer_session_id(&writer, id);
        writer_char(&writer, '"');
    }
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "app", session->app);
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "pak", session->pak);
    writer_prefix(&writer, &first);
    writer_key(&writer, "started_at");
    writer_long(&writer, session->started_at);
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "result", device_session_result_name(result));
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "last_result",
                        device_session_result_name(session->last_result));
    writer_prefix(&writer, &first);
    writer_field_bool(&writer, "cleaned", cleaned);
    writer_prefix(&writer, &first);
    writer_field_bool(&writer, "exit_requested", session->exit_requested);
    writer_prefix(&writer, &first);
    writer_key(&writer, "exit");
    writer_char(&writer, '{');
    {
        int exit_first = 1;
        writer_prefix(&writer, &exit_first);
        writer_field_bool(&writer, "requested", session->exit_requested);
        writer_prefix(&writer, &exit_first);
        writer_field_bool(&writer, "completed", session->exit_completed);
        writer_prefix(&writer, &exit_first);
        writer_field_bool(&writer, "forced", session->exit_forced);
        if (session->exit_action[0] != '\0')
        {
            writer_prefix(&writer, &exit_first);
            writer_field_string(&writer, "action", session->exit_action);
        }
        if (session->exit_requested_at_ms != 0)
        {
            writer_prefix(&writer, &exit_first);
            writer_key(&writer, "requested_at_ms");
            writer_ull(&writer, session->exit_requested_at_ms);
        }
        if (session->exit_deadline_ms != 0)
        {
            writer_prefix(&writer, &exit_first);
            writer_key(&writer, "deadline_ms");
            writer_ull(&writer, session->exit_deadline_ms);
        }
        if (session->exit_result[0] != '\0')
        {
            writer_prefix(&writer, &exit_first);
            writer_field_string(&writer, "result", session->exit_result);
        }
        if (session->exit_detail[0] != '\0')
        {
            writer_prefix(&writer, &exit_first);
            writer_field_string(&writer, "detail", session->exit_detail);
        }
    }
    writer_char(&writer, '}');
    writer_prefix(&writer, &first);
    writer_field_bool(&writer, "overlay_host", session->overlay_host);
    writer_prefix(&writer, &first);
    writer_key(&writer, "restored");
    writer_int(&writer, session->restored_count);
    writer_prefix(&writer, &first);
    writer_key(&writer, "skipped");
    writer_int(&writer, session->skipped_count);
    writer_prefix(&writer, &first);
    writer_key(&writer, "failed");
    writer_int(&writer, session->failed_count);

    writer_prefix(&writer, &first);
    writer_key(&writer, "led");
    writer_char(&writer, '{');
    {
        int led_first = 1;
        writer_prefix(&writer, &led_first);
        writer_field_string(&writer, "state", device_session_led_state_name(session->led_state));
        writer_prefix(&writer, &led_first);
        writer_field_bool(&writer, "used", session->led_used);
        if (session->led_detail[0] != '\0')
        {
            writer_prefix(&writer, &led_first);
            writer_field_string(&writer, "detail", session->led_detail);
        }
    }
    writer_char(&writer, '}');

    writer_prefix(&writer, &first);
    writer_key(&writer, "fields");
    writer_char(&writer, '[');
    for (field = 0; field < DEVICE_SESSION_FIELD_COUNT; field++)
    {
        const DeviceSessionFieldState* state = &session->fields[field];
        int field_first = 1;

        if (field > 0) writer_char(&writer, ',');
        writer_char(&writer, '{');
        writer_prefix(&writer, &field_first);
        writer_field_string(&writer, "name", device_session_field_name((DeviceSessionField)field));
        writer_prefix(&writer, &field_first);
        writer_field_bool(&writer, "available", state->available);
        writer_prefix(&writer, &field_first);
        writer_field_bool(&writer, "captured", state->captured);
        writer_prefix(&writer, &field_first);
        writer_field_bool(&writer, "changed", state->changed);
        writer_prefix(&writer, &field_first);
        writer_field_bool(&writer, "restored", state->restored);
        writer_prefix(&writer, &field_first);
        writer_field_bool(&writer, "skipped", state->skipped);
        if (state->captured)
        {
            writer_prefix(&writer, &field_first);
            writer_key(&writer, "value");
            writer_int(&writer, state->value);
        }
        if (state->detail[0] != '\0')
        {
            writer_prefix(&writer, &field_first);
            writer_field_string(&writer, "detail", state->detail);
        }
        writer_char(&writer, '}');
    }
    writer_char(&writer, ']');

    if (!ok)
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "error", effective_error);
    }
    if (effective_detail != NULL && effective_detail[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "detail", effective_detail);
    }

    writer_char(&writer, '}');
    return writer_finish(&writer);
}

int device_protocol_write_app_action_response(const DeviceSession* session,
                                              DeviceSessionResult result, const char* action,
                                              int handled_by_app, int handled_by_system,
                                              const char* error, const char* detail, char* out,
                                              size_t out_size)
{
    Writer writer;
    int first = 1;
    int ok;
    unsigned long long id;

    writer_init(&writer, out, out_size);
    if (session == NULL)
    {
        writer_raw(&writer, "{\"ok\":false,\"error\":\"bad_request\","
                            "\"detail\":\"missing session\"}");
        return writer_finish(&writer);
    }

    ok = error == NULL && (result == DEVICE_SESSION_OK
                           || result == DEVICE_SESSION_APP_HANDLED);
    id = session->active ? session->id
                         : (session->last_id != 0 ? session->last_id : session->id);

    writer_char(&writer, '{');
    writer_prefix(&writer, &first);
    writer_field_bool(&writer, "ok", ok);
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "service", "devserv");
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "session", session->active ? "active" : "inactive");
    if (id != 0)
    {
        writer_prefix(&writer, &first);
        writer_key(&writer, "session_id");
        writer_char(&writer, '"');
        writer_session_id(&writer, id);
        writer_char(&writer, '"');
    }
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "app", session->app);
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "action", action != NULL ? action : "");
    writer_prefix(&writer, &first);
    writer_field_bool(&writer, "handled_by_app", handled_by_app);
    writer_prefix(&writer, &first);
    writer_field_bool(&writer, "handled_by_system", handled_by_system);
    writer_prefix(&writer, &first);
    writer_field_bool(&writer, "exit_requested", session->exit_requested);
    writer_prefix(&writer, &first);
    writer_key(&writer, "exit");
    writer_char(&writer, '{');
    {
        int exit_first = 1;
        writer_prefix(&writer, &exit_first);
        writer_field_bool(&writer, "requested", session->exit_requested);
        writer_prefix(&writer, &exit_first);
        writer_field_bool(&writer, "completed", session->exit_completed);
        writer_prefix(&writer, &exit_first);
        writer_field_bool(&writer, "forced", session->exit_forced);
        if (session->exit_action[0] != '\0')
        {
            writer_prefix(&writer, &exit_first);
            writer_field_string(&writer, "action", session->exit_action);
        }
        if (session->exit_deadline_ms != 0)
        {
            writer_prefix(&writer, &exit_first);
            writer_key(&writer, "deadline_ms");
            writer_ull(&writer, session->exit_deadline_ms);
        }
        if (session->exit_detail[0] != '\0')
        {
            writer_prefix(&writer, &exit_first);
            writer_field_string(&writer, "detail", session->exit_detail);
        }
    }
    writer_char(&writer, '}');
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "result", device_session_result_name(result));
    if (!ok)
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "error",
                            error != NULL ? error : device_session_result_name(result));
    }
    if (detail != NULL && detail[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "detail", detail);
    }
    writer_char(&writer, '}');
    return writer_finish(&writer);
}

int device_protocol_write_error_response(const char* error, const char* detail, char* out,
                                         size_t out_size)
{
    Writer writer;
    int first = 1;

    writer_init(&writer, out, out_size);
    writer_char(&writer, '{');
    writer_prefix(&writer, &first);
    writer_field_bool(&writer, "ok", 0);
    writer_prefix(&writer, &first);
    writer_field_string(&writer, "error", error != NULL ? error : "bad_request");
    if (detail != NULL && detail[0] != '\0')
    {
        writer_prefix(&writer, &first);
        writer_field_string(&writer, "detail", detail);
    }
    writer_char(&writer, '}');
    return writer_finish(&writer);
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
