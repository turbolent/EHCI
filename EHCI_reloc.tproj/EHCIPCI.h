#ifndef EHCI_PCI_H
#define EHCI_PCI_H
#include "EHCIRegs.h"
typedef struct EHCIPCIOps {
    int (*read)(void *, unsigned, ehci_u32 *);
    int (*write)(void *, unsigned, ehci_u32);
} EHCIPCIOps;
int EHCIPCICommand(const EHCIPCIOps *, void *, unsigned set, unsigned clear);
int EHCIPCIDisableMessages(const EHCIPCIOps *, void *);
int EHCIPCIBar(const EHCIPCIOps *, void *, unsigned *address, unsigned *bytes);
int EHCIPCIMapRange(unsigned address, unsigned bytes, unsigned pageSize,
                    unsigned *start, unsigned *length, unsigned *offset);
#endif
