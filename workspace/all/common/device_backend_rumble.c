#include "device_backend_rumble.h"

#include "json_lite.h"

#include <stdio.h>
#include <string.h>

#define RUMBLE_BACKEND_NAME "nextui-rumble"
#define RUMBLE_KEY_MAX 32

/*
 * The production ops live in device_backend_rumble_platform.c. Keeping the
 * reference weak lets the host adapter tests link without a PLAT_setRumble;
 * when it is absent this module reports unavailable, never success.
 */
#if defined(__GNUC__)
extern const RumbleOps* device_backend_rumble_platform_ops(void)
    __attribute__((weak));
#define RUMBLE_HAS_PLATFORM_OPS 1
#else
#define RUMBLE_HAS_PLATFORM_OPS 0
#endif

typedef struct
{
    char* out;
    size_t size;
    size_t length;
    int ok;
} Writer;

static const RumbleOps* active_ops = NULL;
static void* active_context = NULL;
static const DeviceBackend* composed_base = NULL;
static const DeviceBackend* composed_self = NULL;
static char composed_name[128];
/* The nodes report on/off and a raw level, never the strength that produced
 * them, so the adapter keeps the strength it last wrote for itself. It is
 * reported as the service's record, never as a hardware read. */
static int applied_strength = 0;
static int has_applied = 0;

static void set_detail(char* detail, size_t detail_size, const char* text)
{
    if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "%s", text);
}

static void reset_state(void)
{
    active_ops = NULL;
    active_context = NULL;
    composed_base = NULL;
    composed_self = NULL;
    applied_strength = 0;
    has_applied = 0;
}

static void writer_init(Writer* writer, char* out, size_t size)
{
    writer->out = out;
    writer->size = size;
    writer->length = 0;
    writer->ok = out != NULL && size > 0;
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
    char escaped[96];

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

static void writer_key(Writer* writer, const char* key)
{
    writer_string(writer, key);
    writer_char(writer, ':');
}

static void writer_int(Writer* writer, int value)
{
    char buffer[32];

    snprintf(buffer, sizeof buffer, "%d", value);
    writer_raw(writer, buffer);
}

static void writer_bool(Writer* writer, int value)
{
    writer_raw(writer, value ? "true" : "false");
}

static int writer_finish(Writer* writer)
{
    return writer->ok;
}

static int parse_object_start(const char* args, JsonLiteCursor* cursor)
{
    if (args == NULL || args[0] == '\0') return 0;
    json_lite_init(cursor, args, strlen(args));
    json_lite_skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p != '{') return 0;
    cursor->p++;
    json_lite_skip_ws(cursor);
    /* An empty object never carries a value. */
    if (cursor->p < cursor->end && *cursor->p == '}') return 0;
    return 1;
}

static int parse_key(JsonLiteCursor* cursor, char* key, size_t key_size)
{
    size_t length = 0;

    if (!json_lite_parse_string(cursor, key, key_size, &length)) return 0;
    if (length == 0 || length >= key_size) return 0;
    json_lite_skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p != ':') return 0;
    cursor->p++;
    json_lite_skip_ws(cursor);
    return 1;
}

static int parse_object_end(JsonLiteCursor* cursor)
{
    json_lite_skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p != '}') return 0;
    cursor->p++;
    json_lite_skip_ws(cursor);
    return cursor->p == cursor->end;
}

/*
 * Strict single-key object parser: {"value":N}. Anything else is rejected
 * before the platform is touched, so a typo cannot become a hardware write.
 */
static int parse_value_args(const char* args, long* value)
{
    JsonLiteCursor cursor;
    char key[RUMBLE_KEY_MAX];

    if (value == NULL) return 0;
    if (!parse_object_start(args, &cursor)) return 0;
    if (!parse_key(&cursor, key, sizeof key)) return 0;
    if (strcmp(key, "value") != 0) return 0;
    if (!json_lite_parse_integer(&cursor, value)) return 0;
    return parse_object_end(&cursor);
}

static int ops_available(char* detail, size_t detail_size)
{
    if (active_ops == NULL || active_ops->available == NULL)
    {
        set_detail(detail, detail_size, "rumble backend is not configured");
        return 0;
    }
    return active_ops->available(active_context, detail, detail_size);
}

static int ops_read(RumbleState* state, char* detail, size_t detail_size)
{
    if (active_ops == NULL || active_ops->read == NULL)
    {
        set_detail(detail, detail_size, "rumble backend is not configured");
        return 0;
    }
    return active_ops->read(active_context, state, detail, detail_size);
}

