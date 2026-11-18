#include "device_coop.h"

#include <stdio.h>
#include <string.h>

/*
 * Cooperation state (CONTRACT.md §4-§7). Pure state; no I/O, no clock reads.
 * Every function that depends on time takes now_ms from the caller so the
 * handshake can be tested deterministically and the poll loop stays the single
 * source of time.
 */

static void set_text(char* out, size_t out_size, const char* text)
{
    if (out != NULL && out_size > 0) snprintf(out, out_size, "%s", text != NULL ? text : "");
}

static const char* const HUD_NAMES[DEVICE_COOP_HUD_KINDS] = {"volume", "brightness",
                                                             "colortemp", "mute"};

int device_coop_hud_kind_for_name(const char* name)
{
    int i;

    if (name == NULL) return -1;
    for (i = 0; i < DEVICE_COOP_HUD_KINDS; i++)
        if (strcmp(HUD_NAMES[i], name) == 0) return i;
    return -1;
}

const char* device_coop_hud_kind_name(int kind)
{
    if (kind < 0 || kind >= DEVICE_COOP_HUD_KINDS) return "unknown";
    return HUD_NAMES[kind];
}

void device_coop_init(DeviceCoop* coop)
{
    int kind;

    if (coop == NULL) return;
    memset(coop, 0, sizeof *coop);
    coop->phase = DEVICE_COOP_SLEEP_AWAKE;
    for (kind = 0; kind < DEVICE_COOP_HUD_KINDS; kind++) coop->hud_policy[kind] = 0;
}

void device_coop_session_start(DeviceCoop* coop, const char* session_id, int overlay_declared,
                               unsigned long long now_ms)
{
    if (coop == NULL) return;
    /* Everything session-scoped resets; a new session never inherits the
     * previous one's yield, policy or open overlay (§7.9.1). */
    coop->active = 1;
    set_text(coop->session_id, sizeof coop->session_id, session_id);
    coop->overlay_host_declared = overlay_declared ? 1 : 0;
    coop->overlay_host = coop->overlay_host_declared;
    coop->host_changed_at_ms = now_ms;
    coop->overlay_open = 0;
    {
        int kind;
        for (kind = 0; kind < DEVICE_COOP_HUD_KINDS; kind++)
            coop->hud_policy[kind] = coop->overlay_host_declared ? 1 : 0;
    }
}

void device_coop_session_end(DeviceCoop* coop)
{
    if (coop == NULL) return;
    coop->active = 0;
    coop->session_id[0] = '\0';
    coop->overlay_host_declared = 0;
    coop->overlay_host = 0;
    coop->overlay_open = 0;
    coop->host_changed_at_ms = 0;
    {
        int kind;
        for (kind = 0; kind < DEVICE_COOP_HUD_KINDS; kind++) coop->hud_policy[kind] = 0;
    }
    /* A session-scoped inhibit lease dies with the session (§6.5). A pending
     * attempt does not: the server still has to answer the caller. Its ack set
     * is cleared so an ended subscription can never block it. */
    coop->inhibit_prepare_active = 0;
    coop->inhibit_prepare_reason[0] = '\0';
    coop->inhibit_prepare_detail[0] = '\0';
    coop->inhibit_prepare_until_ms = 0;
    coop->inhibit_automatic_active = 0;
    coop->inhibit_automatic_reason[0] = '\0';
    coop->inhibit_automatic_until_ms = 0;
}

void device_coop_set_hud_policy(DeviceCoop* coop, int kind, int app)
{
    if (coop == NULL || kind < 0 || kind >= DEVICE_COOP_HUD_KINDS) return;
    coop->hud_policy[kind] = app ? 1 : 0;
}

int device_coop_get_hud_policy(const DeviceCoop* coop, int kind)
{
    if (coop == NULL || kind < 0 || kind >= DEVICE_COOP_HUD_KINDS) return 0;
    return coop->hud_policy[kind];
}

int device_coop_is_hosting(const DeviceCoop* coop)
{
    return coop != NULL && coop->active && coop->overlay_host_declared && coop->overlay_host;
}

int device_coop_effective_hud(const DeviceCoop* coop, int kind)
{
    if (coop == NULL || kind < 0 || kind >= DEVICE_COOP_HUD_KINDS) return 0;
    /* Only a session that actually hosts can draw: an unadapted session, or one
     * that yielded, always gets the system HUD (§7.9.2). */
    if (!device_coop_is_hosting(coop)) return 0;
    if (coop->overlay_open) return 1;
    return coop->hud_policy[kind];
}

