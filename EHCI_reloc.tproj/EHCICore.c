/*
 * EHCI 1.0 transport for OPENSTEP. Original implementation, BSD-2-Clause.
 * The USB class clients are privately adapted from the adjacent XHCI driver.
 * Controller-state lock required; PlatformPause releases/reacquires it.
 */
#include "EHCICore.h"
#include "EHCIMemory.h"

#define TD_OFFSET 128U
#define SETUP_OFFSET (TD_OFFSET + EHCI_TD_COUNT * sizeof(EHCIqTD))
#define NO_TD 0xffffffffU

void EHCICoreFail(EHCIControllerState *c, const char *reason, unsigned detail)
{
    if (!c->faultReason) {
        c->faultReason = reason; c->faultDetail = detail;
        EHCIPlatformLog("EHCI: offline: %s (%x), cmd=%x sts=%x\n",
            reason, detail, c->lastCommand, c->lastStatus);
    }
    c->fatal = 1;
}

static ehci_u32 read_op(EHCIControllerState *c, unsigned offset)
{
    ehci_u32 v;
    if (c->fatal) return 0xffffffffU;
    v = EHCIPlatformRead(c, c->operationalOffset + offset);
    if (offset == EHCI_CMD) c->lastCommand = v;
    if (offset == EHCI_STS) c->lastStatus = v;
    if (v == 0xffffffffU) EHCICoreFail(c, "MMIO read", offset);
    return v;
}
static int write_op(EHCIControllerState *c, unsigned offset, ehci_u32 v)
{
    if (c->fatal) return 0;
    if (!EHCIPlatformWrite(c, c->operationalOffset + offset, v)) {
        EHCICoreFail(c, "MMIO write", offset);
        return 0;
    }
    if (offset == EHCI_CMD) c->lastCommand = v;
    return 1;
}
static void barrier(void) { __asm__ volatile("" : : : "memory"); }

static int wait_bits(EHCIControllerState *c, unsigned offset,
                     unsigned mask, unsigned expected, unsigned timeout)
{
    ehci_u64 end = EHCIPlatformMilliseconds() + timeout;
    ehci_u32 v = 0;
    /* Only schedule-status handshakes get a bounded fast path. Port reset,
     * controller reset/halt and software-owner waits still yield normally. */
    unsigned spin = offset == EHCI_STS && mask &&
        !(mask & ~(EHCI_STS_PSS | EHCI_STS_ASS)) ? 1000 : 0;
    for (;;) {
        v = read_op(c, offset);
        if (c->fatal) return 0;
        if ((v & mask) == expected) return 1;
        /* IOSleep/reacquiring the state lock can exceed the deadline during
         * boot. Always observe the hardware after waking before timing out. */
        if (EHCIPlatformMilliseconds() >= end) break;
        if (spin) { EHCIPlatformDelay(c, 10); spin -= 10; }
        else EHCIPlatformPause(c, 1);
    }
    c->waitOffset = offset; c->waitMask = mask;
    c->waitExpected = expected; c->waitObserved = v;
    c->timeouts++;
    return 0;
}

static int dma_alloc(EHCIDMA *d, unsigned bytes)
{
    unsigned long aligned;
    if (bytes > EHCI_PAGE_SIZE) return 0;
    bzero(d, sizeof(*d));
    d->allocationBytes = bytes + EHCI_PAGE_SIZE;
    d->allocation = EHCIPlatformAllocate(d->allocationBytes);
    if (!d->allocation) return 0;
    aligned = ((unsigned long)d->allocation + EHCI_PAGE_SIZE - 1) &
              ~(unsigned long)(EHCI_PAGE_SIZE - 1);
    d->virtualAddress = (void *)aligned;
    d->physicalAddress = EHCIPlatformPhysical(d->virtualAddress);
    d->bytes = bytes;
    if (!d->physicalAddress || (d->physicalAddress & (EHCI_PAGE_SIZE - 1)) ||
        EHCIPlatformPhysical((ehci_u8 *)d->virtualAddress + bytes - 1) !=
            d->physicalAddress + bytes - 1) {
        EHCIPlatformFree(d->allocation, d->allocationBytes);
        bzero(d, sizeof(*d));
        return 0;
    }
    bzero(d->virtualAddress, bytes);
    return 1;
}
static void dma_free(EHCIDMA *d)
{
    if (d->allocation) EHCIPlatformFree(d->allocation, d->allocationBytes);
    bzero(d, sizeof(*d));
}
static void endpoint_free(EHCIEndpoint *e)
{
    unsigned i;
    for (i = 0; i < EHCI_DATA_PAGES; i++) dma_free(&e->data[i]);
    dma_free(&e->descriptors);
    bzero(e, sizeof(*e));
}

int EHCICoreSupportsPCIClass(ehci_u32 code) { return code == 0x0c0320U; }
ehci_u32 EHCIPortWriteValue(ehci_u32 v, ehci_u32 set,
                           ehci_u32 clear, ehci_u32 changes)
{
    /* Never accidentally clear CSC/PEC/OCC from a read-modify-write. */
    return ((v & ~(EHCI_PORT_W1C | clear)) | set) | (changes & EHCI_PORT_W1C);
}

