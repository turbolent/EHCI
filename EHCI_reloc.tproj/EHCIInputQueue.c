/*
 * A fixed queue keeps controller progress independent of OPENSTEP's
 * synchronous EventSrc input callbacks.  Callers provide external locking.
 */
#include "EHCIInputQueue.h"
#include "EHCIMemory.h"

static void
copy_report(EHCIInputEvent *event, ehci_u8 type,
            const ehci_u8 *report, ehci_u32 length, ehci_u64 timestamp)
{
    if (length > EHCI_INPUT_REPORT_BYTES)
        length = EHCI_INPUT_REPORT_BYTES;
    event->type = type;
    event->length = (ehci_u8)length;
    bcopy(report, event->report, length);
    bzero(event->report + length, EHCI_INPUT_REPORT_BYTES - length);
    event->timestamp = timestamp;
}

static EHCIInputEvent *
newest_event(EHCIInputQueue *queue)
{
    ehci_u16 index;
    if (!queue || !queue->count)
        return 0;
    index = (ehci_u16)((queue->tail + EHCI_INPUT_QUEUE_CAPACITY - 1) %
                       EHCI_INPUT_QUEUE_CAPACITY);
    return &queue->events[index];
}

/* Match the historical PS/2 driver: while one pointer event is pending,
 * accumulate subsequent motion instead of replaying every stale packet.
 * A native PCPointerEvent has signed 8-bit deltas, so start another queue
 * segment rather than saturating or wrapping when the sum will not fit.
 * Button changes are queue boundaries and therefore remain ordered. */
static int
merge_pointer_report(EHCIInputEvent *event, const ehci_u8 *report,
                     ehci_u32 length, ehci_u64 timestamp)
{
    int dx;
    int dy;
    if (!event || event->type != EHCI_INPUT_POINTER ||
        event->length < 3 || length < 3)
        return 0;
    if ((event->report[0] & 3) != (report[0] & 3))
        return 0;
    dx = (int)(ehci_s8)event->report[1] + (int)(ehci_s8)report[1];
    dy = (int)(ehci_s8)event->report[2] + (int)(ehci_s8)report[2];
    if (dx < -128 || dx > 127 || dy < -128 || dy > 127)
        return 0;
    event->report[0] = report[0];
    event->report[1] = (ehci_u8)(ehci_s8)dx;
    event->report[2] = (ehci_u8)(ehci_s8)dy;
    event->length = 3;
    event->timestamp = timestamp;
    return 1;
}

static EHCIInputEvent *
newest_type(EHCIInputQueue *queue, ehci_u8 type)
{
    ehci_u16 offset;
    for (offset = 0; offset < queue->count; offset++) {
        ehci_u16 index = (ehci_u16)((queue->tail +
            EHCI_INPUT_QUEUE_CAPACITY - 1 - offset) %
            EHCI_INPUT_QUEUE_CAPACITY);
        if (queue->events[index].type == type)
            return &queue->events[index];
    }
    return 0;
}

void
EHCIInputQueueInitialize(EHCIInputQueue *queue)
{
    if (queue)
        bzero(queue, sizeof(*queue));
}

