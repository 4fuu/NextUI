#ifndef NEXTUPD_TRANSACTION_H
#define NEXTUPD_TRANSACTION_H

#include "update_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Offline system install transactions.
 *
 * The installer consumes the advisory staged.json written by `nextupd stage`,
 * but never trusts it: the caller re-verifies the staged archive and manifest
 * first (and the detached signature when a trusted key is configured), then
 * cross-checks this descriptor against that manifest.
 *
 * Installation only happens in an explicitly supplied target root that already
 * contains a live `.system` tree. There is no default root, no SDCARD lookup and
 * no boot-time activation in this module. The transaction stages a complete
 * candidate tree, keeps one previous `.system` as `.system.bak`, and records
 * every phase in `.nextupd-txn/txn.json` so an interrupted run can be committed
 * or rolled back after a reboot.
 */

#define UPD_MAX_STAGED_BYTES (64U * 1024U)

typedef struct
{
    char id[UPD_MAX_ID + 1];
    char kind[16];
    char version[UPD_MAX_VERSION + 1];
    char platform[UPD_MAX_PLATFORM + 1];
    char archive[UPD_MAX_FILENAME + 1];
    char manifest[UPD_MAX_FILENAME + 1];
    /* Empty for an integrity-only stage: no signature sidecar exists. */
    char signature[UPD_MAX_FILENAME + 1];
    unsigned char archive_sha256[UPD_SHA256_BYTES];
    unsigned char manifest_sha256[UPD_SHA256_BYTES];
    char signature_key_id[UPD_MAX_KEY_ID + 1];
    long long staged_at;
} UpdStaged;

typedef struct
{
    const char* target_root;    /* required; must already contain .system */
    const char* staged_dir;     /* required; contains staged.json and package */
    const char* platform;       /* required; explicit target platform */
    const char* system_version; /* required; running system, vX.Y.Z */
    const UpdManifest* manifest; /* already signature-verified by the caller */
} UpdInstallRequest;

typedef struct
{
    char outcome[32]; /* installed, already_installed, recovered_commit, ... */
    char state[24];
    char version[UPD_MAX_VERSION + 1];
    char previous_version[UPD_MAX_VERSION + 1];
    /* Separate lifecycle for the signed .tmp_update payload. Values include
     * none, unchanged, newer, committed, rolled_back, recovered. */
    char boot_outcome[32];
    char boot_version[UPD_MAX_VERSION + 1];
    char previous_boot_version[UPD_MAX_VERSION + 1];
} UpdTransactionResult;

void upd_transaction_result_init(UpdTransactionResult* result);

/* Read and strictly validate <staged_dir>/staged.json. Writes nothing. */
int upd_staged_load(const char* staged_dir, UpdStaged* staged, UpdError* error);

/*
 * Re-hash the staged manifest and archive and compare every identity field
 * against the verified manifest. When key_id is non-NULL the staged descriptor
 * must also name that signing key. Writes nothing.
 */
int upd_staged_verify(const char* staged_dir, const UpdStaged* staged,
                      const UpdManifest* manifest, const char* key_id, UpdError* error);

/*
 * Install a verified system package. On success `outcome` is "installed" or
 * "already_installed"; a pending transaction from an earlier run is resolved
 * before the new one starts.
 */
int upd_install_execute(const UpdInstallRequest* request, UpdTransactionResult* result,
                        UpdError* error);

/*
 * Offline arbitration entry point. Without force_rollback it completes an
 * interrupted commit when the candidate is already live and restores the
 * backup otherwise. force_rollback explicitly returns a committed/installed
 * tree to its recorded previous version when a backup exists.
 */
int upd_install_recover(const char* target_root, int force_rollback,
                        UpdTransactionResult* result, UpdError* error);

#ifdef __cplusplus
}
#endif

#endif