static int legacy_handoff(EHCIControllerState *c, unsigned first)
{
    unsigned seen[8], offset = first, steps = 0;
    bzero(seen, sizeof(seen));
    while (offset) {
        ehci_u32 v;
        ehci_u64 end;
        if (offset < 0x40 || offset > 0xf8 || (offset & 3) || ++steps > 48 ||
            (seen[offset / 32] & (1U << (offset % 32)))) return 0;
        seen[offset / 32] |= 1U << (offset % 32);
        if (!EHCIPlatformPCIRead(c, offset, &v) || v == 0xffffffffU) return 0;
        if ((v & 255) == 1) {
            if (!EHCIPlatformPCIWrite(c, offset, v | (1U << 24))) return 0;
            end = EHCIPlatformMilliseconds() + 1000;
            do {
                if (!EHCIPlatformPCIRead(c, offset, &v) || v == 0xffffffffU) return 0;
                if (!(v & (1U << 16)) && (v & (1U << 24))) break;
                EHCIPlatformPause(c, 1);
            } while (EHCIPlatformMilliseconds() < end);
            if ((v & (1U << 16)) || !(v & (1U << 24))) return 0;
            /* Disable SMI enables, acknowledge only the documented statuses. */
            if (!EHCIPlatformPCIRead(c, offset + 4, &v) || v == 0xffffffffU ||
                !EHCIPlatformPCIWrite(c, offset + 4, v & 0xe0110000U) ||
                !EHCIPlatformPCIRead(c, offset + 4, &v) || v == 0xffffffffU ||
                (v & 0xe011U)) return 0;
            return 1;
        }
        offset = (v >> 8) & 255;
    }
    return 1;
}

int EHCICoreInitialize(EHCIControllerState *c)
{
    ehci_u32 cap, params, caps;
    unsigned i;
    if (!c || c->mmioBytes < 0x60) return 0;
    cap = EHCIPlatformRead(c, 0);
    params = EHCIPlatformRead(c, EHCI_CAP_PARAMS);
    caps = EHCIPlatformRead(c, EHCI_CAP_CAPS);
    if (cap == 0xffffffffU || params == 0xffffffffU || caps == 0xffffffffU ||
        (cap & 255) < 0x10 || ((cap & 255) & 3) || (cap >> 16) < 0x100)
        return 0;
    c->operationalOffset = cap & 255;
    c->maxPorts = params & 15;
    if (!c->maxPorts || c->operationalOffset + EHCI_PORT(c->maxPorts) + 4 > c->mmioBytes)
        return 0;
    c->hasPortPowerControl = (params >> 4) & 1;
    c->companions = (params >> 12) & 15;
    c->has64Bit = caps & 1;
    c->registersValid = 1;
    if (!legacy_handoff(c, (caps >> 8) & 255)) return 0;
    if (!write_op(c, EHCI_INTR, 0) || !write_op(c, EHCI_CMD, 0) ||
        !wait_bits(c, EHCI_STS, EHCI_STS_HALTED, EHCI_STS_HALTED, 100)) return 0;
    c->haltConfirmed = 1;
    if (!write_op(c, EHCI_CMD, EHCI_CMD_RESET) ||
        !wait_bits(c, EHCI_CMD, EHCI_CMD_RESET, 0, 100)) return 0;
    if (!dma_alloc(&c->asyncHead, sizeof(EHCIQH)) ||
        !dma_alloc(&c->frameList, EHCI_PAGE_SIZE)) return 0;
    {
        EHCIQH *head = (EHCIQH *)c->asyncHead.virtualAddress;
        head->link = c->asyncHead.physicalAddress | EHCI_LINK_QH;
        head->endpoint = EHCI_QH_HEAD | (EHCI_SPEED_HIGH << 12);
        head->next = head->alternate = EHCI_LINK_END;
        head->token = EHCI_QTD_HALTED;
    }
    for (i = 0; i < EHCI_FRAME_COUNT; i++)
        ((ehci_u32 *)c->frameList.virtualAddress)[i] = EHCI_LINK_END;
    if (c->has64Bit && !write_op(c, EHCI_SEGMENT, 0)) return 0;
    if (!write_op(c, EHCI_PERIODIC, c->frameList.physicalAddress) ||
        !write_op(c, EHCI_ASYNC, c->asyncHead.physicalAddress) ||
        !write_op(c, EHCI_STS, EHCI_STS_W1C)) return 0;
    if (read_op(c, EHCI_PERIODIC) != c->frameList.physicalAddress ||
        read_op(c, EHCI_ASYNC) != c->asyncHead.physicalAddress) return 0;
    c->addresses[0] = 1; c->nextGeneration = 1;
    c->keyboardSlot = c->pointerSlot = EHCI_MAX_DEVICES;
    c->interruptModerationUS = 125;
    for (i = 0; i < USB_STORAGE_TARGETS; i++) c->storage[i].generation = 1;
    c->initialized = 1;
    return !c->fatal;
}

int EHCICoreStart(EHCIControllerState *c)
{
    unsigned p;
    if (!c->initialized || c->fatal) return 0;
    /* Native glue opens bus mastering only after initialization succeeds. */
    barrier(); c->dmaArmed = 1; c->haltConfirmed = 0;
    if (!write_op(c, EHCI_CMD, EHCI_CMD_RUN | EHCI_CMD_ITC) ||
        !wait_bits(c, EHCI_STS, EHCI_STS_HALTED, 0, 100) ||
        !write_op(c, EHCI_CONFIG, 1) || read_op(c, EHCI_CONFIG) != 1) return 0;
    c->running = 1;
    for (p = 1; p <= c->maxPorts; p++) {
        ehci_u32 v = read_op(c, EHCI_PORT(p));
        if (c->fatal) return 0;
        if (c->hasPortPowerControl &&
            !write_op(c, EHCI_PORT(p), EHCIPortWriteValue(v, EHCI_PORT_POWER, 0, 0))) return 0;
        c->portChanges |= 1U << p;
    }
    EHCIPlatformPause(c, 20);
    /* A boot disk cannot be diagnosed with runtime status tools. Record
     * the post-handoff port state once, before initial enumeration. */
    for (p = 1; p <= c->maxPorts && !c->fatal; p++) {
        ehci_u32 v = read_op(c, EHCI_PORT(p));
        if (c->fatal) break;
        EHCIPlatformLog("EHCI: initial port %u PORTSC=%08x connected=%u enabled=%u owner=%u power=%u\n",
            p, v, !!(v & EHCI_PORT_CONNECT), !!(v & EHCI_PORT_ENABLE),
            !!(v & EHCI_PORT_OWNER), !!(v & EHCI_PORT_POWER));
    }
    return !c->fatal;
}

