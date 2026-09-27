#import "EHCIUSBKeyboard.h"
#import "EHCIController.h"
#import "USBHID.h"
#import <machkit/NXLock.h>
#import <driverkit/generalFuncs.h>

@implementation EHCIUSBKeyboard

- initWithController:(EHCIController *)controller
{
    if ([super init] == nil)
        return nil;
    _controller = controller;
    _owner = nil;
    _desiredOwner = nil;
    _ownerLock = [NXLock new];
    {
        unsigned i;
        for (i = 0; i < sizeof(_previous); i++)
            _previous[i] = 0;
    }
    [self setUnit:0];
    [self setName:"PCKeyboard0"];
    [self setDeviceKind:"EHCIUSBKeyboard"];
    if ([self registerDevice] == nil) {
        IOLog("EHCI: cannot register PCKeyboard0 (disable PS/2 keyboard)\n");
        return [self free];
    }
    return self;
}

- free
{
    if (_ownerLock)
        [_ownerLock free];
    return [super free];
}

- (int)interfaceId
{
    return NX_EVS_DEVICE_INTERFACE_ACE;
}

- (int)handlerId
{
    return 0;
}

- (void)dispatchKey:(unsigned)key down:(BOOL)down
{
    PCKeyboardEvent event;
    id owner;
    if (!key)
        return;
    [_ownerLock lock];
    owner = _owner;
    [_ownerLock unlock];
    if (owner == nil)
        return;
    IOGetTimestamp(&event.timeStamp);
    event.keyCode = key;
    event.goingDown = down;
    [owner dispatchKeyboardEvent:&event];
}

static int
report_contains(const unsigned char *report, unsigned char usage)
{
    unsigned i;
    for (i = 2; i < 8; i++)
        if (report[i] == usage)
            return 1;
    return 0;
}

- (void)consumeReport:(const unsigned char *)report length:(unsigned)length
{
    unsigned i;
    unsigned char changed;
    BOOL rollover = NO;
    if (!report || length < 8)
        return;

    /* Modifier transitions are represented by HID usages E0 through E7. */
    changed = report[0] ^ _previous[0];
    for (i = 0; i < 8; i++) {
        if (changed & (1 << i))
            [self dispatchKey:USBHIDUsageToPCKey(0xe0 + i)
                         down:(report[0] & (1 << i)) != 0];
    }

    for (i = 2; i < 8; i++)
        if (report[i] >= 1 && report[i] <= 3)
            rollover = YES;
    if (rollover) {
        /* ErrorRollOver/PostFail/ErrorUndefined do not describe releases. */
        _previous[0] = report[0];
        return;
    }

    /* Release old six-key-array entries before pressing new ones. */
    for (i = 2; i < 8; i++) {
        unsigned char usage = _previous[i];
        if (usage > 3 && !report_contains(report, usage))
            [self dispatchKey:USBHIDUsageToPCKey(usage) down:NO];
    }
    for (i = 2; i < 8; i++) {
        unsigned char usage = report[i];
        if (usage > 3 && !report_contains(_previous, usage))
            [self dispatchKey:USBHIDUsageToPCKey(usage) down:YES];
    }
    for (i = 0; i < 8; i++)
        _previous[i] = report[i];
}

- (void)setAlphaLockFeedback:(BOOL)locked
{
    /* HID output bit 1 is Caps Lock. The controller performs EP0 I/O only
     * after event dispatch unwinds, avoiding a callback/control deadlock. */
    [_controller queueKeyboardLEDs:(locked ? 2 : 0)];
}

- (IOReturn)becomeOwner:client
{
    IOReturn result = IO_R_SUCCESS;
    id oldOwner;
    [_ownerLock lock];
    oldOwner = _owner;
    if (_owner && _owner != client) {
        if ([_owner respondsTo:@selector(relinquishOwnershipRequest:)])
            result = [_owner relinquishOwnershipRequest:self];
        else
            result = IO_R_BUSY;
    }
    if (result == IO_R_SUCCESS)
        _owner = client;
    [_ownerLock unlock];
    if (result == IO_R_SUCCESS && oldOwner != client)
        IOLog("EHCI: PCKeyboard0 EventSrc owner attached or changed\n");
    return result;
}

- (IOReturn)relinquishOwnership:client
{
    IOReturn result;
    id waiter = nil;
    [_ownerLock lock];
    if (_owner == client) {
        _owner = nil;
        result = IO_R_SUCCESS;
        if (_desiredOwner != client)
            waiter = _desiredOwner;
    } else {
        result = IO_R_BUSY;
    }
    [_ownerLock unlock];
    if (result == IO_R_SUCCESS)
        IOLog("EHCI: PCKeyboard0 EventSrc owner released\n");
    if (waiter && [waiter respondsTo:@selector(canBecomeOwner:)])
        [waiter canBecomeOwner:self];
    return result;
}

- (IOReturn)desireOwnership:client
{
    IOReturn result;
    [_ownerLock lock];
    if (_desiredOwner && _desiredOwner != client)
        result = IO_R_BUSY;
    else {
        _desiredOwner = client;
        result = IO_R_SUCCESS;
    }
    [_ownerLock unlock];
    return result;
}

@end
