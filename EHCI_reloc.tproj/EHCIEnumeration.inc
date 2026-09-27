/* Included by EHCICore.c. All management runs on one sleepable worker. */
static int device_request(EHCIDevice *d, unsigned type, unsigned request,
                           unsigned value, unsigned index, void *data, unsigned size)
{
    USBSetupPacket s;
    ehci_u16 actual = 0;
    s.requestType = type; s.request = request; s.value = value;
    s.index = index; s.length = size;
    return USBCoreControlTransfer(&d->usb, &s, data, &actual) && actual == size;
}
static unsigned new_generation(EHCIControllerState *c)
{
    if (c->nextGeneration == 0xffffffffU) { EHCICoreFail(c, "generation exhausted", 0); return 0; }
    return ++c->nextGeneration;
}
static unsigned allocate_address(EHCIControllerState *c)
{
    unsigned a;
    for (a = 1; a < 128; a++) if (!(c->addresses[a / 32] & (1U << (a % 32)))) {
        c->addresses[a / 32] |= 1U << (a % 32); return a;
    }
    return 0;
}
static int child_slot(EHCIControllerState *c, unsigned parent, unsigned port)
{
    unsigned i;
    for (i = 0; i < EHCI_MAX_DEVICES; i++)
        if (c->devices[i].used && c->devices[i].parent == parent &&
            (parent ? c->devices[i].parentPort : c->devices[i].port) == port) return (int)i;
    return -1;
}

/* An addressed but unsupported/offline device still answers USB tokens.
 * Isolate its physical port before releasing the address. Failure escalates
 * to controller containment and retains all ownership/DMA for that device. */
static int root_isolated(EHCIControllerState *c, unsigned rootPort)
{
    ehci_u32 v;
    if (!rootPort || rootPort > c->maxPorts) return 0;
    v = read_op(c, EHCI_PORT(rootPort));
    return !c->fatal && (!(v & EHCI_PORT_CONNECT) ||
                        !(v & (EHCI_PORT_ENABLE | EHCI_PORT_RESET)));
}

static int isolate_port(EHCIControllerState *c, unsigned rootPort,
                        unsigned parent, unsigned parentPort)
{
    EHCIDevice *hub;
    ehci_u32 v;
    ehci_u8 status[4];
    if (!parent) {
        if (!rootPort || rootPort > c->maxPorts) return 0;
        v = read_op(c, EHCI_PORT(rootPort));
        if (c->fatal) return 0;
        if (!(v & EHCI_PORT_CONNECT)) return 1;
        c->blockedPorts |= 1U << rootPort;
        return write_op(c, EHCI_PORT(rootPort),
                        EHCIPortWriteValue(v, 0, EHCI_PORT_ENABLE, 0)) &&
               wait_bits(c, EHCI_PORT(rootPort), EHCI_PORT_ENABLE | EHCI_PORT_RESET, 0, 100);
    }
    if (parent > EHCI_MAX_DEVICES || !parentPort ||
        parentPort > EHCI_MAX_HUB_PORTS) return 0;
    /* Child transfer errors can be serviced before the root unplug event.
     * A disabled/disconnected upstream port needs no request to the dead hub. */
    if (root_isolated(c, rootPort)) return 1;
    if (c->fatal) return 0;
    hub = &c->devices[parent - 1];
    if (!hub->used || !hub->ready || !hub->hub || hub->disconnecting ||
        !device_request(hub, 0xa3, 0, 0, parentPort, status, 4))
        return root_isolated(c, rootPort);
    if (!(USBCoreReadLE16(status) & 1)) return 1;
    hub->blockedPorts |= 1U << parentPort;
    if (device_request(hub, 0x23, 1, 1, parentPort, 0, 0) &&
        device_request(hub, 0xa3, 0, 0, parentPort, status, 4) &&
        !(USBCoreReadLE16(status) & 0x12)) return 1;
    /* Hub control sleeps release the lock; unplug may occur during them. */
    return root_isolated(c, rootPort);
}

