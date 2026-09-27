#define MACH_USER_API 1
#import "EHCIController.h"
#import "EHCIVersion.h"
#import "USBStorageBuffer.h"
#import "USBStorageWait.h"
#import <mach/vm_param.h>
#import <driverkit/generalFuncs.h>
#import <driverkit/kernelDriver.h>
#import <kernserv/prototypes.h>
#import <string.h>
#import <stdio.h>
#import <objc/objc-runtime.h>

/* Native OPENSTEP scheduler exports; PCIMSI uses the same timed-wait API.
 * Scheduler calls run with ordinary interrupt state, never raw IF exclusion. */
extern int hz;
extern void thread_wakeup(int event);

static ehci_u64 storageNanoseconds(void)
{
    ns_time_t now;
    IOGetTimestamp(&now);
    return (ehci_u64)now;
}

typedef struct EHCIStorageWaitContext {
    NXLock *lock;
    EHCIControllerState *state;
    EHCIInterruptState *interrupt;
    unsigned target;
    ehci_u32 generation;
    ehci_u8 endpoint;
    void *buffer;
    ehci_u32 *actual;
    int event;
    ehci_u64 *servicedAt;
    ehci_u64 resumedAt;
    ehci_u64 copyNS;
} EHCIStorageWaitContext;

static void waitLock(void *context)
{ [((EHCIStorageWaitContext *)context)->lock lock]; }
static void waitUnlock(void *context)
{ [((EHCIStorageWaitContext *)context)->lock unlock]; }
static int waitResult(void *context)
{
    EHCIStorageWaitContext *w = (EHCIStorageWaitContext *)context;
    ehci_u64 before = 0;
    int result;
    if (w->interrupt->stopping) return USB_TRANSFER_DISCONNECTED;
    if (w->servicedAt) {
        before = storageNanoseconds();
        if (*w->servicedAt && !w->resumedAt) w->resumedAt = before;
    }
    result = EHCICoreStorageTransferResult(w->state, w->target, w->generation,
                                           w->endpoint, w->buffer, w->actual);
    if (w->servicedAt && result != USB_TRANSFER_PENDING)
        w->copyNS += storageNanoseconds() - before;
    return result;
}
static ehci_u64 waitMilliseconds(void *context)
{ (void)context; return EHCIPlatformMilliseconds(); }
static void waitPrepare(void *context, int ticks)
{
    EHCIStorageWaitContext *w = (EHCIStorageWaitContext *)context;
    assert_wait(w->event, FALSE);
    thread_set_timeout(ticks);
}
static void waitBlock(void *context)
{ (void)context; thread_block(); }
static const USBStorageWaitOps storageWaitOps = {
    waitLock, waitUnlock, waitResult, waitMilliseconds, waitPrepare, waitBlock
};

typedef struct EHCIStorageSession {
    EHCIController *controller;
    unsigned target;
    ehci_u32 generation;
} EHCIStorageSession;

static int storageControl(void *context, const USBSetupPacket *setup,
    void *buffer, ehci_u32 *actual, ehci_u64 deadline)
{
    EHCIStorageSession *s = (EHCIStorageSession *)context;
    return [s->controller storageTransfer:s->target generation:s->generation
        endpoint:0 setup:setup buffer:buffer length:setup->length actual:actual deadline:deadline];
}
static int storageBulk(void *context, ehci_u8 endpoint, void *buffer,
    ehci_u32 length, ehci_u32 *actual, ehci_u64 deadline)
{
    EHCIStorageSession *s = (EHCIStorageSession *)context;
    return [s->controller storageTransfer:s->target generation:s->generation
        endpoint:endpoint setup:0 buffer:buffer length:length actual:actual deadline:deadline];
}
static int storageClear(void *context, ehci_u8 endpoint, ehci_u64 deadline)
{
    EHCIStorageSession *s = (EHCIStorageSession *)context;
    return [s->controller storageClearHalt:s->target generation:s->generation
        endpoint:endpoint deadline:deadline];
}
static ehci_u64 storageClock(void *context)
{
    (void)context;
    return EHCIPlatformMilliseconds();
}
static const USBStorageTransport storageTransport = {
    storageControl, storageBulk, storageClear, storageClock
};

