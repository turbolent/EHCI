#include "USBHID.h"

int
USBHIDFindBootInterface(const ehci_u8 *bytes, ehci_u16 length,
                        USBHIDInterface *result)
{
    USBDescriptorIterator iterator;
    const ehci_u8 *descriptor;
    ehci_u8 descriptorLength;
    ehci_u8 descriptorType;
    ehci_u8 config;
    int candidate;
    int next;

    if (!bytes || !result || length < 9 || bytes[1] != USB_DESC_CONFIG ||
        bytes[0] < 9 || bytes[5] == 0)
        return 0;

    config = bytes[5];
    candidate = 0;
    result->configurationValue = config;
    result->interfaceNumber = 0;
    result->alternateSetting = 0;
    result->protocol = 0;
    result->endpointAddress = 0;
    result->interval = 0;
    result->maxPacket = 0;

    USBCoreDescriptorIteratorInitialize(&iterator, bytes, length);
    while ((next = USBCoreDescriptorNext(&iterator, &descriptor,
                                          &descriptorLength,
                                          &descriptorType)) ==
           USB_DESCRIPTOR_FOUND) {
        if (descriptorType == USB_DESC_INTERFACE && descriptorLength >= 9) {
            candidate = (descriptor[5] == USB_CLASS_HID &&
                         descriptor[6] == 1 &&
                         descriptor[3] == 0 &&
                         (descriptor[7] == USB_HID_PROTOCOL_KEYBOARD ||
                          descriptor[7] == USB_HID_PROTOCOL_MOUSE));
            if (candidate) {
                result->interfaceNumber = descriptor[2];
                result->alternateSetting = descriptor[3];
                result->protocol = descriptor[7];
                result->endpointAddress = 0;
            }
        } else if (candidate && descriptorType == USB_DESC_ENDPOINT &&
                   descriptorLength >= 7) {
            ehci_u8 address = descriptor[2];
            ehci_u8 attributes = descriptor[3] & 3;
            ehci_u16 packet = USBCoreReadLE16(descriptor + 4);
            if ((address & USB_DIR_IN) && !(address & 0x70) &&
                (address & 0x0f) != 0 && attributes == 3 &&
                !(packet & 0xf800)) {
                result->endpointAddress = address;
                result->maxPacket = packet & 0x7ff;
                result->interval = descriptor[6];
                if (!result->interval || !result->maxPacket ||
                    result->maxPacket > 1024)
                    return 0;
                if (result->protocol == USB_HID_PROTOCOL_KEYBOARD &&
                    result->maxPacket < 8)
                    return 0;
                if (result->protocol == USB_HID_PROTOCOL_MOUSE &&
                    result->maxPacket < 3)
                    return 0;
                return 1;
            }
        }
    }
    if (next == USB_DESCRIPTOR_MALFORMED)
        return 0;
    return 0;
}

int
USBHIDSetBootProtocol(USBCoreDevice *device, ehci_u8 interfaceNumber)
{
    USBSetupPacket setup;
    setup.requestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    setup.request = USB_HID_SET_PROTOCOL;
    setup.value = 0;             /* boot protocol */
    setup.index = interfaceNumber;
    setup.length = 0;
    return USBCoreControlTransfer(device, &setup, 0, 0);
}

int
USBHIDSetIdle(USBCoreDevice *device, ehci_u8 interfaceNumber)
{
    USBSetupPacket setup;
    setup.requestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    setup.request = USB_HID_SET_IDLE;
    setup.value = 0;             /* send reports only when data changes */
    setup.index = interfaceNumber;
    setup.length = 0;
    return USBCoreControlTransfer(device, &setup, 0, 0);
}

int
USBHIDSetKeyboardLEDs(USBCoreDevice *device, ehci_u8 interfaceNumber,
                      ehci_u8 leds)
{
    USBSetupPacket setup;
    setup.requestType = USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    setup.request = USB_HID_SET_REPORT;
    setup.value = 0x0200;        /* output report, ID zero */
    setup.index = interfaceNumber;
    setup.length = 1;
    return USBCoreControlTransfer(device, &setup, &leds, 0);
}

