#ifndef DEVICE_GRANTS_H
#define DEVICE_GRANTS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Persistent device grants (plan C1), stored as
 *   $SHARED_USERDATA_PATH/device_grants.json
 *
 * A grant authorizes one capability for one installed app identity:
 *   app id + canonical pak directory + manifest version + capability
 *   + pak content fingerprint
 *
 * Identity rules (deliberate, and documented in DEVICE-API.md):
 *   - The app id and canonical pak path must both match. Copying an authorized
 *     pak to another directory does not inherit its grants.
 *   - The manifest version must match too. Replacing or updating an app at the
 *     same path without re-granting is therefore denied rather than silently
 *     inheriting the old permission. Read-only capabilities need no grant.
 *   - The pak content fingerprint (sha256 over install-controlled files) must
 *     also match. A same id/path/version replacement that changes launch.sh,
 *     the binary, or any other fingerprinted file is denied and must be
 *     re-granted. This is content-change detection, not publisher
 *     authentication: an attacker who can rewrite both the pak and the grant
 *     store still wins, and this is a cooperative gate, not a sandbox.
 *
 * Store schema is DEVICE_GRANTS_VERSION (2). Every record requires both
 * fingerprints; a version-1 or hash-less record invalidates the store. The
 * store is fail-closed: any malformed record, unknown schema version, oversized
 * file, or I/O error invalidates the whole store and the gate denies every
 * gated call. Read-only capabilities do not depend on it.
 * Only the settings layer (future C1 UI) writes grants; the app-facing device
 * service never accepts grant mutations.
 */

#define DEVICE_GRANT_APP_MAX 128
#define DEVICE_GRANT_PATH_MAX 512
#define DEVICE_GRANT_VERSION_MAX 32
#define DEVICE_GRANT_CAPABILITY_MAX 48
#define DEVICE_GRANT_MAX 128
#define DEVICE_GRANTS_VERSION 2
#define DEVICE_GRANT_HASH_HEX 65

typedef enum
{
    DEVICE_GRANTS_STATUS_MISSING = 0, /* no file: valid, zero grants */
    DEVICE_GRANTS_STATUS_OK = 1,
    DEVICE_GRANTS_STATUS_INVALID = 2, /* malformed or unsupported: deny all gated calls */
    DEVICE_GRANTS_STATUS_IO_ERROR = 3,
} DeviceGrantsStatus;

typedef struct
{
    char app[DEVICE_GRANT_APP_MAX];
    char pak[DEVICE_GRANT_PATH_MAX];
    char version[DEVICE_GRANT_VERSION_MAX];
    char capability[DEVICE_GRANT_CAPABILITY_MAX];
    char content[DEVICE_GRANT_HASH_HEX];   /* SHA-256 over fingerprinted file bytes */
    char metadata[DEVICE_GRANT_HASH_HEX];  /* SHA-256 over paths/sizes/times/modes */
    long granted_at;
} DeviceGrant;

typedef struct
{
    DeviceGrantsStatus status;
    DeviceGrant grants[DEVICE_GRANT_MAX];
    size_t count;
} DeviceGrants;

static inline const char* device_grants_status_name(DeviceGrantsStatus status)
{
    switch (status)
    {
    case DEVICE_GRANTS_STATUS_MISSING:
        return "missing";
    case DEVICE_GRANTS_STATUS_OK:
        return "ok";
    case DEVICE_GRANTS_STATUS_INVALID:
        return "invalid";
    case DEVICE_GRANTS_STATUS_IO_ERROR:
        return "io_error";
    }
    return "invalid";
}

void device_grants_init(DeviceGrants* grants);

/* Always initializes grants; a missing file is MISSING with zero entries. */
DeviceGrantsStatus device_grants_load(const char* path, DeviceGrants* grants);

/* Atomic replace (temp file + rename). Validates every record first. */
int device_grants_save(const char* path, const DeviceGrants* grants);

/* Find the exact app/path/version/capability record, regardless of hashes. */
const DeviceGrant* device_grants_find(const DeviceGrants* grants, const char* app,
                                      const char* pak, const char* version,
                                      const char* capability);

/* Exact identity match (path/version/app/capability; hashes are the gate's job). */
int device_grants_has(const DeviceGrants* grants, const char* app, const char* pak,
                      const char* version, const char* capability);

/* Any record for this app id and capability, regardless of path/version. */
int device_grants_has_identity(const DeviceGrants* grants, const char* app, const char* capability);

/*
 * Add or replace the grant for (app, pak, capability). Replaces any older
 * version or content fingerprint at the same installation instead of
 * accumulating stale entries. content and metadata must be 64-char lowercase
 * hex SHA-256 strings.
 */
int device_grants_add(DeviceGrants* grants, const char* app, const char* pak,
                      const char* version, const char* capability, const char* content,
                      const char* metadata, long granted_at);

/* Remove exact matches; returns the number removed. */
size_t device_grants_revoke(DeviceGrants* grants, const char* app, const char* pak,
                            const char* version, const char* capability);

/* Remove every grant for one installation (uninstall/replacement cleanup). */
size_t device_grants_revoke_all(DeviceGrants* grants, const char* app, const char* pak);

/* Write an empty valid store, replacing a corrupt one. */
int device_grants_reset(const char* path);

/* Shared validation for grant records and lookups. */
int device_grants_identity_valid(const char* app, const char* pak, const char* version,
                                 const char* capability);

/* 64 lowercase hex characters. */
int device_grants_hash_valid(const char* hash);

#ifdef __cplusplus
}
#endif

#endif
