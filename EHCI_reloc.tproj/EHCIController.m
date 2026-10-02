#define MACH_USER_API 1
#import "EHCIController.h"
#import "EHCIUSBKeyboard.h"
#import "EHCIUSBPointer.h"
#import "EHCIVersion.h"
#import "EHCICompletionWait.h"
#import <driverkit/generalFuncs.h>
#import <driverkit/kernelDriver.h>
#import <driverkit/i386/kernelDriver.h>
#import <driverkit/i386/IOPCIDirectDevice.h>
#import <driverkit/i386/IOPCIDeviceDescription.h>
#import <driverkit/i386/PCI.h>
#import <kernserv/prototypes.h>
#import <mach/vm_param.h>
#import <string.h>
#import <stdio.h>

extern void thread_wakeup(int);
extern int hz;
static int pciRead(void *ctx, unsigned offset, ehci_u32 *v)
{
    unsigned long data;
    EHCIController *c = ctx;
    if ([IODirectDevice getPCIConfigData:&data atRegister:offset
        withDeviceDescription:c->_pciDescription] != IO_R_SUCCESS) return 0;
    *v = data; return 1;
}
static int pciWrite(void *ctx, unsigned offset, ehci_u32 v)
{
    EHCIController *c = ctx;
    return [IODirectDevice setPCIConfigData:v atRegister:offset
        withDeviceDescription:c->_pciDescription] == IO_R_SUCCESS;
}
static const EHCIPCIOps pciOps = {pciRead, pciWrite};
/* Raw functions may only be called inside _boundaryLock. They never acquire
 * the event lock or wait for hardware. Observation-time loss is latched by
 * the calling portable policy or PlatformRead before unlocking. */
static ehci_u32 rawRead(EHCIController *c, unsigned offset)
{
    if (c->_interruptState.mmioLost || !c->_state.mmio ||
        (offset & 3) || offset + 4 > c->_state.mmioBytes) return 0xffffffffU;
    return *(volatile ehci_u32 *)(c->_state.mmio + offset);
}
static int rawWrite(EHCIController *c, unsigned offset, ehci_u32 v)
{
    if (c->_interruptState.mmioLost || !c->_state.mmio ||
        (offset & 3) || offset + 4 > c->_state.mmioBytes) return 0;
    *(volatile ehci_u32 *)(c->_state.mmio + offset) = v;
    return 1;
}
static ehci_u32 irqRead(void *ctx, unsigned r)
{ EHCIController *c = ctx; return rawRead(c, c->_state.operationalOffset + r); }
static int irqWrite(void *ctx, unsigned r, ehci_u32 v)
{ EHCIController *c = ctx; return rawWrite(c, c->_state.operationalOffset + r, v); }
static int irqPCIRead(void *ctx, ehci_u32 *v) { return pciRead(ctx, 4, v); }
static int irqPCIWrite(void *ctx, ehci_u32 v) { return pciWrite(ctx, 4, v); }
static int irqRearm(void *ctx) { return [(EHCIController *)ctx enableAllInterrupts] == IO_R_SUCCESS; }
static void irqPublish(void *ctx)
{ thread_wakeup((int)&((EHCIController *)ctx)->_serviceEvent); }
static const EHCIInterruptOps interruptOps = {
    irqRead, irqWrite, irqPCIRead, irqPCIWrite, irqRearm, irqPublish
};
static void completionLock(void *ctx)
{ EHCIController *c = ctx; [c->_eventLock lock]; [c->_boundaryLock lock]; }
static void completionUnlock(void *ctx)
{ EHCIController *c = ctx; [c->_boundaryLock unlock]; [c->_eventLock unlock]; }
static int completionReady(void *ctx)
{
    EHCIController *c = ctx;
    return EHCICompletionWorkReady(c->_startupReady, c->_interruptState.work,
        c->_interruptState.stopping, c->_state.fatal, c->_state.rescan,
        c->_state.scheduleBusy);
}
static void completionPrepare(void *ctx)
{
    EHCIController *c = ctx;
    assert_wait((int)&c->_serviceEvent, FALSE);
    thread_set_timeout(1);
}
static void completionBlock(void *ctx) { (void)ctx; thread_block(); }
static const EHCICompletionWaitOps completionWaitOps = {
    completionLock, completionUnlock, completionReady, completionPrepare, completionBlock
};
static void completionThread(void *ctx) { [(EHCIController *)ctx runCompletionLoop]; IOExitThread(); }
static void managementThread(void *ctx) { [(EHCIController *)ctx runManagementLoop]; IOExitThread(); }
static void retryThread(void *ctx) { [(EHCIController *)ctx runRetryLoop]; IOExitThread(); }
static void keyboardThread(void *ctx) { [(EHCIController *)ctx runInputLoop:0]; IOExitThread(); }
static void pointerThread(void *ctx) { [(EHCIController *)ctx runInputLoop:1]; IOExitThread(); }