/* Each schedule mutation has a unique owner even across sleep/unlock.
 * Completion service merely requests another scan while this owner exists. */
static int schedule_enter(EHCIControllerState *c, unsigned bits)
{
    ehci_u64 end = EHCIPlatformMilliseconds() + EHCI_TIMEOUT_MS;
    ehci_u32 cmd;
    while (c->scheduleBusy && !c->fatal) {
        if (EHCIPlatformMilliseconds() >= end) return 0;
        EHCIPlatformPause(c, 1);
    }
    if (c->fatal) return 0;
    c->scheduleBusy = 1;
    cmd = read_op(c, EHCI_CMD);
    if (!write_op(c, EHCI_CMD, cmd & ~bits) ||
        !wait_bits(c, EHCI_STS,
            ((bits & EHCI_CMD_PSE) ? EHCI_STS_PSS : 0) |
            ((bits & EHCI_CMD_ASE) ? EHCI_STS_ASS : 0), 0, 100)) {
        EHCICoreFail(c, "schedule stop", bits); c->scheduleBusy = 0; return 0;
    }
    return 1;
}

static void rebuild_schedules(EHCIControllerState *c)
{
    EHCIEndpoint *periodic[EHCI_MAX_DEVICES];
    EHCIQH *tail = (EHCIQH *)c->asyncHead.virtualAddress;
    unsigned n = 0, i, j, k;
    for (i = 0; i < EHCI_MAX_DEVICES; i++) {
        EHCIDevice *d = &c->devices[i];
        for (j = 0; j < EHCI_ENDPOINTS; j++) {
            EHCIEndpoint *e = &d->endpoints[j];
            if (!e->configured || !e->linked) continue;
            if (e->periodic) periodic[n++] = e;
            else { tail->link = e->qhPhysical | EHCI_LINK_QH; tail = e->qh; }
        }
    }
    tail->link = c->asyncHead.physicalAddress | EHCI_LINK_QH;
    /* Descending powers-of-two form a shared periodic tree. Same-period QHs
     * only link to their own phase; a parent must divide the child's period. */
    for (i = 1; i < n; i++) {
        EHCIEndpoint *e = periodic[i];
        j = i;
        while (j && periodic[j - 1]->interval < e->interval) {
            periodic[j] = periodic[j - 1]; j--;
        }
        periodic[j] = e;
    }
    for (i = 0; i < n; i++) {
        periodic[i]->qh->link = EHCI_LINK_END;
        for (j = i + 1; j < n; j++)
            if (periodic[i]->phase % periodic[j]->interval == periodic[j]->phase) {
                periodic[i]->qh->link = periodic[j]->qhPhysical | EHCI_LINK_QH;
                break;
            }
    }
    for (k = 0; k < EHCI_FRAME_COUNT; k++) {
        ehci_u32 link = EHCI_LINK_END;
        for (i = 0; i < n; i++) if (k % periodic[i]->interval == periodic[i]->phase) {
            link = periodic[i]->qhPhysical | EHCI_LINK_QH; break;
        }
        ((volatile ehci_u32 *)c->frameList.virtualAddress)[k] = link;
    }
    barrier();
}
static int schedule_leave(EHCIControllerState *c)
{
    unsigned i, j, bits = 0;
    int ok;
    for (i = 0; i < EHCI_MAX_DEVICES; i++)
        for (j = 0; j < EHCI_ENDPOINTS; j++) {
            EHCIEndpoint *e = &c->devices[i].endpoints[j];
            if (e->linked) bits |= e->periodic ? EHCI_CMD_PSE : EHCI_CMD_ASE;
        }
    rebuild_schedules(c);
    /* EHCI 4.8 leaves the next hardware QH in ASYNCLISTADDR on stop.
     * That QH may have just been unlinked/freed. Both schedules are stopped:
     * rebase to our permanent sentinel and verify before enabling DMA. */
    ok = write_op(c, EHCI_ASYNC, c->asyncHead.physicalAddress) &&
         read_op(c, EHCI_ASYNC) == c->asyncHead.physicalAddress;
    if (ok) ok = write_op(c, EHCI_CMD, EHCI_CMD_RUN | EHCI_CMD_ITC | bits);
    if (ok) ok = wait_bits(c, EHCI_STS, EHCI_STS_PSS | EHCI_STS_ASS,
        ((bits & EHCI_CMD_PSE) ? EHCI_STS_PSS : 0) |
        ((bits & EHCI_CMD_ASE) ? EHCI_STS_ASS : 0), 100);
    if (!ok) EHCICoreFail(c, "schedule start", bits);
    c->scheduleChanges++; c->scheduleBusy = 0; c->rescan = 1;
    EHCIPlatformWake(c->owner);
    return ok;
}

