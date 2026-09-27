#ifndef USB_STORAGE_SCSI_H
#define USB_STORAGE_SCSI_H
#include "USBMassStorage.h"

typedef struct USBStorageSCSIState {
    ehci_u32 generation;
    ehci_u32 removals;
    ehci_u32 blockSize;
    ehci_u32 lastBlock;
    int present;
    int removed;
    int attention;
    int identified;
    ehci_u8 deviceType;
    int capacityValid;
    int ejected;
    int preventRemoval;
    ehci_u8 sense[USB_STORAGE_SENSE_BYTES];
} USBStorageSCSIState;

void USBStorageSCSIObserve(USBStorageSCSIState *, ehci_u32, ehci_u32, int);
USBStorageResult USBStorageSCSIExecute(USBStorageSCSIState *, USBMassStorage *,
    const ehci_u8 *, unsigned, void *, ehci_u32, int, int, ehci_u64);
unsigned USBStorageCDBLength(ehci_u8);
#endif