int device_coop_effective_hud_any(const DeviceCoop* coop)
{
    int kind;

    for (kind = 0; kind < DEVICE_COOP_HUD_KINDS; kind++)
        if (device_coop_effective_hud(coop, kind)) return 1;
    return 0;
}

int device_coop_set_overlay_host(DeviceCoop* coop, int host, unsigned long long now_ms)
{
    if (coop == NULL || !coop->active) return 0;
    host = host ? 1 : 0;
    if (coop->overlay_host == host) return 0;
    coop->overlay_host = host;
    coop->host_changed_at_ms = now_ms;
    /* Yielding closes the overlay state as a safety net (§7.9.1). */
    if (!host) coop->overlay_open = 0;
    return 1;
}

void device_coop_set_overlay_open(DeviceCoop* coop, int open)
{
    if (coop == NULL || !coop->active) return;
    coop->overlay_open = open ? 1 : 0;
}

void device_coop_next_sleep_id(DeviceCoop* coop, char* out, size_t out_size)
{
    static unsigned long long counter;
    unsigned long long value;

    (void)coop;
    /* 16 lowercase hex digits, unique for the server's lifetime. The counter is
     * what guarantees uniqueness; the time-derived part only keeps ids from
     * looking sequential across a restart. */
    counter++;
    value = (device_session_monotonic_ms() * 1000003ULL) ^ (counter * 0x9E3779B97F4A7C15ULL);
    snprintf(out, out_size, "%016llx", value);
}

int device_coop_automatic_suppressed(const DeviceCoop* coop, unsigned long long now_ms)
{
    if (coop == NULL || !coop->inhibit_automatic_active) return 0;
    return now_ms < coop->inhibit_automatic_until_ms;
}

void device_coop_tick(DeviceCoop* coop, unsigned long long now_ms)
{
    if (coop == NULL) return;
    if (coop->inhibit_automatic_active && now_ms >= coop->inhibit_automatic_until_ms)
    {
        coop->inhibit_automatic_active = 0;
        coop->inhibit_automatic_reason[0] = '\0';
        coop->inhibit_automatic_until_ms = 0;
    }
    if (coop->inhibit_prepare_active && now_ms >= coop->inhibit_prepare_until_ms)
    {
        coop->inhibit_prepare_active = 0;
        coop->inhibit_prepare_reason[0] = '\0';
        coop->inhibit_prepare_detail[0] = '\0';
        coop->inhibit_prepare_until_ms = 0;
    }
}