@implementation EHCIController
+ (BOOL)probe:description
{
    EHCIController *c = [[self alloc] initFromDeviceDescription:description];
    if (!c) return NO;
    if (![c startStorageProbeWorker] || ![c registerDevice]) { [c free]; return NO; }
    [c finishStorageRegistration];
    if (![c startNetworkWorker]) { [c free]; return NO; }
    return YES;
}
- initFromDeviceDescription:description
{
    IOPCIConfigSpace pci;
    IORange range;
    unsigned address, bytes, i, mode, mapStart, mapBytes, mapOffset, usbInput = 1;
    ehci_u32 routing;
    const char *value;
    IOConfigTable *table = [description configTable];
    /* IOSCSIController free decrements its counter when unit == counter - 1.
     * With no controllers, ~0U matches -1 and corrupts that counter even
     * though this rejected probe never called superclass initialization. */
    [self setUnit:0x7fffffffU];
    _pciDescription = description;
    _state.owner = self;
    _interruptState.ops = &interruptOps; _interruptState.context = self;
    _boundaryLock = [[NXLock alloc] init]; _eventLock = [[NXLock alloc] init];
    if (!_boundaryLock || !_eventLock) return [self failInitializationAt:__LINE__];
    bzero(&pci, sizeof(pci));
    if ([IODirectDevice getPCIConfigSpace:&pci withDeviceDescription:description] != IO_R_SUCCESS ||
        !EHCICoreSupportsPCIClass(pci.ClassCode)) return [self failInitializationAt:__LINE__];
    /* Even the first PCI fence can fail transiently. Provision its retry
     * owner before acquiring hardware ownership or publishing a superclass
     * I/O task. A fork failure leaves the identified controller untouched. */
    if (!IOForkThread(retryThread, self)) return [self failInitializationAt:__LINE__];
    _workersStarted = 1;
    _pciOwned = 1;
    /* This is the first mutation, before BAR probing or superclass startup. */
    [_boundaryLock lock];
    i = EHCIPCICommand(&pciOps, self, EHCI_PCI_INT_DISABLE, EHCI_PCI_MASTER) &&
        EHCIPCIDisableMessages(&pciOps, self);
    if (i) _interruptState.fenced = _interruptState.dmaFenced = 1;
    [_boundaryLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    value = [table valueForStringKey:"Interrupt Mode"];
    mode = EHCIInterruptModeParse(value);
    if (value) [table freeString:value];
    value = [table valueForStringKey:"Polling Only"];
    if (value) { [table freeString:value]; mode = EHCI_MODE_INVALID; }
    if (!mode) { IOLog("EHCI: Interrupt Mode must be INTx or Polling\n"); return [self failInitializationAt:__LINE__]; }
    _interruptState.mode = mode;
    value = [table valueForStringKey:"USB Input"];
    if (value) {
        if (!strcmp(value, "No") || !strcmp(value, "NO")) usbInput = 0;
        else if (strcmp(value, "Yes") && strcmp(value, "YES")) {
            [table freeString:value];
            IOLog("EHCI: USB Input must be Yes or No\n");
            return [self failInitializationAt:__LINE__];
        }
        [table freeString:value];
    }
    if (mode == EHCI_MODE_INTX) {
        value = [table valueForStringKey:"Share IRQ Levels"];
        i = value && (!strcmp(value, "Yes") || !strcmp(value, "YES"));
        if (value) [table freeString:value];
        if (!i || !pciRead(self, 0x3c, &routing) || !(routing & 0xff00) ||
            ((routing >> 8) & 255) > 4 || !(routing & 255) || (routing & 255) >= 16 || (routing & 255) == 2)
            return [self failInitializationAt:__LINE__];
        _irq = routing & 255;
        if ([description setInterruptList:&_irq num:1] != IO_R_SUCCESS) return [self failInitializationAt:__LINE__];
    } else if ([description setInterruptList:0 num:0] != IO_R_SUCCESS) return [self failInitializationAt:__LINE__];
    if (!EHCIPCIBar(&pciOps, self, &address, &bytes)) return [self failInitializationAt:__LINE__];
    /* DriverKit 4.2 maps whole Mach pages and does not add the BAR's page
     * offset. In particular a 4 KiB BAR can start halfway into an 8 KiB page. */
    if (!EHCIPCIMapRange(address, bytes, page_size, &mapStart, &mapBytes, &mapOffset))
        return [self failInitializationAt:__LINE__];
    range.start = mapStart; range.size = mapBytes;
    if ([description setMemoryRangeList:&range num:1] != IO_R_SUCCESS) return [self failInitializationAt:__LINE__];
    if (![super initFromDeviceDescription:description]) return [self failInitializationAt:__LINE__];
    _scsiThreadStarted = YES;
    {
        char name[24]; id existing; unsigned unit = [self unit];
        /* A previously rejected controller can leave DriverKit's shared
         * counter negative. Recover the registered namespace from sc0. */
        if (unit & 0x80000000U) unit = 0;
        do { sprintf(name, "sc%u", unit++); }
        while (IOGetObjectForDeviceName(name, &existing) == IO_R_SUCCESS);
        [self setUnit:unit - 1]; [self setName:name];
    }
    if (mode == EHCI_MODE_INTX) {
        unsigned *list = [[self deviceDescription] interruptList];
        if ([[self deviceDescription] numInterrupts] != 1 || !list || list[0] != _irq ||
            [self interruptPort] == PORT_NULL) return [self failInitializationAt:__LINE__];
    } else if ([[self deviceDescription] numInterrupts]) return [self failInitializationAt:__LINE__];
    _mappedBytes = mapBytes;
    if ([self mapMemoryRange:0 to:&_mappedAddress findSpace:YES cache:IO_CacheOff] != IO_R_SUCCESS ||
        !_mappedAddress) return [self failInitializationAt:__LINE__];
    _state.mmio = (volatile ehci_u8 *)(_mappedAddress + mapOffset); _state.mmioBytes = bytes;
    for (i = 0; i < 2; i++) {
        _inputLocks[i] = [[NXLock alloc] init];
        EHCIInputQueueInitialize(&_inputQueues[i]);
        if (!_inputLocks[i]) return [self failInitializationAt:__LINE__];
    }
    for (i = 0; i < USB_STORAGE_TARGETS; i++) {
        _storageLocks[i] = [[NXLock alloc] init];
        _storageBuffers[i] = IOMalloc(USB_STORAGE_MAX_REQUEST);
        if (!_storageLocks[i] || !_storageBuffers[i]) return [self failInitializationAt:__LINE__];
    }
    [_eventLock lock];
    i = EHCICoreInitialize(&_state);
    [_eventLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    if (!IOForkThread(completionThread, self)) return [self failInitializationAt:__LINE__];
    [_boundaryLock lock];
    i = EHCIPCICommand(&pciOps, self, EHCI_PCI_MASTER | EHCI_PCI_MEMORY | EHCI_PCI_INT_DISABLE, 0);
    if (i) _interruptState.dmaFenced = 0;
    [_boundaryLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    [_eventLock lock]; i = EHCICoreStart(&_state); [_eventLock unlock];
    if (!i) return [self failInitializationAt:__LINE__];
    [_boundaryLock lock];
    if (mode == EHCI_MODE_INTX) {
        _interruptState.participant = 1; _interruptState.rearmDebt = 1;
        /* Pending port status may intentionally defer opening to the worker. */
        EHCIInterruptRestore(&_interruptState);
    }
    _startupReady = 1;
    [_boundaryLock unlock];
    irqPublish(self);
    [_eventLock lock]; i = EHCICoreServicePorts(&_state); [_eventLock unlock];
    if (!i || _interruptState.stopping) return [self failInitializationAt:__LINE__];
    {
        unsigned targets = 0;
        [_eventLock lock];
        for (i = 0; i < USB_STORAGE_TARGETS; i++)
            if (_state.storage[i].slot) targets++;
        [_eventLock unlock];
        IOLog("EHCI: %s initial scan: %u storage targets\n",
            [self name], targets);
    }
    if (!IOForkThread(managementThread, self) || !IOForkThread(keyboardThread, self) ||
        !IOForkThread(pointerThread, self)) return [self failInitializationAt:__LINE__];
    if (usbInput) {
        _keyboard = [[EHCIUSBKeyboard alloc] initWithController:self];
        if (!_keyboard || ![EHCIUSBPointer attachToController:self description:description])
            return [self failInitializationAt:__LINE__];
        _pointer = [EHCIUSBPointer activePointerDevice];
    } else IOLog("EHCI: USB input disabled by configuration\n");
    _inputReady = 1;
    IOLog("EHCI %s: %04x:%04x MMIO %x, %u ports, %s%s\n", EHCI_VERSION,
        pci.VendorID, pci.DeviceID, address, _state.maxPorts,
        mode == EHCI_MODE_INTX ? "INTx" : "Polling", mode == EHCI_MODE_INTX ? " shared IRQ" : " 5 ms");
    return self;
}
- failInitialization
{ return [self failInitializationAt:0]; }
- failInitializationAt:(unsigned)line
{
    IOLog("EHCI: initialization failed at line %u; MMIO %x/%u cap %u fatal %u lost %u\n",
        line, (unsigned)_mappedAddress, _mappedBytes, _state.operationalOffset,
        _state.fatal, _interruptState.mmioLost);
    [self free]; return nil;
}
- (void)interruptOccurred
{
    /* x86 locked admission precedes any blocking lock acquisition. Pinned
     * lifetime keeps both this token and queued callbacks valid until reboot. */
    __asm__ volatile("lock; incl (%0)" : : "r" (&_interruptState.admitted) : "memory", "cc");
    [_boundaryLock lock];
    EHCIInterruptCallback(&_interruptState);
    __asm__ volatile("lock; decl (%0)" : : "r" (&_interruptState.admitted) : "memory", "cc");
    [_boundaryLock unlock];
}
- (void)runRetryLoop
{
    for (;;) {
        [_boundaryLock lock];
        EHCIInterruptRetry(&_interruptState);
        /* Watchdog detects a stuck asserted source, never services transfers
         * or calls clients. INTx failure goes offline, never silently polls. */
        if (_startupReady)
            EHCIInterruptWatchdog(&_interruptState, EHCIPlatformMilliseconds());
        [_boundaryLock unlock];
        IOSleep(5);
    }
}
- (void)runCompletionLoop
{
    for (;;) {
        unsigned causes = 0, work = 0, fatal = 0;
        [_boundaryLock lock];
        if (_startupReady && !_interruptState.stopping) {
            if (_interruptState.mode == EHCI_MODE_POLLING) EHCIInterruptPoll(&_interruptState);
            work = _interruptState.work;
            if (work) causes = EHCIInterruptTakeWork(&_interruptState);
        }
        [_boundaryLock unlock];
        if (_startupReady) {
            [_eventLock lock];
            if (_interruptState.stopping) {
                EHCICoreFail(&_state, "interrupt stopped", _interruptState.mmioLost);
                [self wakeStorageWaiters];
            } else if (work || _state.rescan) EHCICoreService(&_state, causes);
            fatal = _state.fatal;
            if (fatal) [self wakeStorageWaiters];
            [_eventLock unlock];
            if (fatal) { [self containFatalController]; return; }
            [_boundaryLock lock];
            if (!_interruptState.stopping && !_interruptState.active)
                EHCIInterruptRestore(&_interruptState);
            [_boundaryLock unlock];
        }
        /* Register under both publication locks before blocking. A wake
         * after unlock cancels the registered wait, including before block.
         * The timeout keeps startup/retry progress; idle INTx does not poll. */
        if (_interruptState.mode == EHCI_MODE_POLLING) IOSleep(5);
        else EHCICompletionWait(self, &completionWaitOps);
    }
}
- (void)runManagementLoop
{
    for (;;) {
        unsigned fatal, stopping;
        [_eventLock lock];
        if (!_interruptState.stopping) {
            EHCICoreServicePorts(&_state);
            if (_ledUpdatePending) {
                unsigned leds = _pendingLEDs; _ledUpdatePending = 0;
                EHCICoreSetKeyboardLEDs(&_state, leds);
            }
        }
        fatal = _state.fatal; stopping = _interruptState.stopping;
        [_eventLock unlock];
        if (fatal) [self containFatalController];
        if (fatal || stopping) return;
        IOSleep(10);
    }
}
- (void)containFatalController
{
    unsigned i;
    [_boundaryLock lock];
    if (!_interruptState.stopping) EHCIInterruptStop(&_interruptState);
    else EHCIInterruptRetry(&_interruptState);
    [_boundaryLock unlock];
    [_eventLock lock];
    EHCICoreFail(&_state, "controller containment", _interruptState.mmioLost);
    [self wakeStorageWaiters];
    if (_contained) { [_eventLock unlock]; return; }
    _contained = 1;
    [_eventLock unlock];
    for (i = 0; i < 2; i++) {
        [_inputLocks[i] lock];
        EHCIInputQueueDiscardPending(&_inputQueues[i]); _releasePending[i] = 1;
        [_inputLocks[i] unlock];
    }
}
- (void)handleKeyboardReport:(const unsigned char *)report length:(unsigned)length
{
    [_inputLocks[0] lock];
    if (!EHCIInputQueueEnqueue(&_inputQueues[0], EHCI_INPUT_KEYBOARD, report, length, 0))
        _releasePending[0] = 1;
    [_inputLocks[0] unlock];
}
- (void)handlePointerReport:(const unsigned char *)report length:(unsigned)length
{
    [_inputLocks[1] lock];
    if (!EHCIInputQueueEnqueue(&_inputQueues[1], EHCI_INPUT_POINTER, report, length, 0))
        _releasePending[1] = 1;
    [_inputLocks[1] unlock];
}
- (void)runInputLoop:(unsigned)which
{
    EHCIInputEvent event;
    static const unsigned char released[8] = {0,0,0,0,0,0,0,0};
    for (;;) {
        int have = 0, release = 0;
        [_inputLocks[which] lock];
        if (_inputReady) {
            release = _releasePending[which]; _releasePending[which] = 0;
            if (release) EHCIInputQueueDiscardPending(&_inputQueues[which]);
            else have = EHCIInputQueueDequeue(&_inputQueues[which], &event);
        }
        [_inputLocks[which] unlock];
        if (release || have) {
            if (which) [_pointer consumeReport:release ? released : event.report length:release ? 3 : event.length];
            else [_keyboard consumeReport:release ? released : event.report length:release ? 8 : event.length];
        } else IOSleep(5);
    }
}
- (void)queueKeyboardLEDs:(unsigned char)leds
{ [_eventLock lock]; _pendingLEDs = leds; _ledUpdatePending = 1; [_eventLock unlock]; }
- free
{
    unsigned i;
    if (_boundaryLock && _pciOwned) {
        [_boundaryLock lock];
        EHCIInterruptStop(&_interruptState);
        [_boundaryLock unlock];
    }
    /* IOSCSIController, its I/O task, native providers and queued callbacks
     * have no usable final-drain API in this target. Never release their
     * object, BAR, locks or DMA after superclass publication. */
    if (_scsiThreadStarted || _workersStarted || _interruptState.interruptDebt || _interruptState.dmaDebt) {
        IOLog("EHCI: offline resources and retry worker retained until reboot\n");
        return self;
    }
    for (i = 0; i < 2; i++) [_inputLocks[i] free];
    for (i = 0; i < USB_STORAGE_TARGETS; i++) {
        [_storageLocks[i] free];
        if (_storageBuffers[i]) IOFree(_storageBuffers[i], USB_STORAGE_MAX_REQUEST);
    }
    [_eventLock free]; [_boundaryLock free];
    return [super free];
}
@end

ehci_u32 EHCIPlatformRead(EHCIControllerState *s, ehci_u32 offset)
{
    EHCIController *c = s->owner; ehci_u32 v;
    [c->_boundaryLock lock];
    v = c->_interruptState.stopping ? 0xffffffffU : rawRead(c, offset);
    if (v == 0xffffffffU && !c->_interruptState.mmioLost && !c->_interruptState.stopping)
        EHCIInterruptLost(&c->_interruptState);
    [c->_boundaryLock unlock]; return v;
}
int EHCIPlatformWrite(EHCIControllerState *s, ehci_u32 offset, ehci_u32 v)
{
    EHCIController *c = s->owner; int ok;
    [c->_boundaryLock lock];
    ok = !c->_interruptState.stopping && rawWrite(c, offset, v);
    [c->_boundaryLock unlock]; return ok;
}
int EHCIPlatformPCIRead(EHCIControllerState *s, unsigned offset, ehci_u32 *v)
{ EHCIController *c = s->owner; int ok; [c->_boundaryLock lock]; ok = pciRead(c, offset, v); [c->_boundaryLock unlock]; return ok; }
int EHCIPlatformPCIWrite(EHCIControllerState *s, unsigned offset, ehci_u32 v)
{ EHCIController *c = s->owner; int ok; [c->_boundaryLock lock]; ok = pciWrite(c, offset, v); [c->_boundaryLock unlock]; return ok; }
void *EHCIPlatformAllocate(ehci_u32 bytes) { return IOMalloc(bytes); }
void EHCIPlatformFree(void *p, ehci_u32 bytes) { IOFree(p, bytes); }
ehci_u32 EHCIPlatformPhysical(void *p)
{
    vm_address_t address = 0;
    if (IOPhysicalFromVirtual(IOVmTaskSelf(), (vm_address_t)p, &address) != IO_R_SUCCESS) return 0;
    return address;
}
ehci_u64 EHCIPlatformMilliseconds(void)
{ ns_time_t now; IOGetTimestamp(&now); return (ehci_u64)now / 1000000ULL; }
void EHCIPlatformPause(EHCIControllerState *s, unsigned ms)
{ EHCIController *c = s->owner; [c->_eventLock unlock]; IOSleep(ms); [c->_eventLock lock]; }
void EHCIPlatformDelay(EHCIControllerState *s, unsigned us)
{ (void)s; IODelay(us); }
void EHCIPlatformWake(void *c) { irqPublish(c); }
void EHCIPlatformKeyboardReport(void *c, const ehci_u8 *b, ehci_u32 n)
{ [(EHCIController *)c handleKeyboardReport:b length:n]; }
void EHCIPlatformPointerReport(void *c, const ehci_u8 *b, ehci_u32 n)
{ [(EHCIController *)c handlePointerReport:b length:n]; }
void EHCIPlatformStorageWake(void *c, unsigned t) { [(EHCIController *)c wakeStorageTarget:t]; }
void EHCIPlatformStorageAttached(void *c) { [(EHCIController *)c queueStorageProbe]; }
