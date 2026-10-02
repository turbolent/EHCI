#ifndef EHCI_CORE_H
#define EHCI_CORE_H
#include "EHCIRegs.h"
#include "USBHID.h"
#include "USBMassStorage.h"
#include "USBECM.h"

#define EHCI_TD_COUNT 34U
#define EHCI_ECM_BATCH_FRAMES 16U
#define EHCI_DATA_PAGES 16U
#define EHCI_FRAME_COUNT 1024U
#define EHCI_ENDPOINTS 4U
#define EHCI_EP_CONTROL 0U
#define EHCI_EP_INTERRUPT 1U
#define EHCI_EP_BULK_IN 2U
#define EHCI_EP_BULK_OUT 3U

typedef struct EHCIEndpoint {
    EHCIDMA descriptors, data[EHCI_DATA_PAGES], setup;
    EHCIQH *qh;
    EHCIqTD *td;
    ehci_u32 qhPhysical, tdPhysical;
    ehci_u32 lengths[EHCI_TD_COUNT], total, actual;
    unsigned ecmBatch, batchDone, batchFrames, batchComplete;
    ehci_u8 frameEnd[EHCI_TD_COUNT];
    unsigned count, dataFirst, dataLast, statusTD;
    unsigned address, maxPacket, interval, phase, smask, cmask;
    unsigned configured, linked, periodic, waiting, control, input;
    unsigned toggle, halted, reserved, ttSlot, ttMask;
    int result;
    ehci_u64 deadline;
} EHCIEndpoint;

typedef struct EHCIDevice {
    unsigned used, ready, generation, address, port, speed;
    unsigned parent, parentPort, ttAddress, ttPort, hub, hubPorts;
    unsigned hubProtocol, hubCharacteristics, ttThinkTime;
    unsigned powerMA, selfPowerCapable, selfPowered, externalPorts, nonRemovable;
    unsigned portPowerMA[EHCI_MAX_HUB_PORTS + 1];
    unsigned protocol, interfaceNumber, configurationValue, storage, ecm;
    unsigned hubChanges, disconnecting, blockedPorts, controlBusy;
    USBMassStorageInterface storageInterface;
    USBECMInterface ecmInterface;
    USBCoreDevice usb;
    EHCIEndpoint endpoints[EHCI_ENDPOINTS];
} EHCIDevice;

typedef struct EHCIStorageBinding {
    ehci_u32 generation, removals;
    unsigned slot, lastPort;
} EHCIStorageBinding;

/* Bounded notification evidence, copied before the DMA buffer is rearmed. */
typedef struct EHCIECMNotifySample {
    unsigned generation, milliseconds, length, requested, used, expected;
    unsigned qtdToken, qhToken;
    int result;
    ehci_u8 bytes[32];
} EHCIECMNotifySample;

/* Retained across unplug: native network objects can still receive callbacks. */
typedef struct EHCIECMState {
    unsigned slot, generation, macValid, enabled, filter, appliedFilter;
    unsigned rxActive, txActive, notifyActive, rxDrop, recoveries[4];
    unsigned rxPackets, txPackets, rxErrors, txErrors, removals;
    unsigned notifyPackets, notifyUSBErrors, notifyParseErrors;
    unsigned rxUSBErrors, rxOversize, rxInvalid, rxQueueDrops, rxResyncDrops;
    unsigned rxZeroPackets, rxNoBuffer;
    unsigned rxBatches, txBatches, txInFlight, txQueueHighWater, asyncRearms;
    unsigned txQueueDrops, txInvalid, txUSBErrors, txBackpressure;
    unsigned txNativeQueued, txNativeHighWater;
    EHCIECMNotifySample lastNotify, badNotify;
    ehci_u8 mac[6];
    USBECMNotifications notifications;
    USBECMQueue rx, tx;
} EHCIECMState;

typedef struct EHCIEnumerationFailure {
    const char *stage;
    unsigned rootPort, hubPort, speed, vendor, product, protocol, packet, interval;
} EHCIEnumerationFailure;

typedef struct EHCIControllerState {
    volatile ehci_u8 *mmio;
    ehci_u32 mmioBytes, operationalOffset, maxPorts;
    unsigned running, registersValid, dmaArmed, haltConfirmed, fatal;
    unsigned hasPortPowerControl, companions, has64Bit;
    unsigned portChanges, disconnected, initialized, enumerating, blockedPorts;
    unsigned scheduleBusy, rescan;
    unsigned addresses[4], nextGeneration;
    unsigned keyboardSlot, pointerSlot;
    unsigned interruptModerationUS;
    unsigned transferEvents, keyboardReports, mouseReports, polls;
    unsigned scheduleChanges, timeouts, errors;
    const char *faultReason;
    unsigned faultDetail, lastCommand, lastStatus;
    unsigned waitOffset, waitMask, waitExpected, waitObserved;
    EHCIDMA asyncHead, frameList;
    EHCIDevice devices[EHCI_MAX_DEVICES];
    EHCIStorageBinding storage[USB_STORAGE_TARGETS];
    EHCIECMState *ecm;
    EHCIEnumerationFailure enumerationFailure;
    void *owner;
} EHCIControllerState;

