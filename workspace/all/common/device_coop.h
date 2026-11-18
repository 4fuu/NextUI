#ifndef DEVICE_COOP_H
#define DEVICE_COOP_H

#include <stddef.h>

#include "device_protocol.h"
#include "device_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Cooperation state machine for protocol 1 (CONTRACT.md §4-§7).
 *
 * This module owns everything that is *state* about the cooperation protocol:
 * the per-session HUD policy, the effective overlay host, the overlay-open flag,
 * the sleep handshake phases, the inhibit lease and the last completed cycle.
 * It performs no I/O and never blocks, which is what lets the devserv poll loop
 * serve a `sleep.ack` while a `sleep.request` is still waiting.
 *
 * The subscription registry and the event fan-out live in devserv.c because
 * they own file descriptors. Everything they decide (who is an ack candidate,
 * whether a decision is final, what the policy resolves to) is here.
 *
 * Session identity: this module holds a copy of the active session id and the
 * manifest's overlay declaration. devserv calls device_coop_session_start() and
 * device_coop_session_end() at exactly the points it starts/ends the C3
 * session, so the two can never disagree.
 */

#define DEVICE_COOP_HUD_KINDS 4
#define DEVICE_COOP_SLEEP_ID_SIZE 17
#define DEVICE_COOP_DETAIL_MAX 160
#define DEVICE_COOP_REASON_MAX 64

typedef enum
{
    DEVICE_COOP_HUD_VOLUME = 0,
    DEVICE_COOP_HUD_BRIGHTNESS,
    DEVICE_COOP_HUD_COLORTEMP,
    DEVICE_COOP_HUD_MUTE
} DeviceCoopHudKind;

typedef enum
{
    DEVICE_COOP_SLEEP_AWAKE = 0,
    DEVICE_COOP_SLEEP_PREPARING,
    DEVICE_COOP_SLEEP_SLEEPING
} DeviceCoopSleepPhase;

typedef enum
{
    DEVICE_COOP_DECISION_NONE = 0,
    DEVICE_COOP_DECISION_PROCEED,
    DEVICE_COOP_DECISION_DEFER
} DeviceCoopDecision;

typedef struct
{
    char sleep_id[DEVICE_COOP_SLEEP_ID_SIZE];
    /* The session that owned the cycle when it started; empty for a
     * system-scope cycle. wake.done carries it, and a cycle whose session
     * ended during sleep is not delivered to a newer session. */
    char session_id[DEVICE_SESSION_ID_MAX];
    char reason[DEVICE_COOP_REASON_MAX];
    unsigned long long started_at_ms;
    unsigned long long deadline_at_ms;
    unsigned long long ended_at_ms;
    char result[DEVICE_PROTOCOL_RESULT_MAX];
    char woke_by[DEVICE_PROTOCOL_WOKE_BY_MAX];
    unsigned long long slept_ms;
    int valid;
} DeviceCoopSleepCycle;

typedef struct
{
    /* Session scope. active==0 means no session; the id is then empty. */
    int active;
    char session_id[DEVICE_SESSION_ID_MAX];
    int overlay_host_declared;
    int overlay_host;
    unsigned long long host_changed_at_ms;
    int overlay_open;

    /* HUD policy, stored per kind. 1 = app draws, 0 = system draws. */
    int hud_policy[DEVICE_COOP_HUD_KINDS];

    /* Sleep handshake. */
    DeviceCoopSleepPhase phase;
    char sleep_id[DEVICE_COOP_SLEEP_ID_SIZE];
    char sleep_reason[DEVICE_COOP_REASON_MAX];
    unsigned long long sleep_started_at_ms;
    unsigned long long sleep_deadline_at_ms;
    int sleep_deadline_ms; /* the configured ack window for this attempt */
    unsigned long long sleep_proceed_at_ms;
    char sleep_session_id[DEVICE_SESSION_ID_MAX];
    DeviceCoopDecision decision;
    /* Bitmap of subscriptions that acked this attempt, indexed by slot. */
    unsigned acked;
    unsigned counted;
    int inhibit_prepare_active;
    char inhibit_prepare_reason[DEVICE_COOP_REASON_MAX];
    char inhibit_prepare_detail[DEVICE_COOP_DETAIL_MAX];
    unsigned long long inhibit_prepare_until_ms;
    int inhibit_automatic_active;
    char inhibit_automatic_reason[DEVICE_COOP_REASON_MAX];
    unsigned long long inhibit_automatic_until_ms;
    DeviceCoopSleepCycle last;
} DeviceCoop;

void device_coop_init(DeviceCoop* coop);

