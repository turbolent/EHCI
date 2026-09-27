#ifndef EHCI_CONTROLLER_H
#define EHCI_CONTROLLER_H
#import <driverkit/IOSCSIController.h>
#import <machkit/NXLock.h>
#import "EHCICore.h"
#import "EHCIInterruptState.h"
#import "EHCIPCI.h"
#import "EHCIInputQueue.h"
#import "USBStorageSCSI.h"
#import "EHCIStorageStats.h"
@class EHCIUSBKeyboard;
@class EHCIUSBPointer;
@interface EHCIController : IOSCSIController
{
@public /* C platform bridge; all access follows the documented lock order. */
    EHCIControllerState _state;
    EHCIInterruptState _interruptState;
    id _pciDescription;
    NXLock *_eventLock, *_boundaryLock, *_inputLocks[2];
    vm_address_t _mappedAddress;
    unsigned _mappedBytes, _irq, _pciOwned, _contained;
    volatile unsigned _workersStarted, _startupReady;
    unsigned _serviceEvent;
    EHCIInputQueue _inputQueues[2];
    volatile unsigned _inputReady, _releasePending[2];
    unsigned _pendingLEDs, _ledUpdatePending;
    EHCIUSBKeyboard *_keyboard;
    EHCIUSBPointer *_pointer;
    NXLock *_storageLocks[USB_STORAGE_TARGETS];
    int _storageWaitEvents[USB_STORAGE_TARGETS];
    unsigned _storageActiveRequests;
    BOOL _storageProfileEnabled;
    BOOL _storageProfileActive[USB_STORAGE_TARGETS];
    BOOL _storageTransferActive[USB_STORAGE_TARGETS];
    ehci_u64 _storageServicedAt[USB_STORAGE_TARGETS];
    ehci_u64 _storageMetrics[EHCI_STORAGE_METRICS];
    USBStorageSCSIState _storageDisks[USB_STORAGE_TARGETS];
    USBMassStorage _storageBOT[USB_STORAGE_TARGETS];
    void *_storageBuffers[USB_STORAGE_TARGETS];
    BOOL _scsiThreadStarted;
    id _storageDiskClass;
    id _storageProbeDescription;
    BOOL _storageRegistrationReady;
    BOOL _storageProbePending;
    BOOL _storageProbeRunning;
    int _storageProbeEvent;
}
+ (BOOL)probe:description;
- initFromDeviceDescription:description;
- failInitialization;
- failInitializationAt:(unsigned)line;
- (void)interruptOccurred;
- (void)runCompletionLoop;
- (void)runManagementLoop;
- (void)runRetryLoop;
- (void)runInputLoop:(unsigned)which;
- (void)containFatalController;
- (void)handleKeyboardReport:(const unsigned char *)report length:(unsigned)length;
- (void)handlePointerReport:(const unsigned char *)report length:(unsigned)length;
- (void)queueKeyboardLEDs:(unsigned char)leds;
@end

@interface EHCIController (Storage)
- (BOOL)startStorageProbeWorker;
- (void)finishStorageRegistration;
- (void)queueStorageProbe;
- (void)runStorageProbeLoop;
- (void)waitForStorageProbeExit;
- (void)wakeStorageTarget:(unsigned)target;
- (void)wakeStorageWaiters;
- (sc_status_t)executeRequest:(IOSCSIRequest *)request buffer:(void *)buffer
                       client:(vm_task_t)client;
- (sc_status_t)executeSCSI3Request:(IOSCSI3Request *)request buffer:(void *)buffer
                            client:(vm_task_t)client;
- (sc_status_t)resetSCSIBus;
- (unsigned)maxTransfer;
- (void)getDMAAlignment:(IODMAAlignment *)alignment;
- (int)numberOfTargets;
- (int)storageTransfer:(unsigned)target generation:(ehci_u32)generation
              endpoint:(ehci_u8)endpoint setup:(const USBSetupPacket *)setup
                buffer:(void *)buffer length:(ehci_u32)length
                actual:(ehci_u32 *)actual deadline:(ehci_u64)deadline;
- (int)storageClearHalt:(unsigned)target generation:(ehci_u32)generation
              endpoint:(ehci_u8)endpoint deadline:(ehci_u64)deadline;
- (USBStorageResult)storageExecute:(unsigned)target cdb:(const ehci_u8 *)cdb
                           length:(unsigned)cdbLength buffer:(void *)buffer
                           client:(vm_task_t)client maximum:(unsigned)maximum
                             read:(int)read autoSense:(int)autoSense
                         deadline:(ehci_u64)deadline;

@end

#endif