static void disconnect_device(EHCIControllerState *c, unsigned slot, int isolated)
{
    EHCIDevice *d;
    unsigned i, generation;
    static const ehci_u8 released[8] = {0,0,0,0,0,0,0,0};
    if (slot >= EHCI_MAX_DEVICES || !c->devices[slot].used) return;
    d = &c->devices[slot];
    d->disconnecting = 1; d->ready = 0;
    for (i = 0; i < USB_STORAGE_TARGETS; i++) if (c->storage[i].slot == slot + 1) {
        c->storage[i].slot = 0;
        c->storage[i].generation = new_generation(c);
        c->storage[i].removals++;
        EHCIPlatformStorageWake(c->owner, i);
    }
    if (c->keyboardSlot == slot) {
        c->keyboardSlot = EHCI_MAX_DEVICES;
        EHCIPlatformKeyboardReport(c->owner, released, sizeof(released));
    }
    if (c->pointerSlot == slot) {
        c->pointerSlot = EHCI_MAX_DEVICES;
        EHCIPlatformPointerReport(c->owner, released, 3);
    }
    if (!isolated && !isolate_port(c, d->port, d->parent, d->parentPort)) {
        EHCICoreFail(c, "disconnect isolation", slot); return;
    }
    /* Isolating an upstream hub also isolates all its children; its EP0
     * cannot be used after this point to disable downstream ports. */
    for (i = 0; i < EHCI_MAX_DEVICES; i++)
        if (c->devices[i].used && c->devices[i].parent == slot + 1)
            disconnect_device(c, i, 1);
    /* Failure retains descriptors, address and slot until global containment. */
    if (!schedule_enter(c, EHCI_CMD_ASE | EHCI_CMD_PSE)) return;
    /* Retire every old QH before clearing TT state. Hub control needs the
     * schedules running, so retain address/slot/DMA across that second phase. */
    for (i = 0; i < EHCI_ENDPOINTS; i++) {
        EHCIEndpoint *e = &d->endpoints[i];
        if (!e->configured) continue;
        e->linked = e->waiting = 0;
        e->qh->next = e->qh->alternate = EHCI_LINK_END;
        e->qh->token = EHCI_QTD_HALTED;
    }
    if (!schedule_leave(c)) return;
    if (!isolated && d->parent && d->speed != EHCI_SPEED_HIGH) {
        for (i = 0; i < EHCI_ENDPOINTS; i++) {
            EHCIEndpoint *e = &d->endpoints[i];
            if (e->configured && !e->periodic && !clear_tt(c, d, e)) {
                EHCICoreFail(c, "disconnect TT clear", slot); return;
            }
        }
    }
    /* All child QHs remain unlinked while other schedule users run. */
    generation = new_generation(c);
    c->addresses[d->address / 32] &= ~(1U << (d->address % 32));
    c->addresses[0] |= 1;
    for (i = 0; i < EHCI_ENDPOINTS; i++) endpoint_free(&d->endpoints[i]);
    bzero(d, sizeof(*d)); d->generation = generation;
    c->disconnected &= ~(1U << slot);
}

void EHCICoreDisconnect(EHCIControllerState *c, unsigned slot)
{
    disconnect_device(c, slot, 0);
}

static int clear_tt(EHCIControllerState *c, EHCIDevice *d, EHCIEndpoint *e)
{
    EHCIDevice *hub;
    unsigned value, port, generation = d->generation;
    int ok;
    if (d->speed == EHCI_SPEED_HIGH || e->periodic || !e->count) return 1;
    if (!d->parent || d->parent > EHCI_MAX_DEVICES) return 0;
    if (root_isolated(c, d->port)) return 1;
    if (c->fatal) return 0;
    hub = &c->devices[d->parent - 1];
    if (!hub->ready || !hub->hub || hub->disconnecting) return 0;
    port = hub->hubProtocol == 2 ? d->ttPort : 1;
    /* Failed SET_ADDRESS can leave d->address holding the allocator's
     * reservation. The QH records the address actually used on the wire. */
    value = (e->address & 15) | ((e->qh->endpoint & 127) << 4) |
        (e == &d->endpoints[0] ? 0 : (e->periodic ? 3U : 2U) << 11) |
        ((e->address & 0x80) ? 0x8000U : 0);
    ok = device_request(hub, 0x23, 8, value, port, 0, 0);
    if (!d->used || d->generation != generation) return 0;
    /* A control pipe has both directions in the TT. */
    if (ok && e == &d->endpoints[0])
        ok = device_request(hub, 0x23, 8, value | 0x8000U, port, 0, 0);
    if (!d->used || d->generation != generation) return 0;
    if (ok || root_isolated(c, d->port)) return 1;
    /* A live TT with unretired async state cannot safely service new work.
     * An ongoing disconnect instead owns physical containment and cleanup. */
    if (!d->disconnecting && !hub->disconnecting) EHCICoreFail(c, "TT clear", d->address);
    return 0;
}

