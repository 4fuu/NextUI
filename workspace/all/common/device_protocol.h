#ifndef DEVICE_PROTOCOL_H
#define DEVICE_PROTOCOL_H

#include <stddef.h>

#include "device_gate.h"
#include "device_session.h"
#include "device_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Newline-delimited JSON protocol between devserv and nextdev.
 *
 * Request fields (all flat strings except args):
 *   {"op":"ping"}
 *   {"op":"status","app":"id","pak":"/path/to/App.pak"}
 *   {"op":"call","app":"id","pak":"/path/to/App.pak",
 *    "capability":"display.brightness","action":"get","args":{"value":5}}
 *   {"op":"session-start","app":"id","pak":"/path/to/App.pak"}
 *   {"op":"session-stop","session_id":"0123456789abcdef"}
 *   {"op":"session-status"}
 *   {"op":"app-action","action":"home|overlay","at_ms":12345,
 *    "session_id":"0123456789abcdef"}
 *   {"op":"system-feedback","action":"volume|brightness|colortemp|mute"}
 *   {"op":"app-exit-result","session_id":"0123456789abcdef","forced":true}
 *
 * app-action is the keymon seam: keymon supplies the monotonic press time so a
 * delayed request cannot apply to a newer session. It never carries a PID.
 * system-feedback only asks who owns the visual feedback for a system key; it
 * changes no state and records no exit.
 * app-exit-result is the launch parent's bounded-wait report; a late report is
 * rejected by the same session-id guard as session-stop.
 *
 * There is deliberately no grant/revoke operation here. The app-facing service
 * can only report status, dispatch already-authorized calls, and run the C3
 * session lifecycle. Session operations do not bypass the capability gate.
 */

#define DEVICE_PROTOCOL_LINE_MAX 8192
#define DEVICE_PROTOCOL_ARGS_MAX 2048

/*
 * Cooperation protocol generation (CONTRACT.md §3). `version` stays the C1
 * service revision; `protocol` is this number.
 */
#define DEVICE_PROTOCOL_GENERATION 1
#define DEVICE_PROTOCOL_CONTRACT "nextui-app/1"

#define DEVICE_PROTOCOL_FEATURE_MAX 8
#define DEVICE_PROTOCOL_FEATURE_NAME_MAX 16
#define DEVICE_PROTOCOL_KIND_MAX 8
#define DEVICE_PROTOCOL_KIND_NAME_MAX 16
#define DEVICE_PROTOCOL_SECTION_MAX 8
#define DEVICE_PROTOCOL_SECTION_NAME_MAX 16
#define DEVICE_PROTOCOL_CLIENT_MAX 64
#define DEVICE_PROTOCOL_SLEEP_ID_MAX 17
#define DEVICE_PROTOCOL_REASON_MAX 64
#define DEVICE_PROTOCOL_WOKE_BY_MAX 16
#define DEVICE_PROTOCOL_RESULT_MAX 16

/* Limits (CONTRACT.md §15). The server advertises exactly these. */
#define DEVICE_PROTOCOL_EVENT_QUEUE 64
#define DEVICE_PROTOCOL_SUBSCRIBE_MAX 8
#define DEVICE_PROTOCOL_SLEEP_ACK_MS 1500
#define DEVICE_PROTOCOL_INHIBIT_MAX_MS 300000
#define DEVICE_PROTOCOL_SLEEP_COOLDOWN_MS 1000
#define DEVICE_PROTOCOL_HEARTBEAT_MS 15000
#define DEVICE_PROTOCOL_STATUS_MIN_INTERVAL_MS 500
#define DEVICE_PROTOCOL_STATUS_RESYNC_MS 30000
#define DEVICE_PROTOCOL_FEEDBACK_MIN_INTERVAL_MS 100

typedef enum
{
    DEVICE_OP_INVALID = 0,
    DEVICE_OP_PING,
    DEVICE_OP_STATUS,
    DEVICE_OP_CALL,
    DEVICE_OP_SESSION_START,
    DEVICE_OP_SESSION_STOP,
    DEVICE_OP_SESSION_STATUS,
    DEVICE_OP_APP_ACTION,
    DEVICE_OP_SYSTEM_FEEDBACK,
    DEVICE_OP_APP_EXIT_RESULT,
    /* protocol 1 */
    DEVICE_OP_HELLO,
    DEVICE_OP_SUBSCRIBE,
    DEVICE_OP_BATTERY_STATUS,
    DEVICE_OP_STATUS_GET,
    DEVICE_OP_SLEEP_REQUEST,
    DEVICE_OP_SLEEP_ACK,
    DEVICE_OP_SLEEP_INHIBIT,
    DEVICE_OP_SLEEP_RELEASE,
    DEVICE_OP_SLEEP_FINISHED,
    DEVICE_OP_SLEEP_STATUS,
    DEVICE_OP_HUD_POLICY,
    DEVICE_OP_OVERLAY_STATE,
    DEVICE_OP_OVERLAY_HOST,
} DeviceOp;