/* Session lifecycle. */
void device_coop_session_start(DeviceCoop* coop, const char* session_id, int overlay_declared,
                               unsigned long long now_ms);
/* Ends the session and clears every session-scoped piece of state. */
void device_coop_session_end(DeviceCoop* coop);

/* HUD policy (§7.5). kind is DeviceCoopHudKind; mode 1 = app, 0 = system. */
void device_coop_set_hud_policy(DeviceCoop* coop, int kind, int app);
int device_coop_get_hud_policy(const DeviceCoop* coop, int kind);

/*
 * The routing actually in effect. A kind only goes to the app when the session
 * is active, the effective host is true, and the stored policy says app (or the
 * overlay is open, which forces app while the host is true).
 */
int device_coop_effective_hud(const DeviceCoop* coop, int kind);
int device_coop_effective_hud_any(const DeviceCoop* coop);

/* Overlay host (§7.9). Returns 1 when the value changed. */
int device_coop_set_overlay_host(DeviceCoop* coop, int host, unsigned long long now_ms);
void device_coop_set_overlay_open(DeviceCoop* coop, int open);

/* 1 when the session declared "overlay" and currently hosts it. */
int device_coop_is_hosting(const DeviceCoop* coop);

/* Sleep (§6). */
typedef struct
{
    const char* error; /* NULL on success, otherwise a stable error name */
    const char* detail;
    int subscribers;
    int acks;
    char sleep_id[DEVICE_COOP_SLEEP_ID_SIZE];
} DeviceCoopSleepResult;

/*
 * Begin an attempt. `subscribers` is the number of sleep-family subscriptions
 * that will receive the prepare; when it is 0 the attempt resolves to proceed
 * immediately and the caller only has to report it. `automatic` requests are
 * suppressed by the automatic pre-inhibit lease; a user request is not.
 */
DeviceCoopSleepResult device_coop_sleep_request(DeviceCoop* coop, const char* reason,
                                                const char* initiator, int timeout_ms,
                                                int automatic, int subscribers,
                                                unsigned long long now_ms);

/* 1 when the attempt is still collecting answers and has not hit its deadline. */
int device_coop_sleep_pending(const DeviceCoop* coop, unsigned long long now_ms);

/* Mark that this attempt's deadline has expired: proceed unless inhibited. */
void device_coop_sleep_deadline(DeviceCoop* coop, unsigned long long now_ms);

/*
 * Record an ack from subscription `slot`. `counted` is the bitmap the server
 * built when it started the attempt. Returns a stable error name or NULL, and
 * writes the aggregate decision to `decision_out`.
 */
const char* device_coop_sleep_ack(DeviceCoop* coop, unsigned slot, const char* sleep_id,
                                  unsigned long long now_ms, DeviceCoopDecision* decision_out);

/*
 * Inhibit. With a sleep_id it defers that attempt; without one it installs the
 * automatic pre-inhibit lease. Returns a stable error name or NULL.
 */
const char* device_coop_sleep_inhibit(DeviceCoop* coop, const char* sleep_id,
                                      const char* reason, int timeout_ms, const char* detail,
                                      unsigned long long now_ms, unsigned long long* expires_out);

/* Returns 1 when something was released. */
int device_coop_sleep_release(DeviceCoop* coop, const char* sleep_id, int* scope_prepare,
                              int* scope_automatic);

/*
 * The caller reports the resume. Returns a stable error name or NULL; on
 * success `last` is filled and wake.done may be emitted exactly once.
 */
const char* device_coop_sleep_finished(DeviceCoop* coop, const char* sleep_id,
                                       const char* result, const char* woke_by,
                                       unsigned long long now_ms, DeviceCoopSleepCycle* last);

/* Fill the view the sleep.status writer needs. */
void device_coop_sleep_status(const DeviceCoop* coop, unsigned long long now_ms,
                              DeviceSleepStatusView* view);

/* Expire time-bounded state. Safe to call on every loop iteration. */
void device_coop_tick(DeviceCoop* coop, unsigned long long now_ms);

/* 1 when the automatic pre-inhibit lease currently suppresses automatic sleep. */
int device_coop_automatic_suppressed(const DeviceCoop* coop, unsigned long long now_ms);

/* Generates the next sleep id (16 lowercase hex digits, unique per process). */
void device_coop_next_sleep_id(DeviceCoop* coop, char* out, size_t out_size);

/* HUD kind name/index helpers. */
int device_coop_hud_kind_for_name(const char* name);
const char* device_coop_hud_kind_name(int kind);

#ifdef __cplusplus
}
#endif

#endif