/* USB 2.0 sections 7.2.1 and 11.13: descriptor capability alone does not
 * establish the current supply. Reserve one unit for every external port. */
static int hub_power(EHCIDevice *d, unsigned *selfPowered)
{
    ehci_u8 deviceStatus[2], hubStatus[4];
    if (!device_request(d, 0x80, 0, 0, 0, deviceStatus, 2) ||
        !device_request(d, 0xa0, 0, 0, 0, hubStatus, 4) ||
        (hubStatus[0] & 2)) return 0;
    *selfPowered = d->selfPowerCapable && (deviceStatus[0] & 1) && !(hubStatus[0] & 1);
    if (*selfPowered) return d->powerMA <= 100;
    return d->powerMA && d->externalPorts <= 4 &&
           d->powerMA + d->externalPorts * 100 <= 500 &&
           !(d->hubCharacteristics & 2);
}

static void hub_power_off(EHCIControllerState *c, EHCIDevice *d)
{
    unsigned p;
    ehci_u8 status[4];
    /* Disable the entire subtree on loss/uncertainty of its supply. A USB
     * port-disable alone would leave configured high-current loads powered. */
    c->disconnected |= 1U << (d - c->devices);
    for (p = 1; p <= d->hubPorts && !c->fatal; p++) {
        if (!device_request(d, 0x23, 1, 8, p, 0, 0) ||
            !device_request(d, 0xa3, 0, 0, p, status, 4) ||
            (USBCoreReadLE16(status) & 0x100)) {
            if (!root_isolated(c, d->port)) EHCICoreFail(c, "hub power isolation", p);
            return;
        }
        d->portPowerMA[p] = 0;
    }
}

static int configuration_power(EHCIControllerState *c, EHCIDevice *d)
{
    EHCIDevice *hub;
    unsigned selfPowered, i, total;
    if (d->powerMA > 500 || (!d->powerMA && !d->selfPowerCapable)) return 0;
    if (!d->parent) return 1;
    hub = &c->devices[d->parent - 1];
    if (!hub->ready || hub->disconnecting ||
        (c->disconnected & (1U << (d->parent - 1)))) return 0;
    if (!hub_power(hub, &selfPowered) || (hub->selfPowered && !selfPowered)) {
        hub_power_off(c, hub); return 0;
    }
    hub->selfPowered = selfPowered;
    if (!selfPowered && (hub->nonRemovable & (1U << d->parentPort))) {
        total = hub->powerMA + hub->externalPorts * 100 + d->powerMA;
        for (i = 1; i <= hub->hubPorts; i++)
            if (i != d->parentPort) total += hub->portPowerMA[i];
        if (total > 500) return 0;
        /* Reserve before SET_CONFIGURATION: a failed status stage can still
         * leave the function configured. PORT_ENABLE clear does not release
         * its supply; retain this charge independently of the device slot. */
        hub->portPowerMA[d->parentPort] = d->powerMA;
        return 1;
    }
    return d->powerMA <= (selfPowered ? 500U : 100U);
}

