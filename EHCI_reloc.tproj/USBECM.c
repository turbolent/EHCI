/* Standard CDC-ECM only: no device IDs, proprietary headers or firmware writes. */
#include "USBECM.h"
#include "EHCIMemory.h"

static int endpoint(const ehci_u8 *p, unsigned length)
{
    return length >= 7 && (p[2] & 15) && !(p[2] & 0x70) &&
        USBCoreReadLE16(p + 4) && !(USBCoreReadLE16(p + 4) & 0xf800);
}

int USBECMFindInterface(const ehci_u8 *bytes, ehci_u16 size, USBECMInterface *out)
{
    USBDescriptorIterator it, sub;
    const ehci_u8 *p, *q;
    ehci_u8 n, type, m, qt;
    int rc, found = 0;
    if (!out || !bytes || size < 9 || bytes[0] < 9 || bytes[1] != 2 ||
        USBCoreReadLE16(bytes + 2) != size || !bytes[5]) return -1;
    bzero(out, sizeof(*out));
    USBCoreDescriptorIteratorInitialize(&it, bytes, size);
    while ((rc = USBCoreDescriptorNext(&it, &p, &n, &type)) > 0)
        if ((type == 4 && n < 9) || (type == 5 && n < 7)) return -1;
    if (rc < 0) return -1;
    USBCoreDescriptorIteratorInitialize(&it, bytes, size);
    while ((rc = USBCoreDescriptorNext(&it, &p, &n, &type)) > 0) {
        USBECMInterface candidate;
        unsigned header = 0, un = 0, ether = 0, bad = 0, dataFound = 0;
        if (type != 4) continue;
        if (n < 9) return -1;
        if (p[5] != 2 || p[6] != 6) continue;
        found = 1;
        bzero(&candidate, sizeof(candidate));
        candidate.configuration = bytes[5];
        candidate.controlInterface = p[2]; candidate.controlAlternate = p[3];
        sub = it;
        while (USBCoreDescriptorNext(&sub, &q, &m, &qt) > 0 && qt != 4) {
            if (qt == 0x24) {
                if (m < 3) { bad = 1; break; }
                if (q[2] == 0) {
                    if (m < 5 || header++) bad = 1;
                } else if (q[2] == 6) {
                    if (m != 5 || un++ || q[3] != p[2] || q[4] == p[2]) bad = 1;
                    else candidate.dataInterface = q[4];
                } else if (q[2] == 0x0f) {
                    if (m < 13 || ether++) bad = 1;
                    else {
                        candidate.macString = q[3];
                        candidate.maxSegment = USBCoreReadLE16(q + 8);
                    }
                }
            } else if (qt == 5) {
                if (!endpoint(q, m) || !(q[2] & 0x80) || (q[3] & 3) != 3 ||
                    candidate.notification || !q[6]) bad = 1;
                else {
                    candidate.notification = q[2]; candidate.interval = q[6];
                    candidate.notificationPacket = USBCoreReadLE16(q + 4);
                    if (candidate.notificationPacket < 8) bad = 1;
                }
            }
        }
        if (bad || !header || !un || !ether || !candidate.macString ||
            candidate.maxSegment < USB_ECM_FRAME_MAX ||
            p[4] != (candidate.notification ? 1 : 0)) continue;
        USBCoreDescriptorIteratorInitialize(&sub, bytes, size);
        while (USBCoreDescriptorNext(&sub, &q, &m, &qt) > 0) {
            USBDescriptorIterator eps;
            const ehci_u8 *e;
            ehci_u8 en, et;
            unsigned count = 0, invalid = 0;
            if (qt != 4 || m < 9 || q[2] != candidate.dataInterface || q[5] != 0x0a) continue;
            if (q[4] != 2) continue;
            candidate.bulkIn = candidate.bulkOut = 0;
            candidate.dataAlternate = q[3];
            eps = sub;
            while (USBCoreDescriptorNext(&eps, &e, &en, &et) > 0 && et != 4) {
                if (et != 5) continue;
                count++;
                if (!endpoint(e, en) || (e[3] & 3) != 2 || e[2] == candidate.notification) {
                    invalid = 1; continue;
                }
                if (e[2] & 0x80) {
                    if (candidate.bulkIn) invalid = 1;
                    candidate.bulkIn = e[2]; candidate.inPacket = USBCoreReadLE16(e + 4);
                } else {
                    if (candidate.bulkOut) invalid = 1;
                    candidate.bulkOut = e[2]; candidate.outPacket = USBCoreReadLE16(e + 4);
                }
            }
            if (!invalid && count == 2 && candidate.bulkIn && candidate.bulkOut) {
                dataFound = 1; break;
            }
        }
        if (dataFound) { *out = candidate; return 1; }
    }
    return rc < 0 || found ? -1 : 0;
}

