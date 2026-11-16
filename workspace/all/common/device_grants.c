#include "device_grants.h"

#include "json_lite.h"
#include "pak_manifest.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define DEVICE_GRANTS_MAX_BYTES (256u * 1024u)
#define DEVICE_GRANTS_ESCAPE_MAX (DEVICE_GRANT_PATH_MAX * 2 + 2)

void device_grants_init(DeviceGrants* grants)
{
    if (grants == NULL) return;
    memset(grants, 0, sizeof *grants);
    grants->status = DEVICE_GRANTS_STATUS_MISSING;
}

static int has_control(const char* text)
{
    const unsigned char* p;

    for (p = (const unsigned char*)text; *p != '\0'; p++)
    {
        if (*p < 0x20u || *p == 0x7Fu) return 1;
    }
    return 0;
}

static int capability_name_valid(const char* capability)
{
    const unsigned char* p;
    size_t length;

    if (capability == NULL) return 0;
    length = strlen(capability);
    if (length == 0 || length >= DEVICE_GRANT_CAPABILITY_MAX) return 0;

    for (p = (const unsigned char*)capability; *p != '\0'; p++)
    {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
            || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')
            continue;
        return 0;
    }
    return 1;
}

int device_grants_hash_valid(const char* hash)
{
    size_t length;
    const unsigned char* p;

    if (hash == NULL) return 0;
    length = strlen(hash);
    if (length != DEVICE_GRANT_HASH_HEX - 1) return 0;

    for (p = (const unsigned char*)hash; *p != '\0'; p++)
    {
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f')) continue;
        return 0;
    }
    return 1;
}

int device_grants_identity_valid(const char* app, const char* pak, const char* version,
                                 const char* capability)
{
    size_t length;

    if (!pak_manifest_is_safe_id(app)) return 0;
    if (pak == NULL || pak[0] != '/') return 0;
    length = strlen(pak);
    if (length >= DEVICE_GRANT_PATH_MAX || has_control(pak)) return 0;

    if (version == NULL) return 0;
    length = strlen(version);
    if (length >= DEVICE_GRANT_VERSION_MAX || has_control(version)) return 0;

    return capability_name_valid(capability);
}

static void clear_grant(DeviceGrant* grant)
{
    memset(grant, 0, sizeof *grant);
}

/* ------------------------------------------------------------------------- */
/* Parsing                                                                    */
/* ------------------------------------------------------------------------- */

static int parse_string_field(JsonLiteCursor* cursor, char* out, size_t out_size)
{
    size_t length = 0;

    if (!json_lite_parse_string(cursor, out, out_size, &length)) return 0;
    return length < out_size;
}

