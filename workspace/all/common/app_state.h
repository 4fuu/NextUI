#ifndef APP_STATE_H
#define APP_STATE_H

#include <stddef.h>

#include "pak_manifest.h"

/*
 * Application state slots (plan B3).
 *
 * An application opts in by declaring the exact "saves" capability in pak.json.
 * The declaration alone never makes an application resumable: app_state_inspect()
 * returns APP_STATE_VALID only after the slot metadata parses and every payload
 * file still matches the recorded size and SHA-256 digest.
 *
 * The desktop never parses payload files. It only validates the metadata written
 * by `nextstate commit` and passes the payload directory back to the application
 * on a resume launch.
 */

#define APP_STATE_ID_MAX 128
#define APP_STATE_VERSION_MAX 32
#define APP_STATE_PATH_MAX 512
#define APP_STATE_REASON_MAX 40
#define APP_STATE_REL_PATH_MAX 240
#define APP_STATE_CMD_MAX 2048

#define APP_STATE_MAX_FILES 512
#define APP_STATE_MAX_DEPTH 8
#define APP_STATE_MAX_FILE_BYTES (32ull * 1024ull * 1024ull)
#define APP_STATE_MAX_TOTAL_BYTES (64ull * 1024ull * 1024ull)
#define APP_STATE_MAX_METADATA_BYTES (64u * 1024u)

typedef enum
{
    APP_STATE_ABSENT = 0,      /* declared capability, no committed state */
    APP_STATE_VALID = 1,       /* metadata and payload both validate */
    APP_STATE_INVALID = 2,     /* metadata exists but is corrupt or unsafe */
    APP_STATE_UNSUPPORTED = 3, /* manifest missing/invalid or "saves" not declared */
    APP_STATE_ERROR = 4,       /* I/O or argument error while inspecting */
} AppStateStatus;

typedef struct
{
    AppStateStatus status;
    char app_id[APP_STATE_ID_MAX];
    char app_version[APP_STATE_VERSION_MAX]; /* manifest version, or empty */
    char slot_dir[APP_STATE_PATH_MAX];       /* <root>/<slot> */
    char payload_dir[APP_STATE_PATH_MAX];    /* <slot>/state */
    long long saved_at;                       /* unix seconds from metadata */
    unsigned file_count;
    unsigned long long total_bytes;
    char reason[APP_STATE_REASON_MAX]; /* stable diagnostic, never UI copy */
} AppStateInfo;

typedef enum
{
    APP_STATE_COMMIT_OK = 0,
    APP_STATE_COMMIT_UNSUPPORTED = 1,
    APP_STATE_COMMIT_NO_PAYLOAD = 2,
    APP_STATE_COMMIT_UNSAFE_PATH = 3,
    APP_STATE_COMMIT_TOO_LARGE = 4,
    APP_STATE_COMMIT_IO = 5,
} AppStateCommitResult;

typedef enum
{
    APP_LAUNCH_NORMAL = 0, /* legacy command, plus state env for declared apps */
    APP_LAUNCH_RESUME = 1, /* requires APP_STATE_VALID; sets RESUME=1, no extra argv */
} AppLaunchMode;

/*
 * Inspect <root>/<slot>/resume.meta and verify the payload.
 * `manifest` must come from pak_manifest_load(); it is never re-read here, so
 * callers keep one manifest snapshot for capability and state decisions.
 * `info` may be NULL. Passing NULL is allowed for a yes/no caller.
 */
AppStateStatus app_state_inspect(const char* root, const char* pak_dir,
                                 const PakManifest* manifest, AppStateInfo* info);

/*
 * Convenience predicate for menus and the shared overlay. True only for
 * APP_STATE_VALID, which already requires both the capability declaration and a
 * matching, complete payload. Never treat a declaration or an old file as true.
 */
int app_state_is_resumable(const char* root, const char* pak_dir,
                           const PakManifest* manifest);

/*
 * Create the slot and payload directories for a normal launch. Best effort:
 * returns 0 when they are not usable, but callers should still launch the app.
 * Existing symlinked slot directories are rejected.
 */
int app_state_prepare(const char* root, const char* pak_dir,
                      const PakManifest* manifest, AppStateInfo* info);

/*
 * Publish the current payload as the resumable state. The caller (normally the
 * `nextstate commit` CLI run by the application) writes payload files first,
 * then calls this function. Metadata is replaced atomically only after every
 * file has been validated and hashed. On failure the previous metadata is left
 * untouched; an interrupted save is therefore never advertised as resumable.
 *
 * `error` receives a stable reason such as "unsupported_capability",
 * "no_payload", "unsafe_path", "too_large", or "io_error".
 */
AppStateCommitResult app_state_commit(const char* root, const char* pak_dir,
                                      const PakManifest* manifest,
                                      char* error, size_t error_size);

/*
 * Remove the slot for this application. Idempotent: an absent slot is success.
 * The metadata is removed before the payload, so an interrupted clear can never
 * leave a resumable state behind. Never follows symlinks out of the slot.
 */
int app_state_clear(const char* root, const char* pak_dir,
                    const PakManifest* manifest, char* error, size_t error_size);

/*
 * Build the shell command written to /tmp/next.
 *
 * Normal launch for a saves-capable app:
 *   NEXTUI_APP_ID=... NEXTUI_APP_PAK_DIR=... NEXTUI_APP_STATE_DIR=... \
 *   NEXTUI_APP_RESUME=0 '<pak>/launch.sh'
 *
 * Resume launch (only when app_state_inspect() is VALID): the same command
 * with NEXTUI_APP_RESUME=1. No extra arguments are appended: the system loop's
 * hook parser must keep classifying the launch as a pak, not a rom.
 *
 * Legacy applications keep the exact old command and receive no new variables.
 * Returns 1 on success, 0 when the mode is not available.
 */
int app_state_build_launch(const char* root, const char* pak_dir,
                           const PakManifest* manifest, AppLaunchMode mode,
                           char* out, size_t out_size);

const char* app_state_status_name(AppStateStatus status);
const char* app_state_commit_name(AppStateCommitResult result);

#endif