/* These callbacks operate on already-wired SCSIDisk/generic-client buffers. */
static int clientPhysical(void *context, ehci_u32 address, ehci_u32 *physical)
{
    return IOPhysicalFromVirtual((vm_task_t)context, address,
                                  (unsigned *)physical) == IO_R_SUCCESS;
}
static int clientMap(void *context, ehci_u32 physical, ehci_u32 length, void **mapped)
{
    (void)context;
    return IOMapPhysicalIntoIOTask(physical, length,
                                  (vm_address_t *)mapped) == IO_R_SUCCESS;
}
static void clientUnmap(void *context, void *mapped, ehci_u32 length)
{
    (void)context;
    IOUnmapPhysicalFromIOTask((vm_address_t)mapped, length);
}
static const USBStorageBufferOps clientBufferOps = { clientPhysical, clientMap, clientUnmap };

static sc_status_t scsiStatus(USBStorageResult *result, unsigned char *status,
                              esense_reply_t *sense, int autoSense)
{
    *status = 0;
    bzero(sense, sizeof(*sense));
    switch (result->status) {
    case USB_TRANSFER_OK: return SR_IOST_GOOD;
    case USB_STORAGE_CHECK:
        *status = 2;
        if (autoSense && result->senseValid) {
            unsigned n = sizeof(*sense);
            if (n > sizeof(result->sense)) n = sizeof(result->sense);
            bcopy(result->sense, sense, n);
            return SR_IOST_CHKSV;
        }
        return SR_IOST_CHKSNV;
    case USB_TRANSFER_TIMEOUT: return SR_IOST_IOTO;
    case USB_TRANSFER_DISCONNECTED: return SR_IOST_SELTO;
    case USB_STORAGE_INVALID: return SR_IOST_INVALID;
    default: return SR_IOST_INT;
    }
}

static void storageProbeThread(void *context)
{
    [(EHCIController *)context runStorageProbeLoop];
    IOExitThread();
}

@implementation EHCIController (Storage)

- (BOOL)startStorageProbeWorker
{
    _storageDiskClass = objc_lookUpClass("SCSIDisk");
    if (!_storageDiskClass || ![_storageDiskClass respondsTo:@selector(probe:)]) {
        IOLog("EHCI: SCSIDisk probe is unavailable\n");
        return NO;
    }
    _storageProbeDescription = [[IODeviceDescription alloc] init];
    if (!_storageProbeDescription) return NO;
    [_storageProbeDescription setDirectDevice:self];
    /* Initial enumeration is complete; registerDevice will scan those targets.
     * Only subsequent attachments need the worker's additional scan. */
    [_eventLock lock];
    _storageProbePending = NO;
    _storageProbeRunning = YES;
    [_eventLock unlock];
    if (!IOForkThread(storageProbeThread, self)) {
        _storageProbeRunning = NO;
        IOLog("EHCI: cannot start storage probe worker\n");
        return NO;
    }
    return YES;
}

- (void)finishStorageRegistration
{
    [_eventLock lock];
    /* registerDevice synchronously runs the initial SCSIDisk probe. Only
     * then may the worker probe, including an attachment during that scan. */
    _storageRegistrationReady = YES;
    if (_storageProbePending) thread_wakeup((int)&_storageProbeEvent);
    [_eventLock unlock];
}

/* Called with the event lock held, or during single-threaded enumeration.
 * The callback never allocates or invokes the disk class on the I/O task. */
- (void)queueStorageProbe
{
    _storageProbePending = YES;
    if (_storageRegistrationReady)
        thread_wakeup((int)&_storageProbeEvent);
}

- (void)runStorageProbeLoop
{
    [_eventLock lock];
    while (!_interruptState.stopping) {
        if (!_storageRegistrationReady || !_storageProbePending) {
            assert_wait((int)&_storageProbeEvent, FALSE);
            [_eventLock unlock];
            thread_block();
            [_eventLock lock];
            continue;
        }
        _storageProbePending = NO;
        [_eventLock unlock];
        /* SCSIDisk reserves each target before probing and skips targets
         * already owned by a disk. It may issue synchronous USB I/O here. */
        [_storageDiskClass probe:_storageProbeDescription];
        [_eventLock lock];
        /* An attachment during the scan leaves pending set for another pass. */
    }
    _storageProbeRunning = NO;
    [_eventLock unlock];
}