static int configure_hub(EHCIControllerState *c, EHCIDevice *d, USBEnumerationData *info)
{
    USBDescriptorIterator it;
    const ehci_u8 *desc;
    ehci_u8 len, type, hubdesc[16];
    unsigned alt, chosen = 0, iface = 0, endpoint = 0, packet = 0, interval = 0;
    unsigned candidate = 0, p, delay, descriptorLength;
    int rc;
    if (d->parent || d->speed != EHCI_SPEED_HIGH) return 0;
    /* Prefer the multi-TT alternate, falling back to the single-TT alternate. */
    for (alt = 2; alt; alt--) {
        unsigned desired = alt - 1;
        candidate = 0; endpoint = 0;
        USBCoreDescriptorIteratorInitialize(&it, info->configurationDescriptor, info->configurationLength);
        while ((rc = USBCoreDescriptorNext(&it, &desc, &len, &type)) == USB_DESCRIPTOR_FOUND) {
            if (type == USB_DESC_INTERFACE && len >= 9) {
                candidate = desc[5] == 9 && desc[3] == desired &&
                    ((info->deviceDescriptor[6] == 1 && !desired && !desc[7]) ||
                     (info->deviceDescriptor[6] == 2 && desc[7] == desired + 1));
                if (candidate) { iface = desc[2]; chosen = desired + 1; }
            } else if (candidate && type == USB_DESC_ENDPOINT && len >= 7 &&
                       (desc[3] & 3) == 3 && (desc[2] & 0x80) && (desc[2] & 15)) {
                endpoint = desc[2]; packet = USBCoreReadLE16(desc + 4); interval = desc[6];
                break;
            }
        }
        if (endpoint) break;
    }
    if (!endpoint || !configuration_power(c, d) ||
        !USBCoreSetConfiguration(&d->usb, info->configurationValue) ||
        (alt == 2 && !USBCoreSetInterface(&d->usb, iface, 1))) return 0;
    bzero(hubdesc, sizeof(hubdesc));
    if (!device_request(d, 0xa0, USB_REQ_GET_DESCRIPTOR, 0x2900, 0, hubdesc, 7) ||
        hubdesc[0] < 7 || hubdesc[1] != 0x29 || !hubdesc[2] || hubdesc[2] > EHCI_MAX_HUB_PORTS)
        return 0;
    d->hub = 1; d->hubPorts = hubdesc[2]; d->hubProtocol = chosen;
    d->hubCharacteristics = USBCoreReadLE16(hubdesc + 3);
    d->ttThinkTime = (d->hubCharacteristics >> 5) & 3;
    descriptorLength = 7 + 2 * ((d->hubPorts + 8) / 8);
    if (hubdesc[0] < descriptorLength ||
        !device_request(d, 0xa0, USB_REQ_GET_DESCRIPTOR, 0x2900, 0, hubdesc, descriptorLength) ||
        hubdesc[1] != 0x29 || hubdesc[2] != d->hubPorts) return 0;
    for (p = 1; p <= d->hubPorts; p++) {
        if (hubdesc[7 + p / 8] & (1U << (p % 8))) d->nonRemovable |= 1U << p;
        else d->externalPorts++;
    }
    if (!hub_power(d, &d->selfPowered)) return 0;
    if (packet < (d->hubPorts + 8) / 8 ||
        !EHCICoreConfigureEndpoint(c, d, EHCI_EP_INTERRUPT, endpoint, packet, interval)) return 0;
    for (p = 1; p <= d->hubPorts; p++)
        if (!device_request(d, 0x23, 3, 8, p, 0, 0)) return 0;
    delay = (unsigned)hubdesc[5] * 2 + 20;
    EHCIPlatformPause(c, delay);
    d->hubChanges = (1U << (d->hubPorts + 1)) - 1;
    return 1;
}