static int ops_write(int strength, char* detail, size_t detail_size)
{
    if (active_ops == NULL || active_ops->write == NULL)
    {
        set_detail(detail, detail_size, "rumble backend is not configured");
        return 0;
    }
    return active_ops->write(active_context, strength, detail, detail_size);
}

static void write_state(Writer* writer, const RumbleState* state)
{
    writer_key(writer, "enabled");
    writer_bool(writer, state->enabled != 0);
    writer_char(writer, ',');
    writer_key(writer, "level");
    if (state->level_read)
    {
        char buffer[32];
        snprintf(buffer, sizeof buffer, "%ld", state->level);
        writer_raw(writer, buffer);
    }
    else
    {
        writer_raw(writer, "null");
    }
    writer_char(writer, ',');
    writer_key(writer, "level_unit");
    if (active_ops != NULL && active_ops->level_unit != NULL)
        writer_string(writer, active_ops->level_unit);
    else
        writer_raw(writer, "null");
}

static void write_range(Writer* writer)
{
    writer_key(writer, "min");
    writer_int(writer, RUMBLE_STRENGTH_MIN);
    writer_char(writer, ',');
    writer_key(writer, "max");
    writer_int(writer, RUMBLE_STRENGTH_MAX);
}

static int get_rumble(char* value, size_t value_size, char* detail, size_t detail_size)
{
    Writer writer;
    RumbleState state;

    if (!ops_available(detail, detail_size)) return 0;
    memset(&state, 0, sizeof state);
    state.enabled = -1;
    if (!ops_read(&state, detail, detail_size)) return 0;

    writer_init(&writer, value, value_size);
    writer_char(&writer, '{');
    writer_key(&writer, "strength");
    writer_int(&writer, applied_strength);
    writer_char(&writer, ',');
    writer_key(&writer, "applied");
    writer_bool(&writer, has_applied);
    writer_char(&writer, ',');
    write_state(&writer, &state);
    writer_char(&writer, ',');
    write_range(&writer);
    writer_char(&writer, ',');
    writer_key(&writer, "hardware_ack");
    writer_bool(&writer, 0);
    writer_char(&writer, '}');
    if (!writer_finish(&writer))
    {
        set_detail(detail, detail_size, "rumble response is too large");
        return 0;
    }
    return 1;
}

static int set_rumble(const char* args, char* value, size_t value_size, char* detail,
                      size_t detail_size)
{
    Writer writer;
    RumbleState state;
    long requested;
    int previous;
    int readback;

    if (!parse_value_args(args, &requested))
    {
        char text[128];
        snprintf(text, sizeof text, "input.rumble set expects {\"value\":%d..%d}",
                 RUMBLE_STRENGTH_MIN, RUMBLE_STRENGTH_MAX);
        set_detail(detail, detail_size, text);
        return 0;
    }
    if (requested < RUMBLE_STRENGTH_MIN || requested > RUMBLE_STRENGTH_MAX)
    {
        char text[128];
        snprintf(text, sizeof text, "rumble strength %ld is outside %d..%d", requested,
                 RUMBLE_STRENGTH_MIN, RUMBLE_STRENGTH_MAX);
        set_detail(detail, detail_size, text);
        return 0;
    }
    if (!ops_available(detail, detail_size)) return 0;

    previous = applied_strength;
    if (!ops_write((int)requested, detail, detail_size)) return 0;

    memset(&state, 0, sizeof state);
    state.enabled = -1;
    if (!ops_read(&state, detail, detail_size)) return 0;
    /* PLAT_setRumble returns void and ignores a failed write, so the nodes are
     * the only evidence the call took effect. A motor that is off after a
     * non-zero request (or on after a zero request) is a failure, never a
     * success with a hopeful response. */
    readback = state.enabled == (requested != 0);
    if (!readback)
    {
        char text[192];
        snprintf(text, sizeof text,
                 "rumble write was accepted but the enable node reads %s after strength %ld",
                 state.enabled != 0 ? "on" : "off", requested);
        set_detail(detail, detail_size, text);
        return 0;
    }

    applied_strength = (int)requested;
    has_applied = 1;

    writer_init(&writer, value, value_size);
    writer_char(&writer, '{');
    writer_key(&writer, "strength");
    writer_int(&writer, (int)requested);
    writer_char(&writer, ',');
    writer_key(&writer, "previous");
    writer_int(&writer, previous);
    writer_char(&writer, ',');
    write_state(&writer, &state);
    writer_char(&writer, ',');
    write_range(&writer);
    writer_char(&writer, ',');
    writer_key(&writer, "readback");
    writer_bool(&writer, readback);
    writer_char(&writer, ',');
    writer_key(&writer, "hardware_ack");
    writer_bool(&writer, 0);
    writer_char(&writer, '}');
    if (!writer_finish(&writer))
    {
        set_detail(detail, detail_size, "rumble response is too large");
        return 0;
    }
    return 1;
}