int
EHCIInputQueueEnqueue(EHCIInputQueue *queue, ehci_u8 type,
                      const ehci_u8 *report, ehci_u32 length,
                      ehci_u64 timestamp)
{
    EHCIInputEvent *event;
    if (!queue || !report || !length ||
        (type != EHCI_INPUT_KEYBOARD && type != EHCI_INPUT_POINTER))
        return 0;

    queue->submitted++;
    if (type == EHCI_INPUT_POINTER) {
        event = newest_event(queue);
        if (merge_pointer_report(event, report, length, timestamp)) {
            queue->coalesced++;
            return 1;
        }
    }
    if (queue->count < EHCI_INPUT_QUEUE_CAPACITY) {
        event = &queue->events[queue->tail];
        copy_report(event, type, report, length, timestamp);
        queue->tail = (ehci_u16)((queue->tail + 1) %
                                 EHCI_INPUT_QUEUE_CAPACITY);
        queue->count++;
        queue->enqueued++;
        if (queue->count > queue->highWater)
            queue->highWater = queue->count;
        return 1;
    }

    /* Pointer and keyboard have separate queues in the controller.  If the
     * pointer queue is pathologically full, discard its oldest segment so
     * the newest button state is retained. */
    if (type == EHCI_INPUT_POINTER) {
        if (queue->events[queue->head].type == EHCI_INPUT_POINTER) {
            queue->head = (ehci_u16)((queue->head + 1) %
                                     EHCI_INPUT_QUEUE_CAPACITY);
            queue->count--;
            queue->droppedPointer++;
            event = &queue->events[queue->tail];
            copy_report(event, type, report, length, timestamp);
            queue->tail = (ehci_u16)((queue->tail + 1) %
                                     EHCI_INPUT_QUEUE_CAPACITY);
            queue->count++;
            queue->enqueued++;
            return 1;
        }
        queue->droppedPointer++;
        return 0;
    }

    /* Keyboard reports are complete state snapshots.  Replacing the newest
     * queued snapshot preserves final state, especially key releases. */
    event = newest_type(queue, EHCI_INPUT_KEYBOARD);
    if (event) {
        copy_report(event, type, report, length, timestamp);
        queue->coalesced++;
        return 1;
    }

    /* Keyboard state has priority: a missed release can leave a key held.
     * A pointer report never evicts the only queued keyboard snapshot. */
    if (type == EHCI_INPUT_KEYBOARD) {
        event = &queue->events[(queue->tail +
            EHCI_INPUT_QUEUE_CAPACITY - 1) % EHCI_INPUT_QUEUE_CAPACITY];
        if (event->type == EHCI_INPUT_POINTER)
            queue->droppedPointer++;
        else
            queue->droppedKeyboard++;
        copy_report(event, type, report, length, timestamp);
        queue->coalesced++;
        return 1;
    }
    return 0;
}

int
EHCIInputQueueDequeue(EHCIInputQueue *queue, EHCIInputEvent *event)
{
    if (!queue || !event || !queue->count)
        return 0;
    *event = queue->events[queue->head];
    queue->head = (ehci_u16)((queue->head + 1) %
                             EHCI_INPUT_QUEUE_CAPACITY);
    queue->count--;
    queue->dequeued++;
    return 1;
}

void
EHCIInputQueueDiscardPending(EHCIInputQueue *queue)
{
    if (!queue)
        return;
    queue->head = queue->tail;
    queue->count = 0;
}

void
EHCIInputGapInitialize(EHCIInputGapTracker *tracker)
{
    if (tracker)
        bzero(tracker, sizeof(*tracker));
}

int
EHCIInputGapObserve(EHCIInputGapTracker *tracker, ehci_u64 timestamp,
                    ehci_u64 *gapNanoseconds)
{
    ehci_u64 delta;
    if (gapNanoseconds)
        *gapNanoseconds = 0;
    if (!tracker || !timestamp)
        return 0;
    if (!tracker->lastTimestamp || timestamp <= tracker->lastTimestamp) {
        tracker->lastTimestamp = timestamp;
        tracker->consecutiveActive = 0;
        return 0;
    }
    delta = timestamp - tracker->lastTimestamp;
    tracker->lastTimestamp = timestamp;
    if (delta <= EHCI_INPUT_ACTIVE_NS) {
        if (tracker->consecutiveActive < EHCI_INPUT_GAP_ARM_REPORTS)
            tracker->consecutiveActive++;
        return 0;
    }
    if (delta >= EHCI_INPUT_GAP_NS &&
        tracker->consecutiveActive >= EHCI_INPUT_GAP_ARM_REPORTS) {
        tracker->consecutiveActive = 0;
        tracker->detected++;
        if (gapNanoseconds)
            *gapNanoseconds = delta;
        return 1;
    }
    tracker->consecutiveActive = 0;
    return 0;
}
