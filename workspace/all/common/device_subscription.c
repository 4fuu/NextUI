#include "device_subscription.h"

#include <stdio.h>
#include <string.h>

void device_subscription_init(DeviceSubscription* subscription)
{
    if (subscription == NULL) return;
    memset(subscription, 0, sizeof *subscription);
    subscription->fd = -1;
    device_status_snapshot_init(&subscription->status);
}

int device_subscription_attach(DeviceSubscription* subscription, int fd, int has_session,
                               const char* session_id, unsigned families,
                               unsigned status_sections, int min_interval_ms)
{
    if (subscription == NULL || fd < 0) return 0;
    memset(subscription, 0, sizeof *subscription);
    subscription->used = 1;
    subscription->fd = fd;
    subscription->has_session = has_session ? 1 : 0;
    if (subscription->has_session && session_id != NULL)
        snprintf(subscription->session_id, sizeof subscription->session_id, "%s", session_id);
    subscription->families = families;
    subscription->status_sections = status_sections;
    subscription->min_interval_ms = min_interval_ms > 0 ? min_interval_ms
                                                       : DEVICE_PROTOCOL_STATUS_MIN_INTERVAL_MS;
    subscription->seq = 0;
    subscription->queue_head = 0;
    subscription->queue_count = 0;
    device_status_snapshot_init(&subscription->status);
    return 1;
}

void device_subscription_detach(DeviceSubscription* subscription)
{
    if (subscription == NULL) return;
    memset(subscription, 0, sizeof *subscription);
    subscription->fd = -1;
    device_status_snapshot_init(&subscription->status);
}

unsigned device_subscription_next_seq(DeviceSubscription* subscription)
{
    if (subscription == NULL) return 0;
    return ++subscription->seq;
}

/*
 * Make room for one more event. The oldest droppable entry is evicted first;
 * when every queued event is protected there is no room and the caller must
 * overflow. Returns 1 when room exists.
 */
static int make_room(DeviceSubscription* subscription)
{
    int index;

    if (subscription->queue_count < DEVICE_SUBSCRIPTION_QUEUE) return 1;
    for (index = 0; index < subscription->queue_count; index++)
    {
        int position = (subscription->queue_head + index) % DEVICE_SUBSCRIPTION_QUEUE;
        if (subscription->queue[position].priority == DEVICE_EVENT_PRIORITY_DROPPABLE)
        {
            /* Shift the later entries down over the hole. The queue is small
             * (64) and this only happens under backpressure. */
            int shift;
            for (shift = index; shift + 1 < subscription->queue_count; shift++)
            {
                int from = (subscription->queue_head + shift + 1) % DEVICE_SUBSCRIPTION_QUEUE;
                int to = (subscription->queue_head + shift) % DEVICE_SUBSCRIPTION_QUEUE;
                subscription->queue[to] = subscription->queue[from];
            }
            subscription->queue_count--;
            subscription->dropped++;
            return 1;
        }
    }
    return 0;
}

int device_subscription_push(DeviceSubscription* subscription, const char* line,
                             DeviceEventPriority priority)
{
    DeviceSubscriptionEvent* event;
    size_t length;

    if (subscription == NULL || !subscription->used || line == NULL) return 0;
    length = strlen(line);
    if (length == 0 || length >= DEVICE_SUBSCRIPTION_LINE_MAX)
    {
        /* A line that cannot fit is a server bug, but it must never corrupt the
         * stream: treat it as dropped. */
        return 0;
    }

    if (!make_room(subscription))
    {
        subscription->dropped++;
        subscription->overflowed = 1;
        subscription->close_after_flush = 1;
        return -1;
    }

    event = &subscription->queue[(subscription->queue_head + subscription->queue_count)
                                 % DEVICE_SUBSCRIPTION_QUEUE];
    memcpy(event->line, line, length);
    event->line[length] = '\0';
    event->length = length;
    event->priority = priority;
    subscription->queue_count++;
    return 1;
}

int device_subscription_pending(const DeviceSubscription* subscription)
{
    return subscription != NULL ? subscription->queue_count : 0;
}

const DeviceSubscriptionEvent* device_subscription_peek(const DeviceSubscription* subscription)
{
    if (subscription == NULL || subscription->queue_count == 0) return NULL;
    return &subscription->queue[subscription->queue_head];
}

void device_subscription_pop(DeviceSubscription* subscription)
{
    if (subscription == NULL || subscription->queue_count == 0) return;
    subscription->queue_head = (subscription->queue_head + 1) % DEVICE_SUBSCRIPTION_QUEUE;
    subscription->queue_count--;
    if (subscription->queue_count == 0) subscription->queue_head = 0;
}