typedef struct
{
    DeviceOp op;
    char app[DEVICE_GRANT_APP_MAX];
    char pak[DEVICE_GRANT_PATH_MAX];
    char capability[DEVICE_GRANT_CAPABILITY_MAX];
    char action[DEVICE_GATE_ACTION_MAX];
    char session_id[DEVICE_SESSION_ID_MAX];
    unsigned long long at_ms;
    int forced;
    int has_forced;
    int has_args;
    char args[DEVICE_PROTOCOL_ARGS_MAX];

    /* protocol 1. Every new field has a has_* flag so a handler can tell
     * "absent" from "present and zero". */
    char client[DEVICE_PROTOCOL_CLIENT_MAX];
    int protocol;
    int has_protocol;
    char features[DEVICE_PROTOCOL_FEATURE_MAX][DEVICE_PROTOCOL_FEATURE_NAME_MAX];
    int feature_count;
    int has_features;
    char kinds[DEVICE_PROTOCOL_KIND_MAX][DEVICE_PROTOCOL_KIND_NAME_MAX];
    int kind_count;
    int has_kinds;
    char fields[DEVICE_PROTOCOL_SECTION_MAX][DEVICE_PROTOCOL_SECTION_NAME_MAX];
    int field_count;
    int has_fields;
    char mode[8];
    int has_mode;
    int open;
    int has_open;
    int host;
    int has_host;
    char sleep_id[DEVICE_PROTOCOL_SLEEP_ID_MAX];
    char reason[DEVICE_PROTOCOL_REASON_MAX];
    char initiator[16];
    char woke_by[DEVICE_PROTOCOL_WOKE_BY_MAX];
    char result[DEVICE_PROTOCOL_RESULT_MAX];
    int timeout_ms;
    int has_timeout_ms;
    int paused;
    int has_paused;
    int saved;
    int has_saved;
    char detail[DEVICE_SESSION_DETAIL_MAX];
    int min_interval_ms;
    int has_min_interval_ms;
} DeviceRequestMessage;

void device_request_message_init(DeviceRequestMessage* message);

/* Strict parse of one request line. Returns 1 on success. */
int device_protocol_parse_request(const char* line, DeviceRequestMessage* message);

/*
 * Strict parse of one request line, including the protocol 1 operations.
 * device_protocol_parse_request() is the C1/C3/C4 surface and keeps rejecting
 * the new ops so an old caller can never half-handle one; devserv and the
 * cooperation tests use this entry point.
 */
int device_protocol_parse_message(const char* line, DeviceRequestMessage* message);

/* Feature / HUD-kind / status-section names accepted by the parser. */
int device_protocol_feature_is_known(const char* name);
int device_protocol_hud_kind_is_known(const char* name);
int device_protocol_sleep_reason_is_valid(const char* reason);
int device_protocol_woke_by_is_valid(const char* woke_by);
int device_protocol_sleep_result_is_valid(const char* result);
int device_protocol_sleep_id_is_valid(const char* sleep_id);
int device_protocol_session_id_is_valid(const char* session_id);

/* Build one request line (no trailing newline). */
int device_protocol_build_request(const char* op, const char* app, const char* pak,
                                  const char* capability, const char* action, const char* args,
                                  char* out, size_t out_size);

/* Session requests reuse the app/pak/id fields without capability/action. */
int device_protocol_build_session_request(const char* op, const char* app, const char* pak,
                                          const char* session_id, char* out, size_t out_size);

/* C4 keymon request (no PID) and launch-parent outcome report. */
int device_protocol_build_app_action_request(const char* action, unsigned long long at_ms,
                                             const char* session_id, char* out,
                                             size_t out_size);
int device_protocol_build_app_exit_result_request(const char* session_id, int forced,
                                                  char* out, size_t out_size);

/* Response writers; each returns 1 when the whole line fit. */
int device_protocol_write_ping_response(char* out, size_t out_size);
int device_protocol_write_ping_response_protocol(char* out, size_t out_size);

/*
 * Writes the shared `limits` object body (no surrounding braces), used by ping,
 * hello and subscription.ready so every place advertises the same numbers.
 */
