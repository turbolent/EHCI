#ifndef USB_MASS_STORAGE_H
#define USB_MASS_STORAGE_H

#include "USBCore.h"

#define USB_STORAGE_TARGETS 4
#define USB_STORAGE_MAX_TRANSFER 65536UL
/* OPENSTEP's raw disk path can issue 128 KiB even when maxTransfer was 64 KiB.
 * A BOT command may span several bounded host-controller bulk transfers. */
#define USB_STORAGE_MAX_REQUEST 131072UL
#define USB_STORAGE_SENSE_BYTES 18

/* Separate from the original boolean EP0 interface used by HID. */
#define USB_TRANSFER_OK            0
#define USB_TRANSFER_STALL         1
#define USB_TRANSFER_TIMEOUT       2
#define USB_TRANSFER_DISCONNECTED  3
#define USB_TRANSFER_ERROR         4
#define USB_TRANSFER_PENDING       5
#define USB_STORAGE_CHECK          6
#define USB_STORAGE_INVALID        7

typedef struct USBMassStorageInterface {
    ehci_u8 number;
    ehci_u8 configuration;
    ehci_u8 bulkIn;
    ehci_u8 bulkOut;
    ehci_u16 inPacket;
    ehci_u16 outPacket;
} USBMassStorageInterface;

/* Deadlines are absolute monotonic milliseconds, including all BOT phases. */
typedef struct USBStorageTransport {
    int (*control)(void *, const USBSetupPacket *, void *, ehci_u32 *, ehci_u64);
    int (*bulk)(void *, ehci_u8, void *, ehci_u32, ehci_u32 *, ehci_u64);
    int (*clearHalt)(void *, ehci_u8, ehci_u64);
    ehci_u64 (*milliseconds)(void *);
} USBStorageTransport;

typedef struct USBMassStorage {
    const USBStorageTransport *transport;
    void *context;
    USBMassStorageInterface interface;
    ehci_u32 tag;
    int unusable;
} USBMassStorage;

typedef struct USBStorageResult {
    int status;
    ehci_u32 actual;
    ehci_u8 sense[USB_STORAGE_SENSE_BYTES];
    int senseValid;
} USBStorageResult;

int USBMassStorageFindInterface(const ehci_u8 *, ehci_u16,
                                 USBMassStorageInterface *);
int USBMassStorageGetMaxLUN(USBMassStorage *, ehci_u8 *, ehci_u64);
int USBMassStorageReset(USBMassStorage *, ehci_u64);
USBStorageResult USBMassStorageCommand(USBMassStorage *, const ehci_u8 *,
    unsigned, void *, ehci_u32, int, int, ehci_u64);

#endif
