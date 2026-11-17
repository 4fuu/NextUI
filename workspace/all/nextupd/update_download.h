#ifndef NEXTUPD_DOWNLOAD_H
#define NEXTUPD_DOWNLOAD_H

#include "update_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Update transport.
 *
 * Plain HTTP and local paths are handled in-process so fixture servers can
 * exercise short reads, stalls, and timeouts deterministically. HTTPS is handed
 * to the system curl binary with TLS verification left on. The device image
 * ships no system CA store, so the transport points curl at
 * .system/res/ca-bundle.crt when that file is installed; otherwise curl's own
 * default trust store applies. When curl is absent the caller gets
 * transport_unavailable instead of an unverified fallback.
 *
 * Neither path trusts the bytes it receives. The caller must verify the signed
 * manifest and the archive digest before staging.
 */

#define UPD_DEFAULT_TIMEOUT_SECONDS 30

/*
 * Record a target root for CA bundle lookup. The transport already checks
 * $SYSTEM_PATH and the fixed device mount; this lets a caller that knows an
 * explicit root expose <root>/.system/res/ca-bundle.crt without adding a
 * download option. It is a lookup hint only and never required.
 */
void upd_download_set_target_root(const char* target_root);

/*
 * Copy source to destination. destination is created with O_EXCL|O_NOFOLLOW and
 * removed on every failure, so a caller can use a private .part path and never
 * publish a short file.
 */
int upd_download_to_file(const char* source, const char* destination, uint64_t max_bytes,
                         int timeout_seconds, UpdError* error);

/*
 * Download source into a private temporary file outside the staging root.
 * Out_path receives the full path; the caller owns it and must unlink it.
 */
int upd_download_temporary(const char* source, uint64_t max_bytes, int timeout_seconds,
                           char* out_path, size_t out_size, UpdError* error);

#ifdef __cplusplus
}
#endif

#endif
