#include "device_backend_leds.h"

#include "json_lite.h"

#include <stdio.h>
#include <string.h>

#define LED_BACKEND_NAME "nextui-leds"
#define LED_BACKEND_KEY_MAX 32
#define LED_BACKEND_COLOR_MAX 16

typedef struct
{
    char* out;
    size_t size;
    size_t length;
    int ok;
} Writer;

static LedLease lease;
static const LedLeaseOps* lease_ops = NULL;
static void* lease_context = NULL;
static int lease_ready = 0;

static void reset_state(void)
{
    memset(&lease, 0, sizeof lease);
    lease_ops = NULL;
    lease_context = NULL;
    lease_ready = 0;
}

static int ensure_lease(void)
{
    int count;

    if (lease_ready) return 1;
    if (lease_ops == NULL) return 0;
    count = lease_ops->zone_count != NULL ? lease_ops->zone_count(lease_context) : 0;
    led_lease_init(&lease, lease_ops, lease_context, count);
    lease_ready = 1;
    return 1;
}

static void set_detail(char* detail, size_t detail_size, const char* text)
{
    if (detail != NULL && detail_size > 0) snprintf(detail, detail_size, "%s", text);
}

static int backend_available(void* context, const char* capability, char* detail,
                             size_t detail_size)
{
    (void)context;
    if (lease_ops == NULL)
    {
        set_detail(detail, detail_size, "led backend is not configured");
        return 0;
    }
    if (capability == NULL || strcmp(capability, "led.control") != 0)
    {
        set_detail(detail, detail_size, "not an led capability");
        return 0;
    }
    if (!ensure_lease())
    {
        set_detail(detail, detail_size, "led lease is not configured");
        return 0;
    }
    return led_lease_available(&lease, detail, detail_size);
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
    char escaped[LED_LEASE_NAME_MAX * 6 + 1];

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

static void writer_color(Writer* writer, uint32_t color)
{
    char buffer[16];

    snprintf(buffer, sizeof buffer, "\"#%06X\"", (unsigned)(color & 0xFFFFFFu));
    writer_raw(writer, buffer);
}

static int writer_finish(Writer* writer)
{
    return writer->ok;
}

static void write_summary(Writer* writer, int active, int depth, int zone_count)
{
    writer_char(writer, '{');
    writer_key(writer, "active");
    writer_raw(writer, active ? "true" : "false");
    writer_char(writer, ',');
    writer_key(writer, "depth");
    writer_int(writer, depth);
    writer_char(writer, ',');
    writer_key(writer, "zone_count");
    writer_int(writer, zone_count);
    writer_char(writer, '}');
}

static int write_zone(Writer* writer, const LedLease* state, int index)
{
    LedZoneInfo info;
    const LedZoneState* zone = led_lease_get_zone(state, index);

    if (zone == NULL) return 0;
    memset(&info, 0, sizeof info);
    info.shared_brightness_peer = -1;
    (void)led_lease_zone_info(state, index, &info);

    writer_char(writer, '{');
    writer_key(writer, "zone");
    writer_int(writer, index);
    writer_char(writer, ',');
    writer_key(writer, "name");
    writer_string(writer, info.name);
    writer_char(writer, ',');
    writer_key(writer, "color");
    writer_color(writer, zone->color1);
    writer_char(writer, ',');
    writer_key(writer, "color2");
    writer_color(writer, zone->color2);
    writer_char(writer, ',');
    writer_key(writer, "effect");
    writer_int(writer, zone->effect);
    writer_char(writer, ',');
    writer_key(writer, "speed");
    writer_int(writer, zone->speed);
    writer_char(writer, ',');
    writer_key(writer, "brightness");
    writer_int(writer, zone->brightness);
    writer_char(writer, ',');
    writer_key(writer, "inbrightness");
    writer_int(writer, zone->inbrightness);
    writer_char(writer, ',');
    writer_key(writer, "cycles");
    writer_int(writer, zone->cycles);
    writer_char(writer, ',');
    writer_key(writer, "trigger");
    writer_int(writer, zone->trigger);
    writer_char(writer, ',');
    writer_key(writer, "shared_brightness");
    writer_raw(writer, info.shared_brightness_peer >= 0 ? "true" : "false");
    writer_char(writer, '}');
    return 1;
}

static void write_changed(Writer* writer, const LedZonePatch* patches, int patch_count,
                          unsigned shared)
{
    int i;
    int first = 1;

    writer_key(writer, "changed");
    writer_char(writer, '[');
    for (i = 0; i < patch_count; i++)
    {
        if (!first) writer_char(writer, ',');
        first = 0;
        writer_int(writer, patches[i].index);
    }
    writer_char(writer, ']');

    writer_char(writer, ',');
    writer_key(writer, "shared");
    writer_char(writer, '[');
    first = 1;
    for (i = 0; i < lease.zone_count; i++)
    {
        if ((shared & (1u << i)) == 0) continue;
        if (!first) writer_char(writer, ',');
        first = 0;
        writer_int(writer, i);
    }
    writer_char(writer, ']');
}

static int parse_color(JsonLiteCursor* cursor, uint32_t* out)
{
    char text[LED_BACKEND_COLOR_MAX];
    size_t length = 0;
    const char* p;
    uint32_t value = 0;
    int digits = 0;

    json_lite_skip_ws(cursor);
    if (cursor->p < cursor->end && *cursor->p == '"')
    {
        if (!json_lite_parse_string(cursor, text, sizeof text, &length)) return 0;
        if (length >= sizeof text) return 0;
        p = text;
        if (*p == '#') p++;
        while (*p != '\0')
        {
            int digit;
            char ch = *p++;
            if (ch >= '0' && ch <= '9') digit = ch - '0';
            else if (ch >= 'a' && ch <= 'f') digit = ch - 'a' + 10;
            else if (ch >= 'A' && ch <= 'F') digit = ch - 'A' + 10;
            else return 0;
            if (++digits > 6) return 0;
            value = (value << 4) | (uint32_t)digit;
        }
        if (digits != 6) return 0;
        *out = value;
        return 1;
    }

    {
        long number;
        if (!json_lite_parse_integer(cursor, &number)) return 0;
        if (number < 0 || number > 0xFFFFFF) return 0;
        *out = (uint32_t)number;
    }
    return 1;
}

static int parse_field(JsonLiteCursor* cursor, const char* key, LedZonePatch* patch)
{
    long number;

    if (strcmp(key, "zone") == 0)
    {
        if (!json_lite_parse_integer(cursor, &number)) return 0;
        if (number < 0 || number > 255) return 0;
        patch->index = (int)number;
        return 1;
    }
    if (strcmp(key, "color") == 0 || strcmp(key, "color1") == 0)
    {
        if (!parse_color(cursor, &patch->color1)) return 0;
        patch->has_color1 = 1;
        return 1;
    }
    if (strcmp(key, "color2") == 0)
    {
        if (!parse_color(cursor, &patch->color2)) return 0;
        patch->has_color2 = 1;
        return 1;
    }
    if (strcmp(key, "effect") == 0)
    {
        if (!json_lite_parse_integer(cursor, &number)) return 0;
        if (number < 0 || number > 255) return 0;
        patch->effect = (int)number;
        patch->has_effect = 1;
        return 1;
    }
    if (strcmp(key, "speed") == 0)
    {
        if (!json_lite_parse_integer(cursor, &number)) return 0;
        if (number < 0 || number > 60000) return 0;
        patch->speed = (int)number;
        patch->has_speed = 1;
        return 1;
    }
    if (strcmp(key, "brightness") == 0)
    {
        if (!json_lite_parse_integer(cursor, &number)) return 0;
        if (number < 0 || number > 100) return 0;
        patch->brightness = (int)number;
        patch->has_brightness = 1;
        return 1;
    }
    if (strcmp(key, "inbrightness") == 0)
    {
        if (!json_lite_parse_integer(cursor, &number)) return 0;
        if (number < 0 || number > 100) return 0;
        patch->inbrightness = (int)number;
        patch->has_inbrightness = 1;
        return 1;
    }
    if (strcmp(key, "cycles") == 0)
    {
        if (!json_lite_parse_integer(cursor, &number)) return 0;
        if (number < -1 || number > 1000000) return 0;
        patch->cycles = (int)number;
        patch->has_cycles = 1;
        return 1;
    }
    if (strcmp(key, "trigger") == 0)
    {
        if (!json_lite_parse_integer(cursor, &number)) return 0;
        if (number < 0 || number > 255) return 0;
        patch->trigger = (int)number;
        patch->has_trigger = 1;
        return 1;
    }

    return json_lite_skip_value(cursor, 1);
}

static int parse_patch_fields(JsonLiteCursor* cursor, LedZonePatch* patch)
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
        char key[LED_BACKEND_KEY_MAX];
        size_t key_length = 0;

        json_lite_skip_ws(cursor);
        if (!json_lite_parse_string(cursor, key, sizeof key, &key_length)) return 0;
        if (key_length >= sizeof key) return 0;
        json_lite_skip_ws(cursor);
        if (cursor->p >= cursor->end || *cursor->p != ':') return 0;
        cursor->p++;
        json_lite_skip_ws(cursor);
        if (!parse_field(cursor, key, patch)) return 0;
        json_lite_skip_ws(cursor);
        if (cursor->p >= cursor->end) return 0;
        if (*cursor->p == ',')
        {
            cursor->p++;
            continue;
        }
        if (*cursor->p == '}')
        {
            cursor->p++;
            return 1;
        }
        return 0;
    }
}