/* Native glue serializes every MMIO/PCI operation with IRQ containment.
 * Core entry points require the controller-state lock. Sleep releases that
 * lock in the native glue so completion service can progress. */
ehci_u32 EHCIPlatformRead(EHCIControllerState *, ehci_u32 offset);
int EHCIPlatformWrite(EHCIControllerState *, ehci_u32 offset, ehci_u32 value);
int EHCIPlatformPCIRead(EHCIControllerState *, unsigned, ehci_u32 *);
int EHCIPlatformPCIWrite(EHCIControllerState *, unsigned, ehci_u32);
void *EHCIPlatformAllocate(ehci_u32);
void EHCIPlatformFree(void *, ehci_u32);
ehci_u32 EHCIPlatformPhysical(void *);
ehci_u64 EHCIPlatformMilliseconds(void);
void EHCIPlatformPause(EHCIControllerState *, unsigned milliseconds);
/* Bounded hardware wait only: keeps the state lock, never the boundary lock. */
void EHCIPlatformDelay(EHCIControllerState *, unsigned microseconds);
void EHCIPlatformWake(void *);
void EHCIPlatformKeyboardReport(void *, const ehci_u8 *, ehci_u32);
void EHCIPlatformPointerReport(void *, const ehci_u8 *, ehci_u32);
void EHCIPlatformStorageWake(void *, unsigned);
void EHCIPlatformStorageAttached(void *);
void EHCIPlatformNetworkWake(void *);
#ifdef KERNEL
#import <driverkit/generalFuncs.h>
#define EHCIPlatformLog IOLog
#else
void EHCIPlatformLog(const char *, ...);
#endif

int EHCICoreInitialize(EHCIControllerState *);
int EHCICoreStart(EHCIControllerState *);
int EHCICoreQuiesce(EHCIControllerState *);
int EHCICoreReleaseDMA(EHCIControllerState *, int busMasterDisabled);
int EHCICoreService(EHCIControllerState *, unsigned causes);
int EHCICoreServicePorts(EHCIControllerState *);
int EHCICoreSetKeyboardLEDs(EHCIControllerState *, ehci_u8);
int EHCICoreSupportsPCIClass(ehci_u32);
void EHCICoreFail(EHCIControllerState *, const char *, unsigned);
EHCIDevice *EHCICoreStorageDevice(EHCIControllerState *, unsigned, ehci_u32);
int EHCICoreStorageBulkStart(EHCIControllerState *, unsigned, ehci_u32,
                            ehci_u8, const void *, ehci_u32);
int EHCICoreStorageControlStart(EHCIControllerState *, unsigned, ehci_u32,
                               const USBSetupPacket *, const void *);
int EHCICoreStorageTransferResult(EHCIControllerState *, unsigned, ehci_u32,
                                  ehci_u8, void *, ehci_u32 *);
int EHCICoreStorageRecoverEndpoint(EHCIControllerState *, unsigned, ehci_u32,
                                   ehci_u8, ehci_u64);
int EHCICoreStorageRetireEndpoint(EHCIControllerState *, unsigned, ehci_u32,
                                  ehci_u8, ehci_u64);
void EHCICoreStorageOffline(EHCIControllerState *, unsigned, ehci_u32);
EHCIDevice *EHCICoreECMDevice(EHCIControllerState *, unsigned generation);
void EHCICoreECMPump(EHCIControllerState *);
int EHCICoreECMBatchStart(EHCIControllerState *, EHCIDevice *, EHCIEndpoint *);
int EHCICoreECMTransmit(EHCIControllerState *, unsigned generation,
                        const ehci_u8 *, unsigned);
void EHCICoreECMEnable(EHCIControllerState *, unsigned enabled, unsigned filter);
/* Shared schedule helpers are also exercised directly by host tests. */
int EHCICoreConfigureEndpoint(EHCIControllerState *, EHCIDevice *, unsigned,
                             unsigned address, unsigned packet, unsigned interval);
int EHCICoreSubmit(EHCIControllerState *, EHCIDevice *, EHCIEndpoint *,
                   const USBSetupPacket *, const void *, unsigned, ehci_u64);
int EHCICoreFinish(EHCIControllerState *, EHCIDevice *, EHCIEndpoint *);
int EHCICoreCancel(EHCIControllerState *, EHCIDevice *, EHCIEndpoint *);
void EHCICoreDisconnect(EHCIControllerState *, unsigned slot);
unsigned EHCIIntervalFrames(unsigned speed, unsigned interval);
ehci_u32 EHCIPortWriteValue(ehci_u32 current, ehci_u32 set,
                           ehci_u32 clear, ehci_u32 changes);
#endif
