#include "EHCIPCI.h"
#include "EHCIMemory.h"
int EHCIPCIMapRange(unsigned address, unsigned bytes, unsigned pageSize,
                    unsigned *start, unsigned *length, unsigned *offset)
{
    ehci_u64 end;
    if (!bytes || pageSize < 4096 || (pageSize & (pageSize - 1))) return 0;
    end = ((ehci_u64)address + bytes + pageSize - 1) & ~(ehci_u64)(pageSize - 1);
    *start = address & ~(pageSize - 1); *offset = address - *start;
    if (end > 0x100000000ULL || end - *start > 0xffffffffU) return 0;
    *length = (unsigned)(end - *start);
    return 1;
}
static int readpci(const EHCIPCIOps *o, void *c, unsigned r, ehci_u32 *v)
{ return o->read(c, r, v) && *v != 0xffffffffU; }
int EHCIPCICommand(const EHCIPCIOps *o, void *c, unsigned set, unsigned clear)
{
    ehci_u32 v, after;
    if (!readpci(o, c, 4, &v) || (v & 65535) == 65535) return 0;
    v = ((v & 65535) | set) & ~clear;
    /* Upper half is W1C PCI Status: never echo it. */
    return o->write(c, 4, v) && readpci(o, c, 4, &after) && (after & 65535) == v;
}
int EHCIPCIDisableMessages(const EHCIPCIOps *o, void *c)
{
    ehci_u32 v, after;
    unsigned seen[8], offset, count = 0, msi = 0, msix = 0;
    bzero(seen, sizeof(seen));
    if (!readpci(o, c, 4, &v)) return 0;
    if (!(v & (1U << 20))) return 1;
    if (!readpci(o, c, 0x34, &v)) return 0;
    offset = v & 255;
    while (offset) {
        unsigned next, mask = 0, set = 0;
        if (offset < 0x40 || offset > 0xfc || (offset & 3) || ++count > 48 ||
            (seen[offset / 32] & (1U << (offset % 32)))) return 0;
        seen[offset / 32] |= 1U << (offset % 32);
        if (!readpci(o, c, offset, &v)) return 0;
        next = (v >> 8) & 255;
        if ((v & 255) == 5) { if (msi++) return 0; mask = 1U << 16; }
        if ((v & 255) == 0x11) { if (msix++) return 0; mask = 1U << 31; set = 1U << 30; }
        if ((v & 255) == 1) {
            if (offset > 0xf8 || !readpci(o, c, offset + 4, &after) || (after & 3)) return 0;
        }
        if (mask && (!o->write(c, offset, (v & ~mask) | set) ||
            !readpci(o, c, offset, &after) || (after & mask) || (after & set) != set)) return 0;
        offset = next;
    }
    return 1;
}
int EHCIPCIBar(const EHCIPCIOps *o, void *c, unsigned *address, unsigned *bytes)
{
    ehci_u32 low, high = 0, ml = 0, mh = 0xffffffffU, after, command;
    ehci_u64 size;
    int wide, valid = 0, restored = 1;
    if (!readpci(o, c, 4, &command) || (command & 65535) == 65535 ||
        !(command & EHCI_PCI_INT_DISABLE) || (command & EHCI_PCI_MASTER) ||
        !readpci(o, c, 0x10, &low) || !(low & ~15U) || (low & 1) ||
        ((low & 6) != 0 && (low & 6) != 4)) return 0;
    wide = (low & 6) == 4;
    if (wide && (!readpci(o, c, 0x14, &high) || high)) return 0;
    if (!EHCIPCICommand(o, c, EHCI_PCI_INT_DISABLE, EHCI_PCI_MEMORY | 1)) return 0;
    if (o->write(c, 0x10, 0xffffffffU) && (!wide || o->write(c, 0x14, 0xffffffffU)) &&
        o->read(c, 0x10, &ml) && (!wide || o->read(c, 0x14, &mh))) valid = 1;
    if (!o->write(c, 0x10, low) || !readpci(o, c, 0x10, &after) || after != low) restored = 0;
    if (wide && (!o->write(c, 0x14, high) || !readpci(o, c, 0x14, &after) || after != high)) restored = 0;
    /* Never reopen decode after an unverified BAR restore. */
    if (!restored) return 0;
    if (!EHCIPCICommand(o, c, (command & 65535) | EHCI_PCI_MEMORY, 0)) return 0;
    if (!valid) return 0;
    size = ~(((ehci_u64)mh << 32) | (ml & ~15U)) + 1;
    if (size < 0x100 || size > 0x100000 || (size & (size - 1)) ||
        (low & ~15U) & ((unsigned)size - 1) || (ehci_u64)(low & ~15U) + size > 0x100000000ULL) return 0;
    *address = low & ~15U; *bytes = (unsigned)size;
    return 1;
}