static int parse_patches(const char* args, LedZonePatch* patches, int max_patches, int* out_count)
{
    JsonLiteCursor cursor;
    int count = 0;

    *out_count = 0;
    if (args == NULL || args[0] == '\0') return 1;
    json_lite_init(&cursor, args, strlen(args));
    json_lite_skip_ws(&cursor);
    if (cursor.p >= cursor.end || *cursor.p != '{') return 0;
    cursor.p++;
    json_lite_skip_ws(&cursor);

    if (cursor.p < cursor.end && *cursor.p == '}')
    {
        cursor.p++;
        json_lite_skip_ws(&cursor);
        return cursor.p == cursor.end;
    }

    for (;;)
    {
        char key[LED_BACKEND_KEY_MAX];
        size_t key_length = 0;

        json_lite_skip_ws(&cursor);
        if (!json_lite_parse_string(&cursor, key, sizeof key, &key_length)) return 0;
        if (key_length >= sizeof key) return 0;
        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end || *cursor.p != ':') return 0;
        cursor.p++;
        json_lite_skip_ws(&cursor);

        if (strcmp(key, "zones") == 0)
        {
            if (cursor.p >= cursor.end || *cursor.p != '[') return 0;
            cursor.p++;
            json_lite_skip_ws(&cursor);
            if (cursor.p < cursor.end && *cursor.p == ']')
            {
                cursor.p++;
            }
            else
            {
                for (;;)
                {
                    LedZonePatch patch;
                    if (count >= max_patches) return 0;
                    memset(&patch, 0, sizeof patch);
                    patch.index = -1;
                    if (!parse_patch_fields(&cursor, &patch)) return 0;
                    if (patch.index < 0) return 0;
                    patches[count++] = patch;
                    json_lite_skip_ws(&cursor);
                    if (cursor.p >= cursor.end) return 0;
                    if (*cursor.p == ',')
                    {
                        cursor.p++;
                        continue;
                    }
                    if (*cursor.p == ']')
                    {
                        cursor.p++;
                        break;
                    }
                    return 0;
                }
            }
        }
        else
        {
            /* The whole top-level object is one patch. parse_field already
             * consumed the value for this key; consume the remaining fields. */
            LedZonePatch patch;
            if (count >= max_patches) return 0;
            memset(&patch, 0, sizeof patch);
            patch.index = -1;
            if (!parse_field(&cursor, key, &patch)) return 0;
            for (;;)
            {
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
                if (!json_lite_parse_string(&cursor, key, sizeof key, &key_length)) return 0;
                if (key_length >= sizeof key) return 0;
                json_lite_skip_ws(&cursor);
                if (cursor.p >= cursor.end || *cursor.p != ':') return 0;
                cursor.p++;
                json_lite_skip_ws(&cursor);
                if (!parse_field(&cursor, key, &patch)) return 0;
            }
            if (patch.index < 0) return 0;
            patches[count++] = patch;
            break;
        }

        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end) return 0;
        if (*cursor.p == ',')
        {
            cursor.p++;
            continue;
        }
        if (*cursor.p == '}')
        {
            cursor.p++;
            break;
        }
        return 0;
    }

    json_lite_skip_ws(&cursor);
    if (cursor.p != cursor.end) return 0;
    *out_count = count;
    return 1;
}

