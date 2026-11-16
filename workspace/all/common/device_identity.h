#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Content identity for an installed pak (C1 follow-up).
 *
 * The identity covers the install-controlled files in the pak directory:
 * pak.json, launch.sh, and every other regular file recursively, minus a small
 * set of writable-state names. It deliberately does not claim publisher
 * authenticity: it only detects that the bytes an app is running changed since
 * the user granted a capability.
 *
 * Scope and limits:
 *   - max 256 included files, 32 MiB total, 8 directory levels
 *   - symlinks and special files make the identity unsupported (fail closed)
 *   - read/list failures make it unreadable (fail closed)
 *   - excluded writable state: saves/savestates/states/save/config/cache/tmp/
 *     logs/userdata directories; *.log, *.cfg, *.ini, *.sav, *.srm, *.state,
 *     *.st files; default.cfg, .DS_Store, Thumbs.db
 * Executable code must not live in an excluded name; an excluded file can
 * change without affecting the grant.
 *
 * Three hashes are exposed:
 *   - metadata: paths, sizes, modes, inode, mtime and ctime (cheap; used to skip
 *     reads)
 *   - content:  paths, sizes, and file bytes (SHA-256)
 * include_content=0 computes only metadata and leaves content empty.
 */

#define DEVICE_IDENTITY_MAX_FILES 256
#define DEVICE_IDENTITY_MAX_BYTES (32u * 1024u * 1024u)
#define DEVICE_IDENTITY_MAX_DEPTH 8
#define DEVICE_IDENTITY_HASH_HEX 65
#define DEVICE_IDENTITY_DETAIL_MAX 160

typedef enum
{
    DEVICE_IDENTITY_OK = 0,
    DEVICE_IDENTITY_UNREADABLE = 1,
    DEVICE_IDENTITY_UNSUPPORTED = 2,
    DEVICE_IDENTITY_TOO_LARGE = 3,
} DeviceIdentityStatus;

typedef struct
{
    DeviceIdentityStatus status;
    char content[DEVICE_IDENTITY_HASH_HEX];
    char metadata[DEVICE_IDENTITY_HASH_HEX];
    int file_count;
    unsigned long long total_bytes;
    char detail[DEVICE_IDENTITY_DETAIL_MAX];
} DeviceIdentity;

static inline const char* device_identity_status_name(DeviceIdentityStatus status)
{
    switch (status)
    {
    case DEVICE_IDENTITY_OK:
        return "ok";
    case DEVICE_IDENTITY_UNREADABLE:
        return "unreadable";
    case DEVICE_IDENTITY_UNSUPPORTED:
        return "unsupported";
    case DEVICE_IDENTITY_TOO_LARGE:
        return "too_large";
    }
    return "unsupported";
}

void device_identity_init(DeviceIdentity* identity);

/*
 * Scan pak_dir. Returns identity->status and fills detail on failure. The
 * caller passes a canonical directory (devserv resolves it first).
 */
DeviceIdentityStatus device_identity_scan(const char* pak_dir, int include_content,
                                          DeviceIdentity* identity);

#ifdef __cplusplus
}
#endif

#endif