static int attach_device(EHCIControllerState *c, unsigned rootPort,
                         unsigned parent, unsigned parentPort, unsigned speed)
{
    EHCIDevice *d;
    USBEnumerationData info;
    USBHIDInterface hid;
    ehci_u8 first[8];
    ehci_u16 actual;
    unsigned slot, address, target;
    const char *stage = "endpoint zero";
    bzero(&info, sizeof(info));
    bzero(&hid, sizeof(hid));
    for (slot = 0; slot < EHCI_MAX_DEVICES && c->devices[slot].used; slot++) {}
    if (slot == EHCI_MAX_DEVICES || !(address = allocate_address(c))) {
        /* A reset port also needs isolation if no slot/address is available:
         * otherwise it still responds at address zero during the next attach. */
        if (!isolate_port(c, rootPort, parent, parentPort)) EHCICoreFail(c, "no address isolation", rootPort);
        return 0;
    }
    d = &c->devices[slot]; bzero(d, sizeof(*d));
    d->used = 1; d->generation = new_generation(c); d->port = rootPort;
    d->parent = parent; d->parentPort = parentPort; d->speed = speed;
    if (parent && speed != EHCI_SPEED_HIGH) {
        d->ttAddress = c->devices[parent - 1].address; d->ttPort = parentPort;
    }
    USBCoreDeviceInitialize(&d->usb, &usb_ops, c, d);
    if (!EHCICoreConfigureEndpoint(c, d, 0, 0, speed == EHCI_SPEED_HIGH ? 64 : 8, 0)) goto fail;
    stage = "initial descriptor";
    if (!USBCoreGetDescriptor(&d->usb, 1, 0, first, 8, &actual) || actual != 8 ||
        first[0] < 18 || first[1] != 1 || !update_ep0(c, d, first[7])) goto fail;
    stage = "set address";
    if (!device_request(d, 0, 5, address, 0, 0, 0)) goto fail;
    d->address = address;
    EHCIPlatformPause(c, 2);
    stage = "device/configuration descriptors";
    if (!USBCoreEnumerateDevice(&d->usb, &info)) goto fail;
    d->configurationValue = info.configurationValue;
    d->powerMA = (unsigned)info.configurationDescriptor[8] * 2;
    d->selfPowerCapable = (info.configurationDescriptor[7] & 0x40) != 0;
    if (info.deviceDescriptor[4] == 9) {
        stage = "hub configuration";
        if (!configure_hub(c, d, &info)) goto fail;
    } else if (USBHIDFindBootInterface(info.configurationDescriptor, info.configurationLength, &hid)) {
        stage = "duplicate HID provider";
        if ((hid.protocol == USB_HID_PROTOCOL_KEYBOARD && c->keyboardSlot != EHCI_MAX_DEVICES) ||
            (hid.protocol == USB_HID_PROTOCOL_MOUSE && c->pointerSlot != EHCI_MAX_DEVICES)) goto fail;
        stage = "HID power budget";
        if (!configuration_power(c, d)) goto fail;
        stage = "HID set configuration";
        if (!USBCoreSetConfiguration(&d->usb, hid.configurationValue)) goto fail;
        stage = "HID set boot protocol";
        if (!USBHIDSetBootProtocol(&d->usb, hid.interfaceNumber)) goto fail;
        stage = "HID interrupt endpoint";
        if (!EHCICoreConfigureEndpoint(c, d, EHCI_EP_INTERRUPT, hid.endpointAddress, hid.maxPacket, hid.interval)) goto fail;
        /* SET_IDLE is optional on some otherwise valid boot mice. */
        USBHIDSetIdle(&d->usb, hid.interfaceNumber);
        d->protocol = hid.protocol; d->interfaceNumber = hid.interfaceNumber;
        if (hid.protocol == USB_HID_PROTOCOL_KEYBOARD) c->keyboardSlot = slot;
        else c->pointerSlot = slot;
    } else if (USBMassStorageFindInterface(info.configurationDescriptor, info.configurationLength, &d->storageInterface)) {
        stage = "storage configuration";
        for (target = 0; target < USB_STORAGE_TARGETS && c->storage[target].slot; target++) {}
        if (target == USB_STORAGE_TARGETS ||
            !configuration_power(c, d) ||
            !USBCoreSetConfiguration(&d->usb, d->storageInterface.configuration) ||
            !EHCICoreConfigureEndpoint(c, d, EHCI_EP_BULK_IN, d->storageInterface.bulkIn, d->storageInterface.inPacket, 0) ||
            !EHCICoreConfigureEndpoint(c, d, EHCI_EP_BULK_OUT, d->storageInterface.bulkOut, d->storageInterface.outPacket, 0))
            goto fail;
        d->storage = 1;
        c->storage[target].generation = new_generation(c);
        c->storage[target].slot = slot + 1; c->storage[target].lastPort = rootPort;
    } else { stage = "unsupported USB interface"; goto fail; }
    d->ready = 1;
    stage = "first interrupt transfer";
    if (d->endpoints[EHCI_EP_INTERRUPT].configured &&
        EHCICoreSubmit(c, d, &d->endpoints[EHCI_EP_INTERRUPT], 0, 0,
                       d->endpoints[EHCI_EP_INTERRUPT].maxPacket, ~(ehci_u64)0)) goto fail;
    EHCIPlatformLog("EHCI: port %u address %u %s ready (%04x:%04x, speed %u)\n",
        rootPort, address, d->hub ? "hub" : d->storage ? "storage" :
        d->protocol == 1 ? "keyboard" : "mouse", info.vendorID, info.productID, speed);
    if (d->storage) EHCIPlatformStorageAttached(c->owner);
    return 1;
fail:
    c->enumerationFailure.stage = stage;
    c->enumerationFailure.rootPort = rootPort;
    c->enumerationFailure.hubPort = parentPort;
    c->enumerationFailure.speed = speed;
    c->enumerationFailure.vendor = info.vendorID;
    c->enumerationFailure.product = info.productID;
    c->enumerationFailure.protocol = hid.protocol;
    c->enumerationFailure.packet = hid.maxPacket;
    c->enumerationFailure.interval = hid.interval;
    EHCIPlatformLog("EHCI: port %u hub port %u address %u enumeration rejected at %s (%04x:%04x, speed %u, HID %u packet %u interval %u)\n",
        rootPort, parentPort, address, stage, info.vendorID, info.productID,
        speed, hid.protocol, hid.maxPacket, hid.interval);
    /* SET_ADDRESS may have succeeded even if its status stage failed.
     * Keep the reserved address until physical isolation is verified. */
    d->address = address;
    EHCICoreDisconnect(c, slot);
    return 0;
}