static int parse_get_zone(const char* args, int* out_zone)
{
    JsonLiteCursor cursor;
    int zone = -1;

    *out_zone = -1;
    if (args == NULL || args[0] == '\0') return 1;
    json_lite_init(&cursor, args, strlen(args));
    json_lite_skip_ws(&cursor);
    if (cursor.p >= cursor.end || *cursor.p != '{') return 0;
    cursor.p++;
    for (;;)
    {
        char key[LED_BACKEND_KEY_MAX];
        size_t key_length = 0;

        json_lite_skip_ws(&cursor);
        if (cursor.p < cursor.end && *cursor.p == '}')
        {
            cursor.p++;
            break;
        }
        if (!json_lite_parse_string(&cursor, key, sizeof key, &key_length)) return 0;
        if (key_length >= sizeof key) return 0;
        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end || *cursor.p != ':') return 0;
        cursor.p++;
        json_lite_skip_ws(&cursor);
        if (strcmp(key, "zone") == 0)
        {
            long number;
            if (!json_lite_parse_integer(&cursor, &number)) return 0;
            if (number < 0 || number > 255) return 0;
            zone = (int)number;
        }
        else if (!json_lite_skip_value(&cursor, 1))
        {
            return 0;
        }
        json_lite_skip_ws(&cursor);
        if (cursor.p < cursor.end && *cursor.p == ',')
        {
            cursor.p++;
            continue;
        }
        if (cursor.p < cursor.end && *cursor.p == '}')
        {
            cursor.p++;
            break;
        }
        return 0;
    }
    json_lite_skip_ws(&cursor);
    if (cursor.p != cursor.end) return 0;
    *out_zone = zone;
    return 1;
}