unsigned EHCIIntervalFrames(unsigned speed, unsigned interval)
{
    unsigned frames, result = 1;
    if (!interval || (speed == EHCI_SPEED_HIGH && interval > 16)) return 0;
    frames = speed == EHCI_SPEED_HIGH ? ((1U << (interval - 1)) / 8) : interval;
    while (result < 1024 && (result << 1) <= frames) result <<= 1;
    return result;
}
static unsigned periodic_cost(EHCIEndpoint *e)
{
    /* Conservative high-speed bus time in ns, including bit stuffing. */
    return 2000 + ((e->maxPacket * 7 + 5) / 6) * 17;
}
static unsigned split_bus_cost(EHCIControllerState *c, EHCIDevice *d,
                               EHCIEndpoint *e)
{
    /* USB 2.0 5.11.3 non-isochronous bus time, rounded upward in ns.
     * Include 1 us host allowance, 1 us for each LS hub setup, and the
     * hub's advertised TT think time (8/16/24/32 full-speed bit times). */
    unsigned bits = 4 + (e->maxPacket * 56 + 5) / 6;
    unsigned overhead = 1000 + (c->devices[d->parent - 1].ttThinkTime + 1) * 8 * 84;
    if (d->speed == EHCI_SPEED_LOW) return overhead + 64060 + 2000 + bits * 677;
    return overhead + 9107 + bits * 84;
}
static int reserve_periodic(EHCIControllerState *c, EHCIDevice *d,
                            EHCIEndpoint *e, unsigned usbInterval)
{
    unsigned phase, start, i, u, frame;
    e->interval = EHCIIntervalFrames(d->speed, usbInterval);
    if (!e->interval) return 0;
    for (phase = 0; phase < e->interval; phase++) {
        for (start = 0; start < (d->speed == EHCI_SPEED_HIGH ? 8U : 4U); start++) {
            unsigned smask = 1U << start, cmask = 0, occupied = smask;
            int valid = 1;
            if (d->speed != EHCI_SPEED_HIGH) {
                cmask = 0x1cU << start;
                occupied = 0x1fU << start;
            } else if (usbInterval <= 3) {
                unsigned period = 1U << (usbInterval - 1);
                if (start >= period) break;
                smask = 0;
                for (u = start; u < 8; u += period) smask |= 1U << u;
            }
            for (frame = phase; valid && frame < EHCI_FRAME_COUNT; frame += e->interval) {
                unsigned cost[8];
                unsigned ttCost = d->speed == EHCI_SPEED_HIGH ? 0 : split_bus_cost(c, d, e);
                unsigned ttStarts = 1;
                bzero(cost, sizeof(cost));
                for (i = 0; i < EHCI_MAX_DEVICES && valid; i++) {
                    EHCIDevice *other = &c->devices[i];
                    EHCIEndpoint *p = &other->endpoints[EHCI_EP_INTERRUPT];
                    if (p == e || !p->reserved || frame % p->interval != p->phase) continue;
                    if (d->speed != EHCI_SPEED_HIGH && other->speed != EHCI_SPEED_HIGH &&
                        other->ttAddress == d->ttAddress &&
                        (c->devices[d->parent - 1].hubProtocol != 2 || other->ttPort == d->ttPort)) {
                        /* Match rebuild_schedules' actual QH order. A TT may
                         * discard an earlier response when asked for a later
                         * one (USB 2.0 11.18.8). Shared CS microframes must
                         * therefore visit endpoints in start-split order.
                         * Equal starts use the same QH order for SS and CS. */
                        unsigned otherFirst = p->interval > e->interval ||
                            (p->interval == e->interval && other < d);
                        if ((cmask & p->cmask) &&
                            (otherFirst ? p->smask > smask : smask > p->smask)) {
                            valid = 0; break;
                        }
                        if (smask & p->smask) {
                            ttCost += split_bus_cost(c, other, p); ttStarts++;
                        }
                    }
                    for (u = 0; u < 8; u++)
                        if ((p->smask | p->cmask) & (1U << u)) cost[u] += periodic_cost(p);
                }
                /* TTs pipeline multiple periodic transactions. Complete-split
                 * retry windows may overlap; reserve the downstream bus time
                 * at the start-split instead (USB 2.0 11.18-11.19). Limiting
                 * each start bucket to 125 us forbids spill into the next one.
                 * Starts 0..3 plus three CS retries stay within this H-frame;
                 * at most 500 us/frame is admitted, below the 900 us limit. */
                if (ttCost > 125000 || ttStarts > 16) valid = 0;
                for (u = 0; u < 8; u++)
                    if (((smask | cmask) & (1U << u)) && cost[u] + periodic_cost(e) > 100000)
                        valid = 0;
            }
            if (valid) {
                e->phase = phase; e->smask = smask; e->cmask = cmask;
                e->ttSlot = start; e->ttMask = occupied; e->reserved = 1;
                return 1;
            }
        }
    }
    return 0;
}

int EHCICoreConfigureEndpoint(EHCIControllerState *c, EHCIDevice *d,
    unsigned index, unsigned address, unsigned packet, unsigned interval)
{
    EHCIEndpoint *e;
    unsigned pages, i;
    if (index >= EHCI_ENDPOINTS || !packet || packet > 1024 ||
        (d->speed != EHCI_SPEED_HIGH && (!d->parent || !d->ttAddress)) ||
        (address & 0x70) || (d->speed == EHCI_SPEED_LOW && packet > 8) ||
        (d->speed == EHCI_SPEED_FULL && packet > 64) ||
        (index >= EHCI_EP_BULK_IN && (d->speed == EHCI_SPEED_LOW || packet > 512))) return 0;
    if (index >= EHCI_EP_BULK_IN &&
        (d->speed == EHCI_SPEED_HIGH ? packet != 512 :
         (packet != 8 && packet != 16 && packet != 32 && packet != 64))) return 0;
    e = &d->endpoints[index];
    if (e->configured) return 0;
    e->address = address; e->maxPacket = packet; e->periodic = index == EHCI_EP_INTERRUPT;
    if (e->periodic && !reserve_periodic(c, d, e, interval)) return 0;
    if (!dma_alloc(&e->descriptors, EHCI_PAGE_SIZE)) goto fail;
    e->qh = (EHCIQH *)e->descriptors.virtualAddress;
    e->td = (EHCIqTD *)((ehci_u8 *)e->descriptors.virtualAddress + TD_OFFSET);
    e->qhPhysical = e->descriptors.physicalAddress;
    e->tdPhysical = e->qhPhysical + TD_OFFSET;
    pages = index >= EHCI_EP_BULK_IN ? EHCI_DATA_PAGES : 1;
    for (i = 0; i < pages; i++) if (!dma_alloc(&e->data[i], EHCI_PAGE_SIZE)) goto fail;
    e->qh->link = e->qh->next = e->qh->alternate = EHCI_LINK_END;
    e->qh->token = EHCI_QTD_HALTED;
    e->configured = 1;
    return 1;
fail:
    endpoint_free(e);
    return 0;
}