static int hub_status(EHCIDevice *d, unsigned port, unsigned *status, unsigned *change)
{
    ehci_u8 b[4];
    if (!device_request(d, port ? 0xa3 : 0xa0, 0, 0, port, b, 4)) return 0;
    *status = USBCoreReadLE16(b); *change = USBCoreReadLE16(b + 2);
    return 1;
}
static int debounce_port(EHCIControllerState *c, EHCIDevice *hub,
                          unsigned port, unsigned *status)
{
    unsigned change;
    ehci_u64 end = EHCIPlatformMilliseconds() + 1500;
    /* Connection-change bits are sticky and only this management worker
     * acknowledges them. Any bounce restarts the full 100 ms interval. */
    do {
        EHCIPlatformPause(c, 100);
        if (hub) {
            if (!hub_status(hub, port, status, &change) ||
                !(*status & 1) || (*status & 8)) return 0;
            if (!(change & 1)) return 1;
            if (!device_request(hub, 0x23, 1, 16, port, 0, 0)) return 0;
        } else {
            *status = read_op(c, EHCI_PORT(port));
            if (c->fatal || !(*status & EHCI_PORT_CONNECT) ||
                (*status & (EHCI_PORT_OWNER | EHCI_PORT_OC))) return 0;
            if (!(*status & EHCI_PORT_CSC)) return 1;
            if (!write_op(c, EHCI_PORT(port),
                EHCIPortWriteValue(*status, 0, 0, EHCI_PORT_CSC))) return 0;
        }
    } while (EHCIPlatformMilliseconds() < end && !c->fatal);
    return 0;
}
static void service_hub(EHCIControllerState *c, unsigned slot)
{
    EHCIDevice *d = &c->devices[slot];
    unsigned p, status, change, changes = d->hubChanges;
    d->hubChanges = 0;
    if (changes & 1) {
        unsigned selfPowered;
        if (!hub_status(d, 0, &status, &change) ||
            !hub_power(d, &selfPowered) || (d->selfPowered && !selfPowered)) {
            hub_power_off(c, d); return;
        }
        d->selfPowered = selfPowered;
        if (change & 1) device_request(d, 0x20, 1, 0, 0, 0, 0);
        if (change & 2) device_request(d, 0x20, 1, 1, 0, 0, 0);
        if (status & 2) { c->disconnected |= 1U << slot; return; }
    }
    for (p = 1; p <= d->hubPorts && !c->fatal &&
        !(c->disconnected & (1U << slot)); p++) if (changes & (1U << p)) {
        int child;
        unsigned bit;
        ehci_u64 end;
        if (!hub_status(d, p, &status, &change)) { c->disconnected |= 1U << slot; return; }
        if (!(status & 1)) d->portPowerMA[p] = 0;
        for (bit = 0; bit < 5; bit++) if (change & (1U << bit))
            if (!device_request(d, 0x23, 1, 16 + bit, p, 0, 0)) return;
        child = child_slot(c, slot + 1, p);
        if (child >= 0 && (!(status & 1) || !(status & 2) || (change & 1) || (status & 8))) {
            EHCICoreDisconnect(c, child); child = -1;
        }
        if ((change & 1) || !(status & 1)) d->blockedPorts &= ~(1U << p);
        if (c->fatal || !(status & 1) || (status & 8) || child >= 0 ||
            (d->blockedPorts & (1U << p))) continue;
        if (!debounce_port(c, d, p, &status)) goto reject_port;
        if (!device_request(d, 0x23, 3, 4, p, 0, 0)) goto reject_port;
        end = EHCIPlatformMilliseconds() + 500;
        do {
            EHCIPlatformPause(c, 10);
            if (!hub_status(d, p, &status, &change)) goto reject_port;
            if (!(status & 0x10)) break;
        } while (EHCIPlatformMilliseconds() < end);
        if ((status & 0x13) != 3) goto reject_port;
        if (!device_request(d, 0x23, 1, 20, p, 0, 0)) goto reject_port;
        /* A completed USB reset returns the function to default power. */
        d->portPowerMA[p] = 0;
        EHCIPlatformPause(c, 10);
        attach_device(c, d->port, slot + 1, p, (status & 0x400) ? EHCI_SPEED_HIGH :
            (status & 0x200) ? EHCI_SPEED_LOW : EHCI_SPEED_FULL);
        continue;
reject_port:
        /* A reset may have enabled an unaddressed device even if its control
         * status/acknowledgement failed. Do not expose a second address zero. */
        if (!isolate_port(c, d->port, slot + 1, p)) EHCICoreFail(c, "hub port isolation", p);
    }
}