/*
 * HID usages are physical key positions. These values are the normalized
 * one-byte PC key numbers consumed by EventSrcPCKeyboard/PCKeymap; character
 * and layout translation deliberately happens above this driver.
 */
unsigned
USBHIDUsageToPCKey(ehci_u8 usage)
{
    static const ehci_u8 letters[26] = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17,
        0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13,
        0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c
    };
    static const ehci_u8 numbers[10] = {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b
    };
    static const ehci_u8 punctuation[12] = {
        0x1c, 0x01, 0x0e, 0x0f, 0x39, 0x0c,
        0x0d, 0x1a, 0x1b, 0x2b, 0x2b, 0x27
    };

    if (usage >= 0x04 && usage <= 0x1d)
        return letters[usage - 0x04];
    if (usage >= 0x1e && usage <= 0x27)
        return numbers[usage - 0x1e];
    if (usage >= 0x28 && usage <= 0x33)
        return punctuation[usage - 0x28];
    if (usage >= 0x3a && usage <= 0x43)
        return 0x3b + (usage - 0x3a);       /* F1 through F10 */

    switch (usage) {
    case 0x34: return 0x28; /* quote */
    case 0x35: return 0x29; /* grave */
    case 0x36: return 0x33; /* comma */
    case 0x37: return 0x34; /* period */
    case 0x38: return 0x35; /* slash */
    case 0x39: return 0x3a; /* caps lock */
    case 0x44: return 0x57; /* F11 */
    case 0x45: return 0x58; /* F12 */
    case 0x46: return 0x6e; /* print screen */
    case 0x47: return 0x46; /* scroll lock */
    case 0x48: return 0x6f; /* pause */
    case 0x49: return 0x68; /* insert */
    case 0x4a: return 0x6c; /* home */
    case 0x4b: return 0x6a; /* page up */
    case 0x4c: return 0x69; /* delete */
    case 0x4d: return 0x6d; /* end */
    case 0x4e: return 0x6b; /* page down */
    case 0x4f: return 0x67; /* right */
    case 0x50: return 0x66; /* left */
    case 0x51: return 0x65; /* down */
    case 0x52: return 0x64; /* up */
    case 0x53: return 0x45; /* num lock */
    case 0x54: return 0x63; /* keypad slash */
    case 0x55: return 0x37; /* keypad star */
    case 0x56: return 0x4a; /* keypad minus */
    case 0x57: return 0x4e; /* keypad plus */
    case 0x58: return 0x62; /* keypad enter */
    case 0x59: return 0x4f; /* keypad 1 */
    case 0x5a: return 0x50;
    case 0x5b: return 0x51;
    case 0x5c: return 0x4b;
    case 0x5d: return 0x4c;
    case 0x5e: return 0x4d;
    case 0x5f: return 0x47;
    case 0x60: return 0x48;
    case 0x61: return 0x49; /* keypad 9 */
    case 0x62: return 0x52; /* keypad 0 */
    case 0x63: return 0x53; /* keypad dot */
    case 0x64: return 0x56; /* ISO non-US backslash */
    case 0xe0: return 0x1d; /* left control */
    case 0xe1: return 0x2a; /* left shift */
    case 0xe2: return 0x38; /* left alt */
    case 0xe4: return 0x60; /* right control */
    case 0xe5: return 0x36; /* right shift */
    case 0xe6: return 0x61; /* right alt */
    default:   return 0;
    }
}

int
USBHIDBootMouseReportChanged(const ehci_u8 *report, ehci_u32 length,
                             ehci_u8 previousButtons)
{
    if (!report || length < 3)
        return 0;
    return (report[0] & 3) != (previousButtons & 3) ||
           report[1] != 0 || report[2] != 0;
}
