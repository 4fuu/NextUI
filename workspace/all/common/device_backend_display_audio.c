#include "device_backend_display_audio.h"

#include "json_lite.h"

#include <stdio.h>
#include <string.h>

#define DISPLAY_AUDIO_BACKEND_NAME "nextui-display-audio"
#define DISPLAY_AUDIO_KEY_MAX 32

typedef struct
{
    char* out;
    size_t size;
    size_t length;
    int ok;
} Writer;

static const DisplayAudioOps* active_ops = NULL;
static void* active_context = NULL;
static const DeviceBackend* composed_base = NULL;
static const DeviceBackend* composed_self = NULL;
static char composed_name[128];

/*
 * The production msettings ops live in device_backend_display_audio_platform.c.
 * Keeping the reference weak lets the host adapter tests link without
 * msettings; when it is absent this module reports unavailable, never success.
 */
#if defined(__GNUC__)
extern const DisplayAudioOps* device_backend_display_audio_platform_ops(void)
    __attribute__((weak));
#define DISPLAY_AUDIO_HAS_PLATFORM_OPS 1
#else
#define DISPLAY_AUDIO_HAS_PLATFORM_OPS 0
#endif

static void set_detail(char* detail, size_t detail_size, const char* text)
{
    if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "%s", text);
}

static int capability_is_ours(const char* capability)
{
    if (capability == NULL) return 0;
    return strcmp(capability, "display.get") == 0
           || strcmp(capability, "display.brightness") == 0
           || strcmp(capability, "display.colortemp") == 0
           || strcmp(capability, "audio.volume") == 0
           || strcmp(capability, "audio.sink") == 0;
}

static int capability_field(const char* capability, DisplayAudioField* field)
{
    if (capability == NULL || field == NULL) return 0;

    if (strcmp(capability, "display.brightness") == 0) *field = DISPLAY_AUDIO_BRIGHTNESS;
    else if (strcmp(capability, "display.colortemp") == 0) *field = DISPLAY_AUDIO_COLORTEMP;
    else if (strcmp(capability, "audio.volume") == 0) *field = DISPLAY_AUDIO_VOLUME;
    else if (strcmp(capability, "audio.sink") == 0) *field = DISPLAY_AUDIO_SINK;
    else return 0;
    return 1;
}

static const char* field_name(DisplayAudioField field)
{
    switch (field)
    {
    case DISPLAY_AUDIO_BRIGHTNESS: return "brightness";
    case DISPLAY_AUDIO_COLORTEMP: return "colortemp";
    case DISPLAY_AUDIO_VOLUME: return "volume";
    case DISPLAY_AUDIO_MUTE: return "mute";
    case DISPLAY_AUDIO_SINK: return "audio sink";
    default: return "unknown field";
    }
}

static int field_min(DisplayAudioField field)
{
    (void)field;
    return 0;
}

static int field_max(DisplayAudioField field)
{
    switch (field)
    {
    case DISPLAY_AUDIO_BRIGHTNESS: return DISPLAY_AUDIO_BRIGHTNESS_MAX;
    case DISPLAY_AUDIO_COLORTEMP: return DISPLAY_AUDIO_COLORTEMP_MAX;
    case DISPLAY_AUDIO_VOLUME: return DISPLAY_AUDIO_VOLUME_MAX;
    case DISPLAY_AUDIO_MUTE: return DISPLAY_AUDIO_MUTE_MAX;
    case DISPLAY_AUDIO_SINK: return DISPLAY_AUDIO_SINK_MAX;
    default: return -1;
    }
}

static const char* sink_name(int index)
{
    switch (index)
    {
    case DISPLAY_AUDIO_SINK_DEFAULT: return "default";
    case DISPLAY_AUDIO_SINK_BLUETOOTH: return "bluetooth";
    case DISPLAY_AUDIO_SINK_USBDAC: return "usbdac";
    default: return "unknown";
    }
}

