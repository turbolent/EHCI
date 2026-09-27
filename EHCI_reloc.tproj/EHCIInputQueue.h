#ifndef EHCI_INPUT_QUEUE_H
#define EHCI_INPUT_QUEUE_H

#include "EHCITypes.h"

#define EHCI_INPUT_REPORT_BYTES   8
#define EHCI_INPUT_QUEUE_CAPACITY 128

/* A boot mouse normally reports every 8-10 ms while moving.  Require a
 * sustained stream before classifying a later delay.  A HID endpoint cannot
 * distinguish an actual traffic stall from the user stopping the mouse, so
 * resulting gaps are meaningful only during deliberately continuous motion. */
#define EHCI_INPUT_ACTIVE_NS        25000000ULL
#define EHCI_INPUT_GAP_NS           50000000ULL
#define EHCI_INPUT_GAP_ARM_REPORTS  16U

#define EHCI_INPUT_KEYBOARD 1
#define EHCI_INPUT_POINTER  2

typedef struct EHCIInputEvent {
    ehci_u8 type;
    ehci_u8 length;
    ehci_u8 report[EHCI_INPUT_REPORT_BYTES];
    ehci_u64 timestamp;
} EHCIInputEvent;

typedef struct EHCIInputGapTracker {
    ehci_u64 lastTimestamp;
    ehci_u16 consecutiveActive;
    ehci_u32 detected;
} EHCIInputGapTracker;

typedef struct EHCIInputQueue {
    EHCIInputEvent events[EHCI_INPUT_QUEUE_CAPACITY];
    ehci_u16 head;
    ehci_u16 tail;
    ehci_u16 count;
    ehci_u16 highWater;
    ehci_u32 submitted;
    ehci_u32 enqueued;
    ehci_u32 dequeued;
    ehci_u32 coalesced;
    ehci_u32 droppedKeyboard;
    ehci_u32 droppedPointer;
} EHCIInputQueue;

/* Adjacent pointer reports with the same native button state are accumulated
 * while their signed 8-bit X/Y sum fits one PCPointer event.  A button edge or
 * an overflowing motion sum starts another ordered queue segment. */
void EHCIInputQueueInitialize(EHCIInputQueue *queue);
int EHCIInputQueueEnqueue(EHCIInputQueue *queue, ehci_u8 type,
                          const ehci_u8 *report, ehci_u32 length,
                          ehci_u64 timestamp);
int EHCIInputQueueDequeue(EHCIInputQueue *queue, EHCIInputEvent *event);
void EHCIInputQueueDiscardPending(EHCIInputQueue *queue);
void EHCIInputGapInitialize(EHCIInputGapTracker *tracker);
int EHCIInputGapObserve(EHCIInputGapTracker *tracker, ehci_u64 timestamp,
                        ehci_u64 *gapNanoseconds);

#endif