- (void)waitForStorageProbeExit
{
    BOOL running;
    if (!_eventLock) return;
    do {
        [_eventLock lock];
        running = _storageProbeRunning;
        [_eventLock unlock];
        if (running) IOSleep(1);
    } while (running);
}

/* Called with _eventLock held. The event addresses live with the pinned
 * controller, and per-target serialization permits only one waiter each. */
- (void)wakeStorageTarget:(unsigned)target
{
    if (target < USB_STORAGE_TARGETS) {
        if (_storageTransferActive[target] && !_storageServicedAt[target])
            _storageServicedAt[target] = storageNanoseconds();
        thread_wakeup((int)&_storageWaitEvents[target]);
    }
}
- (void)wakeStorageWaiters
{
    unsigned target;
    thread_wakeup((int)&_storageProbeEvent);
    for (target = 0; target < USB_STORAGE_TARGETS; target++)
        [self wakeStorageTarget:target];
}

- (unsigned)maxTransfer { return USB_STORAGE_MAX_REQUEST; }
- (int)numberOfTargets { return USB_STORAGE_TARGETS; }
- (void)getDMAAlignment:(IODMAAlignment *)alignment
{
    alignment->readStart = alignment->writeStart = 1;
    alignment->readLength = alignment->writeLength = 1;
}

- (int)storageTransfer:(unsigned)target generation:(ehci_u32)generation
              endpoint:(ehci_u8)endpoint setup:(const USBSetupPacket *)setup
                buffer:(void *)buffer length:(ehci_u32)length
                actual:(ehci_u32 *)actual deadline:(ehci_u64)deadline
{
    int rc, recovery;
    EHCIStorageWaitContext wait;
    ehci_u64 start = 0, submitted = 0, finished, serviced;
    unsigned stage;
    BOOL profile;
    *actual = 0;
    if (target >= USB_STORAGE_TARGETS) return USB_TRANSFER_DISCONNECTED;
    if (EHCIPlatformMilliseconds() >= deadline) return USB_TRANSFER_TIMEOUT;
    [_eventLock lock];
    profile = _storageProfileActive[target];
    if (profile) {
        start = storageNanoseconds();
        _storageServicedAt[target] = 0;
        _storageTransferActive[target] = YES;
    }
    if (_interruptState.stopping || _state.fatal) rc = USB_TRANSFER_DISCONNECTED;
    else if (setup) rc = EHCICoreStorageControlStart(&_state, target, generation, setup, buffer);
    else rc = EHCICoreStorageBulkStart(&_state, target, generation, endpoint, buffer, length);
    if (profile) {
        submitted = storageNanoseconds();
        if (rc) _storageTransferActive[target] = NO;
    }
    [_eventLock unlock];
    if (rc) return rc;
    wait.lock = _eventLock; wait.state = &_state; wait.interrupt = &_interruptState;
    wait.target = target; wait.generation = generation; wait.endpoint = endpoint;
    wait.buffer = buffer; wait.actual = actual;
    wait.event = (int)&_storageWaitEvents[target];
    wait.servicedAt = profile ? &_storageServicedAt[target] : 0;
    wait.resumedAt = wait.copyNS = 0;
    /* Register while holding _eventLock, release it, then block. A completion
     * between unlock and block cancels the registered wait, so no wake is lost.
     * The reader never polls the hardware event ring. */
    rc = USBStorageWait(&wait, &storageWaitOps, deadline, (unsigned)hz);
    if (profile) {
        finished = storageNanoseconds();
        stage = setup ? EHCI_STAGE_CONTROL :
            (!(endpoint & 0x80) && length == 31 ? EHCI_STAGE_CBW :
             ((endpoint & 0x80) && length == 13 ? EHCI_STAGE_CSW : EHCI_STAGE_DATA));
        stage = EHCI_STAT_STAGE_BASE + stage * EHCI_STAGE_METRICS;
        [_eventLock lock];
        serviced = _storageServicedAt[target];
        _storageTransferActive[target] = NO;
        _storageMetrics[stage + EHCI_STAGE_COUNT]++;
        if (rc) _storageMetrics[stage + EHCI_STAGE_ERRORS]++;
        _storageMetrics[stage + EHCI_STAGE_PREPARE_NS] += submitted - start;
        if (serviced >= submitted && serviced)
            _storageMetrics[stage + EHCI_STAGE_SERVICE_NS] += serviced - submitted;
        if (wait.resumedAt >= serviced && serviced)
            _storageMetrics[stage + EHCI_STAGE_RESUME_NS] += wait.resumedAt - serviced;
        _storageMetrics[stage + EHCI_STAGE_COPY_NS] += wait.copyNS;
        _storageMetrics[stage + EHCI_STAGE_TOTAL_NS] += finished - start;
        [_eventLock unlock];
    }
    /* EP0 stalls are expected for GET MAX LUN. Reset the host endpoint without
     * marking the USB device for reprobe or clearing a non-existent EP0 halt. */
    if (rc == USB_TRANSFER_TIMEOUT || (!endpoint && rc == USB_TRANSFER_STALL)) {
        ehci_u64 recoveryDeadline = deadline;
        if (rc == USB_TRANSFER_TIMEOUT) recoveryDeadline = deadline + 5000;
        [_eventLock lock];
        recovery = EHCICoreStorageRecoverEndpoint(&_state, target, generation,
                                                    endpoint, recoveryDeadline);
        [_eventLock unlock];
        if (recovery && rc == USB_TRANSFER_STALL) rc = recovery;
    } else if (rc && rc != USB_TRANSFER_DISCONNECTED) {
        /* Retire failed split work before BOT reset/clear-halt can reuse a
         * shared TT buffer. Preserve the halt/toggle until USB recovery. */
        [_eventLock lock];
        recovery = EHCICoreStorageRetireEndpoint(&_state, target, generation,
                         endpoint, EHCIPlatformMilliseconds() + 5000);
        [_eventLock unlock];
        if (recovery) rc = recovery;
    }
    if (_state.fatal) [self containFatalController];
    return rc;
}

