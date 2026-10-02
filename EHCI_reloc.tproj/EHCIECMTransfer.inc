/* Finite ECM batches. Bulk descriptors are written only with the async
 * schedule stopped; no live QH overlay or prefetched qTD is patched. EHCI 4.10.2:
 * RX alternate links advance after short packets. DTC=0 lets hardware carry
 * the actual toggle across independently short-terminated Ethernet frames. */
typedef char ecm_batch_fits_tds[EHCI_ECM_BATCH_FRAMES * 2 <= EHCI_TD_COUNT ? 1 : -1];
typedef char ecm_batch_fits_pages[EHCI_ECM_BATCH_FRAMES <= EHCI_DATA_PAGES ? 1 : -1];
typedef char ecm_descriptors_fit[SETUP_OFFSET + 8 <= EHCI_PAGE_SIZE ? 1 : -1];

static int ecm_schedule_leave(EHCIControllerState *c, unsigned rebuild)
{
    int ok;
    if (rebuild) return schedule_leave(c);
    /* Existing bulk QH: horizontal links and all periodic descriptors stay
     * unchanged. EHCI 4.8 permits rebasing ASYNCLISTADDR with ASS=0 while
     * the periodic schedule continues. Retain the sentinel-rebase workaround. */
    ok = write_op(c, EHCI_ASYNC, c->asyncHead.physicalAddress) &&
        read_op(c, EHCI_ASYNC) == c->asyncHead.physicalAddress;
    if (ok) ok = write_op(c, EHCI_CMD, read_op(c, EHCI_CMD) | EHCI_CMD_ASE);
    if (ok) ok = wait_bits(c, EHCI_STS, EHCI_STS_ASS, EHCI_STS_ASS, 100);
    if (!ok) EHCICoreFail(c, "ECM async restart", EHCI_CMD_ASE);
    c->scheduleChanges++; c->scheduleBusy = 0; c->rescan = 1;
    EHCIPlatformWake(c->owner);
    return ok;
}

int EHCICoreECMBatchStart(EHCIControllerState *c, EHCIDevice *d, EHCIEndpoint *e)
{
    EHCIECMState *n = c->ecm;
    unsigned gen = d->generation, input = e == &d->endpoints[EHCI_EP_BULK_IN];
    unsigned frames, count = 0, i, ping, rebuild;
    if (!n || EHCICoreECMDevice(c, gen) != d ||
        (!input && e != &d->endpoints[EHCI_EP_BULK_OUT])) return USB_TRANSFER_DISCONNECTED;
    if (!e->configured || e->waiting || e->halted) return USB_TRANSFER_ERROR;
    if (!n->enabled || (!input && !n->tx.count)) return USB_TRANSFER_OK;
    if (!schedule_enter(c, EHCI_CMD_ASE)) return USB_TRANSFER_ERROR;
    /* schedule_enter may yield, including across disable or disconnect. */
    if (EHCICoreECMDevice(c, gen) != d) {
        ecm_schedule_leave(c, 0); return USB_TRANSFER_DISCONNECTED;
    }
    /* Test linkage only after acquiring schedule ownership. First submission
     * and recovery need a topology rebuild, so stop the periodic schedule too. */
    rebuild = !e->linked;
    if (rebuild && (!write_op(c, EHCI_CMD, read_op(c, EHCI_CMD) & ~EHCI_CMD_PSE) ||
        !wait_bits(c, EHCI_STS, EHCI_STS_PSS, 0, 100))) {
        EHCICoreFail(c, "ECM periodic stop", EHCI_CMD_PSE);
        c->scheduleBusy = 0; return USB_TRANSFER_ERROR;
    }
    if (EHCICoreECMDevice(c, gen) != d) {
        ecm_schedule_leave(c, rebuild); return USB_TRANSFER_DISCONNECTED;
    }
    if (!n->enabled || (!input && !n->tx.count)) {
        ecm_schedule_leave(c, rebuild); return USB_TRANSFER_OK;
    }
    if (!e->configured || e->waiting || e->halted) {
        ecm_schedule_leave(c, rebuild); return USB_TRANSFER_ERROR;
    }
    frames = input ? EHCI_ECM_BATCH_FRAMES : n->tx.count;
    if (frames > EHCI_ECM_BATCH_FRAMES) frames = EHCI_ECM_BATCH_FRAMES;
    e->ecmBatch = 1; e->batchDone = e->batchComplete = 0; e->batchFrames = frames;
    e->control = 0; e->input = input; e->total = e->actual = 0;
    e->result = USB_TRANSFER_PENDING; e->statusTD = NO_TD;
    e->deadline = input ? ~(ehci_u64)0 : EHCIPlatformMilliseconds() + EHCI_TIMEOUT_MS;
    bzero(e->frameEnd, sizeof(e->frameEnd));
    bzero(e->lengths, sizeof(e->lengths));
    for (i = 0; i < frames; i++) {
        unsigned length = USB_ECM_RX_BYTES;
        if (!input) {
            USBECMFrame *f = &n->tx.frames[n->tx.head];
            length = USBECMPrepareFrame(e->data[i].virtualAddress, f->bytes, f->length);
            n->tx.head = (n->tx.head + 1) % USB_ECM_QUEUE_SIZE; n->tx.count--;
        }
        fill_td(e, count++, e->data[i].physicalAddress, length,
            input ? EHCI_PID_IN : EHCI_PID_OUT, 0);
        if (!input && !(length % e->maxPacket))
            fill_td(e, count++, 0, 0, EHCI_PID_OUT, 0);
        e->frameEnd[count - 1] = 1;
        e->td[count - 1].token |= EHCI_QTD_IOC;
        e->total += length;
    }
    if (input) for (i = 0; i + 1 < count; i++) e->td[i].alternate = e->td[i].next;
    e->count = e->dataLast = count; e->dataFirst = 0;
    ping = input ? 0 : e->qh->token & 1U;
    e->qh->endpoint = d->address | ((e->address & 15) << 8) | (d->speed << 12) |
        (e->maxPacket << 16) | (d->speed == EHCI_SPEED_HIGH ? 4U << 28 : 0);
    e->qh->capabilities = EHCI_QH_MULT | (d->ttAddress << 16) | (d->ttPort << 23);
    e->qh->current = 0; e->qh->alternate = EHCI_LINK_END;
    e->qh->token = (e->toggle ? EHCI_QTD_TOGGLE : 0) | ping;
    bzero((void *)e->qh->buffer, sizeof(e->qh->buffer));
    bzero((void *)e->qh->high, sizeof(e->qh->high));
    e->qh->next = e->tdPhysical;
    barrier(); e->waiting = e->linked = 1;
    if (input) { n->rxBatches++; n->rxActive = 1; }
    else { n->txBatches++; n->txInFlight = frames; n->txActive = 1; }
    if (!rebuild) n->asyncRearms++;
    if (!ecm_schedule_leave(c, rebuild)) return USB_TRANSFER_ERROR;
    return USB_TRANSFER_OK;
}