int device_protocol_write_limits(char* out, size_t out_size);

/* Feature-family mask helpers for the event channel. */
typedef enum
{
    DEVICE_EVENT_FAMILY_SLEEP = 1u << 0,
    DEVICE_EVENT_FAMILY_STATUS = 1u << 1,
    DEVICE_EVENT_FAMILY_HUD = 1u << 2,
    DEVICE_EVENT_FAMILY_OVERLAY = 1u << 3,
    DEVICE_EVENT_FAMILY_ALL = 0xFu
} DeviceEventFamily;

unsigned device_protocol_family_for_name(const char* name);
const char* device_protocol_family_name(unsigned family);

/* `hello` response (CONTRACT.md §3.2). session may be NULL when inactive. */
int device_protocol_write_hello_response(const DeviceSession* session, int overlay_host,
                                         int overlay_host_declared, int overlay_open,
                                         int matches_pak, int battery_available,
                                         const DeviceStatusBattery* battery, int protocol,
                                         unsigned features, char* out, size_t out_size);

int device_protocol_write_protocol_unsupported_response(char* out, size_t out_size);

/* `subscription.ready` (CONTRACT.md §5.1). */
int device_protocol_write_subscription_ready(unsigned long long at_ms, unsigned families,
                                             const char* session_id, int overlay_host,
                                             char* out, size_t out_size);

/* `battery.status` and `status.get` (CONTRACT.md §8). */
int device_protocol_write_battery_response(const DeviceStatusBattery* battery,
                                           unsigned long long at_ms, char* out, size_t out_size);
int device_protocol_write_status_response_sections(const DeviceStatusSnapshot* status,
                                                   unsigned sections, int overlay_open,
                                                   unsigned long long at_ms, char* out,
                                                   size_t out_size);

/* `sleep.status` (CONTRACT.md §6.9). */
typedef struct
{
    const char* phase; /* awake|preparing|sleeping */
    int pending;
    char pending_sleep_id[DEVICE_PROTOCOL_SLEEP_ID_MAX];
    char pending_reason[DEVICE_PROTOCOL_REASON_MAX];
    int pending_deadline_ms;
    unsigned long long pending_deadline_at_ms;
    int pending_subscribers;
    int pending_acks;
    int inhibit_active;
    const char* inhibit_scope; /* prepare|automatic */
    char inhibit_reason[DEVICE_PROTOCOL_REASON_MAX];
    unsigned long long inhibit_expires_at_ms;
    char inhibit_sleep_id[DEVICE_PROTOCOL_SLEEP_ID_MAX];
    int last_valid;
    char last_sleep_id[DEVICE_PROTOCOL_SLEEP_ID_MAX];
    char last_reason[DEVICE_PROTOCOL_REASON_MAX];
    char last_result[DEVICE_PROTOCOL_RESULT_MAX];
    char last_woke_by[DEVICE_PROTOCOL_WOKE_BY_MAX];
    unsigned long long last_started_at_ms;
    unsigned long long last_ended_at_ms;
    unsigned long long last_slept_ms;
} DeviceSleepStatusView;

int device_protocol_write_sleep_status_response(const DeviceSleepStatusView* status,
                                                unsigned long long at_ms, char* out,
                                                size_t out_size);

/* Sleep handshake responses (CONTRACT.md §6.2, §6.4, §6.5, §6.6, §6.7). */
typedef struct
{
    const char* decision; /* proceed|defer */
    char sleep_id[DEVICE_PROTOCOL_SLEEP_ID_MAX];
    int subscribers;
    int acks;
    int inhibit;
    char inhibit_reason[DEVICE_PROTOCOL_REASON_MAX];
    char inhibit_detail[DEVICE_SESSION_DETAIL_MAX];
    unsigned long long inhibit_until_ms;
    unsigned long long defer_until_ms;
    const char* detail;
} DeviceSleepDecisionView;

int device_protocol_write_sleep_request_response(const DeviceSleepDecisionView* view, char* out,
                                                 size_t out_size);
int device_protocol_write_sleep_ack_response(const char* sleep_id, const char* decision,
                                             const char* detail, char* out, size_t out_size);
int device_protocol_write_sleep_inhibit_response(const char* scope, const char* sleep_id,
                                                 const char* decision, int inhibit_active,
                                                 unsigned long long expires_at_ms,
                                                 const char* reason, char* out,
                                                 size_t out_size);
int device_protocol_write_sleep_release_response(int released, const char* scope, char* out,
                                                 size_t out_size);
