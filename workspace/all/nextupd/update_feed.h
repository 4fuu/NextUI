#ifndef NEXTUPD_FEED_H
#define NEXTUPD_FEED_H

#include "update_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * nextui-feed-v1.
 *
 * The feed is routing metadata, not a trust anchor. Each entry names a
 * nextui-update-v1 manifest and the SHA-256 of its archive. Integrity is the
 * default: the manifest binds the archive size and digest, and every file in
 * the archive has its own digest. An entry may also name a detached Ed25519
 * signature; it is used only when the caller supplies a trusted public key.
 *
 * Feed entries are the compatibility matrix: one row per platform and version,
 * with the minimum running system that may install it.
 *
 * The official feed is this project's GitHub Releases. A caller may override
 * it with --feed or NEXTUI_UPDATE_FEED for local testing or a deliberate
 * third-party source reached over ADB.
 */

#define UPD_DEFAULT_FEED_URL \
    "https://github.com/4fuu/NextUI/releases/latest/download/nextui-feed.json"

#define UPD_MAX_FEED_BYTES (2U * 1024U * 1024U)
#define UPD_MAX_FEED_ENTRIES 64
#define UPD_MAX_URL 2048

typedef struct
{
    char platform[UPD_MAX_PLATFORM + 1];
    char version[UPD_MAX_VERSION + 1];
    char min_system[UPD_MAX_VERSION + 1];
    int has_min_system;
    char manifest_url[UPD_MAX_URL + 1];
    char signature_url[UPD_MAX_URL + 1]; /* empty when the feed omits one */
    char archive_url[UPD_MAX_URL + 1];
    char archive_name[UPD_MAX_FILENAME + 1];
    uint64_t archive_size;
    unsigned char archive_sha256[UPD_SHA256_BYTES];
} UpdFeedEntry;

typedef struct
{
    char channel[32];
    char generated_at[40];
    UpdFeedEntry* entries;
    size_t entry_count;
} UpdFeed;

void upd_feed_init(UpdFeed* feed);
void upd_feed_free(UpdFeed* feed);

/* Load and strictly validate a nextui-feed-v1 document from a local file. */
int upd_feed_load(const char* path, UpdFeed* feed, UpdError* error);

/*
 * Select the newest compatible entry for platform whose min_system is not newer
 * than system_version. Returns UPD_E_PLATFORM when the feed has no row for the
 * platform or every row requires a newer system, and UPD_E_INVALID for a
 * malformed system version. The returned pointer is owned by the feed.
 */
int upd_feed_select(const UpdFeed* feed, const char* platform, const char* system_version,
                    const UpdFeedEntry** entry, UpdError* error);

/*
 * Cross-check routing metadata against the already verified manifest. A feed
 * cannot substitute a different package, version, minimum system, or archive
 * digest for the one the caller verified.
 */
int upd_feed_entry_matches_manifest(const UpdFeedEntry* entry, const UpdManifest* manifest,
                                    UpdError* error);

/* True for http:// and https:// sources. A plain path is local. */
int upd_url_is_remote(const char* source);

/*
 * Resolve an absolute or relative asset location against a feed source. Remote
 * feeds may use root-relative URLs; local feeds resolve against their directory.
 */
int upd_url_resolve(const char* feed_source, const char* location, char* out, size_t out_size,
                    UpdError* error);

#ifdef __cplusplus
}
#endif

#endif
