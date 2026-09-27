#ifndef EHCI_INTERRUPT_STATE_H
#define EHCI_INTERRUPT_STATE_H
#include "EHCIRegs.h"
#define EHCI_MODE_INVALID 0U
#define EHCI_MODE_INTX 1U
#define EHCI_MODE_POLLING 2U
typedef struct EHCIInterruptOps {
    ehci_u32 (*read)(void *, unsigned);
    int (*write)(void *, unsigned, ehci_u32);
    int (*pciRead)(void *, ehci_u32 *);
    int (*pciWrite)(void *, ehci_u32);
    int (*rearm)(void *);
    void (*publish)(void *);
} EHCIInterruptOps;
typedef struct EHCIInterruptState {
    unsigned mode, participant, active, generation;
    volatile unsigned stopping, admitted;
    unsigned mmioLost, interruptDebt, dmaDebt, rearmDebt;
    unsigned fenced, dmaFenced, pending, work;
    unsigned callbacks, foreign, owned, retries, failures, services;
    unsigned watchdogPending;
    ehci_u64 watchdogSince;
    const EHCIInterruptOps *ops;
    void *context;
} EHCIInterruptState;
unsigned EHCIInterruptModeParse(const char *);
/* All functions below require the native short MMIO/PCI/rearm lock. */
void EHCIInterruptLost(EHCIInterruptState *);
int EHCIInterruptFence(EHCIInterruptState *, int dma);
void EHCIInterruptCallback(EHCIInterruptState *);
void EHCIInterruptRetry(EHCIInterruptState *);
void EHCIInterruptWatchdog(EHCIInterruptState *, ehci_u64 now);
unsigned EHCIInterruptTakeWork(EHCIInterruptState *);
int EHCIInterruptRestore(EHCIInterruptState *);
void EHCIInterruptPoll(EHCIInterruptState *);
void EHCIInterruptStop(EHCIInterruptState *);
#endif
