#ifndef NEXTUPD_APP_H
#define NEXTUPD_APP_H

#include "update_transaction.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Offline application install transactions.
 *
 * A signed `nextui-update-v1` package with `kind: app` installs into the
 * launcher's canonical app root, `Tools/<platform>/<id>.pak/`, under an
 * explicit target root. The package never chooses its own destination: the
 * installer derives it from the signed manifest id, the explicit target
 * platform and the target root, so a package cannot land in an arbitrary SD
 * location, overwrite a different app, or touch `.system`, `.tmp_update`, or
 * user data.
 *
 * This transaction is independent of the system transaction. It uses its own
 * state directory `.nextupd-app-txn/<id>/`, its own record schema
 * `nextui-app-install-txn-v1`, and its own per-app backup. It never reads or
 * writes `.nextupd-txn`, `.system`, `.system.bak`, `.tmp_update`, or
 * `.tmp_update.bak`, and the `nextui-install-txn-v1` contract is untouched.
 *
 * The caller re-verifies the staged archive, manifest and signature (through
 * the same `nextupd stage`/`check` primitives the system install uses) before
 * calling here; this module re-hashes every extracted file and cross-checks the
 * destination and version policy, and it keeps one previous app as a backup so
 * an interrupted install can be committed or rolled back after a reboot.
 */

/*
 * Install a verified app package. On success `outcome` is "installed" or
 * "already_installed". A pending transaction for the same app id from an
 * earlier run is resolved before the new one starts. An equal version writes
 * nothing; an older version is refused with `downgrade_refused`.
 */
int upd_app_install_execute(const UpdInstallRequest* request, UpdTransactionResult* result,
                            UpdError* error);

/*
 * Offline arbitration entry point for app transactions. Scans
 * `.nextupd-app-txn/` for a pending record and completes an interrupted commit
 * or restores the per-app backup. force_rollback explicitly returns a committed
 * app to its recorded previous version. Never touches the system transaction.
 */
int upd_app_install_recover(const char* target_root, int force_rollback,
                            UpdTransactionResult* result, UpdError* error);

/* Nonzero when <target_root>/.nextupd-app-txn/ holds at least one txn.json. */
int upd_app_txn_present(const char* target_root);

#ifdef __cplusplus
}
#endif

#endif
