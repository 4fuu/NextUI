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
} DeviceOp;

typedef struct
{
    DeviceOp op;
    char app[DEVICE_GRANT_APP_MAX];
    char pak[DEVICE_GRANT_PATH_MAX];
    char capability[DEVICE_GRANT_CAPABILITY_MAX];
    char action[DEVICE_GATE_ACTION_MAX];
    char session_id[DEVICE_SESSION_ID_MAX];
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

/* Response writers; each returns 1 when the whole line fit. */
int device_protocol_write_ping_response(char* out, size_t out_size);
int device_protocol_write_call_response(const DeviceCallResult* result, char* out, size_t out_size);
int device_protocol_write_status_response(const DeviceStatusResult* status, char* out,
                                          size_t out_size);
int device_protocol_write_session_response(const DeviceSession* session,
                                           DeviceSessionResult result, int cleaned,
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
