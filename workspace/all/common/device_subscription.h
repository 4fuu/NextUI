#ifndef DEVICE_SUBSCRIPTION_H
#define DEVICE_SUBSCRIPTION_H

#include <stddef.h>

#include "device_protocol.h"
#include "device_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Event subscriptions (CONTRACT.md §5).
 *
 * One subscription is one event connection: a file descriptor, the session it
 * is bound to, the event families it enabled, its status field mask, its
 * coalescing interval, its sequence counter and a bounded queue.
 *
 * This module owns the queue and the seq counter. devserv owns the descriptor
 * and performs the actual write, because the poll loop decides when a
 * subscription is writable. Keeping the queue here makes the drop order and the
 * overflow rule testable without sockets.
 *
 * Drop order (§5.3): status and heartbeat events are dropped first when the
 * queue is full; sleep.prepare, wake.done and session.ended are never dropped.
 * If a queue cannot make room for a protected event it reports overflow, and
 * devserv sends `overflow` (best effort) and closes the connection.
 */

#define DEVICE_SUBSCRIPTION_QUEUE 64
#define DEVICE_SUBSCRIPTION_LINE_MAX DEVICE_PROTOCOL_LINE_MAX

typedef enum
{
    /* Priority for the drop order. Lower is more droppable. */
    DEVICE_EVENT_PRIORITY_DROPPABLE = 0, /* status.*, heartbeat */
    DEVICE_EVENT_PRIORITY_PROTECTED = 1  /* sleep.prepare, wake.done, session.ended */
} DeviceEventPriority;

typedef struct
{
    char line[DEVICE_SUBSCRIPTION_LINE_MAX];
    size_t length;
    DeviceEventPriority priority;
} DeviceSubscriptionEvent;

typedef struct
{
    int used; /* slot is assigned to a live connection */
    int fd;
    int has_session;
    char session_id[DEVICE_SESSION_ID_MAX];
    unsigned families;       /* DeviceEventFamily mask */
    unsigned status_sections; /* DeviceStatusSection mask */
    int min_interval_ms;
    unsigned seq;
    /* 0 = fine, 1 = overflow detected and the overflow event is still owed,
     * 2 = overflow event queued/sent. Set only by device_subscription_push(). */
    int overflowed;
    /* Events evicted or refused because of backpressure; reported in the
     * overflow event so the client knows how much state it lost. */
    int dropped;
    int close_after_flush;
    unsigned long long last_send_ms;
    unsigned long long last_heartbeat_ms;
    DeviceSubscriptionEvent queue[DEVICE_SUBSCRIPTION_QUEUE];
    int queue_head;
    int queue_count;
    /* The last full snapshot this subscription was sent, for change detection
     * and for the 30 s resync. */
    DeviceStatusSnapshot status;
    int have_status;
    /* Sections that changed since the last status.changed, and when the last
     * status event went out (the coalescing floor is min_interval_ms). */
    unsigned pending_sections;
    unsigned long long last_status_ms;
    unsigned long long last_snapshot_ms;
} DeviceSubscription;

void device_subscription_init(DeviceSubscription* subscription);

/* Assign a slot to a connection. Returns 1 on success. */
int device_subscription_attach(DeviceSubscription* subscription, int fd, int has_session,
                               const char* session_id, unsigned families,
                               unsigned status_sections, int min_interval_ms);

void device_subscription_detach(DeviceSubscription* subscription);

/* Queue one pre-built line. Returns:
 *   1  queued (room was made by evicting the oldest droppable event if needed)
 *   0  refused (not attached, or the line does not fit)
 *  -1  no room can be made: the queue holds only protected events, so the
 *      subscriber has overflowed and devserv must close it after flushing */
int device_subscription_push(DeviceSubscription* subscription, const char* line,
                             DeviceEventPriority priority);

/* Number of queued events, and a pointer to the oldest one (NULL when empty). */
int device_subscription_pending(const DeviceSubscription* subscription);
const DeviceSubscriptionEvent* device_subscription_peek(const DeviceSubscription* subscription);
void device_subscription_pop(DeviceSubscription* subscription);

/* The next seq value, consumed by the caller when it builds a line. */
unsigned device_subscription_next_seq(DeviceSubscription* subscription);

#ifdef __cplusplus
}
#endif

#endif
