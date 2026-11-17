#ifndef DEVICE_PROTOCOL_H
#define DEVICE_PROTOCOL_H

#include <stddef.h>

#include "device_gate.h"
#include "device_session.h"

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
} DeviceRequestMessage;

void device_request_message_init(DeviceRequestMessage* message);

/* Strict parse of one request line. Returns 1 on success. */
int device_protocol_parse_request(const char* line, DeviceRequestMessage* message);

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
int device_protocol_write_call_response(const DeviceCallResult* result, char* out, size_t out_size);
int device_protocol_write_status_response(const DeviceStatusResult* status, char* out,
                                          size_t out_size);
int device_protocol_write_session_response(const DeviceSession* session,
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
