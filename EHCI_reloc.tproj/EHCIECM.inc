/* Included after enumeration helpers; one native worker owns ECM pumping.
 * All entry points hold the controller event lock. USB waits can release it. */
EHCIDevice *EHCICoreECMDevice(EHCIControllerState *c, unsigned generation)
{
    EHCIECMState *n = c->ecm;
    EHCIDevice *d;
    if (!n || !n->slot || n->slot > EHCI_MAX_DEVICES || c->fatal || !c->running ||
        n->generation != generation || (c->disconnected & (1U << (n->slot - 1)))) return 0;
    d = &c->devices[n->slot - 1];
    return d->used && d->ready && d->ecm && !d->disconnecting &&
        d->generation == generation ? d : 0;
}

void EHCICoreECMEnable(EHCIControllerState *c, unsigned enabled, unsigned filter)
{
    EHCIECMState *n = c->ecm;
    if (!n) return;
    n->enabled = enabled != 0; n->filter = filter;
    if (!enabled) {
        n->txErrors += n->tx.count;
        n->tx.count = n->tx.head = n->rx.count = n->rx.head = 0;
    }
    EHCIPlatformNetworkWake(c->owner);
}

int EHCICoreECMTransmit(EHCIControllerState *c, unsigned generation,
                       const ehci_u8 *bytes, unsigned length)
{
    EHCIECMState *n = c->ecm;
    if (!EHCICoreECMDevice(c, generation) || !n->enabled ||
        (n->notifications.linkKnown && !n->notifications.linkUp) ||
        !USBECMQueuePush(&n->tx, bytes, length)) {
        if (n) n->txErrors++;
        return 0;
    }
    EHCIPlatformNetworkWake(c->owner); return 1;
}

static void ecm_offline(EHCIControllerState *c, unsigned generation, const char *reason)
{
    EHCIDevice *d = EHCICoreECMDevice(c, generation);
    if (!d) return;
    EHCIPlatformLog("EHCI: ECM offline: %s\n", reason);
    c->disconnected |= 1U << (c->ecm->slot - 1);
    EHCIPlatformNetworkWake(c->owner);
}

static int ecm_recover(EHCIControllerState *c, EHCIDevice *d, unsigned gen, unsigned index)
{
    EHCIEndpoint *e = &d->endpoints[index];
    EHCIECMState *n = c->ecm;
    if (++n->recoveries[index] > 3) return 0;
    if (!EHCICoreCancel(c, d, e) || EHCICoreECMDevice(c, gen) != d ||
        !clear_tt(c, d, e) || EHCICoreECMDevice(c, gen) != d ||
        !device_request(d, 2, 1, 0, e->address, 0, 0) ||
        EHCICoreECMDevice(c, gen) != d) return 0;
    e->toggle = 0; e->halted = 0;
    EHCIPlatformLog("EHCI: ECM endpoint %02x recovered (%u/3)\n", e->address, n->recoveries[index]);
    return 1;
}

static void ecm_sample_notification(EHCIECMState *n, EHCIEndpoint *e, unsigned gen)
{
    EHCIECMNotifySample *s = &n->lastNotify;
    unsigned length = e->actual;
    bzero(s, sizeof(*s));
    s->generation = gen; s->milliseconds = (unsigned)EHCIPlatformMilliseconds();
    s->length = length; s->requested = e->total;
    s->used = n->notifications.used; s->expected = n->notifications.expected;
    s->qtdToken = e->td[0].token; s->qhToken = e->qh->token;
    if (length > sizeof(s->bytes)) length = sizeof(s->bytes);
    if (length) bcopy(e->data[0].virtualAddress, s->bytes, length);
}

