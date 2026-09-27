#ifndef EHCI_USB_KEYBOARD_H
#define EHCI_USB_KEYBOARD_H

#import <driverkit/IODevice.h>
#import "OpenStepInputCompat.h"

@class EHCIController;

@interface EHCIUSBKeyboard : IODevice
{
@private
    EHCIController *_controller;
    id _owner;
    id _desiredOwner;
    id _ownerLock;
    unsigned char _previous[8];
}

- initWithController:(EHCIController *)controller;
- (void)consumeReport:(const unsigned char *)report length:(unsigned)length;
- (void)setAlphaLockFeedback:(BOOL)locked;
- (int)interfaceId;
- (int)handlerId;
- (IOReturn)becomeOwner:client;
- (IOReturn)relinquishOwnership:client;
- (IOReturn)desireOwnership:client;

@end

#endif