- (int)storageClearHalt:(unsigned)target generation:(ehci_u32)generation
              endpoint:(ehci_u8)endpoint deadline:(ehci_u64)deadline
{
    USBSetupPacket setup;
    ehci_u32 actual;
    int rc;
    setup.requestType = 2; setup.request = 1; /* CLEAR_FEATURE(ENDPOINT_HALT) */
    setup.value = 0; setup.index = endpoint; setup.length = 0;
    rc = [self storageTransfer:target generation:generation endpoint:0 setup:&setup
        buffer:0 length:0 actual:&actual deadline:deadline];
    if (!rc) {
        [_eventLock lock];
        rc = EHCICoreStorageRecoverEndpoint(&_state, target, generation, endpoint, deadline);
        [_eventLock unlock];
    }
    if (_state.fatal) [self containFatalController];
    return rc;
}

- (USBStorageResult)storageExecute:(unsigned)target cdb:(const ehci_u8 *)cdb
                           length:(unsigned)cdbLength buffer:(void *)buffer
                           client:(vm_task_t)client maximum:(unsigned)maximum
                             read:(int)read autoSense:(int)autoSense
                         deadline:(ehci_u64)deadline
{
    USBStorageResult result;
    EHCIStorageSession session;
    USBStorageBuffer pages;
    EHCIDevice *device;
    USBMassStorage *bot;
    EHCIStorageBinding binding;
    int present;
    BOOL profile;
    ehci_u64 started, locked, stamp, mapNS = 0, copyNS = 0, unmapNS = 0;
    bzero(&result, sizeof(result)); result.status = USB_STORAGE_INVALID;
    if (target >= USB_STORAGE_TARGETS || maximum > USB_STORAGE_MAX_REQUEST ||
        !_storageLocks[target]) return result;
    started = storageNanoseconds();
    [_storageLocks[target] lock];
    [_eventLock lock];
    locked = storageNanoseconds();
    _storageActiveRequests++;
    profile = _storageProfileEnabled && cdbLength &&
        (cdb[0] == 0x08 || cdb[0] == 0x28 || cdb[0] == 0xa8 || cdb[0] == 0x88);
    _storageProfileActive[target] = profile;
    binding = _state.storage[target];
    device = EHCICoreStorageDevice(&_state, target, binding.generation);
    present = device != 0 && !_interruptState.stopping;
    bot = &_storageBOT[target];
    if (_storageDisks[target].generation != binding.generation) bot->unusable = 0;
    if (device) bot->interface = device->storageInterface;
    USBStorageSCSIObserve(&_storageDisks[target], binding.generation, binding.removals, present);
    [_eventLock unlock];
    stamp = profile ? storageNanoseconds() : 0;
    if (!USBStorageBufferMap(&pages, &clientBufferOps, (void *)client,
            page_size, buffer, maximum, client == IOVmTaskSelf())) {
        if (profile) mapNS = storageNanoseconds() - stamp;
        goto out;
    }
    if (profile) mapNS = storageNanoseconds() - stamp;
    session.controller = self; session.target = target; session.generation = binding.generation;
    bot->transport = &storageTransport; bot->context = &session;
    result = USBStorageSCSIExecute(&_storageDisks[target], bot, cdb, cdbLength,
        _storageBuffers[target], maximum, read, autoSense, deadline);
    [_eventLock lock];
    /* Even a command that finished before unplug may not publish its data to
     * a caller after the attachment generation has changed. */
    if (_state.storage[target].generation != binding.generation ||
        (present && (!EHCICoreStorageDevice(&_state, target, binding.generation) ||
                     _interruptState.stopping))) {
        result.status = USB_TRANSFER_DISCONNECTED; result.actual = 0;
    }
    stamp = profile ? storageNanoseconds() : 0;
    if (!result.status && result.actual <= maximum)
        USBStorageBufferCopy(&pages, _storageBuffers[target], (unsigned)result.actual);
    else if (result.actual > maximum) { result.status = USB_TRANSFER_ERROR; result.actual = 0; }
    if (profile) copyNS = storageNanoseconds() - stamp;
    if (bot->unusable) EHCICoreStorageOffline(&_state, target, binding.generation);
    [_eventLock unlock];
    bot->context = 0;
    stamp = profile ? storageNanoseconds() : 0;
    USBStorageBufferUnmap(&pages);
    if (profile) unmapNS = storageNanoseconds() - stamp;
out:
    [_eventLock lock];
    if (profile) {
        _storageMetrics[EHCI_STAT_READS]++;
        if (result.status) _storageMetrics[EHCI_STAT_ERRORS]++;
        else _storageMetrics[EHCI_STAT_BYTES] += result.actual;
        _storageMetrics[EHCI_STAT_REQUEST_NS] += storageNanoseconds() - started;
        _storageMetrics[EHCI_STAT_LOCK_NS] += locked - started;
        _storageMetrics[EHCI_STAT_MAP_NS] += mapNS;
        _storageMetrics[EHCI_STAT_CLIENT_COPY_NS] += copyNS;
        _storageMetrics[EHCI_STAT_UNMAP_NS] += unmapNS;
    }
    _storageProfileActive[target] = NO;
    _storageActiveRequests--;
    [_eventLock unlock];
    [_storageLocks[target] unlock];
    if (_state.fatal) [self containFatalController];
    return result;
}