static int field_available(DisplayAudioField field, char* detail, size_t detail_size)
{
    if (active_ops == NULL || active_ops->available == NULL)
    {
        set_detail(detail, detail_size, "display/audio backend is not configured");
        return 0;
    }
    return active_ops->available(active_context, field, detail, detail_size);
}

static int field_read(DisplayAudioField field, int* value, char* detail, size_t detail_size)
{
    if (active_ops == NULL || active_ops->read == NULL)
    {
        set_detail(detail, detail_size, "display/audio backend is not configured");
        return 0;
    }
    return active_ops->read(active_context, field, value, detail, detail_size);
}

static int field_write(DisplayAudioField field, int value, char* detail, size_t detail_size)
{
    if (active_ops == NULL || active_ops->write == NULL)
    {
        set_detail(detail, detail_size, "display/audio backend is not configured");
        return 0;
    }
    return active_ops->write(active_context, field, value, detail, detail_size);
}

static int backend_available(void* context, const char* capability, char* detail,
                             size_t detail_size)
{
    DisplayAudioField field;

    (void)context;

    if (capability == NULL)
    {
        set_detail(detail, detail_size, "capability is required");
        return 0;
    }
    if (strcmp(capability, "display.get") == 0)
    {
        if (!field_available(DISPLAY_AUDIO_BRIGHTNESS, detail, detail_size)) return 0;
        return field_available(DISPLAY_AUDIO_COLORTEMP, detail, detail_size);
    }
    if (strcmp(capability, "audio.volume") == 0)
        return field_available(DISPLAY_AUDIO_VOLUME, detail, detail_size);
    if (!capability_field(capability, &field))
    {
        set_detail(detail, detail_size, "not a display or audio capability");
        return 0;
    }
    return field_available(field, detail, detail_size);
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

static void writer_field(Writer* writer, const char* key, int value)
{
    writer_key(writer, key);
    writer_int(writer, value);
}

static void writer_pair_separator(Writer* writer)
{
    writer_char(writer, ',');
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
    char key[DISPLAY_AUDIO_KEY_MAX];

    if (value == NULL) return 0;
    if (!parse_object_start(args, &cursor)) return 0;
    if (!parse_key(&cursor, key, sizeof key)) return 0;
    if (strcmp(key, "value") != 0) return 0;
    if (!json_lite_parse_integer(&cursor, value)) return 0;
    return parse_object_end(&cursor);
}

static int parse_sink_args(const char* args, int* sink)
{
    JsonLiteCursor cursor;
    char key[DISPLAY_AUDIO_KEY_MAX];

    if (sink == NULL) return 0;
    if (!parse_object_start(args, &cursor)) return 0;
    if (!parse_key(&cursor, key, sizeof key)) return 0;

    if (strcmp(key, "value") == 0)
    {
        long number;
        if (!json_lite_parse_integer(&cursor, &number)) return 0;
        if (number < DISPLAY_AUDIO_SINK_MIN || number > DISPLAY_AUDIO_SINK_MAX) return 0;
        *sink = (int)number;
    }
    else if (strcmp(key, "sink") == 0)
    {
        char name[DISPLAY_AUDIO_KEY_MAX];
        size_t length = 0;

        if (!json_lite_parse_string(&cursor, name, sizeof name, &length)) return 0;
        if (strcmp(name, "default") == 0 || strcmp(name, "speaker") == 0)
            *sink = DISPLAY_AUDIO_SINK_DEFAULT;
        else if (strcmp(name, "bluetooth") == 0)
            *sink = DISPLAY_AUDIO_SINK_BLUETOOTH;
        else if (strcmp(name, "usbdac") == 0)
            *sink = DISPLAY_AUDIO_SINK_USBDAC;
        else
            return 0;
    }
    else
    {
        return 0;
    }

    return parse_object_end(&cursor);
}

static int write_level_get(Writer* writer, DisplayAudioField field, int level)
{
    writer_char(writer, '{');
    writer_field(writer, "level", level);
    writer_pair_separator(writer);
    writer_field(writer, "min", field_min(field));
    writer_pair_separator(writer);
    writer_field(writer, "max", field_max(field));
    writer_char(writer, '}');
    return writer_finish(writer);
}

static int write_level_set(Writer* writer, DisplayAudioField field, int level, int previous,
                           int readback)
{
    writer_char(writer, '{');
    writer_field(writer, "level", level);
    writer_pair_separator(writer);
    writer_field(writer, "previous", previous);
    writer_pair_separator(writer);
    writer_field(writer, "min", field_min(field));
    writer_pair_separator(writer);
    writer_field(writer, "max", field_max(field));
    writer_pair_separator(writer);
    writer_field(writer, "readback", readback);
    writer_pair_separator(writer);
    writer_key(writer, "verified");
    writer_string(writer, "settings");
    writer_pair_separator(writer);
    writer_key(writer, "hardware_ack");
    writer_bool(writer, 0);
    writer_char(writer, '}');
    return writer_finish(writer);
}

static int write_mute_set(Writer* writer, int mute, int previous, int readback)
{
    writer_char(writer, '{');
    writer_field(writer, "mute", mute);
    writer_pair_separator(writer);
    writer_field(writer, "previous", previous);
    writer_pair_separator(writer);
    writer_field(writer, "readback", readback);
    writer_pair_separator(writer);
    writer_key(writer, "verified");
    writer_string(writer, "settings");
    writer_pair_separator(writer);
    writer_key(writer, "hardware_ack");
    writer_bool(writer, 0);
    writer_char(writer, '}');
    return writer_finish(writer);
}

static int write_sink_get(Writer* writer, int sink)
{
    writer_char(writer, '{');
    writer_key(writer, "sink");
    writer_string(writer, sink_name(sink));
    writer_pair_separator(writer);
    writer_field(writer, "index", sink);
    writer_pair_separator(writer);
    writer_key(writer, "options");
    writer_raw(writer, "[\"default\",\"bluetooth\",\"usbdac\"]");
    writer_char(writer, '}');
    return writer_finish(writer);
}

static int write_sink_set(Writer* writer, int sink, int previous, int readback)
{
    writer_char(writer, '{');
    writer_key(writer, "sink");
    writer_string(writer, sink_name(sink));
    writer_pair_separator(writer);
    writer_field(writer, "index", sink);
    writer_pair_separator(writer);
    writer_key(writer, "previous");
    writer_string(writer, sink_name(previous));
    writer_pair_separator(writer);
    writer_field(writer, "readback", readback);
    writer_pair_separator(writer);
    writer_key(writer, "verified");
    writer_string(writer, "settings");
    writer_pair_separator(writer);
    writer_key(writer, "hardware_ack");
    writer_bool(writer, 0);
    writer_char(writer, '}');
    return writer_finish(writer);
}

static int write_display_get(Writer* writer, int brightness, int colortemp)
{
    writer_char(writer, '{');
    writer_field(writer, "brightness", brightness);
    writer_pair_separator(writer);
    writer_field(writer, "brightness_min", DISPLAY_AUDIO_BRIGHTNESS_MIN);
    writer_pair_separator(writer);
    writer_field(writer, "brightness_max", DISPLAY_AUDIO_BRIGHTNESS_MAX);
    writer_pair_separator(writer);
    writer_field(writer, "colortemp", colortemp);
    writer_pair_separator(writer);
    writer_field(writer, "colortemp_min", DISPLAY_AUDIO_COLORTEMP_MIN);
    writer_pair_separator(writer);
    writer_field(writer, "colortemp_max", DISPLAY_AUDIO_COLORTEMP_MAX);
    writer_char(writer, '}');
    return writer_finish(writer);
}

static int write_volume_get(Writer* writer, int volume, int mute_available, int mute,
                            int sink_available, int sink)
{
    writer_char(writer, '{');
    writer_field(writer, "level", volume);
    writer_pair_separator(writer);
    writer_field(writer, "min", DISPLAY_AUDIO_VOLUME_MIN);
    writer_pair_separator(writer);
    writer_field(writer, "max", DISPLAY_AUDIO_VOLUME_MAX);
    writer_pair_separator(writer);
    writer_key(writer, "mute_supported");
    writer_bool(writer, mute_available);
    if (mute_available)
    {
        writer_pair_separator(writer);
        writer_field(writer, "mute", mute);
    }
    if (sink_available)
    {
        writer_pair_separator(writer);
        writer_key(writer, "sink");
        writer_string(writer, sink_name(sink));
    }
    writer_char(writer, '}');
    return writer_finish(writer);
}

static int apply_level(DisplayAudioField field, const char* args, char* value, size_t value_size,
                       char* detail, size_t detail_size)
{
    Writer writer;
    long requested;
    int previous;
    int readback;

    if (!parse_value_args(args, &requested))
    {
        char text[128];
        snprintf(text, sizeof text, "%s expects {\"value\":%d..%d}", field_name(field),
                 field_min(field), field_max(field));
        set_detail(detail, detail_size, text);
        return 0;
    }
    if (requested < field_min(field) || requested > field_max(field))
    {
        char text[128];
        snprintf(text, sizeof text, "%s value %ld is outside %d..%d", field_name(field), requested,
                 field_min(field), field_max(field));
        set_detail(detail, detail_size, text);
        return 0;
    }
    if (!field_available(field, detail, detail_size)) return 0;
    if (!field_read(field, &previous, detail, detail_size)) return 0;
    if (!field_write(field, (int)requested, detail, detail_size)) return 0;
    if (!field_read(field, &readback, detail, detail_size)) return 0;
    if (readback != (int)requested)
    {
        char text[192];
        snprintf(text, sizeof text,
                 "%s write was accepted but read back %d instead of %ld; "
                 "msettings writes are void and cannot confirm the hardware",
                 field_name(field), readback, requested);
        set_detail(detail, detail_size, text);
        return 0;
    }

    writer_init(&writer, value, value_size);
    if (!write_level_set(&writer, field, (int)requested, previous, readback))
    {
        set_detail(detail, detail_size, "display/audio response is too large");
        return 0;
    }
    return 1;
}

static int apply_mute(const char* args, char* value, size_t value_size, char* detail,
                      size_t detail_size)
{
    Writer writer;
    long requested;
    int previous;
    int readback;

    if (!parse_value_args(args, &requested))
    {
        set_detail(detail, detail_size, "setmute expects {\"value\":0..1}");
        return 0;
    }
    if (requested < DISPLAY_AUDIO_MUTE_MIN || requested > DISPLAY_AUDIO_MUTE_MAX)
    {
        char text[96];
        snprintf(text, sizeof text, "mute value %ld is outside 0..1", requested);
        set_detail(detail, detail_size, text);
        return 0;
    }
    if (!field_available(DISPLAY_AUDIO_MUTE, detail, detail_size)) return 0;
    if (!field_read(DISPLAY_AUDIO_MUTE, &previous, detail, detail_size)) return 0;
    if (!field_write(DISPLAY_AUDIO_MUTE, (int)requested, detail, detail_size)) return 0;
    if (!field_read(DISPLAY_AUDIO_MUTE, &readback, detail, detail_size)) return 0;
    if (readback != (int)requested)
    {
        char text[160];
        snprintf(text, sizeof text,
                 "mute write was accepted but read back %d instead of %ld", readback,
                 requested);
        set_detail(detail, detail_size, text);
        return 0;
    }

    writer_init(&writer, value, value_size);
    if (!write_mute_set(&writer, (int)requested, previous, readback))
    {
        set_detail(detail, detail_size, "display/audio response is too large");
        return 0;
    }
    return 1;
}

static int apply_sink(const char* args, char* value, size_t value_size, char* detail,
                      size_t detail_size)
{
    Writer writer;
    int requested;
    int previous;
    int readback;

    if (!parse_sink_args(args, &requested))
    {
        set_detail(detail, detail_size,
                   "audio sink expects {\"sink\":\"default|bluetooth|usbdac\"} "
                   "or {\"value\":0..2}");
        return 0;
    }
    if (!field_available(DISPLAY_AUDIO_SINK, detail, detail_size)) return 0;
    if (!field_read(DISPLAY_AUDIO_SINK, &previous, detail, detail_size)) return 0;
    if (!field_write(DISPLAY_AUDIO_SINK, requested, detail, detail_size)) return 0;
    if (!field_read(DISPLAY_AUDIO_SINK, &readback, detail, detail_size)) return 0;
    if (readback != requested)
    {
        char text[160];
        snprintf(text, sizeof text,
                 "audio sink write was accepted but read back %d instead of %d", readback,
                 requested);
        set_detail(detail, detail_size, text);
        return 0;
    }

    writer_init(&writer, value, value_size);
    if (!write_sink_set(&writer, requested, previous, readback))
    {
        set_detail(detail, detail_size, "display/audio response is too large");
        return 0;
    }
    return 1;
}

static int get_display(char* value, size_t value_size, char* detail, size_t detail_size)
{
    Writer writer;
    int brightness;
    int colortemp;

    if (!field_available(DISPLAY_AUDIO_BRIGHTNESS, detail, detail_size)) return 0;
    if (!field_available(DISPLAY_AUDIO_COLORTEMP, detail, detail_size)) return 0;
    if (!field_read(DISPLAY_AUDIO_BRIGHTNESS, &brightness, detail, detail_size)) return 0;
    if (!field_read(DISPLAY_AUDIO_COLORTEMP, &colortemp, detail, detail_size)) return 0;

    writer_init(&writer, value, value_size);
    if (!write_display_get(&writer, brightness, colortemp))
    {
        set_detail(detail, detail_size, "display/audio response is too large");
        return 0;
    }
    return 1;
}

static int get_volume(char* value, size_t value_size, char* detail, size_t detail_size)
{
    Writer writer;
    DisplayAudioField field = DISPLAY_AUDIO_VOLUME;
    int volume;
    int mute_available;
    int mute = 0;
    int sink_available;
    int sink = 0;

    if (!field_available(field, detail, detail_size)) return 0;
    if (!field_read(field, &volume, detail, detail_size)) return 0;

    mute_available = field_available(DISPLAY_AUDIO_MUTE, NULL, 0);
    if (mute_available) mute_available = field_read(DISPLAY_AUDIO_MUTE, &mute, NULL, 0);

    sink_available = field_available(DISPLAY_AUDIO_SINK, NULL, 0);
    if (sink_available) sink_available = field_read(DISPLAY_AUDIO_SINK, &sink, NULL, 0);

    writer_init(&writer, value, value_size);
    if (!write_volume_get(&writer, volume, mute_available, mute, sink_available, sink))
    {
        set_detail(detail, detail_size, "display/audio response is too large");
        return 0;
    }
    return 1;
}

static int get_level(DisplayAudioField field, char* value, size_t value_size, char* detail,
                     size_t detail_size)
{
    Writer writer;
    int current;

    if (!field_available(field, detail, detail_size)) return 0;
    if (!field_read(field, &current, detail, detail_size)) return 0;

    writer_init(&writer, value, value_size);
    if (!write_level_get(&writer, field, current))
    {
        set_detail(detail, detail_size, "display/audio response is too large");
        return 0;
    }
    return 1;
}

static int get_sink(char* value, size_t value_size, char* detail, size_t detail_size)
{
    Writer writer;
    int current;

    if (!field_available(DISPLAY_AUDIO_SINK, detail, detail_size)) return 0;
    if (!field_read(DISPLAY_AUDIO_SINK, &current, detail, detail_size)) return 0;

    writer_init(&writer, value, value_size);
    if (!write_sink_get(&writer, current))
    {
        set_detail(detail, detail_size, "display/audio response is too large");
        return 0;
    }
    return 1;
}

static int backend_call(void* context, const char* capability, const char* action, const char* args,
                        char* value, size_t value_size, char* detail, size_t detail_size)
{
    const char* verb = action != NULL && action[0] != '\0' ? action : "get";
    DisplayAudioField field;

    (void)context;

    if (capability == NULL)
    {
        set_detail(detail, detail_size, "capability is required");
        return 0;
    }

    if (strcmp(capability, "display.get") == 0)
    {
        if (strcmp(verb, "get") != 0)
        {
            set_detail(detail, detail_size, "display.get supports only the get action");
            return 0;
        }
        return get_display(value, value_size, detail, detail_size);
    }

    if (!capability_field(capability, &field))
    {
        set_detail(detail, detail_size, "not a display or audio capability");
        return 0;
    }

    if (strcmp(verb, "get") == 0)
    {
        if (strcmp(capability, "audio.volume") == 0)
            return get_volume(value, value_size, detail, detail_size);
        if (strcmp(capability, "audio.sink") == 0)
            return get_sink(value, value_size, detail, detail_size);
        return get_level(field, value, value_size, detail, detail_size);
    }
    if (strcmp(capability, "audio.volume") == 0 && strcmp(verb, "setmute") == 0)
        return apply_mute(args, value, value_size, detail, detail_size);
    if (strcmp(verb, "set") == 0 && strcmp(capability, "audio.sink") == 0)
        return apply_sink(args, value, value_size, detail, detail_size);
    if (strcmp(verb, "set") == 0) return apply_level(field, args, value, value_size, detail,
                                                     detail_size);

    {
        char text[128];
        snprintf(text, sizeof text, "unsupported %s action", capability);
        set_detail(detail, detail_size, text);
    }
    return 0;
}

static const DeviceBackend backend = {
    DISPLAY_AUDIO_BACKEND_NAME,
    NULL,
    backend_available,
    backend_call,
};

/*
 * An existing backend keeps any capability it already reports available. This
 * adapter fills the display/audio capabilities the base does not implement
 * (the production LED backend implements none), so composing it can never
 * silently replace another backend's working path or its tests.
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
    if (composed_self != NULL && capability_is_ours(capability))
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
    if (composed_self != NULL && capability_is_ours(capability))
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

const DeviceBackend* device_backend_display_audio_with_ops(const DisplayAudioOps* ops,
                                                           void* context)
{
    active_ops = ops;
    active_context = context;
    composed_self = &backend;
    return &backend;
}

const DeviceBackend* device_backend_display_audio(void)
{
#if DISPLAY_AUDIO_HAS_PLATFORM_OPS
    if (device_backend_display_audio_platform_ops != NULL)
        return device_backend_display_audio_with_ops(
            device_backend_display_audio_platform_ops(), NULL);
#endif
    return device_backend_none();
}

const DeviceBackend* device_backend_display_audio_compose(const DeviceBackend* base)
{
    const DeviceBackend* self = composed_self;

    if (self == NULL) self = device_backend_display_audio();
    composed_self = self;
    composed_base = base;
    snprintf(composed_name, sizeof composed_name, "%s+%s",
             self->name != NULL ? self->name : DISPLAY_AUDIO_BACKEND_NAME,
             base != NULL && base->name != NULL ? base->name : "none");
    composed_backend.name = composed_name;
    return &composed_backend;
}

void device_backend_display_audio_reset(void)
{
    active_ops = NULL;
    active_context = NULL;
    composed_base = NULL;
    composed_self = NULL;
}