DeviceCoopSleepResult device_coop_sleep_request(DeviceCoop* coop, const char* reason,
                                                const char* initiator, int timeout_ms,
                                                int automatic, int subscribers,
                                                unsigned long long now_ms)
{
    DeviceCoopSleepResult result;
    DeviceCoopSleepCycle stale;

    memset(&result, 0, sizeof result);
    memset(&stale, 0, sizeof stale);
    (void)initiator; /* diagnostic only; devserv logs it (§6.2) */
    if (coop == NULL)
    {
        result.error = "bad_request";
        result.detail = "no cooperation state";
        return result;
    }

    /* A caller that never reported its resume leaves phase=sleeping. Finalize it
     * as unknown so the new attempt can start (§6.2). */
    if (coop->phase == DEVICE_COOP_SLEEP_SLEEPING)
    {
        stale = coop->last;
        coop->last.valid = 1;
        set_text(coop->last.sleep_id, sizeof coop->last.sleep_id, coop->sleep_id);
        set_text(coop->last.reason, sizeof coop->last.reason, coop->sleep_reason);
        set_text(coop->last.result, sizeof coop->last.result, "unknown");
        set_text(coop->last.woke_by, sizeof coop->last.woke_by, "unknown");
        coop->last.started_at_ms = coop->sleep_started_at_ms;
        coop->last.ended_at_ms = now_ms;
        coop->last.slept_ms = now_ms > coop->sleep_started_at_ms
                                  ? now_ms - coop->sleep_started_at_ms
                                  : 0;
        coop->phase = DEVICE_COOP_SLEEP_AWAKE;
        result.error = NULL;
        result.detail = "previous cycle finalized as unknown";
        (void)stale;
    }

    if (coop->phase == DEVICE_COOP_SLEEP_PREPARING)
    {
        result.error = "sleep_busy";
        result.detail = "a sleep attempt is already preparing";
        return result;
    }
    if (coop->phase == DEVICE_COOP_SLEEP_SLEEPING)
    {
        result.error = "sleep_busy";
        result.detail = "the device is already sleeping";
        return result;
    }

    device_coop_tick(coop, now_ms);
    /* The automatic pre-inhibit suppresses automatic initiation only; an
     * explicit user request always produces a prepare (§6.5). */
    if (automatic && device_coop_automatic_suppressed(coop, now_ms))
    {
        result.error = NULL;
        result.detail = "suppressed by the automatic pre-inhibit lease";
        result.sleep_id[0] = '\0';
        result.subscribers = 0;
        result.acks = 0;
        return result;
    }

    device_coop_next_sleep_id(coop, coop->sleep_id, sizeof coop->sleep_id);
    set_text(coop->sleep_reason, sizeof coop->sleep_reason, reason != NULL ? reason : "user");
    coop->sleep_started_at_ms = now_ms;
    coop->sleep_deadline_ms = timeout_ms > 0 ? timeout_ms : DEVICE_PROTOCOL_SLEEP_ACK_MS;
    coop->sleep_deadline_at_ms = now_ms + (unsigned long long)coop->sleep_deadline_ms;
    coop->sleep_proceed_at_ms = 0;
    coop->decision = DEVICE_COOP_DECISION_NONE;
    coop->acked = 0;
    coop->counted = 0;
    coop->phase = DEVICE_COOP_SLEEP_PREPARING;
    coop->inhibit_prepare_active = 0;
    coop->inhibit_prepare_reason[0] = '\0';
    coop->inhibit_prepare_detail[0] = '\0';
    coop->inhibit_prepare_until_ms = 0;

    result.sleep_id[0] = '\0';
    set_text(result.sleep_id, sizeof result.sleep_id, coop->sleep_id);
    result.subscribers = subscribers;
    result.acks = 0;
    result.detail = "waiting for subscribers";

    /* With nobody to ask the contract resolves immediately. */
    if (subscribers <= 0)
    {
        coop->phase = DEVICE_COOP_SLEEP_SLEEPING;
        coop->decision = DEVICE_COOP_DECISION_PROCEED;
        coop->sleep_proceed_at_ms = now_ms;
        result.detail = "no subscribers";
    }
    return result;
}

int device_coop_sleep_pending(const DeviceCoop* coop, unsigned long long now_ms)
{
    if (coop == NULL || coop->phase != DEVICE_COOP_SLEEP_PREPARING) return 0;
    return now_ms < coop->sleep_deadline_at_ms;
}

void device_coop_sleep_deadline(DeviceCoop* coop, unsigned long long now_ms)
{
    if (coop == NULL || coop->phase != DEVICE_COOP_SLEEP_PREPARING) return;
    /* The liveness guarantee: a missing ack never blocks sleep. An active
     * prepare-scoped inhibit still defers. */
    if (coop->inhibit_prepare_active && now_ms < coop->inhibit_prepare_until_ms)
    {
        coop->decision = DEVICE_COOP_DECISION_DEFER;
        coop->phase = DEVICE_COOP_SLEEP_AWAKE;
        return;
    }
    coop->decision = DEVICE_COOP_DECISION_PROCEED;
    coop->phase = DEVICE_COOP_SLEEP_SLEEPING;
    coop->sleep_proceed_at_ms = now_ms;
}

const char* device_coop_sleep_ack(DeviceCoop* coop, unsigned slot, const char* sleep_id,
                                  unsigned long long now_ms, DeviceCoopDecision* decision_out)
{
    (void)now_ms;
    if (decision_out != NULL) *decision_out = DEVICE_COOP_DECISION_NONE;
    if (coop == NULL) return "bad_request";
    if (coop->phase != DEVICE_COOP_SLEEP_PREPARING
        || coop->decision != DEVICE_COOP_DECISION_NONE)
        return "sleep_stale";
    if (sleep_id == NULL || strcmp(sleep_id, coop->sleep_id) != 0) return "sleep_stale";
    if (slot >= 32) return "bad_request";

    coop->acked |= 1u << slot;
    /* A second ack from the same subscription is idempotent (§6.4). */
    if (decision_out != NULL)
        *decision_out = coop->decision == DEVICE_COOP_DECISION_NONE ? DEVICE_COOP_DECISION_DEFER
                                                                   : coop->decision;

    if (coop->inhibit_prepare_active) return NULL;
    if (coop->counted != 0 && (coop->acked & coop->counted) == coop->counted)
    {
        coop->decision = DEVICE_COOP_DECISION_PROCEED;
        coop->phase = DEVICE_COOP_SLEEP_SLEEPING;
        coop->sleep_proceed_at_ms = now_ms;
        if (decision_out != NULL) *decision_out = DEVICE_COOP_DECISION_PROCEED;
    }
    return NULL;
}