static int parse_grant(JsonLiteCursor* cursor, DeviceGrant* grant)
{
    enum
    {
        SEEN_APP = 1u << 0,
        SEEN_PAK = 1u << 1,
        SEEN_VERSION = 1u << 2,
        SEEN_CAPABILITY = 1u << 3,
        SEEN_CONTENT = 1u << 4,
        SEEN_METADATA = 1u << 5,
    };
    unsigned seen = 0;

    clear_grant(grant);

    json_lite_skip_ws(cursor);
    if (cursor->p >= cursor->end || *cursor->p != '{') return 0;
    cursor->p++;
    json_lite_skip_ws(cursor);

    if (cursor->p < cursor->end && *cursor->p == '}')
    {
        return 0; /* a grant needs app, pak, and capability */
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

        if (key_length < sizeof key && strcmp(key, "app") == 0)
        {
            if (!(seen & SEEN_APP))
            {
                seen |= SEEN_APP;
                if (!parse_string_field(cursor, grant->app, sizeof grant->app)) return 0;
            }
            else if (!json_lite_skip_value(cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "pak") == 0)
        {
            if (!(seen & SEEN_PAK))
            {
                seen |= SEEN_PAK;
                if (!parse_string_field(cursor, grant->pak, sizeof grant->pak)) return 0;
            }
            else if (!json_lite_skip_value(cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "version") == 0)
        {
            if (!(seen & SEEN_VERSION))
            {
                seen |= SEEN_VERSION;
                if (!parse_string_field(cursor, grant->version, sizeof grant->version)) return 0;
            }
            else if (!json_lite_skip_value(cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "capability") == 0)
        {
            if (!(seen & SEEN_CAPABILITY))
            {
                seen |= SEEN_CAPABILITY;
                if (!parse_string_field(cursor, grant->capability, sizeof grant->capability))
                    return 0;
            }
            else if (!json_lite_skip_value(cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "content") == 0)
        {
            if (!(seen & SEEN_CONTENT))
            {
                seen |= SEEN_CONTENT;
                if (!parse_string_field(cursor, grant->content, sizeof grant->content)) return 0;
            }
            else if (!json_lite_skip_value(cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "metadata") == 0)
        {
            if (!(seen & SEEN_METADATA))
            {
                seen |= SEEN_METADATA;
                if (!parse_string_field(cursor, grant->metadata, sizeof grant->metadata)) return 0;
            }
            else if (!json_lite_skip_value(cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "granted_at") == 0)
        {
            long value;
            if (!json_lite_parse_integer(cursor, &value) || value < 0) return 0;
            grant->granted_at = value;
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

    if ((seen & (SEEN_APP | SEEN_PAK | SEEN_CAPABILITY | SEEN_CONTENT | SEEN_METADATA))
        != (SEEN_APP | SEEN_PAK | SEEN_CAPABILITY | SEEN_CONTENT | SEEN_METADATA))
        return 0;

    return device_grants_identity_valid(grant->app, grant->pak, grant->version,
                                        grant->capability)
           && device_grants_hash_valid(grant->content)
           && device_grants_hash_valid(grant->metadata);
}

static int parse_store(const char* text, size_t length, DeviceGrants* grants)
{
    JsonLiteCursor cursor;
    int seen_version = 0;
    int seen_grants = 0;

    json_lite_init(&cursor, text, length);
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
        char key[32];
        size_t key_length = 0;

        if (!json_lite_parse_string(&cursor, key, sizeof key, &key_length)) return 0;
        json_lite_skip_ws(&cursor);
        if (cursor.p >= cursor.end || *cursor.p != ':') return 0;
        cursor.p++;
        json_lite_skip_ws(&cursor);

        if (key_length < sizeof key && strcmp(key, "version") == 0)
        {
            long version;
            if (!seen_version)
            {
                seen_version = 1;
                if (!json_lite_parse_integer(&cursor, &version)
                    || version != DEVICE_GRANTS_VERSION)
                    return 0;
            }
            else if (!json_lite_skip_value(&cursor, 2))
            {
                return 0;
            }
        }
        else if (key_length < sizeof key && strcmp(key, "grants") == 0)
        {
            if (!seen_grants)
            {
                seen_grants = 1;
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
                        if (grants->count >= DEVICE_GRANT_MAX) return 0;
                        if (!parse_grant(&cursor, &grants->grants[grants->count])) return 0;
                        grants->count++;

                        json_lite_skip_ws(&cursor);
                        if (cursor.p >= cursor.end) return 0;
                        if (*cursor.p == ']')
                        {
                            cursor.p++;
                            break;
                        }
                        if (*cursor.p != ',') return 0;
                        cursor.p++;
                        json_lite_skip_ws(&cursor);
                    }
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

    return seen_version && seen_grants;
}

DeviceGrantsStatus device_grants_load(const char* path, DeviceGrants* grants)
{
    struct stat st;
    FILE* file;
    char* buffer;
    size_t length;
    int read_error;
    int too_long;

    if (grants == NULL) return DEVICE_GRANTS_STATUS_INVALID;
    device_grants_init(grants);

    if (path == NULL || path[0] == '\0')
    {
        grants->status = DEVICE_GRANTS_STATUS_INVALID;
        return grants->status;
    }

    if (stat(path, &st) != 0)
    {
        if (errno == ENOENT) return grants->status; /* MISSING */
        grants->status = DEVICE_GRANTS_STATUS_IO_ERROR;
        return grants->status;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > (off_t)DEVICE_GRANTS_MAX_BYTES)
    {
        grants->status = DEVICE_GRANTS_STATUS_INVALID;
        return grants->status;
    }

    file = fopen(path, "rb");
    if (file == NULL)
    {
        grants->status = DEVICE_GRANTS_STATUS_IO_ERROR;
        return grants->status;
    }

    buffer = malloc(DEVICE_GRANTS_MAX_BYTES + 1);
    if (buffer == NULL)
    {
        fclose(file);
        grants->status = DEVICE_GRANTS_STATUS_IO_ERROR;
        return grants->status;
    }

    length = fread(buffer, 1, DEVICE_GRANTS_MAX_BYTES, file);
    read_error = ferror(file) != 0;
    too_long = !feof(file);
    fclose(file);

    if (read_error || too_long)
    {
        free(buffer);
        grants->status = DEVICE_GRANTS_STATUS_IO_ERROR;
        return grants->status;
    }
    buffer[length] = '\0';

    if (!parse_store(buffer, length, grants))
    {
        free(buffer);
        device_grants_init(grants);
        grants->status = DEVICE_GRANTS_STATUS_INVALID;
        return grants->status;
    }
    free(buffer);

    grants->status = DEVICE_GRANTS_STATUS_OK;
    return grants->status;
}

/* ------------------------------------------------------------------------- */
/* Saving                                                                     */
/* ------------------------------------------------------------------------- */

static int write_escaped(FILE* file, const char* text, char* scratch, size_t scratch_size)
{
    if (!json_lite_escape(text, scratch, scratch_size)) return 0;
    return fputs(scratch, file) >= 0;
}

int device_grants_save(const char* path, const DeviceGrants* grants)
{
    char temporary[DEVICE_GRANT_PATH_MAX + 32];
    char scratch[DEVICE_GRANTS_ESCAPE_MAX];
    size_t i;
    FILE* file;
    int descriptor;
    int ok = 1;
    int written;

    if (path == NULL || path[0] == '\0' || grants == NULL) return 0;
    if (grants->count > DEVICE_GRANT_MAX) return 0;

    for (i = 0; i < grants->count; i++)
    {
        if (!device_grants_identity_valid(grants->grants[i].app, grants->grants[i].pak,
                                          grants->grants[i].version,
                                          grants->grants[i].capability)
            || !device_grants_hash_valid(grants->grants[i].content)
            || !device_grants_hash_valid(grants->grants[i].metadata))
            return 0;
    }

    written = snprintf(temporary, sizeof temporary, "%s.tmp.%ld", path, (long)getpid());
    if (written < 0 || (size_t)written >= sizeof temporary) return 0;

    descriptor = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (descriptor < 0) return 0;
    file = fdopen(descriptor, "wb");
    if (file == NULL)
    {
        close(descriptor);
        (void)remove(temporary);
        return 0;
    }

    if (fprintf(file, "{\"version\":%d,\"grants\":[", DEVICE_GRANTS_VERSION) < 0) ok = 0;

    for (i = 0; ok && i < grants->count; i++)
    {
        const DeviceGrant* grant = &grants->grants[i];
        if (i > 0 && fputc(',', file) == EOF) ok = 0;
        if (ok && fprintf(file, "{\"app\":\"") < 0) ok = 0;
        if (ok && !write_escaped(file, grant->app, scratch, sizeof scratch)) ok = 0;
        if (ok && fprintf(file, "\",\"pak\":\"") < 0) ok = 0;
        if (ok && !write_escaped(file, grant->pak, scratch, sizeof scratch)) ok = 0;
        if (ok && fprintf(file, "\",\"version\":\"") < 0) ok = 0;
        if (ok && !write_escaped(file, grant->version, scratch, sizeof scratch)) ok = 0;
        if (ok && fprintf(file, "\",\"capability\":\"") < 0) ok = 0;
        if (ok && !write_escaped(file, grant->capability, scratch, sizeof scratch)) ok = 0;
        if (ok && fprintf(file, "\",\"content\":\"") < 0) ok = 0;
        if (ok && !write_escaped(file, grant->content, scratch, sizeof scratch)) ok = 0;
        if (ok && fprintf(file, "\",\"metadata\":\"") < 0) ok = 0;
        if (ok && !write_escaped(file, grant->metadata, scratch, sizeof scratch)) ok = 0;
        if (ok && fprintf(file, "\",\"granted_at\":%ld}", grant->granted_at) < 0) ok = 0;
    }

    if (ok && fprintf(file, "]}\n") < 0) ok = 0;
    if (ok && fflush(file) != 0) ok = 0;
    if (ok && fsync(fileno(file)) != 0) ok = 0;
    if (fclose(file) != 0) ok = 0;

    if (!ok)
    {
        (void)remove(temporary);
        return 0;
    }

    if (rename(temporary, path) != 0)
    {
        (void)remove(temporary);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Queries and mutations                                                      */
/* ------------------------------------------------------------------------- */

const DeviceGrant* device_grants_find(const DeviceGrants* grants, const char* app,
                                      const char* pak, const char* version,
                                      const char* capability)
{
    size_t i;

    if (grants == NULL || app == NULL || pak == NULL || version == NULL || capability == NULL)
        return NULL;

    for (i = 0; i < grants->count; i++)
    {
        const DeviceGrant* grant = &grants->grants[i];
        if (strcmp(grant->app, app) == 0 && strcmp(grant->pak, pak) == 0
            && strcmp(grant->version, version) == 0
            && strcmp(grant->capability, capability) == 0)
            return grant;
    }
    return NULL;
}

int device_grants_has(const DeviceGrants* grants, const char* app, const char* pak,
                      const char* version, const char* capability)
{
    if (!device_grants_identity_valid(app, pak, version, capability)) return 0;
    return device_grants_find(grants, app, pak, version, capability) != NULL;
}

int device_grants_has_identity(const DeviceGrants* grants, const char* app, const char* capability)
{
    size_t i;

    if (grants == NULL || app == NULL || capability == NULL) return 0;

    for (i = 0; i < grants->count; i++)
    {
        const DeviceGrant* grant = &grants->grants[i];
        if (strcmp(grant->app, app) == 0 && strcmp(grant->capability, capability) == 0) return 1;
    }
    return 0;
}

static void remove_at(DeviceGrants* grants, size_t index)
{
    if (index + 1 < grants->count)
        memmove(&grants->grants[index], &grants->grants[index + 1],
                (grants->count - index - 1) * sizeof grants->grants[0]);
    grants->count--;
    clear_grant(&grants->grants[grants->count]);
}

int device_grants_add(DeviceGrants* grants, const char* app, const char* pak,
                      const char* version, const char* capability, const char* content,
                      const char* metadata, long granted_at)
{
    size_t i;

    if (grants == NULL || !device_grants_identity_valid(app, pak, version, capability))
        return 0;
    if (!device_grants_hash_valid(content) || !device_grants_hash_valid(metadata)) return 0;

    /* One grant per installation and capability: a version change replaces the
     * old entry instead of leaving a stale version behind. */
    for (i = 0; i < grants->count;)
    {
        const DeviceGrant* grant = &grants->grants[i];
        if (strcmp(grant->app, app) == 0 && strcmp(grant->pak, pak) == 0
            && strcmp(grant->capability, capability) == 0)
            remove_at(grants, i);
        else
            i++;
    }

    if (grants->count >= DEVICE_GRANT_MAX) return 0;

    memset(&grants->grants[grants->count], 0, sizeof grants->grants[0]);
    snprintf(grants->grants[grants->count].app, sizeof grants->grants[0].app, "%s", app);
    snprintf(grants->grants[grants->count].pak, sizeof grants->grants[0].pak, "%s", pak);
    snprintf(grants->grants[grants->count].version, sizeof grants->grants[0].version, "%s",
             version);
    snprintf(grants->grants[grants->count].capability,
             sizeof grants->grants[0].capability, "%s", capability);
    snprintf(grants->grants[grants->count].content, sizeof grants->grants[0].content, "%s",
             content);
    snprintf(grants->grants[grants->count].metadata, sizeof grants->grants[0].metadata, "%s",
             metadata);
    grants->grants[grants->count].granted_at = granted_at;
    grants->count++;
    return 1;
}

size_t device_grants_revoke(DeviceGrants* grants, const char* app, const char* pak,
                            const char* version, const char* capability)
{
    size_t removed = 0;
    size_t i;

    if (grants == NULL || app == NULL || pak == NULL || version == NULL || capability == NULL)
        return 0;

    for (i = 0; i < grants->count;)
    {
        const DeviceGrant* grant = &grants->grants[i];
        if (strcmp(grant->app, app) == 0 && strcmp(grant->pak, pak) == 0
            && strcmp(grant->version, version) == 0
            && strcmp(grant->capability, capability) == 0)
        {
            remove_at(grants, i);
            removed++;
        }
        else
        {
            i++;
        }
    }
    return removed;
}

size_t device_grants_revoke_all(DeviceGrants* grants, const char* app, const char* pak)
{
    size_t removed = 0;
    size_t i;

    if (grants == NULL || app == NULL || pak == NULL) return 0;

    for (i = 0; i < grants->count;)
    {
        const DeviceGrant* grant = &grants->grants[i];
        if (strcmp(grant->app, app) == 0 && strcmp(grant->pak, pak) == 0)
        {
            remove_at(grants, i);
            removed++;
        }
        else
        {
            i++;
        }
    }
    return removed;
}

int device_grants_reset(const char* path)
{
    DeviceGrants empty;

    device_grants_init(&empty);
    return device_grants_save(path, &empty);
}