int device_protocol_write_sleep_finished_response(const char* sleep_id,
                                                  unsigned long long slept_ms, char* out,
                                                  size_t out_size);

/* `hud.policy` (CONTRACT.md §7.5). */
typedef struct
{
    int volume;
    int brightness;
    int colortemp;
    int mute;
} DeviceHudPolicyView;

int device_protocol_write_hud_policy_response(const char* session_id, int overlay_open,
                                              int overlay_host,
                                              const DeviceHudPolicyView* policy,
                                              const DeviceHudPolicyView* effective, char* out,
                                              size_t out_size);

/* `overlay.state` and `overlay.host` (CONTRACT.md §7.4, §7.9.1). */
int device_protocol_write_overlay_state_response(const char* session_id, int overlay_host,
                                                 int overlay_open, char* out, size_t out_size);
int device_protocol_write_overlay_host_response(const char* session_id, int overlay_host,
                                                int overlay_host_declared,
                                                unsigned long long host_changed_at_ms,
                                                int overlay_open, const char* effective_hud,
                                                char* out, size_t out_size);

/* Event lines (CONTRACT.md §5.2). */
int device_protocol_write_heartbeat_event(unsigned seq, unsigned long long at_ms, char* out,
                                          size_t out_size);
int device_protocol_write_overflow_event(unsigned seq, unsigned long long at_ms, int dropped,
                                         const char* session_id, char* out, size_t out_size);
int device_protocol_write_session_ended_event(unsigned seq, unsigned long long at_ms,
                                              const char* session_id, const char* reason,
                                              char* out, size_t out_size);
int device_protocol_write_sleep_prepare_event(unsigned seq, unsigned long long at_ms,
                                              const char* session_id, const char* sleep_id,
                                              const char* reason, int deadline_ms,
                                              unsigned long long deadline_at_ms,
                                              const DeviceStatusBattery* battery, char* out,
                                              size_t out_size);
int device_protocol_write_wake_done_event(unsigned seq, unsigned long long at_ms,
                                          const char* session_id, const char* sleep_id,
                                          unsigned long long slept_ms, const char* reason,
                                          const char* woke_by, const char* result,
                                          const DeviceStatusBattery* battery, char* out,
                                          size_t out_size);
int device_protocol_write_status_snapshot_event(unsigned seq, unsigned long long at_ms,
                                                const char* session_id, int initial,
                                                const DeviceStatusSnapshot* status,
                                                unsigned sections, int overlay_open,
                                                char* out, size_t out_size);
int device_protocol_write_status_changed_event(unsigned seq, unsigned long long at_ms,
                                               const char* session_id,
                                               const DeviceStatusSnapshot* status,
                                               unsigned sections, int overlay_open, char* out,
                                               size_t out_size);
int device_protocol_write_feedback_event(unsigned seq, unsigned long long at_ms,
                                         const char* session_id, const char* kind, int value,
                                         int min, int max, int mute, int locked,
                                         int overlay_open, const char* sink, const char* source,
                                         char* out, size_t out_size);
int device_protocol_write_overlay_home_event(unsigned seq, unsigned long long at_ms,
                                             const char* session_id, const char* action,
                                             char* out, size_t out_size);

/* Generic success/error envelope with extra raw JSON fields, used by the new
 * operations that do not need a dedicated writer. */
int device_protocol_write_ok_response(const char* extra_json, char* out, size_t out_size);

int device_protocol_write_call_response(const DeviceCallResult* result, char* out, size_t out_size);
int device_protocol_write_status_response(const DeviceStatusResult* status, char* out,
                                          size_t out_size);
/* The session response carries the runtime overlay state alongside the C3
 * fields: effective host, manifest declaration, and whether the in-process
 * overlay is open (CONTRACT.md §4, §7.9.1). */
int device_protocol_write_session_response(const DeviceSession* session, int overlay_host,
                                           int overlay_host_declared, int overlay_open,
                                           DeviceSessionResult result, int cleaned,
                                           const char* error, const char* detail, char* out,
                                           size_t out_size);
int device_protocol_write_app_action_response(const DeviceSession* session,
                                              DeviceSessionResult result, const char* action,
                                              int handled_by_app, int handled_by_system,
                                              const char* error, const char* detail, char* out,
                                              size_t out_size);
int device_protocol_write_error_response(const char* error, const char* detail, char* out,
                                         size_t out_size);

/* 1 = {"ok":true,...}, 0 = {"ok":false,...}, -1 = malformed/unreadable. */
int device_protocol_response_ok(const char* line);

#ifdef __cplusplus
}
#endif

#endif