- (sc_status_t)executeRequest:(IOSCSIRequest *)request buffer:(void *)buffer client:(vm_task_t)client
{
    USBStorageResult result;
    ns_time_t start, end;
    unsigned n;
    if (!request) return SR_IOST_INVALID;
    IOGetTimestamp(&start);
    bzero(&result, sizeof(result)); result.status = USB_STORAGE_INVALID;
    n = request->cdbLength ? request->cdbLength : USBStorageCDBLength(((ehci_u8 *)&request->cdb)[0]);
    if (request->target >= USB_STORAGE_TARGETS || request->lun) result.status = USB_TRANSFER_DISCONNECTED;
    else if (n <= sizeof(request->cdb) && request->maxTransfer >= 0)
        result = [self storageExecute:request->target cdb:(ehci_u8 *)&request->cdb length:n
            buffer:buffer client:client maximum:request->maxTransfer read:request->read
            autoSense:!request->ignoreChkcond deadline:EHCIPlatformMilliseconds() +
                (ehci_u64)(request->timeoutLength > 0 ? request->timeoutLength : 5) * 1000];
    request->driverStatus = scsiStatus(&result, &request->scsiStatus, &request->senseData,
                                       !request->ignoreChkcond);
    request->bytesTransferred = result.status == USB_TRANSFER_OK ? (int)result.actual : 0;
    IOGetTimestamp(&end); request->totalTime = end - start; request->latentTime = 0;
    return request->driverStatus;
}