void EHCICoreECMPump(EHCIControllerState *c)
{
    EHCIECMState *n = c->ecm;
    EHCIDevice *d;
    EHCIEndpoint *e;
    unsigned gen, desired, index;
    if (!n) return;
    gen = n->generation; d = EHCICoreECMDevice(c, gen);
    if (!d) {
        n->txErrors += n->tx.count + (n->txActive != 0);
        n->rx.count = n->rx.head = n->tx.count = n->tx.head = 0;
        n->rxActive = n->txActive = n->notifyActive = 0;
        return;
    }
    desired = n->enabled ? n->filter : 0;
    if (desired != n->appliedFilter) {
        int ok = USBECMSetFilter(&d->usb, d->ecmInterface.controlInterface, desired);
        if (EHCICoreECMDevice(c, gen) != d) return;
        if (!ok) { ecm_offline(c, gen, "packet filter request failed"); return; }
        n->appliedFilter = desired;
    }
    for (index = EHCI_EP_INTERRUPT; index <= EHCI_EP_BULK_OUT; index++) {
        unsigned *active = index == EHCI_EP_INTERRUPT ? &n->notifyActive :
            index == EHCI_EP_BULK_IN ? &n->rxActive : &n->txActive;
        int rc;
        e = &d->endpoints[index];
        if (!e->configured) continue;
        if (index != EHCI_EP_INTERRUPT && !n->enabled) {
            if (*active) {
                if (!EHCICoreCancel(c, d, e) || EHCICoreECMDevice(c, gen) != d) return;
                /* A cancelled IN may have consumed part of a frame. Clear
                 * halt/toggle on both sides before using the pipe again. */
                if (!ecm_recover(c, d, gen, index)) {
                    ecm_offline(c, gen, "disable retirement failed"); return;
                }
                if (index == EHCI_EP_BULK_IN) n->rxDrop = 1;
                else n->txErrors++;
                n->recoveries[index] = 0;
                *active = 0;
            }
            continue;
        }
        if (*active && e->waiting && index == EHCI_EP_BULK_OUT &&
            EHCIPlatformMilliseconds() >= e->deadline) {
            if (!EHCICoreCancel(c, d, e) || EHCICoreECMDevice(c, gen) != d) return;
            e->result = USB_TRANSFER_TIMEOUT; c->timeouts++;
        }
        if (*active && !e->waiting) {
            rc = e->result; *active = 0;
            if (rc) {
                if (index == EHCI_EP_BULK_OUT) n->txErrors++;
                if (index == EHCI_EP_INTERRUPT) n->notifyUSBErrors++;
                if (index == EHCI_EP_BULK_IN) { n->rxErrors++; n->rxUSBErrors++; }
                if (rc != USB_TRANSFER_STALL && rc != USB_TRANSFER_TIMEOUT) {
                    ecm_offline(c, gen, "USB transfer failed"); return;
                }
                if (!ecm_recover(c, d, gen, index)) {
                    ecm_offline(c, gen, "endpoint recovery exhausted"); return;
                }
                if (index == EHCI_EP_BULK_IN) n->rxDrop = 1;
                if (index == EHCI_EP_INTERRUPT) n->notifications.used = n->notifications.expected = 0;
            } else {
                n->recoveries[index] = 0;
                if (index == EHCI_EP_BULK_OUT) n->txPackets++;
                else if (index == EHCI_EP_INTERRUPT) {
                    int changes;
                    n->notifyPackets++;
                    ecm_sample_notification(n, e, gen);
                    changes = USBECMNotification(&n->notifications, d->ecmInterface.controlInterface,
                        d->ecmInterface.dataInterface, e->data[0].virtualAddress, e->actual);
                    n->lastNotify.result = changes;
                    if (changes > 0 && (changes & 1))
                        EHCIPlatformLog("EHCI: ECM link %s\n", n->notifications.linkUp ? "up" : "down");
                    if (changes > 0 && (changes & 1) && !n->notifications.linkUp) {
                        n->txErrors += n->tx.count; n->tx.count = n->tx.head = 0;
                    }
                    if (changes > 0 && (changes & 2))
                        EHCIPlatformLog("EHCI: ECM speed downstream %u upstream %u bit/s\n",
                            n->notifications.downstream, n->notifications.upstream);
                    if (changes < 0) {
                        /* Status notifications are not Ethernet RX frames. */
                        n->notifyParseErrors++;
                        if (n->badNotify.generation != gen) n->badNotify = n->lastNotify;
                    }
                } else if (e->actual == USB_ECM_RX_BYTES) {
                    n->rxDrop = 1; n->rxErrors++; n->rxOversize++;
                } else if (n->rxDrop) { n->rxDrop = 0; n->rxResyncDrops++; }
                else if (e->actual) {
                    if (!USBECMQueuePush(&n->rx, e->data[0].virtualAddress, e->actual)) {
                        n->rxErrors++;
                        if (e->actual < 14 || e->actual > USB_ECM_FRAME_MAX) n->rxInvalid++;
                        else n->rxQueueDrops++;
                    }
                    else n->rxPackets++;
                } else n->rxZeroPackets++;
            }
        }
        if (!*active) {
            if (index == EHCI_EP_BULK_OUT) {
                USBECMFrame *f = &n->transmitting;
                unsigned length;
                if (!USBECMQueuePop(&n->tx, f)) continue;
                /* In-place padding is safe: each queue entry has full MTU capacity. */
                length = USBECMPrepareFrame(f->bytes, f->bytes, f->length);
                rc = EHCICoreSubmit(c, d, e, 0, f->bytes, length,
                    EHCIPlatformMilliseconds() + EHCI_TIMEOUT_MS);
                if (EHCICoreECMDevice(c, gen) != d) return;
            } else {
                rc = EHCICoreSubmit(c, d, e, 0, 0,
                    index == EHCI_EP_BULK_IN ? USB_ECM_RX_BYTES : e->maxPacket, ~(ehci_u64)0);
                if (EHCICoreECMDevice(c, gen) != d) return;
            }
            if (rc) { ecm_offline(c, gen, "transfer submission failed"); return; }
            *active = 1;
        }
    }
}
