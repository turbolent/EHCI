#ifndef USB_ECM_H
#define USB_ECM_H
#include "USBCore.h"

#define USB_ECM_FRAME_MAX 1514U
#define USB_ECM_FRAME_MIN 60U
#define USB_ECM_RX_BYTES 2048U
#define USB_ECM_QUEUE_SIZE 32U
#define USB_ECM_FILTER_DIRECTED 0x0004U
#define USB_ECM_FILTER_BROADCAST 0x0008U
#define USB_ECM_FILTER_ALL_MULTICAST 0x0002U
#define USB_ECM_FILTER_PROMISCUOUS 0x0001U
#define USB_ECM_FILTER_DEFAULT (USB_ECM_FILTER_DIRECTED | USB_ECM_FILTER_BROADCAST)

typedef struct USBECMInterface {
    ehci_u8 configuration, controlInterface, controlAlternate;
    ehci_u8 dataInterface, dataAlternate, macString;
    ehci_u8 bulkIn, bulkOut, notification, interval;
    ehci_u16 inPacket, outPacket, notificationPacket, maxSegment;
} USBECMInterface;

typedef struct USBECMNotifications {
    ehci_u8 bytes[16];
    unsigned used, expected, linkKnown, linkUp, speedKnown;
    ehci_u32 upstream, downstream;
} USBECMNotifications;

typedef struct USBECMFrame {
    unsigned length;
    ehci_u8 bytes[USB_ECM_FRAME_MAX];
} USBECMFrame;
typedef struct USBECMQueue {
    unsigned head, count;
    USBECMFrame frames[USB_ECM_QUEUE_SIZE];
} USBECMQueue;

/* 1 valid ECM function, 0 no ECM interface, -1 malformed/unsupported ECM. */
int USBECMFindInterface(const ehci_u8 *, ehci_u16, USBECMInterface *);
int USBECMDecodeMAC(const ehci_u8 *, unsigned, ehci_u8 *);
int USBECMReadMAC(USBCoreDevice *, ehci_u8, ehci_u8 *);
int USBECMSetFilter(USBCoreDevice *, ehci_u8, ehci_u16);
/* Accept only interfaces in this ECM union. Bits 0/1 report a first or changed
 * link/speed value; duplicates return zero; -1 means invalid notification. */
int USBECMNotification(USBECMNotifications *, unsigned, unsigned, const ehci_u8 *, unsigned);
unsigned USBECMPrepareFrame(ehci_u8 *, const ehci_u8 *, unsigned);
int USBECMQueuePush(USBECMQueue *, const ehci_u8 *, unsigned);
int USBECMQueuePop(USBECMQueue *, USBECMFrame *);
#endif
