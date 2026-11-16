#ifndef DEVICE_GATE_H
#define DEVICE_GATE_H

#include <stddef.h>

#include "device_backend.h"
#include "device_grants.h"
#include "device_identity.h"
#include "pak_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Dispatch and legacy authorization gate for the device service (plan C1).
 *
 * Default policy: an installed pak may use every capability this build
 * implements. `pak.json` declarations describe what the app supports; they are
 * not an authorization request and never block the default path. A backend
 * that is not implemented still reports `unavailable`; an unknown capability
 * is still rejected.
 *
 * `device_gate_set_default_allow(1)` selects that policy. The legacy
 * grant/identity checks below remain available for explicit compatibility
 * builds (`device_gate_set_default_allow(0)`, used by the old gate tests);
 * devserv selects default-allow unless started with --require-grants.
 *
 * Legacy grant path (when default-allow is off): every call is resolved as
 *   1. pak directory and manifest at that exact path
 *   2. known capability name
 *   3. declared request (gated capabilities only)
 *   4. exact grant: app id + canonical path + manifest version + capability
 *   5. pak content identity: metadata signature, then SHA-256 content hash
 *   6. backend availability
 *   7. backend dispatch
 *
 * With default-allow on, steps 3-5 are skipped; capability validity,
 * availability and dispatch are unchanged. The settings layer owns the legacy
 * grant store; the app-facing protocol still exposes no grant operation.
 */

#define DEVICE_GATE_DETAIL_MAX 160
#define DEVICE_GATE_VALUE_MAX 512
#define DEVICE_GATE_STATUS_MAX 32
#define DEVICE_GATE_ACTION_MAX 32

typedef enum
{
    DEVICE_GATE_OK = 0,
    DEVICE_GATE_BAD_REQUEST,
    DEVICE_GATE_UNKNOWN_CAPABILITY,
    DEVICE_GATE_INVALID_MANIFEST,
    DEVICE_GATE_NOT_DECLARED,
    DEVICE_GATE_GRANT_STORE_INVALID,
    DEVICE_GATE_GRANT_STORE_ERROR,
    DEVICE_GATE_IDENTITY_UNAVAILABLE,
    DEVICE_GATE_IDENTITY_MISMATCH,
    DEVICE_GATE_UNAUTHORIZED,
    DEVICE_GATE_UNAVAILABLE,
    DEVICE_GATE_BACKEND_ERROR,
} DeviceGateError;

static inline const char* device_gate_error_name(DeviceGateError error)
{
    switch (error)
    {
    case DEVICE_GATE_OK:
        return "ok";
    case DEVICE_GATE_BAD_REQUEST:
        return "bad_request";
    case DEVICE_GATE_UNKNOWN_CAPABILITY:
        return "unknown_capability";
    case DEVICE_GATE_INVALID_MANIFEST:
        return "invalid_manifest";
    case DEVICE_GATE_NOT_DECLARED:
        return "not_declared";
    case DEVICE_GATE_GRANT_STORE_INVALID:
        return "grant_store_invalid";
    case DEVICE_GATE_GRANT_STORE_ERROR:
        return "grant_store_error";
    case DEVICE_GATE_IDENTITY_UNAVAILABLE:
        return "identity_unavailable";
    case DEVICE_GATE_IDENTITY_MISMATCH:
        return "identity_mismatch";
    case DEVICE_GATE_UNAUTHORIZED:
        return "unauthorized";
    case DEVICE_GATE_UNAVAILABLE:
        return "unavailable";
    case DEVICE_GATE_BACKEND_ERROR:
        return "backend_error";
    }
    return "backend_error";
}

typedef struct
{
    char app[DEVICE_GRANT_APP_MAX];               /* optional/claimed; empty = derive */
    char pak[DEVICE_GRANT_PATH_MAX];              /* pak directory */
    char capability[DEVICE_GRANT_CAPABILITY_MAX];
    char action[DEVICE_GATE_ACTION_MAX];          /* optional; default "get" */
    const char* args;                             /* optional raw JSON value */
} DeviceCallRequest;

typedef struct
{
    DeviceGateError error;
    char detail[DEVICE_GATE_DETAIL_MAX];
    char capability[DEVICE_GRANT_CAPABILITY_MAX];
    int supported;
    int declared;
    int available;
    int authorized;
    char value[DEVICE_GATE_VALUE_MAX];            /* JSON value on success */
} DeviceCallResult;

typedef struct
{
    char name[DEVICE_GRANT_CAPABILITY_MAX];
    int supported;
    int grant_required;
    int declared;
    int available;
    int authorized;
} DeviceCapabilityStatus;

typedef struct
{
    DeviceGateError error;
    char detail[DEVICE_GATE_DETAIL_MAX];
    char app[DEVICE_GRANT_APP_MAX];
    char pak[DEVICE_GRANT_PATH_MAX];
    PakManifestStatus manifest_status;
    DeviceGrantsStatus grants_status;
    DeviceIdentityStatus identity_status;
    char identity_detail[DEVICE_IDENTITY_DETAIL_MAX];
    const char* backend_name;
    int capability_count;
    DeviceCapabilityStatus capabilities[DEVICE_GATE_STATUS_MAX];
} DeviceStatusResult;

void device_call_request_init(DeviceCallRequest* request);
void device_call_result_init(DeviceCallResult* result);
void device_status_result_init(DeviceStatusResult* status);

/*
 * Select the permission policy for this process. 1 (devserv default): installed
 * apps may call every known, implemented capability without a grant or a
 * manifest declaration. 0: legacy grant/identity enforcement remains available
 * for compatibility tests and explicit --require-grants builds.
 */
void device_gate_set_default_allow(int enabled);
int device_gate_default_allow(void);

DeviceGateError device_gate_call(const DeviceCallRequest* request, const char* grants_path,
                                 const DeviceBackend* backend, DeviceCallResult* result);

DeviceGateError device_gate_status(const char* claimed_app, const char* pak_path,
                                   const char* grants_path, const DeviceBackend* backend,
                                   DeviceStatusResult* status);

/*
 * Settings-layer operations. These are not reachable from devserv's app
 * socket. grant() re-reads pak.json and refuses undeclared, unknown, and
 * read-only capabilities.
 */
DeviceGateError device_gate_grant(const DeviceCallRequest* request, const char* grants_path,
                                  char* detail, size_t detail_size);
DeviceGateError device_gate_revoke(const DeviceCallRequest* request, const char* grants_path,
                                   char* detail, size_t detail_size);
DeviceGateError device_gate_revoke_all(const char* claimed_app, const char* pak_path,
                                       const char* grants_path, char* detail,
                                       size_t detail_size);
DeviceGateError device_gate_reset_grants(const char* grants_path, char* detail,
                                         size_t detail_size);

#ifdef __cplusplus
}
#endif

#endif