static void ecm_receive_frame(EHCIECMState *n, const ehci_u8 *bytes, unsigned actual)
{
    if (actual == USB_ECM_RX_BYTES) {
        n->rxDrop = 1; n->rxErrors++; n->rxOversize++;
    } else if (n->rxDrop) { n->rxDrop = 0; n->rxResyncDrops++; }
    else if (!actual) n->rxZeroPackets++;
    else if (!USBECMQueuePush(&n->rx, bytes, actual)) {
        n->rxErrors++;
        if (actual < 14 || actual > USB_ECM_FRAME_MAX) n->rxInvalid++;
        else n->rxQueueDrops++;
    } else n->rxPackets++;
}

static int ecm_batch_finish(EHCIControllerState *c, EHCIDevice *d, EHCIEndpoint *e)
{
    EHCIECMState *n = c->ecm;
    int result = USB_TRANSFER_OK;
    if (!n || EHCICoreECMDevice(c, n->generation) != d) return USB_TRANSFER_PENDING;
    barrier();
    while (e->batchDone < e->count) {
        unsigned index = e->batchDone, token = e->td[index].token;
        unsigned left = EHCI_QTD_REMAIN(token), actual, packets;
        if (token & EHCI_QTD_ACTIVE) return USB_TRANSFER_PENDING;
        if (left > e->lengths[index]) { result = USB_TRANSFER_ERROR; break; }
        if (token & EHCI_QTD_ERRORS) {
            result = token & (EHCI_QTD_ERRORS & ~EHCI_QTD_HALTED) ?
                USB_TRANSFER_ERROR : USB_TRANSFER_STALL;
            break;
        }
        actual = e->lengths[index] - left;
        if (!e->input && left) { result = USB_TRANSFER_ERROR; break; }
        packets = (actual + e->maxPacket - 1) / e->maxPacket;
        if ((e->input && left && !(actual % e->maxPacket)) || !e->lengths[index]) packets++;
        e->toggle ^= packets & 1;
        e->actual += actual; e->batchDone++;
        if (e->input) {
            ecm_receive_frame(n, e->data[index].virtualAddress, actual);
            n->recoveries[EHCI_EP_BULK_IN] = 0;
        }
        if (e->frameEnd[index]) {
            e->batchComplete++; c->transferEvents++;
            if (!e->input) { n->txPackets++; if (n->txInFlight) n->txInFlight--; }
        }
    }
    e->waiting = 0; e->result = result;
    if (result) { e->halted = 1; c->errors++; }
    return result;
}