static void fill_td(EHCIEndpoint *e, unsigned n, unsigned physical,
                    unsigned bytes, unsigned pid, unsigned toggle)
{
    EHCIqTD *t = &e->td[n];
    bzero(t, sizeof(*t));
    t->next = t->alternate = EHCI_LINK_END;
    t->buffer[0] = physical;
    t->token = EHCI_QTD_ACTIVE | EHCI_QTD_CERR | pid | EHCI_QTD_BYTES(bytes) |
        (toggle ? EHCI_QTD_TOGGLE : 0);
    e->lengths[n] = bytes;
    if (n) e->td[n - 1].next = e->tdPhysical + n * sizeof(EHCIqTD);
}

static int submit_transfer(EHCIControllerState *c, EHCIDevice *d, EHCIEndpoint *e,
    const USBSetupPacket *setup, const void *buffer, unsigned length,
    ehci_u64 deadline, unsigned controlOwner)
{
    unsigned count = 0, remaining = length, offset = 0, page = 0, toggle;
    unsigned generation = d->generation;
    unsigned control = setup != 0;
    if (!c->running || c->fatal || !d->used || d->disconnecting || !e->configured)
        return USB_TRANSFER_DISCONNECTED;
    if (e->waiting || e->halted ||
        (e == &d->endpoints[0] && d->controlBusy && !controlOwner)) return USB_TRANSFER_ERROR;
    if (length > (e->data[1].allocation ? USB_STORAGE_MAX_TRANSFER : EHCI_PAGE_SIZE) ||
        (control && setup->length != length) ||
        (length && !buffer && !(control ? setup->requestType & USB_DIR_IN : e->address & USB_DIR_IN)))
        return USB_TRANSFER_ERROR;
    if (EHCIPlatformMilliseconds() >= deadline) return USB_TRANSFER_TIMEOUT;
    /* Stop both schedules because their shared lists are rebuilt together. */
    if (!schedule_enter(c, EHCI_CMD_ASE | EHCI_CMD_PSE)) return USB_TRANSFER_ERROR;
    if (!d->used || d->generation != generation || d->disconnecting) {
        schedule_leave(c); return USB_TRANSFER_DISCONNECTED;
    }
    /* schedule_enter may release the state lock. Endpoint availability and
     * deadline must still hold after acquiring schedule ownership. */
    if (!e->configured || e->waiting || e->halted ||
        (e == &d->endpoints[0] && d->controlBusy && !controlOwner)) {
        schedule_leave(c); return USB_TRANSFER_ERROR;
    }
    if (EHCIPlatformMilliseconds() >= deadline) {
        schedule_leave(c); return USB_TRANSFER_TIMEOUT;
    }
    e->control = control;
    e->ecmBatch = 0;
    e->input = control ? (setup->requestType & USB_DIR_IN) != 0 : (e->address & USB_DIR_IN) != 0;
    e->total = length; e->actual = 0; e->result = USB_TRANSFER_PENDING;
    e->deadline = deadline; e->statusTD = NO_TD;
    bzero(e->lengths, sizeof(e->lengths));
    toggle = control ? 1 : e->toggle;
    if (control) {
        ehci_u8 *bytes = (ehci_u8 *)e->descriptors.virtualAddress + SETUP_OFFSET;
        bytes[0] = setup->requestType; bytes[1] = setup->request;
        bytes[2] = (ehci_u8)setup->value; bytes[3] = (ehci_u8)(setup->value >> 8);
        bytes[4] = (ehci_u8)setup->index; bytes[5] = (ehci_u8)(setup->index >> 8);
        bytes[6] = (ehci_u8)setup->length; bytes[7] = (ehci_u8)(setup->length >> 8);
        fill_td(e, count++, e->qhPhysical + SETUP_OFFSET, 8, EHCI_PID_SETUP, 0);
    }
    e->dataFirst = count;
    do {
        unsigned chunk = remaining > EHCI_PAGE_SIZE ? EHCI_PAGE_SIZE : remaining;
        if (!chunk && control) break;
        if (!e->input && chunk) bcopy((const ehci_u8 *)buffer + offset, e->data[page].virtualAddress, chunk);
        fill_td(e, count++, e->data[page].physicalAddress, chunk,
                 e->input ? EHCI_PID_IN : EHCI_PID_OUT, toggle);
        toggle ^= ((chunk + e->maxPacket - 1) / e->maxPacket) & 1;
        remaining -= chunk; offset += chunk; page++;
    } while (remaining);
    /* ECM frame boundaries use a short packet, including an explicit ZLP for
     * exact packet multiples. Never apply this to storage/BOT transfers. */
    if (d->ecm && e == &d->endpoints[EHCI_EP_BULK_OUT] &&
        !control && length && !(length % e->maxPacket))
        fill_td(e, count++, 0, 0, EHCI_PID_OUT, toggle);
    e->dataLast = count;
    /* Every IN qTD can terminate a bulk chain on a short packet. */
    if (e->input) for (page = e->dataFirst; page < e->dataLast; page++)
        e->td[page].token |= EHCI_QTD_IOC;
    if (control) {
        e->statusTD = count;
        fill_td(e, count++, 0, 0, length && e->input ? EHCI_PID_OUT : EHCI_PID_IN, 1);
        /* A short control IN must still execute its status stage. */
        for (page = e->dataFirst; page < e->dataLast; page++)
            e->td[page].alternate = e->tdPhysical + e->statusTD * sizeof(EHCIqTD);
    }
    e->td[count - 1].token |= EHCI_QTD_IOC;
    e->count = count;
    /* DTC=1: explicit qTD toggles; QH overlay supplies the next toggle on
     * non-control completion, including a short or zero-length packet. */
    e->qh->endpoint = d->address | ((e->address & 15) << 8) | (d->speed << 12) |
        EHCI_QH_DTC | (e->maxPacket << 16) |
        (control && d->speed != EHCI_SPEED_HIGH ? EHCI_QH_CONTROL : 0) |
        (!e->periodic && d->speed == EHCI_SPEED_HIGH ? (4U << 28) : 0);
    e->qh->capabilities = EHCI_QH_MULT | e->smask | (e->cmask << 8) |
        (d->ttAddress << 16) | (d->ttPort << 23);
    e->qh->current = 0; e->qh->alternate = EHCI_LINK_END;
    e->qh->token = 0;
    bzero((void *)e->qh->buffer, sizeof(e->qh->buffer));
    bzero((void *)e->qh->high, sizeof(e->qh->high));
    e->qh->next = e->tdPhysical;
    barrier(); e->waiting = 1; e->linked = 1;
    if (!schedule_leave(c)) return USB_TRANSFER_ERROR;
    return USB_TRANSFER_OK;
}

