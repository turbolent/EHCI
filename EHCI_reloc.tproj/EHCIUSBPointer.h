#ifndef EHCI_USB_POINTER_H
#define EHCI_USB_POINTER_H

#import "OpenStepInputCompat.h"

@class EHCIController;

@interface EHCIUSBPointer : PCPointer
{
@private
    EHCIController *_controller;
}

+ (BOOL)attachToController:(EHCIController *)controller
               description:deviceDescription;
- (BOOL)mouseInit:deviceDescription;
- (BOOL)setEventTarget:eventTarget;
- (void)consumeReport:(const unsigned char *)report length:(unsigned)length;

@end

#endif
