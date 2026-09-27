#import "EHCIUSBPointer.h"
#import "EHCIController.h"
#import <driverkit/generalFuncs.h>

static EHCIController *controllerBeingAttached;

@implementation EHCIUSBPointer

+ (BOOL)attachToController:(EHCIController *)controller
               description:deviceDescription
{
    BOOL result;
    controllerBeingAttached = controller;
    result = [self probe:deviceDescription]; /* PCPointer establishes singleton */
    controllerBeingAttached = nil;
    return result;
}

- (BOOL)mouseInit:deviceDescription
{
    _controller = controllerBeingAttached;
    if (!_controller)
        return NO;
    target = nil;
    resolution = 100;
    inverted = NO;
    [self setName:"EHCIUSBPointer"];
    [self setDeviceKind:"EHCIUSBPointer"];
    return YES;
}

- (BOOL)setEventTarget:eventTarget
{
    BOOL result = [super setEventTarget:eventTarget];
    IOLog("EHCI: PCPointer EventSrc target %s\n",
          result ? "attached" : "rejected");
    return result;
}

- (void)consumeReport:(const unsigned char *)report length:(unsigned)length
{
    PCPointerEvent event;
    id eventTarget;
    int y;
    if (!report || length < 3)
        return;
    eventTarget = target;
    if (eventTarget == nil)
        return;
    IOGetTimestamp(&event.timeStamp);
    event.data.buf[0] = report[0] & 3; /* native ABI has left/right only */
    event.data.buf[1] = report[1];
    /* HID Y grows down; PCPointer's historical PS/2 input grows up and the
     * stock EventSrc negates it. Convert here so screen motion stays natural. */
    y = -(int)(signed char)report[2];
    if (y > 127)
        y = 127;
    event.data.buf[2] = (unsigned char)(signed char)y;
    event.data.buf[3] = 0;
    [eventTarget dispatchPointerEvent:&event];
}

@end
