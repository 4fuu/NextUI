#ifndef NEXTUPD_MANIFEST_H
#define NEXTUPD_MANIFEST_H

#include "update_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * nextui-update-v1 manifest.
 *
 * The manifest is the verified identity of one package. It names the package
 * id, kind, version, platform, archive bytes and every regular file inside the
 * archive. An optional detached signature covers the exact manifest bytes, so
 * the writer below is canonical and reformatting a manifest invalidates a
 * signature. The writer is canonical in both modes.
 */

typedef struct
{
    char path[UPD_MAX_REL_PATH + 1];
    uint64_t size;
    unsigned char sha256[UPD_SHA256_BYTES];
} UpdFile;

typedef struct
{
    char id[UPD_MAX_ID + 1];
    char kind[16];
    char version[UPD_MAX_VERSION + 1];
    char platform[UPD_MAX_PLATFORM + 1];
    char min_system[UPD_MAX_VERSION + 1];
    int has_min_system;

    char archive_name[UPD_MAX_FILENAME + 1];
    uint64_t archive_size;
    unsigned char archive_sha256[UPD_SHA256_BYTES];

    UpdFile* files;
    size_t file_count;
    uint64_t total_bytes;

    /* Original bytes when loaded from disk; NULL for generated manifests. */
    unsigned char* raw;
    size_t raw_length;
} UpdManifest;

void upd_manifest_init(UpdManifest* manifest);
void upd_manifest_free(UpdManifest* manifest);

/* Append one file entry. Duplicate detection happens in finalize. */
int upd_manifest_add_file(UpdManifest* manifest, const char* path, uint64_t size,
                          const unsigned char sha256[UPD_SHA256_BYTES], UpdError* error);

/* Validate identity fields and archive naming; does not require file entries. */
int upd_manifest_check_identity(const UpdManifest* manifest, UpdError* error);

/* Validate identity and entries, sort files, and reject duplicates. */
int upd_manifest_finalize(UpdManifest* manifest, UpdError* error);

/*
 * Load a manifest from disk. On success manifest->raw owns the exact bytes that
 * a detached signature must cover. On failure the manifest is left empty.
 */
int upd_manifest_load(const char* path, UpdManifest* manifest, UpdError* error);

/* Render canonical JSON to a heap buffer (NUL-terminated, length excludes NUL). */
int upd_manifest_render(const UpdManifest* manifest, char** out, size_t* out_length,
                        UpdError* error);

/* Render and publish atomically. */
int upd_manifest_write(const UpdManifest* manifest, const char* path, UpdError* error);

int upd_manifest_is_system(const UpdManifest* manifest);
int upd_manifest_platform_matches(const UpdManifest* manifest, const char* platform);
int upd_manifest_check_min_system(const UpdManifest* manifest, const char* system_version,
                                  UpdError* error);
int upd_is_known_platform(const char* platform);

/*
 * Boot payload contract.
 *
 * A system package may carry a `.tmp_update/` subtree. When it does, the
 * signed manifest must also list the files the boot chain needs to recover an
 * interrupted install without reading `.system`:
 *
 *   .tmp_update/updater
 *   .tmp_update/nextupd-recover.sh
 *   .tmp_update/<platform>.sh
 *   .tmp_update/<platform>/nextupd.elf
 *
 * The subtree is optional so existing `.system`-only packages keep working.
 * Paths stay inside safe relative names; the installer still applies the same
 * ZIP parser and manifest hash checks as before.
 */
int upd_manifest_has_boot(const UpdManifest* manifest);
int upd_manifest_boot_bytes(const UpdManifest* manifest, uint64_t* bytes);
int upd_manifest_check_boot(const UpdManifest* manifest, UpdError* error);
int upd_boot_required_tool(const char* platform, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
