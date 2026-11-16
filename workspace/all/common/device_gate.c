#include "device_gate.h"

#include "device_caps.h"
#include "json_lite.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
    char app[DEVICE_GRANT_APP_MAX];
    char pak[DEVICE_GRANT_PATH_MAX];
    char version[DEVICE_GRANT_VERSION_MAX];
    PakManifestStatus manifest_status;
    PakManifest manifest;
} ResolvedApp;

/* Library default stays legacy so existing gate tests keep their contract;
 * devserv explicitly selects default-allow at startup. */
static int default_allow = 0;

void device_gate_set_default_allow(int enabled)
{
    default_allow = enabled ? 1 : 0;
}

int device_gate_default_allow(void)
{
    return default_allow;
}

static void set_detail(char* detail, size_t detail_size, const char* format, ...)
{
    va_list arguments;

    if (detail == NULL || detail_size == 0) return;
    va_start(arguments, format);
    vsnprintf(detail, detail_size, format, arguments);
    va_end(arguments);
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

static int path_join(char* out, size_t out_size, const char* directory, const char* name)
{
    int written = snprintf(out, out_size, "%s/%s", directory, name);
    return written >= 0 && (size_t)written < out_size;
}

static int regular_file_exists(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int path_basename(const char* path, char* out, size_t out_size)
{
    size_t end = strlen(path);
    size_t start;
    size_t length;

    while (end > 0 && path[end - 1] == '/') end--;
    if (end == 0) return 0;

    start = end;
    while (start > 0 && path[start - 1] != '/') start--;
    length = end - start;
    if (length == 0 || length >= out_size) return 0;

    memcpy(out, path + start, length);
    out[length] = '\0';
    return 1;
}

static DeviceGateError resolve_app(const char* claimed_app, const char* pak_path,
                                   ResolvedApp* app, char* detail, size_t detail_size)
{
    char* resolved;
    char probe[DEVICE_GRANT_PATH_MAX + 16];
    struct stat st;

    memset(app, 0, sizeof *app);

    if (claimed_app != NULL && claimed_app[0] != '\0' && !pak_manifest_is_safe_id(claimed_app))
    {
        set_detail(detail, detail_size, "invalid app id");
        return DEVICE_GATE_BAD_REQUEST;
    }
    if (pak_path == NULL || pak_path[0] == '\0' || has_control(pak_path))
    {
        set_detail(detail, detail_size, "invalid pak path");
        return DEVICE_GATE_BAD_REQUEST;
    }

    resolved = realpath(pak_path, NULL);
    if (resolved == NULL)
    {
        set_detail(detail, detail_size, "pak path does not exist");
        return DEVICE_GATE_BAD_REQUEST;
    }
    if (strlen(resolved) >= sizeof app->pak)
    {
        free(resolved);
        set_detail(detail, detail_size, "pak path is too long");
        return DEVICE_GATE_BAD_REQUEST;
    }
    memcpy(app->pak, resolved, strlen(resolved) + 1);
    free(resolved);

    if (stat(app->pak, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        set_detail(detail, detail_size, "pak path is not a directory");
        return DEVICE_GATE_BAD_REQUEST;
    }

    if (!path_join(probe, sizeof probe, app->pak, "pak.json")
        || (!regular_file_exists(probe)
            && (!path_join(probe, sizeof probe, app->pak, "launch.sh")
                || !regular_file_exists(probe))))
    {
        set_detail(detail, detail_size, "pak has neither pak.json nor launch.sh");
        return DEVICE_GATE_BAD_REQUEST;
    }

    app->manifest_status = pak_manifest_load(app->pak, &app->manifest);

    if (app->manifest_status == PAK_MANIFEST_OK && app->manifest.id[0] != '\0')
    {
        memcpy(app->app, app->manifest.id, strlen(app->manifest.id) + 1);
    }
    else if (!path_basename(app->pak, app->app, sizeof app->app)
             || !pak_manifest_is_safe_id(app->app))
    {
        set_detail(detail, detail_size, "pak has no usable app id");
        return DEVICE_GATE_BAD_REQUEST;
    }

    if (claimed_app != NULL && claimed_app[0] != '\0' && strcmp(claimed_app, app->app) != 0)
    {
        set_detail(detail, detail_size, "app id does not match the pak manifest");
        return DEVICE_GATE_IDENTITY_MISMATCH;
    }

    if (app->manifest_status == PAK_MANIFEST_OK && app->manifest.version_valid)
        memcpy(app->version, app->manifest.version, strlen(app->manifest.version) + 1);

    return DEVICE_GATE_OK;
}

static int action_valid(const char* action)
{
    const unsigned char* p;
    size_t length = strlen(action);

    if (length == 0 || length >= DEVICE_GATE_ACTION_MAX) return 0;
    for (p = (const unsigned char*)action; *p != '\0'; p++)
    {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
            || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-')
            continue;
        return 0;
    }
    return 1;
}

static int json_value_valid(const char* value)
{
    JsonLiteCursor cursor;

    if (value == NULL) return 0;
    json_lite_init(&cursor, value, strlen(value));
    json_lite_skip_ws(&cursor);
    if (!json_lite_skip_value(&cursor, 0)) return 0;
    json_lite_skip_ws(&cursor);
    return cursor.p == cursor.end;
}

static int value_valid(const char* value, size_t value_size)
{
    size_t length;

    if (value == NULL) return 0;
    length = strlen(value);
    if (length == 0 || length >= value_size) return 0;
    return json_value_valid(value);
}

static DeviceGateError load_grants_for_call(const char* grants_path, DeviceGrants* grants,
                                            char* detail, size_t detail_size)
{
    DeviceGrantsStatus status = device_grants_load(grants_path, grants);

    if (status == DEVICE_GRANTS_STATUS_INVALID)
    {
        set_detail(detail, detail_size, "grant store is invalid; gated capabilities are denied");
        return DEVICE_GATE_GRANT_STORE_INVALID;
    }
    if (status == DEVICE_GRANTS_STATUS_IO_ERROR)
    {
        set_detail(detail, detail_size, "grant store could not be read");
        return DEVICE_GATE_GRANT_STORE_ERROR;
    }
    return DEVICE_GATE_OK;
}

void device_call_request_init(DeviceCallRequest* request)
{
    if (request != NULL) memset(request, 0, sizeof *request);
}

void device_call_result_init(DeviceCallResult* result)
{
    if (result != NULL) memset(result, 0, sizeof *result);
}

void device_status_result_init(DeviceStatusResult* status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof *status);
    status->backend_name = "none";
    status->manifest_status = PAK_MANIFEST_ABSENT;
    status->grants_status = DEVICE_GRANTS_STATUS_MISSING;
}

DeviceGateError device_gate_call(const DeviceCallRequest* request, const char* grants_path,
                                 const DeviceBackend* backend, DeviceCallResult* result)
{
    ResolvedApp app;
    const DeviceCapabilityInfo* capability;
    char action[DEVICE_GATE_ACTION_MAX];
    char backend_detail[DEVICE_GATE_DETAIL_MAX];
    DeviceGrants grants;
    DeviceGateError error;
    const DeviceGrant* grant;

    if (result == NULL) return DEVICE_GATE_BAD_REQUEST;
    device_call_result_init(result);

    if (request == NULL
        || (!default_allow && (grants_path == NULL || grants_path[0] == '\0')))
    {
        set_detail(result->detail, sizeof result->detail, "invalid request");
        result->error = DEVICE_GATE_BAD_REQUEST;
        return result->error;
    }
    if (backend == NULL) backend = device_backend_none();

    if (request->capability[0] == '\0')
    {
        set_detail(result->detail, sizeof result->detail, "capability is required");
        result->error = DEVICE_GATE_BAD_REQUEST;
        return result->error;
    }
    memcpy(result->capability, request->capability, strlen(request->capability) + 1);

    if (request->action[0] == '\0')
        snprintf(action, sizeof action, "get");
    else
        snprintf(action, sizeof action, "%s", request->action);
    if (!action_valid(action))
    {
        set_detail(result->detail, sizeof result->detail, "invalid action");
        result->error = DEVICE_GATE_BAD_REQUEST;
        return result->error;
    }
    if (request->args != NULL && request->args[0] != '\0' && !json_value_valid(request->args))
    {
        set_detail(result->detail, sizeof result->detail, "args is not a JSON value");
        result->error = DEVICE_GATE_BAD_REQUEST;
        return result->error;
    }

    error = resolve_app(request->app, request->pak, &app, result->detail, sizeof result->detail);
    if (error != DEVICE_GATE_OK)
    {
        result->error = error;
        return error;
    }

    result->declared = app.manifest_status == PAK_MANIFEST_OK
                       && pak_manifest_requests_device_capability(&app.manifest,
                                                                  request->capability);

    capability = device_capability_find(request->capability);
    if (capability == NULL)
    {
        set_detail(result->detail, sizeof result->detail, "unknown device capability");
        result->error = DEVICE_GATE_UNKNOWN_CAPABILITY;
        return result->error;
    }
    result->supported = 1;

    /*
     * Availability is reported even on a denial so clients can distinguish
     * "not granted" from "granted but this hardware is absent". Authorization
     * is still checked first and decides the error.
     */
    backend_detail[0] = '\0';
    result->available = backend->available(backend->context, request->capability,
                                           backend_detail, sizeof backend_detail);

    if (default_allow)
    {
        /* Installed apps get every implemented capability; the manifest is
         * descriptive, not an authorization request. Availability still decides
         * whether the backend can actually serve this call. */
        result->authorized = 1;
    }
    else if (capability->grant_required)
    {
        if (app.manifest_status == PAK_MANIFEST_INVALID)
        {
            set_detail(result->detail, sizeof result->detail,
                       "pak.json is invalid; gated capabilities are denied");
            result->error = DEVICE_GATE_INVALID_MANIFEST;
            return result->error;
        }
        if (!result->declared)
        {
            set_detail(result->detail, sizeof result->detail,
                       "pak.json does not request this capability");
            result->error = DEVICE_GATE_NOT_DECLARED;
            return result->error;
        }

        error = load_grants_for_call(grants_path, &grants, result->detail, sizeof result->detail);
        if (error != DEVICE_GATE_OK)
        {
            result->error = error;
            return error;
        }

        grant = device_grants_find(&grants, app.app, app.pak, app.version,
                                   request->capability);
        if (grant != NULL)
        {
            DeviceIdentity identity;
            DeviceIdentityStatus identity_status = device_identity_scan(app.pak, 0, &identity);

            if (identity_status != DEVICE_IDENTITY_OK)
            {
                set_detail(result->detail, sizeof result->detail, "pak identity is %s: %s",
                           device_identity_status_name(identity_status), identity.detail);
                result->error = DEVICE_GATE_IDENTITY_UNAVAILABLE;
                return result->error;
            }

            /*
             * Metadata is only a cheap "unchanged" witness. Any metadata change
             * (including a touch that preserves bytes) triggers a content
             * re-hash, so a same-version replacement cannot inherit silently.
             */
            if (strcmp(identity.metadata, grant->metadata) == 0)
            {
                result->authorized = 1;
            }
            else
            {
                identity_status = device_identity_scan(app.pak, 1, &identity);
                if (identity_status != DEVICE_IDENTITY_OK)
                {
                    set_detail(result->detail, sizeof result->detail, "pak identity is %s: %s",
                               device_identity_status_name(identity_status), identity.detail);
                    result->error = DEVICE_GATE_IDENTITY_UNAVAILABLE;
                    return result->error;
                }
                if (strcmp(identity.content, grant->content) == 0)
                {
                    result->authorized = 1;
                }
                else
                {
                    set_detail(result->detail, sizeof result->detail,
                               "pak content changed since the grant");
                    result->error = DEVICE_GATE_IDENTITY_MISMATCH;
                    return result->error;
                }
            }
        }
        else if (device_grants_has_identity(&grants, app.app, request->capability))
        {
            set_detail(result->detail, sizeof result->detail,
                       "grant belongs to a different app installation");
            result->error = DEVICE_GATE_IDENTITY_MISMATCH;
            return result->error;
        }
        else
        {
            set_detail(result->detail, sizeof result->detail,
                       "capability is not granted for this app installation");
            result->error = DEVICE_GATE_UNAUTHORIZED;
            return result->error;
        }
    }
    else
    {
        /* Read-only defaults are open even for legacy paks without a manifest. */
        result->authorized = 1;
    }

    if (!result->available)
    {
        set_detail(result->detail, sizeof result->detail, "%s",
                   backend_detail[0] != '\0' ? backend_detail : "capability is unavailable");
        result->error = DEVICE_GATE_UNAVAILABLE;
        return result->error;
    }

    result->value[0] = '\0';
    backend_detail[0] = '\0';
    if (!backend->call(backend->context, request->capability, action, request->args,
                       result->value, sizeof result->value, backend_detail,
                       sizeof backend_detail))
    {
        set_detail(result->detail, sizeof result->detail, "%s",
                   backend_detail[0] != '\0' ? backend_detail : "backend call failed");
        result->error = DEVICE_GATE_BACKEND_ERROR;
        return result->error;
    }

    if (result->value[0] == '\0') snprintf(result->value, sizeof result->value, "null");
    if (!value_valid(result->value, sizeof result->value))
    {
        set_detail(result->detail, sizeof result->detail, "backend returned an invalid value");
        result->error = DEVICE_GATE_BACKEND_ERROR;
        return result->error;
    }

    result->error = DEVICE_GATE_OK;
    return DEVICE_GATE_OK;
}

DeviceGateError device_gate_status(const char* claimed_app, const char* pak_path,
                                   const char* grants_path, const DeviceBackend* backend,
                                   DeviceStatusResult* status)
{
    ResolvedApp app;
    DeviceGrants grants;
    DeviceIdentity identity;
    DeviceGateError error;
    size_t i;

    if (status == NULL) return DEVICE_GATE_BAD_REQUEST;
    device_status_result_init(status);

    if (!default_allow && (grants_path == NULL || grants_path[0] == '\0'))
    {
        set_detail(status->detail, sizeof status->detail, "invalid grant store path");
        status->error = DEVICE_GATE_BAD_REQUEST;
        return status->error;
    }
    if (backend == NULL) backend = device_backend_none();
    status->backend_name = backend->name != NULL ? backend->name : "none";

    error = resolve_app(claimed_app, pak_path, &app, status->detail, sizeof status->detail);
    if (error != DEVICE_GATE_OK)
    {
        status->error = error;
        return error;
    }

    memcpy(status->app, app.app, strlen(app.app) + 1);
    memcpy(status->pak, app.pak, strlen(app.pak) + 1);
    status->manifest_status = app.manifest_status;
    if (default_allow)
    {
        /* The legacy store is diagnostic only; it cannot change authorization. */
        status->grants_status = (grants_path != NULL && grants_path[0] != '\0')
                                    ? device_grants_load(grants_path, &grants)
                                    : DEVICE_GRANTS_STATUS_MISSING;
    }
    else
    {
        status->grants_status = device_grants_load(grants_path, &grants);
    }
    status->identity_status = device_identity_scan(app.pak, 1, &identity);
    if (status->identity_status != DEVICE_IDENTITY_OK)
        snprintf(status->identity_detail, sizeof status->identity_detail, "%s",
                 identity.detail);

    for (i = 0; i < device_capability_total() && status->capability_count < DEVICE_GATE_STATUS_MAX;
         i++)
    {
        const DeviceCapabilityInfo* info = device_capability_at(i);
        DeviceCapabilityStatus* entry = &status->capabilities[status->capability_count];
        char ignored[DEVICE_GATE_DETAIL_MAX];

        memset(entry, 0, sizeof *entry);
        snprintf(entry->name, sizeof entry->name, "%s", info->name);
        entry->supported = 1;
        entry->grant_required = default_allow ? 0 : info->grant_required;
        entry->declared = app.manifest_status == PAK_MANIFEST_OK
                          && pak_manifest_requests_device_capability(&app.manifest, info->name);
        ignored[0] = '\0';
        entry->available = backend->available(backend->context, info->name, ignored,
                                              sizeof ignored);

        if (default_allow)
        {
            /* Permission is the install itself. `available` still reports the
             * backend honestly; no grant store participates in this decision. */
            entry->authorized = 1;
        }
        else if (!info->grant_required)
        {
            entry->authorized = 1;
        }
        else if (status->identity_status == DEVICE_IDENTITY_OK
                 && (status->grants_status == DEVICE_GRANTS_STATUS_OK
                     || status->grants_status == DEVICE_GRANTS_STATUS_MISSING))
        {
            const DeviceGrant* grant = device_grants_find(&grants, app.app, app.pak,
                                                          app.version, info->name);
            entry->authorized = grant != NULL && strcmp(identity.content, grant->content) == 0;
        }

        status->capability_count++;
    }

    /* Declared names this system does not understand are reported, not hidden. */
    if (app.manifest_status == PAK_MANIFEST_OK)
    {
        for (i = 0;
             i < (size_t)app.manifest.device_capability_count
             && status->capability_count < DEVICE_GATE_STATUS_MAX;
             i++)
        {
            const char* name = app.manifest.device_capabilities[i];
            DeviceCapabilityStatus* entry;
            int already = 0;
            int j;

            if (device_capability_find(name) != NULL) continue;
            for (j = 0; j < status->capability_count; j++)
            {
                if (strcmp(status->capabilities[j].name, name) == 0)
                {
                    already = 1;
                    break;
                }
            }
            if (already) continue;

            entry = &status->capabilities[status->capability_count];
            memset(entry, 0, sizeof *entry);
            snprintf(entry->name, sizeof entry->name, "%s", name);
            entry->grant_required = default_allow ? 0 : 1;
            entry->declared = 1;
            status->capability_count++;
        }
    }

    status->error = DEVICE_GATE_OK;
    return DEVICE_GATE_OK;
}

DeviceGateError device_gate_grant(const DeviceCallRequest* request, const char* grants_path,
                                  char* detail, size_t detail_size)
{
    ResolvedApp app;
    const DeviceCapabilityInfo* capability;
    DeviceGrants grants;
    DeviceIdentity identity;
    DeviceGateError error;

    set_detail(detail, detail_size, "");
    if (request == NULL || grants_path == NULL || grants_path[0] == '\0')
    {
        set_detail(detail, detail_size, "invalid grant request");
        return DEVICE_GATE_BAD_REQUEST;
    }
    if (request->capability[0] == '\0')
    {
        set_detail(detail, detail_size, "capability is required");
        return DEVICE_GATE_BAD_REQUEST;
    }

    error = resolve_app(request->app, request->pak, &app, detail, detail_size);
    if (error != DEVICE_GATE_OK) return error;

    capability = device_capability_find(request->capability);
    if (capability == NULL)
    {
        set_detail(detail, detail_size, "unknown device capability");
        return DEVICE_GATE_UNKNOWN_CAPABILITY;
    }
    if (!capability->grant_required)
    {
        set_detail(detail, detail_size, "read-only capability needs no grant");
        return DEVICE_GATE_BAD_REQUEST;
    }
    if (app.manifest_status == PAK_MANIFEST_INVALID)
    {
        set_detail(detail, detail_size, "pak.json is invalid");
        return DEVICE_GATE_INVALID_MANIFEST;
    }
    if (!pak_manifest_requests_device_capability(&app.manifest, request->capability))
    {
        set_detail(detail, detail_size, "pak.json does not request this capability");
        return DEVICE_GATE_NOT_DECLARED;
    }

    {
        DeviceIdentityStatus identity_status = device_identity_scan(app.pak, 1, &identity);
        if (identity_status != DEVICE_IDENTITY_OK)
        {
            set_detail(detail, detail_size, "pak identity is %s: %s",
                       device_identity_status_name(identity_status), identity.detail);
            return DEVICE_GATE_IDENTITY_UNAVAILABLE;
        }
    }

    error = load_grants_for_call(grants_path, &grants, detail, detail_size);
    if (error != DEVICE_GATE_OK) return error;

    if (!device_grants_add(&grants, app.app, app.pak, app.version, request->capability,
                           identity.content, identity.metadata, (long)time(NULL)))
    {
        set_detail(detail, detail_size, "grant store is full or the record is invalid");
        return DEVICE_GATE_GRANT_STORE_ERROR;
    }
    if (!device_grants_save(grants_path, &grants))
    {
        set_detail(detail, detail_size, "could not write the grant store");
        return DEVICE_GATE_GRANT_STORE_ERROR;
    }

    set_detail(detail, detail_size, "granted");
    return DEVICE_GATE_OK;
}

DeviceGateError device_gate_revoke(const DeviceCallRequest* request, const char* grants_path,
                                   char* detail, size_t detail_size)
{
    ResolvedApp app;
    const DeviceCapabilityInfo* capability;
    DeviceGrants grants;
    DeviceGateError error;

    set_detail(detail, detail_size, "");
    if (request == NULL || grants_path == NULL || grants_path[0] == '\0')
    {
        set_detail(detail, detail_size, "invalid revoke request");
        return DEVICE_GATE_BAD_REQUEST;
    }
    if (request->capability[0] == '\0')
    {
        set_detail(detail, detail_size, "capability is required");
        return DEVICE_GATE_BAD_REQUEST;
    }

    error = resolve_app(request->app, request->pak, &app, detail, detail_size);
    if (error != DEVICE_GATE_OK) return error;

    capability = device_capability_find(request->capability);
    if (capability == NULL)
    {
        set_detail(detail, detail_size, "unknown device capability");
        return DEVICE_GATE_UNKNOWN_CAPABILITY;
    }
    if (!capability->grant_required)
    {
        set_detail(detail, detail_size, "read-only capability has no grant");
        return DEVICE_GATE_BAD_REQUEST;
    }

    error = load_grants_for_call(grants_path, &grants, detail, detail_size);
    if (error != DEVICE_GATE_OK) return error;

    (void)device_grants_revoke(&grants, app.app, app.pak, app.version, request->capability);
    if (!device_grants_save(grants_path, &grants))
    {
        set_detail(detail, detail_size, "could not write the grant store");
        return DEVICE_GATE_GRANT_STORE_ERROR;
    }

    set_detail(detail, detail_size, "revoked");
    return DEVICE_GATE_OK;
}

DeviceGateError device_gate_revoke_all(const char* claimed_app, const char* pak_path,
                                       const char* grants_path, char* detail,
                                       size_t detail_size)
{
    ResolvedApp app;
    DeviceGrants grants;
    DeviceGateError error;

    set_detail(detail, detail_size, "");
    if (grants_path == NULL || grants_path[0] == '\0')
    {
        set_detail(detail, detail_size, "invalid grant store path");
        return DEVICE_GATE_BAD_REQUEST;
    }

    error = resolve_app(claimed_app, pak_path, &app, detail, detail_size);
    if (error != DEVICE_GATE_OK) return error;

    error = load_grants_for_call(grants_path, &grants, detail, detail_size);
    if (error != DEVICE_GATE_OK) return error;

    (void)device_grants_revoke_all(&grants, app.app, app.pak);
    if (!device_grants_save(grants_path, &grants))
    {
        set_detail(detail, detail_size, "could not write the grant store");
        return DEVICE_GATE_GRANT_STORE_ERROR;
    }

    set_detail(detail, detail_size, "revoked installation grants");
    return DEVICE_GATE_OK;
}

DeviceGateError device_gate_reset_grants(const char* grants_path, char* detail,
                                         size_t detail_size)
{
    set_detail(detail, detail_size, "");
    if (grants_path == NULL || grants_path[0] == '\0')
    {
        set_detail(detail, detail_size, "invalid grant store path");
        return DEVICE_GATE_BAD_REQUEST;
    }
    if (!device_grants_reset(grants_path))
    {
        set_detail(detail, detail_size, "could not write the grant store");
        return DEVICE_GATE_GRANT_STORE_ERROR;
    }
    set_detail(detail, detail_size, "grant store reset");
    return DEVICE_GATE_OK;
}