- (sc_status_t)executeSCSI3Request:(IOSCSI3Request *)request buffer:(void *)buffer client:(vm_task_t)client
{
    USBStorageResult result;
    ns_time_t start, end;
    unsigned n;
    if (!request) return SR_IOST_INVALID;
    IOGetTimestamp(&start);
    bzero(&result, sizeof(result)); result.status = USB_STORAGE_INVALID;
    n = request->cdbLength ? request->cdbLength : USBStorageCDBLength(((ehci_u8 *)&request->cdb)[0]);
    if (request->target >= USB_STORAGE_TARGETS || request->lun) result.status = USB_TRANSFER_DISCONNECTED;
    else if (n <= sizeof(request->cdb) && request->maxTransfer >= 0)
        result = [self storageExecute:(unsigned)request->target cdb:(ehci_u8 *)&request->cdb length:n
            buffer:buffer client:client maximum:request->maxTransfer read:request->read
            autoSense:1 deadline:EHCIPlatformMilliseconds() +
                (ehci_u64)(request->timeoutLength > 0 ? request->timeoutLength : 5) * 1000];
    request->driverStatus = scsiStatus(&result, &request->scsiStatus, &request->senseData, 1);
    request->bytesTransferred = result.status == USB_TRANSFER_OK ? (int)result.actual : 0;
    IOGetTimestamp(&end); request->totalTime = end - start; request->latentTime = 0;
    return request->driverStatus;
}

- (sc_status_t)resetSCSIBus
{
    unsigned target;
    int failed = 0;
    ehci_u64 deadline = EHCIPlatformMilliseconds() + 5000;
    [_eventLock lock];
    _storageActiveRequests++;
    [_eventLock unlock];
    for (target = 0; target < USB_STORAGE_TARGETS; target++) {
        EHCIStorageSession session;
        EHCIDevice *device;
        USBMassStorage *bot = &_storageBOT[target];
        [_storageLocks[target] lock];
        [_eventLock lock];
        session.controller = self; session.target = target;
        session.generation = _state.storage[target].generation;
        device = EHCICoreStorageDevice(&_state, target, session.generation);
        if (device) bot->interface = device->storageInterface;
        [_eventLock unlock];
        if (device) {
            bot->transport = &storageTransport; bot->context = &session;
            if (USBMassStorageReset(bot, deadline)) {
                failed = 1;
                [_eventLock lock];
                EHCICoreStorageOffline(&_state, target, session.generation);
                [_eventLock unlock];
            }
            bot->context = 0;
            _storageDisks[target].capacityValid = 0;
            _storageDisks[target].attention = 1;
        }
        [_storageLocks[target] unlock];
    }
    [_eventLock lock];
    _storageActiveRequests--;
    [_eventLock unlock];
    return failed ? SR_IOST_INT : SR_IOST_GOOD;
}

- (IOReturn)getIntValues:(unsigned *)values forParameter:(IOParameterName)parameter
                  count:(unsigned *)count
{
    unsigned i;
    if (strcmp(parameter, EHCI_STORAGE_STATS_PARAMETER))
        return [super getIntValues:values forParameter:parameter count:count];
    if (*count < EHCI_STORAGE_STATS_WORDS) {
        *count = EHCI_STORAGE_STATS_WORDS;
        return IO_R_INVALID_ARG;
    }
    [_eventLock lock];
    values[0] = EHCI_STORAGE_STATS_VERSION;
    values[1] = _state.interruptModerationUS;
    values[2] = _storageProfileEnabled;
    values[3] = _storageActiveRequests;
    values[4] = _interruptState.callbacks;
    values[5] = _interruptState.failures;
    values[6] = _state.keyboardReports;
    values[7] = _state.mouseReports;
    values[8] = _state.scheduleChanges;
    values[9] = _interruptState.services;
    for (i = 0; i < EHCI_STORAGE_METRICS; i++) {
        values[EHCI_STORAGE_STATS_HEADER + 2*i] = (unsigned)_storageMetrics[i];
        values[EHCI_STORAGE_STATS_HEADER + 2*i + 1] = (unsigned)(_storageMetrics[i] >> 32);
    }
    [_eventLock unlock];
    *count = EHCI_STORAGE_STATS_WORDS;
    return IO_R_SUCCESS;
}

