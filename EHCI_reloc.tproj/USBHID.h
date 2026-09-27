#ifndef EHCI_USB_HID_H
#define EHCI_USB_HID_H

#include "USBCore.h"

#define USB_HID_SET_REPORT      0x09
#define USB_HID_SET_IDLE        0x0a
#define USB_HID_SET_PROTOCOL    0x0b

#define USB_CLASS_HID           3
#define USB_HID_PROTOCOL_KEYBOARD 1
#define USB_HID_PROTOCOL_MOUSE  2

typedef struct USBHIDInterface {
    ehci_u8 configurationValue;
    ehci_u8 interfaceNumber;
    ehci_u8 alternateSetting;
    ehci_u8 protocol;
    ehci_u8 endpointAddress;
    ehci_u8 interval;
    ehci_u16 maxPacket;
} USBHIDInterface;

int USBHIDFindBootInterface(const ehci_u8 *bytes, ehci_u16 length,
                            USBHIDInterface *result);
int USBHIDSetBootProtocol(USBCoreDevice *device,
                          ehci_u8 interfaceNumber);
int USBHIDSetIdle(USBCoreDevice *device, ehci_u8 interfaceNumber);
int USBHIDSetKeyboardLEDs(USBCoreDevice *device,
                          ehci_u8 interfaceNumber, ehci_u8 leds);
unsigned USBHIDUsageToPCKey(ehci_u8 usage);
int USBHIDBootMouseReportChanged(const ehci_u8 *report, ehci_u32 length,
                                 ehci_u8 previousButtons);

#endif