int EHCICoreSubmit(EHCIControllerState *c, EHCIDevice *d, EHCIEndpoint *e,
    const USBSetupPacket *setup, const void *buffer, unsigned length, ehci_u64 deadline)
{
    return submit_transfer(c, d, e, setup, buffer, length, deadline, 0);
}

#include "EHCIECMTransfer.inc"

int EHCICoreFinish(EHCIControllerState *c, EHCIDevice *d, EHCIEndpoint *e)
{
    unsigned n, actual = 0, shortSeen = 0, toggle = e->toggle;
    int result = USB_TRANSFER_OK;
    if (!e->waiting) return e->result;
    if (e->ecmBatch) return ecm_batch_finish(c, d, e);
    barrier();
    for (n = 0; n < e->count; n++) {
        unsigned token = e->td[n].token, left = EHCI_QTD_REMAIN(token);
        unsigned data = n >= e->dataFirst && n < e->dataLast;
        if (data && shortSeen) continue;
        if (token & EHCI_QTD_ACTIVE) return USB_TRANSFER_PENDING;
        if (left > e->lengths[n]) { result = USB_TRANSFER_ERROR; break; }
        if (token & EHCI_QTD_ERRORS) {
            result = (token & (EHCI_QTD_ERRORS & ~EHCI_QTD_HALTED)) ?
                USB_TRANSFER_ERROR : USB_TRANSFER_STALL;
            /* A STALL preserves bytes from successful earlier packets in
             * this qTD. BOT must receive them before validating CSW residue.
             * Other hardware errors do not authorize delivery of this data. */
            if (data && result == USB_TRANSFER_STALL) actual += e->lengths[n] - left;
            break;
        }
        if (data) {
            unsigned done = e->lengths[n] - left;
            unsigned packets = (done + e->maxPacket - 1) / e->maxPacket;
            actual += done;
            if (e->input && left && !(done % e->maxPacket)) packets++;
            if (!e->lengths[n]) packets = 1;
            toggle ^= packets & 1;
            if (left) {
                if (!e->input) { result = USB_TRANSFER_ERROR; break; }
                shortSeen = 1;
            }
        }
    }
    e->waiting = 0; e->actual = actual; e->result = result;
    if (!e->control && !result) e->toggle = toggle;
    if (result) { e->halted = 1; c->errors++; }
    c->transferEvents++;
    return result;
}

int EHCICoreCancel(EHCIControllerState *c, EHCIDevice *d, EHCIEndpoint *e)
{
    unsigned generation = d->generation;
    if (!e->configured) return 1;
    if (!schedule_enter(c, EHCI_CMD_ASE | EHCI_CMD_PSE)) return 0;
    if (generation != d->generation) { schedule_leave(c); return 0; }
    e->linked = 0; e->waiting = 0;
    e->qh->next = e->qh->alternate = EHCI_LINK_END;
    e->qh->token = EHCI_QTD_HALTED;
    return schedule_leave(c);
}

static void copy_input(EHCIEndpoint *e, void *buffer)
{
    unsigned left = e->actual, page = 0, offset = 0;
    if (!buffer) return;
    while (left) {
        unsigned n = left > EHCI_PAGE_SIZE ? EHCI_PAGE_SIZE : left;
        bcopy(e->data[page++].virtualAddress, (ehci_u8 *)buffer + offset, n);
        offset += n; left -= n;
    }
}