- (IOReturn)setIntValues:(unsigned *)values forParameter:(IOParameterName)parameter
                  count:(unsigned)count
{
    IOReturn result = IO_R_SUCCESS;
    BOOL moderation = !strcmp(parameter, EHCI_IMOD_PARAMETER);
    if (!moderation && strcmp(parameter, EHCI_STORAGE_PROFILE_PARAMETER))
        return [super setIntValues:values forParameter:parameter count:count];
    if (count != 1 || (moderation ? values[0] != 125 : values[0] > 2))
        return IO_R_INVALID_ARG;
    [_eventLock lock];
    /* Do not wait for target locks from a DriverKit parameter callback: that
     * could block the same I/O task that must deliver storage completions. */
    if (_interruptState.stopping || _state.fatal) result = IO_R_NO_DEVICE;
    else if (_storageActiveRequests) result = IO_R_BUSY;
    else if (moderation) {
        /* Interrupt moderation uses one microframe (125 us), fixed in USBCMD.ITC. */
    } else {
        /* 0 disables, 1 enables, 2 resets totals without changing enablement. */
        if (values[0] == 2) bzero(_storageMetrics, sizeof(_storageMetrics));
        else _storageProfileEnabled = values[0] ? YES : NO;
    }
    [_eventLock unlock];
    if (_state.fatal) [self containFatalController];
    return result;
}

- (IOReturn)getCharValues:(unsigned char *)values forParameter:(IOParameterName)parameter
                   count:(unsigned *)count
{
    unsigned slot;
    for (slot = 0; slot < EHCI_MAX_DEVICES; slot++) {
        char name[32];
        sprintf(name, "EHCIDeviceStatus%u", slot);
        if (!strcmp(parameter, name)) {
            char report[512];
            unsigned n;
            EHCIDevice *d = &_state.devices[slot];
            EHCIEndpoint *e = &d->endpoints[EHCI_EP_INTERRUPT];
            [_eventLock lock];
            if (!d->used) strcpy(report, "unused");
            else sprintf(report, "address=%u ready=%u removing=%u speed=%u root=%u parent=%u port=%u hub=%u hid=%u storage=%u irq_waiting=%u irq_halted=%u",
                d->address, d->ready, d->disconnecting, d->speed, d->port,
                d->parent, d->parentPort, d->hub, d->protocol, d->storage,
                e->waiting, e->halted);
            [_eventLock unlock];
            n = strlen(report) + 1;
            if (*count < n) { *count = n; return IO_R_INVALID_ARG; }
            bcopy(report, values, n); *count = n;
            return IO_R_SUCCESS;
        }
    }
    if (!strcmp(parameter, "EHCIRuntimeState")) {
        char report[512];
        unsigned n;
        [_eventLock lock];
        sprintf(report, "fatal=%u reason=%s detail=%x cmd=%x sts=%x timeouts=%u errors=%u wait=%x/%x/%x/%x",
            _state.fatal, _state.faultReason ? _state.faultReason : "none",
            _state.faultDetail, _state.lastCommand, _state.lastStatus,
            _state.timeouts, _state.errors, _state.waitOffset, _state.waitMask,
            _state.waitExpected, _state.waitObserved);
        [_eventLock unlock];
        n = strlen(report) + 1;
        if (*count < n) { *count = n; return IO_R_INVALID_ARG; }
        bcopy(report, values, n); *count = n;
        return IO_R_SUCCESS;
    }
    if (!strcmp(parameter, "EHCIEnumerationFailure")) {
        char report[256];
        unsigned n;
        EHCIEnumerationFailure failure;
        [_eventLock lock]; failure = _state.enumerationFailure; [_eventLock unlock];
        if (failure.stage)
            sprintf(report, "stage=%s root=%u hub_port=%u vid=%04x pid=%04x speed=%u hid=%u packet=%u interval=%u",
                failure.stage, failure.rootPort, failure.hubPort, failure.vendor,
                failure.product, failure.speed, failure.protocol, failure.packet, failure.interval);
        else strcpy(report, "none");
        n = strlen(report) + 1;
        if (*count < n) { *count = n; return IO_R_INVALID_ARG; }
        bcopy(report, values, n); *count = n;
        return IO_R_SUCCESS;
    }
    if (!strcmp(parameter, "EHCIDriverVersion")) {
        unsigned n = strlen(EHCI_VERSION) + 1;
        if (*count < n) { *count = n; return IO_R_INVALID_ARG; }
        bcopy(EHCI_VERSION, values, n); *count = n; return IO_R_SUCCESS;
    }
    return [super getCharValues:values forParameter:parameter count:count];
}
@end