const char* device_coop_sleep_inhibit(DeviceCoop* coop, const char* sleep_id,
                                      const char* reason, int timeout_ms, const char* detail,
                                      unsigned long long now_ms, unsigned long long* expires_out)
{
    unsigned long long expires;

    if (coop == NULL) return "bad_request";
    if (reason == NULL || reason[0] == '\0' || strlen(reason) > 64) return "bad_request";
    if (timeout_ms < 1000 || timeout_ms > DEVICE_PROTOCOL_INHIBIT_MAX_MS) return "bad_request";
    expires = now_ms + (unsigned long long)timeout_ms;

    if (sleep_id != NULL && sleep_id[0] != '\0')
    {
        /* Prepare-scoped: only a pending attempt can be deferred. */
        if (coop->phase != DEVICE_COOP_SLEEP_PREPARING
            || coop->decision != DEVICE_COOP_DECISION_NONE)
            return "sleep_stale";
        if (strcmp(sleep_id, coop->sleep_id) != 0) return "sleep_stale";
        coop->inhibit_prepare_active = 1;
        set_text(coop->inhibit_prepare_reason, sizeof coop->inhibit_prepare_reason, reason);
        set_text(coop->inhibit_prepare_detail, sizeof coop->inhibit_prepare_detail, detail);
        coop->inhibit_prepare_until_ms = expires;
        coop->decision = DEVICE_COOP_DECISION_DEFER;
        coop->phase = DEVICE_COOP_SLEEP_AWAKE;
        if (expires_out != NULL) *expires_out = expires;
        return NULL;
    }

    /* Automatic pre-inhibit belongs to an app session (§6.5). */
    if (!coop->active) return "not_active";
    coop->inhibit_automatic_active = 1;
    set_text(coop->inhibit_automatic_reason, sizeof coop->inhibit_automatic_reason, reason);
    coop->inhibit_automatic_until_ms = expires;
    if (expires_out != NULL) *expires_out = expires;
    return NULL;
}

int device_coop_sleep_release(DeviceCoop* coop, const char* sleep_id, int* scope_prepare,
                              int* scope_automatic)
{
    int released = 0;

    if (scope_prepare != NULL) *scope_prepare = 0;
    if (scope_automatic != NULL) *scope_automatic = 0;
    if (coop == NULL) return 0;

    if (sleep_id != NULL && sleep_id[0] != '\0')
    {
        if (coop->inhibit_prepare_active && strcmp(sleep_id, coop->sleep_id) == 0)
        {
            coop->inhibit_prepare_active = 0;
            coop->inhibit_prepare_reason[0] = '\0';
            coop->inhibit_prepare_detail[0] = '\0';
            coop->inhibit_prepare_until_ms = 0;
            released = 1;
            if (scope_prepare != NULL) *scope_prepare = 1;
        }
        return released;
    }

    if (coop->inhibit_automatic_active)
    {
        coop->inhibit_automatic_active = 0;
        coop->inhibit_automatic_reason[0] = '\0';
        coop->inhibit_automatic_until_ms = 0;
        released = 1;
        if (scope_automatic != NULL) *scope_automatic = 1;
    }
    return released;
}