int EHCICoreServicePorts(EHCIControllerState *c)
{
    unsigned p, i, changes;
    if (!c->running || c->fatal || c->enumerating) return 0;
    c->enumerating = 1;
    for (i = 0; i < EHCI_MAX_DEVICES && !c->fatal; i++)
        if (c->disconnected & (1U << i)) EHCICoreDisconnect(c, i);
    changes = c->portChanges; c->portChanges = 0;
    for (p = 1; p <= c->maxPorts && !c->fatal; p++) if (changes & (1U << p)) {
        ehci_u32 v = read_op(c, EHCI_PORT(p));
        int child = child_slot(c, 0, p);
        if (c->fatal) break;
        if (!write_op(c, EHCI_PORT(p), EHCIPortWriteValue(v, 0, 0, v))) break;
        if (child >= 0 && (!(v & EHCI_PORT_CONNECT) || (v & EHCI_PORT_CSC) ||
            !(v & EHCI_PORT_ENABLE) || (v & EHCI_PORT_OC))) {
            EHCICoreDisconnect(c, child); child = -1;
        }
        if ((v & EHCI_PORT_CSC) || !(v & EHCI_PORT_CONNECT)) c->blockedPorts &= ~(1U << p);
        if (c->fatal || !(v & EHCI_PORT_CONNECT) || (v & (EHCI_PORT_OWNER | EHCI_PORT_OC)) ||
            child >= 0 || (c->blockedPorts & (1U << p))) continue;
        if (!debounce_port(c, 0, p, &v)) {
            if (!c->fatal && !isolate_port(c, p, 0, 0)) EHCICoreFail(c, "debounce isolation", p);
            continue;
        }
        if ((v & EHCI_PORT_LINE) != EHCI_PORT_LOW) {
            write_op(c, EHCI_PORT(p), EHCIPortWriteValue(v, EHCI_PORT_RESET, EHCI_PORT_ENABLE, 0));
            EHCIPlatformPause(c, 50);
            v = read_op(c, EHCI_PORT(p));
            write_op(c, EHCI_PORT(p), EHCIPortWriteValue(v, 0, EHCI_PORT_RESET, 0));
            if (!wait_bits(c, EHCI_PORT(p), EHCI_PORT_RESET, 0, 100)) {
                /* A late reset completion could enable address zero after
                 * we move to another port. Its isolation is unproven. */
                EHCICoreFail(c, "root reset", p); break;
            }
            EHCIPlatformPause(c, 10);
            v = read_op(c, EHCI_PORT(p));
        }
        if ((v & (EHCI_PORT_CONNECT | EHCI_PORT_ENABLE)) == (EHCI_PORT_CONNECT | EHCI_PORT_ENABLE))
            attach_device(c, p, 0, 0, EHCI_SPEED_HIGH);
        else {
            if (c->companions)
                write_op(c, EHCI_PORT(p), EHCIPortWriteValue(v, EHCI_PORT_OWNER, 0, 0));
            EHCIPlatformLog("EHCI: port %u low/full-speed device requires a USB2 hub or companion driver\n", p);
        }
    }
    for (i = 0; i < EHCI_MAX_DEVICES && !c->fatal; i++)
        if (c->devices[i].ready && c->devices[i].hub && c->devices[i].hubChanges)
            service_hub(c, i);
    c->enumerating = 0;
    return !c->fatal;
}
int EHCICoreSetKeyboardLEDs(EHCIControllerState *c, ehci_u8 leds)
{
    EHCIDevice *d;
    if (c->keyboardSlot >= EHCI_MAX_DEVICES || c->fatal) return 0;
    d = &c->devices[c->keyboardSlot];
    return d->ready && USBHIDSetKeyboardLEDs(&d->usb, d->interfaceNumber, leds);
}