static int hex_digit(unsigned c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
int USBECMDecodeMAC(const ehci_u8 *s, unsigned length, ehci_u8 *mac)
{
    unsigned i, any = 0;
    if (!s || !mac || length != 26 || s[0] != 26 || s[1] != 3) return 0;
    for (i = 0; i < 6; i++) {
        int a = hex_digit(s[2 + i * 4]), b = hex_digit(s[4 + i * 4]);
        if (a < 0 || b < 0 || s[3 + i * 4] || s[5 + i * 4]) return 0;
        mac[i] = (ehci_u8)((a << 4) | b); any |= mac[i];
    }
    return any && !(mac[0] & 1);
}
int USBECMReadMAC(USBCoreDevice *d, ehci_u8 index, ehci_u8 *mac)
{
    USBSetupPacket s;
    ehci_u8 languages[255], bytes[26];
    ehci_u16 actual;
    if (!index || !USBCoreGetDescriptor(d, 3, 0, languages, sizeof(languages), &actual) ||
        actual < 4 || languages[1] != 3 || languages[0] > actual ||
        languages[0] < 4 || (languages[0] & 1)) return 0;
    s.requestType = 0x80; s.request = 6; s.value = 0x300 | index;
    s.index = USBCoreReadLE16(languages + 2); s.length = sizeof(bytes);
    return USBCoreControlTransfer(d, &s, bytes, &actual) &&
        USBECMDecodeMAC(bytes, actual, mac);
}
int USBECMSetFilter(USBCoreDevice *d, ehci_u8 interfaceNumber, ehci_u16 filter)
{
    USBSetupPacket s;
    s.requestType = 0x21; s.request = 0x43; s.value = filter;
    s.index = interfaceNumber; s.length = 0;
    return USBCoreControlTransfer(d, &s, 0, 0);
}
static ehci_u32 le32(const ehci_u8 *p)
{ return USBCoreReadLE16(p) | ((ehci_u32)USBCoreReadLE16(p + 2) << 16); }

int USBECMNotification(USBECMNotifications *s, unsigned control, unsigned data,
                       const ehci_u8 *bytes, unsigned length)
{
    unsigned i;
    int result = 0;
    if (!s || (!bytes && length)) return -1;
    for (i = 0; i < length; i++) {
        if (s->used >= sizeof(s->bytes)) goto invalid;
        s->bytes[s->used++] = bytes[i];
        if (s->used == 8) {
            unsigned payload = USBCoreReadLE16(s->bytes + 6);
            unsigned interfaceNumber = USBCoreReadLE16(s->bytes + 4);
            /* AX88179B reports the union's data interface here. The endpoint
             * belongs to the control interface, but its notification can name
             * either member of this function. Do not accept unrelated ones. */
            if (s->bytes[0] != 0xa1 || (interfaceNumber != control && interfaceNumber != data) ||
                payload > 8) goto invalid;
            if ((s->bytes[1] == 0 && payload != 0) ||
                (s->bytes[1] == 0x2a && payload != 8)) goto invalid;
            s->expected = 8 + payload;
        }
        if (s->expected && s->used == s->expected) {
            if (s->bytes[1] == 0 && s->expected == 8) {
                unsigned value = USBCoreReadLE16(s->bytes + 2);
                if (value > 1) goto invalid;
                if (!s->linkKnown || s->linkUp != value) result |= 1;
                s->linkKnown = 1; s->linkUp = value;
            } else if (s->bytes[1] == 0x2a && s->expected == 16) {
                ehci_u32 downstream = le32(s->bytes + 8), upstream = le32(s->bytes + 12);
                if (!s->speedKnown || s->downstream != downstream || s->upstream != upstream)
                    result |= 2;
                s->downstream = downstream; s->upstream = upstream; s->speedKnown = 1;
            }
            s->used = s->expected = 0;
        }
    }
    return result;
invalid:
    s->used = s->expected = 0; return -1;
}
unsigned USBECMPrepareFrame(ehci_u8 *out, const ehci_u8 *in, unsigned length)
{
    unsigned padded = length < USB_ECM_FRAME_MIN ? USB_ECM_FRAME_MIN : length;
    if (!out || !in || length < 14 || length > USB_ECM_FRAME_MAX) return 0;
    bcopy(in, out, length);
    if (padded > length) bzero(out + length, padded - length);
    return padded;
}
int USBECMQueuePush(USBECMQueue *q, const ehci_u8 *bytes, unsigned length)
{
    USBECMFrame *f;
    if (!q || !bytes || length < 14 || length > USB_ECM_FRAME_MAX || q->count >= USB_ECM_QUEUE_SIZE) return 0;
    f = &q->frames[(q->head + q->count) % USB_ECM_QUEUE_SIZE];
    f->length = length; bcopy(bytes, f->bytes, length); q->count++; return 1;
}
int USBECMQueuePop(USBECMQueue *q, USBECMFrame *out)
{
    if (!q || !out || !q->count) return 0;
    *out = q->frames[q->head]; q->head = (q->head + 1) % USB_ECM_QUEUE_SIZE;
    q->count--; return 1;
}