const char* device_coop_sleep_finished(DeviceCoop* coop, const char* sleep_id,
                                       const char* result, const char* woke_by,
                                       unsigned long long now_ms, DeviceCoopSleepCycle* last)
{
    if (coop == NULL) return "bad_request";
    if (sleep_id == NULL || strcmp(sleep_id, coop->sleep_id) != 0) return "sleep_stale";
    if (coop->phase != DEVICE_COOP_SLEEP_SLEEPING) return "sleep_stale";
    if (coop->last.valid && strcmp(coop->last.sleep_id, sleep_id) == 0) return "sleep_stale";

    coop->last.valid = 1;
    set_text(coop->last.sleep_id, sizeof coop->last.sleep_id, sleep_id);
    set_text(coop->last.reason, sizeof coop->last.reason, coop->sleep_reason);
    set_text(coop->last.result, sizeof coop->last.result,
             result != NULL ? result : "ok");
    set_text(coop->last.woke_by, sizeof coop->last.woke_by,
             woke_by != NULL ? woke_by : "unknown");
    coop->last.started_at_ms = coop->sleep_started_at_ms;
    coop->last.ended_at_ms = now_ms;
    coop->last.slept_ms = now_ms > coop->sleep_proceed_at_ms
                              ? now_ms - coop->sleep_proceed_at_ms
                              : 0;

    coop->phase = DEVICE_COOP_SLEEP_AWAKE;
    coop->decision = DEVICE_COOP_DECISION_NONE;
    coop->acked = 0;
    coop->counted = 0;
    coop->sleep_id[0] = '\0';
    coop->sleep_reason[0] = '\0';
    coop->sleep_started_at_ms = 0;
    coop->sleep_deadline_at_ms = 0;
    coop->sleep_proceed_at_ms = 0;
    if (last != NULL) *last = coop->last;
    return NULL;
}

void device_coop_sleep_status(const DeviceCoop* coop, unsigned long long now_ms,
                              DeviceSleepStatusView* view)
{
    if (view == NULL) return;
    memset(view, 0, sizeof *view);

    if (coop == NULL)
    {
        view->phase = "awake";
        return;
    }

    /* A phase that has been "sleeping" for more than 24 h is a lost caller;
     * report awake and an unknown last result (§6.2). */
    if (coop->phase == DEVICE_COOP_SLEEP_SLEEPING && coop->sleep_proceed_at_ms != 0
        && now_ms > coop->sleep_proceed_at_ms
        && now_ms - coop->sleep_proceed_at_ms > 24ULL * 60ULL * 60ULL * 1000ULL)
    {
        view->phase = "awake";
    }
    else
    {
        view->phase = coop->phase == DEVICE_COOP_SLEEP_PREPARING ? "preparing"
                     : coop->phase == DEVICE_COOP_SLEEP_SLEEPING ? "sleeping"
                                                                 : "awake";
    }

    if (coop->phase == DEVICE_COOP_SLEEP_PREPARING && coop->decision == DEVICE_COOP_DECISION_NONE)
    {
        view->pending = 1;
        set_text(view->pending_sleep_id, sizeof view->pending_sleep_id, coop->sleep_id);
        set_text(view->pending_reason, sizeof view->pending_reason, coop->sleep_reason);
        view->pending_deadline_at_ms = coop->sleep_deadline_at_ms;
        view->pending_deadline_ms =
            now_ms < coop->sleep_deadline_at_ms
                ? (int)(coop->sleep_deadline_at_ms - now_ms)
                : 0;
        view->pending_subscribers = (int)__builtin_popcount(coop->counted);
        view->pending_acks = (int)__builtin_popcount(coop->acked & coop->counted);
    }

    if (coop->inhibit_prepare_active && now_ms < coop->inhibit_prepare_until_ms)
    {
        view->inhibit_active = 1;
        view->inhibit_scope = "prepare";
        set_text(view->inhibit_reason, sizeof view->inhibit_reason,
                 coop->inhibit_prepare_reason);
        view->inhibit_expires_at_ms = coop->inhibit_prepare_until_ms;
        set_text(view->inhibit_sleep_id, sizeof view->inhibit_sleep_id, coop->sleep_id);
    }
    else if (coop->inhibit_automatic_active && now_ms < coop->inhibit_automatic_until_ms)
    {
        view->inhibit_active = 1;
        view->inhibit_scope = "automatic";
        set_text(view->inhibit_reason, sizeof view->inhibit_reason,
                 coop->inhibit_automatic_reason);
        view->inhibit_expires_at_ms = coop->inhibit_automatic_until_ms;
    }

    if (coop->last.valid)
    {
        view->last_valid = 1;
        set_text(view->last_sleep_id, sizeof view->last_sleep_id, coop->last.sleep_id);
        set_text(view->last_reason, sizeof view->last_reason, coop->last.reason);
        set_text(view->last_result, sizeof view->last_result, coop->last.result);
        set_text(view->last_woke_by, sizeof view->last_woke_by, coop->last.woke_by);
        view->last_started_at_ms = coop->last.started_at_ms;
        view->last_ended_at_ms = coop->last.ended_at_ms;
        view->last_slept_ms = coop->last.slept_ms;
    }
}