static int backend_call(void* context, const char* capability, const char* action,
                        const char* args, char* value, size_t value_size, char* detail,
                        size_t detail_size)
{
    Writer writer;
    const char* verb = action != NULL && action[0] != '\0' ? action : "get";

    (void)context;
    if (capability == NULL || strcmp(capability, "led.control") != 0)
    {
        set_detail(detail, detail_size, "not an led capability");
        return 0;
    }
    if (!ensure_lease())
    {
        set_detail(detail, detail_size, "led backend is not configured");
        return 0;
    }

    writer_init(&writer, value, value_size);

    if (strcmp(verb, "get") == 0)
    {
        int zone = -1;
        if (!parse_get_zone(args, &zone))
        {
            set_detail(detail, detail_size, "invalid led get args");
            return 0;
        }
        if (zone >= 0)
        {
            if (!led_lease_active(&lease))
            {
                set_detail(detail, detail_size, "no active led lease");
                return 0;
            }
            if (!write_zone(&writer, &lease, zone))
            {
                set_detail(detail, detail_size, "led zone is outside this device");
                return 0;
            }
        }
        else
        {
            write_summary(&writer, led_lease_active(&lease), led_lease_depth(&lease),
                          lease.zone_count);
        }
    }
    else if (strcmp(verb, "acquire") == 0)
    {
        LedZonePatch patches[LED_LEASE_MAX_ZONES];
        int patch_count = 0;
        LedLeaseError error;

        if (!parse_patches(args, patches, LED_LEASE_MAX_ZONES, &patch_count))
        {
            set_detail(detail, detail_size, "invalid led acquire args");
            return 0;
        }
        error = led_lease_acquire(&lease);
        if (error != LED_LEASE_OK)
        {
            set_detail(detail, detail_size, led_lease_last_detail(&lease));
            return 0;
        }
        if (patch_count > 0)
        {
            unsigned shared = 0;
            error = led_lease_apply(&lease, patches, patch_count, &shared);
            if (error != LED_LEASE_OK)
            {
                (void)led_lease_restore_default(&lease);
                set_detail(detail, detail_size, led_lease_last_detail(&lease));
                return 0;
            }
        }
        write_summary(&writer, 1, led_lease_depth(&lease), lease.zone_count);
    }
    else if (strcmp(verb, "set") == 0)
    {
        LedZonePatch patches[LED_LEASE_MAX_ZONES];
        int patch_count = 0;
        unsigned shared = 0;
        LedLeaseError error;

        if (!led_lease_active(&lease))
        {
            set_detail(detail, detail_size, "no active led lease");
            return 0;
        }
        if (!parse_patches(args, patches, LED_LEASE_MAX_ZONES, &patch_count) || patch_count == 0)
        {
            set_detail(detail, detail_size, "invalid led set args");
            return 0;
        }
        error = led_lease_apply(&lease, patches, patch_count, &shared);
        if (error != LED_LEASE_OK)
        {
            set_detail(detail, detail_size, led_lease_last_detail(&lease));
            return 0;
        }
        writer_char(&writer, '{');
        writer_key(&writer, "active");
        writer_raw(&writer, "true");
        writer_char(&writer, ',');
        writer_key(&writer, "depth");
        writer_int(&writer, led_lease_depth(&lease));
        writer_char(&writer, ',');
        write_changed(&writer, patches, patch_count, shared);
        writer_char(&writer, '}');
    }
    else if (strcmp(verb, "release") == 0)
    {
        LedLeaseError error;
        int released = led_lease_active(&lease);

        error = led_lease_release(&lease);
        if (error == LED_LEASE_NOT_HELD)
        {
            released = 0;
        }
        else if (error != LED_LEASE_OK)
        {
            set_detail(detail, detail_size, led_lease_last_detail(&lease));
            return 0;
        }
        writer_char(&writer, '{');
        writer_key(&writer, "active");
        writer_raw(&writer, led_lease_active(&lease) ? "true" : "false");
        writer_char(&writer, ',');
        writer_key(&writer, "depth");
        writer_int(&writer, led_lease_depth(&lease));
        writer_char(&writer, ',');
        writer_key(&writer, "released");
        writer_raw(&writer, released ? "true" : "false");
        writer_char(&writer, '}');
    }
    else if (strcmp(verb, "restore") == 0)
    {
        LedLeaseError error = led_lease_restore_default(&lease);
        if (error != LED_LEASE_OK)
        {
            set_detail(detail, detail_size, led_lease_last_detail(&lease));
            return 0;
        }
        writer_char(&writer, '{');
        writer_key(&writer, "active");
        writer_raw(&writer, "false");
        writer_char(&writer, ',');
        writer_key(&writer, "depth");
        writer_int(&writer, 0);
        writer_char(&writer, '}');
    }
    else
    {
        set_detail(detail, detail_size, "unsupported led action");
        return 0;
    }

    if (!writer_finish(&writer))
    {
        set_detail(detail, detail_size, "led response is too large");
        return 0;
    }
    return 1;
}

static const DeviceBackend backend = {
    LED_BACKEND_NAME,
    NULL,
    backend_available,
    backend_call,
};

const DeviceBackend* device_backend_leds_with_ops(const LedLeaseOps* ops, void* context)
{
    reset_state();
    lease_ops = ops;
    lease_context = context;
    return &backend;
}

int device_backend_leds_restore(void)
{
    if (!lease_ready) return 1;
    return led_lease_restore_default(&lease) == LED_LEASE_OK;
}

void device_backend_leds_reset(void)
{
    reset_state();
}
