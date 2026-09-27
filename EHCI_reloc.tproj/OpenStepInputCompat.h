#ifndef EHCI_OPENSTEP_INPUT_COMPAT_H
#define EHCI_OPENSTEP_INPUT_COMPAT_H

/*
 * ABI declarations derived from historical NeXT PCKeyboardDefs.h,
 * PCPointerDefs.h, PCPointer.h, and ev_types.h as published in Darwin 0.1.
 */
#import <driverkit/IODevice.h>
#import <driverkit/IODirectDevice.h>
#import <kernserv/clock_timer.h>

#define NX_EVS_DEVICE_INTERFACE_ACE 3
#define RESOLUTION "Resolution"
#define INVERTED "Inverted"

typedef struct _t_PCKeyboardEvent {
    ns_time_t timeStamp;
    unsigned keyCode;
    BOOL goingDown;
} PCKeyboardEvent;

typedef struct _t_PCPointerEvent {
    ns_time_t timeStamp;
    union {
        unsigned char buf[4];
        struct {
            unsigned int leftButton:1;
            unsigned int rightButton:1;
            unsigned int pad:6;
            int dx:8;
            int dy:8;
        } values;
    } data;
} PCPointerEvent;

/* Informal target declarations give GCC 2.7 the correct message return ABI
 * without importing the private kernel headers or creating replacement
 * EventSrc classes. */
@interface Object (EHCIInputTargets)
- (void)dispatchKeyboardEvent:(PCKeyboardEvent *)event;
- (IOReturn)relinquishOwnershipRequest:device;
- (IOReturn)canBecomeOwner:device;
- (void)dispatchPointerEvent:(PCPointerEvent *)event;
@end

/* The ivar layout must match the kernel's existing PCPointer superclass. */
@interface PCPointer : IODirectDevice
{
    id target;
    unsigned resolution;
    BOOL inverted;
@private
    int _reserved[4];
}
+ (id)activePointerDevice;
- (BOOL)setEventTarget:eventTarget;
- (int)getResolution;
- (BOOL)getInverted;
- (BOOL)mouseInit:deviceDescription;
@end

#endif