static int clear_tt(EHCIControllerState *, EHCIDevice *, EHCIEndpoint *);
static int control_transfer(void *host, void *device, const USBSetupPacket *setup,
                             void *buffer, ehci_u16 *actual)
{
    EHCIControllerState *c = (EHCIControllerState *)host;
    EHCIDevice *d = (EHCIDevice *)device;
    EHCIEndpoint *e = &d->endpoints[0];
    ehci_u64 end = EHCIPlatformMilliseconds() + EHCI_TIMEOUT_MS;
    unsigned generation = d->generation;
    int rc, ok = 0;
    if (actual) *actual = 0;
    /* Hub management and storage TT recovery share the hub's EP0. Hold this
     * logical ownership across every unlocked sleep and result consumption.
     * Generation checks prevent an old waiter from claiming a reused slot. */
    for (;;) {
        if (c->fatal || !d->used || d->disconnecting || d->generation != generation)
            return 0;
        if (EHCIPlatformMilliseconds() >= end) return 0;
        if (!d->controlBusy) break;
        EHCIPlatformPause(c, 1);
    }
    d->controlBusy = 1;
    if (e->halted) {
        if (!EHCICoreCancel(c, d, e) || !clear_tt(c, d, e) ||
            !d->used || d->disconnecting || d->generation != generation) goto done;
        e->halted = 0;
    }
    rc = submit_transfer(c, d, e, setup, buffer, setup->length, end, 1);
    if (rc) goto done;
    while (e->waiting && d->used && !d->disconnecting && d->generation == generation && !c->fatal) {
        if (EHCIPlatformMilliseconds() >= end) {
            EHCICoreCancel(c, d, e);
            if (d->generation == generation) {
                e->result = USB_TRANSFER_TIMEOUT;
                /* Unlinking an incomplete split control transfer can leave
                 * either async TT buffer busy. Recover before the next SETUP. */
                e->halted = 1;
            }
            c->timeouts++; goto done;
        }
        EHCIPlatformPause(c, 1);
    }
    if (c->fatal || !d->used || d->disconnecting || d->generation != generation || e->result) goto done;
    if (e->input) copy_input(e, buffer);
    if (actual) *actual = (ehci_u16)e->actual;
    ok = 1;
done:
    /* Optional requests (notably HID SET_IDLE) may never be followed by a
     * second EP0 request. Release shared async TT capacity before returning
     * an error, rather than leaving recovery until the next SETUP. */
    if (!ok && !c->fatal && d->used && !d->disconnecting &&
        d->generation == generation && e->halted && d->speed != EHCI_SPEED_HIGH) {
        if (EHCICoreCancel(c, d, e) && d->used && !d->disconnecting &&
            d->generation == generation) (void)clear_tt(c, d, e);
    }
    if (d->generation == generation) d->controlBusy = 0;
    return ok;
}
static int update_ep0(void *host, void *device, ehci_u8 packet)
{
    EHCIDevice *d = (EHCIDevice *)device;
    (void)host;
    if (d->speed == EHCI_SPEED_HIGH && packet != 64) return 0;
    if (d->speed == EHCI_SPEED_LOW && packet != 8) return 0;
    if (packet != 8 && packet != 16 && packet != 32 && packet != 64) return 0;
    d->endpoints[0].maxPacket = packet;
    return 1;
}
static const USBTransportOperations usb_ops = { control_transfer, update_ep0 };

int EHCICoreService(EHCIControllerState *c, unsigned causes)
{
    unsigned i, j;
    if (!c->running || c->fatal) return 0;
    if (causes & EHCI_STS_FATAL) { EHCICoreFail(c, "host system error", causes); return 0; }
    if (causes & EHCI_STS_PORT) c->portChanges |= (1U << (c->maxPorts + 1)) - 2;
    if (c->scheduleBusy) { c->rescan = 1; return 1; }
    c->rescan = 0; c->polls++;
    for (i = 0; i < EHCI_MAX_DEVICES; i++) {
        EHCIDevice *d = &c->devices[i];
        if (!d->used || d->disconnecting) continue;
        for (j = 0; j < EHCI_ENDPOINTS; j++) {
            EHCIEndpoint *e = &d->endpoints[j];
            int rc;
            unsigned completed = e->batchDone;
            if (!e->waiting) continue;
            rc = EHCICoreFinish(c, d, e);
            if (rc == USB_TRANSFER_PENDING) {
                if (d->ecm && e->ecmBatch && completed != e->batchDone)
                    EHCIPlatformNetworkWake(c->owner);
                continue;
            }
            EHCIPlatformWake(c->owner);
            if (d->ecm) {
                EHCIPlatformNetworkWake(c->owner);
                continue; /* ECM worker owns result consumption and rearming. */
            }
            if (d->storage) {
                unsigned target;
                for (target = 0; target < USB_STORAGE_TARGETS; target++)
                    if (c->storage[target].slot == i + 1)
                        EHCIPlatformStorageWake(c->owner, target);
            }
            if (j == EHCI_EP_INTERRUPT && d->ready) {
                const ehci_u8 *report = (const ehci_u8 *)e->data[0].virtualAddress;
                if (rc) { c->disconnected |= 1U << i; continue; }
                if (d->hub) {
                    unsigned n;
                    for (n = 0; n < e->actual && n < 2; n++) d->hubChanges |= (unsigned)report[n] << (8 * n);
                } else if (d->protocol == USB_HID_PROTOCOL_KEYBOARD) {
                    c->keyboardReports++; EHCIPlatformKeyboardReport(c->owner, report, e->actual);
                } else if (d->protocol == USB_HID_PROTOCOL_MOUSE) {
                    c->mouseReports++; EHCIPlatformPointerReport(c->owner, report, e->actual);
                }
                /* No deadline on idle interrupt IN: an indefinite NAK is normal. */
                if (EHCICoreSubmit(c, d, e, 0, 0, e->maxPacket, ~(ehci_u64)0))
                    c->disconnected |= 1U << i;
            }
        }
    }
    return !c->fatal;
}

