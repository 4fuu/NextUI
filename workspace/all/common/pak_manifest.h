#ifndef PAK_MANIFEST_H
#define PAK_MANIFEST_H

#include <stddef.h>

/*
 * pak.json is the single application manifest (plan X1). This module only
 * consumes metadata: it does not launch apps, render icons, or enforce
 * min_system. A pak without pak.json keeps the legacy launch.sh-only behavior.
 *
 * Capabilities are opt-in. The launcher must not infer saves/resume support
 * from an executable, a title, or any other field; only an explicitly declared
 * known capability is reported.
 */

#define PAK_MANIFEST_ID_MAX 128
#define PAK_MANIFEST_NAME_MAX 256
#define PAK_MANIFEST_VERSION_MAX 32
#define PAK_MANIFEST_PATH_MAX 512

typedef enum
{
    PAK_MANIFEST_ABSENT = 0,  /* no pak.json: legacy pak, no metadata */
    PAK_MANIFEST_OK = 1,      /* valid JSON object; individual fields may be missing */
    PAK_MANIFEST_INVALID = 2, /* pak.json exists but cannot be parsed */
} PakManifestStatus;

typedef enum
{
    PAK_CAPABILITY_SAVES = 1u << 0,
    PAK_CAPABILITY_OVERLAY = 1u << 1,
} PakCapability;

typedef struct
{
    PakManifestStatus status;
    char id[PAK_MANIFEST_ID_MAX];            /* manifest id, else pak directory name */
    char name[PAK_MANIFEST_NAME_MAX];        /* metadata only; launcher keeps the directory name */
    char version[PAK_MANIFEST_VERSION_MAX];  /* vX.Y.Z; valid only when version_valid is set */
    char exec[PAK_MANIFEST_PATH_MAX];        /* metadata only; launch.sh still owns launching */
    char icon[PAK_MANIFEST_PATH_MAX];        /* metadata only; phase B1 renders no icons */
    char min_system[PAK_MANIFEST_VERSION_MAX];
    char apostrophe[PAK_MANIFEST_VERSION_MAX]; /* build-time framework metadata, never a system tag */
    unsigned capabilities;                     /* PAK_CAPABILITY_* mask; 0 unless declared */
    int capabilities_declared;                 /* 1 when the array was present (even if empty) */
    int version_valid;
    int min_system_valid;
} PakManifest;

/*
 * Read <pak_dir>/pak.json. The result is always initialized:
 *   ABSENT  - no pak.json (legacy pak); all metadata empty, capabilities 0
 *   OK      - top-level JSON object parsed; missing/wrong-typed fields stay empty
 *   INVALID - unreadable, oversized, or malformed; all metadata empty
 * A bad manifest never prevents the caller from treating the directory as a pak.
 */
PakManifestStatus pak_manifest_load(const char* pak_dir, PakManifest* manifest);

/*
 * Scan helper for the launcher. Loads the manifest and, when a valid version
 * and safe id are present, records "<version_dir>/<id>.version" (atomic
 * replace, unchanged files are left alone). Best-effort: failures to write the
 * record do not change the returned manifest status and never abort a scan.
 */
PakManifestStatus pak_manifest_note_scan(const char* pak_dir, const char* version_dir);

/*
 * Exact capability lookup. Unknown or absent capabilities return 0; deliberately
 * no aliases (for example "resume" is not "saves").
 */
int pak_manifest_has_capability(const PakManifest* manifest, PakCapability capability);

/*
 * Pure min_system comparison against a system tag (for example "v6.15.0").
 * Returns 1 when the requirement is met. No manifest, no min_system, or a
 * malformed min_system is treated as "no enforceable requirement" (1) so a bad
 * declaration cannot lock the user out; a malformed system tag with a valid
 * requirement returns 0. This function never reads version.txt.
 */
int pak_manifest_meets_min_system(const PakManifest* manifest, const char* system_version);

#endif