static int backend_available(void* context, const char* capability, char* detail,
                             size_t detail_size)
{
    (void)context;

    if (capability == NULL)
    {
        set_detail(detail, detail_size, "capability is required");
        return 0;
    }
    if (strcmp(capability, "input.rumble") != 0)
    {
        set_detail(detail, detail_size, "not a rumble capability");
        return 0;
    }
    return ops_available(detail, detail_size);
}

static int backend_call(void* context, const char* capability, const char* action,
                        const char* args, char* value, size_t value_size, char* detail,
                        size_t detail_size)
{
    const char* verb = action != NULL && action[0] != '\0' ? action : "get";

    (void)context;

    if (capability == NULL)
    {
        set_detail(detail, detail_size, "capability is required");
        return 0;
    }
    if (strcmp(capability, "input.rumble") != 0)
    {
        set_detail(detail, detail_size, "not a rumble capability");
        return 0;
    }
    if (strcmp(verb, "get") == 0) return get_rumble(value, value_size, detail, detail_size);
    if (strcmp(verb, "set") == 0) return set_rumble(args, value, value_size, detail, detail_size);

    {
        char text[128];
        snprintf(text, sizeof text, "unsupported %s action", capability);
        set_detail(detail, detail_size, text);
    }
    return 0;
}

static const DeviceBackend backend = {
    RUMBLE_BACKEND_NAME,
    NULL,
    backend_available,
    backend_call,
};

/*
 * An existing backend keeps any capability it already reports available. This
 * adapter fills the rumble capability the base does not implement, so composing
 * it can never silently replace another backend's working path or its tests.
 */
static int base_handles(const char* capability)
{
    char ignored[128];

    if (composed_base == NULL || composed_base->available == NULL) return 0;
    return composed_base->available(composed_base->context, capability, ignored,
                                    sizeof ignored);
}

static int composed_available(void* context, const char* capability, char* detail,
                              size_t detail_size)
{
    (void)context;

    if (base_handles(capability))
    {
        set_detail(detail, detail_size, "served by the composed base backend");
        return 1;
    }
    if (composed_self != NULL && strcmp(capability, "input.rumble") == 0)
        return composed_self->available(composed_self->context, capability, detail, detail_size);
    if (composed_base != NULL)
        return composed_base->available(composed_base->context, capability, detail, detail_size);
    set_detail(detail, detail_size, "no hardware backend wired");
    return 0;
}

static int composed_call(void* context, const char* capability, const char* action,
                         const char* args, char* value, size_t value_size, char* detail,
                         size_t detail_size)
{
    (void)context;

    if (base_handles(capability))
        return composed_base->call(composed_base->context, capability, action, args, value,
                                   value_size, detail, detail_size);
    if (composed_self != NULL && strcmp(capability, "input.rumble") == 0)
        return composed_self->call(composed_self->context, capability, action, args, value,
                                   value_size, detail, detail_size);
    if (composed_base != NULL)
        return composed_base->call(composed_base->context, capability, action, args, value,
                                   value_size, detail, detail_size);
    set_detail(detail, detail_size, "no hardware backend wired");
    return 0;
}

static DeviceBackend composed_backend = {
    composed_name,
    NULL,
    composed_available,
    composed_call,
};

const DeviceBackend* device_backend_rumble_with_ops(const RumbleOps* ops, void* context)
{
    reset_state();
    active_ops = ops;
    active_context = context;
    composed_self = &backend;
    return &backend;
}

const DeviceBackend* device_backend_rumble(void)
{
#if RUMBLE_HAS_PLATFORM_OPS
    if (device_backend_rumble_platform_ops != NULL)
        return device_backend_rumble_with_ops(device_backend_rumble_platform_ops(), NULL);
#endif
    return device_backend_none();
}

const DeviceBackend* device_backend_rumble_compose(const DeviceBackend* base)
{
    const DeviceBackend* self = composed_self;

    if (self == NULL) self = device_backend_rumble();
    composed_self = self;
    composed_base = base;
    snprintf(composed_name, sizeof composed_name, "%s+%s",
             self->name != NULL ? self->name : RUMBLE_BACKEND_NAME,
             base != NULL && base->name != NULL ? base->name : "none");
    composed_backend.name = composed_name;
    return &composed_backend;
}

void device_backend_rumble_reset(void)
{
    reset_state();
}