EHCIDevice *EHCICoreStorageDevice(EHCIControllerState *c, unsigned target, ehci_u32 generation)
{
    unsigned slot;
    if (target >= USB_STORAGE_TARGETS || c->fatal || !c->running ||
        c->storage[target].generation != generation) return 0;
    slot = c->storage[target].slot;
    if (!slot || slot > EHCI_MAX_DEVICES || !c->devices[slot - 1].ready ||
        c->devices[slot - 1].disconnecting) return 0;
    return &c->devices[slot - 1];
}
static EHCIEndpoint *storage_endpoint(EHCIDevice *d, unsigned address)
{
    unsigned i;
    if (!address) return &d->endpoints[0];
    for (i = EHCI_EP_BULK_IN; i < EHCI_ENDPOINTS; i++)
        if (d->endpoints[i].configured && d->endpoints[i].address == address) return &d->endpoints[i];
    return 0;
}
int EHCICoreStorageBulkStart(EHCIControllerState *c, unsigned target, ehci_u32 gen,
    ehci_u8 address, const void *buffer, ehci_u32 length)
{
    EHCIDevice *d = EHCICoreStorageDevice(c, target, gen);
    EHCIEndpoint *e = d ? storage_endpoint(d, address) : 0;
    if (!e || !address) return USB_TRANSFER_DISCONNECTED;
    return EHCICoreSubmit(c, d, e, 0, buffer, length, EHCIPlatformMilliseconds() + EHCI_TIMEOUT_MS);
}
int EHCICoreStorageControlStart(EHCIControllerState *c, unsigned target, ehci_u32 gen,
    const USBSetupPacket *setup, const void *buffer)
{
    EHCIDevice *d = EHCICoreStorageDevice(c, target, gen);
    if (!d) return USB_TRANSFER_DISCONNECTED;
    return EHCICoreSubmit(c, d, &d->endpoints[0], setup, buffer, setup->length,
                           EHCIPlatformMilliseconds() + EHCI_TIMEOUT_MS);
}
int EHCICoreStorageTransferResult(EHCIControllerState *c, unsigned target, ehci_u32 gen,
    ehci_u8 address, void *buffer, ehci_u32 *actual)
{
    EHCIDevice *d = EHCICoreStorageDevice(c, target, gen);
    EHCIEndpoint *e = d ? storage_endpoint(d, address) : 0;
    if (!e) return USB_TRANSFER_DISCONNECTED;
    if (e->waiting) return USB_TRANSFER_PENDING;
    *actual = 0;
    if (!e->result || e->result == USB_TRANSFER_STALL) {
        *actual = e->actual;
        if (e->input) copy_input(e, buffer);
    }
    return e->result;
}
/* TT recovery implementation lives beside hub control below. */
int EHCICoreStorageRetireEndpoint(EHCIControllerState *c, unsigned target, ehci_u32 gen,
    ehci_u8 address, ehci_u64 deadline)
{
    EHCIDevice *d = EHCICoreStorageDevice(c, target, gen);
    EHCIEndpoint *e = d ? storage_endpoint(d, address) : 0;
    if (!e) return USB_TRANSFER_DISCONNECTED;
    if (EHCIPlatformMilliseconds() >= deadline) return USB_TRANSFER_TIMEOUT;
    if (!EHCICoreCancel(c, d, e) || !clear_tt(c, d, e)) return USB_TRANSFER_ERROR;
    if (EHCICoreStorageDevice(c, target, gen) != d) return USB_TRANSFER_DISCONNECTED;
    e->halted = 1;
    return USB_TRANSFER_OK;
}
int EHCICoreStorageRecoverEndpoint(EHCIControllerState *c, unsigned target, ehci_u32 gen,
    ehci_u8 address, ehci_u64 deadline)
{
    EHCIDevice *d;
    EHCIEndpoint *e;
    int rc = EHCICoreStorageRetireEndpoint(c, target, gen, address, deadline);
    if (rc) return rc;
    d = EHCICoreStorageDevice(c, target, gen);
    e = d ? storage_endpoint(d, address) : 0;
    if (!e) return USB_TRANSFER_DISCONNECTED;
    e->halted = 0; e->toggle = 0; e->result = USB_TRANSFER_OK;
    return USB_TRANSFER_OK;
}
void EHCICoreStorageOffline(EHCIControllerState *c, unsigned target, ehci_u32 gen)
{
    if (target < USB_STORAGE_TARGETS && c->storage[target].generation == gen && c->storage[target].slot)
        c->disconnected |= 1U << (c->storage[target].slot - 1);
}

int EHCICoreQuiesce(EHCIControllerState *c)
{
    if (!c->registersValid) return !c->dmaArmed;
    if (c->fatal) return 0;
    if (!write_op(c, EHCI_INTR, 0) || !write_op(c, EHCI_CMD, 0) ||
        !wait_bits(c, EHCI_STS, EHCI_STS_HALTED, EHCI_STS_HALTED, 100)) return 0;
    c->running = 0; c->haltConfirmed = 1;
    return 1;
}
int EHCICoreReleaseDMA(EHCIControllerState *c, int busMasterDisabled)
{
    unsigned i, j;
    if (c->dmaArmed && !c->haltConfirmed && !busMasterDisabled) return 0;
    for (i = 0; i < EHCI_MAX_DEVICES; i++)
        for (j = 0; j < EHCI_ENDPOINTS; j++) endpoint_free(&c->devices[i].endpoints[j]);
    dma_free(&c->asyncHead); dma_free(&c->frameList);
    if (c->ecm) { EHCIPlatformFree(c->ecm, sizeof(*c->ecm)); c->ecm = 0; }
    c->dmaArmed = 0;
    return 1;
}

/* Hub/enumeration routines are compiled in the same translation unit so the
 * descriptor allocator and synchronous EP0 machinery have private linkage. */
#include "EHCIEnumeration.inc"
#include "EHCIECM.inc"
