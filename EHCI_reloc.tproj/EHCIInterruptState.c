#include "EHCIInterruptState.h"
#include <string.h>
unsigned EHCIInterruptModeParse(const char *s)
{
    if (!s || !strcmp(s, "INTx")) return EHCI_MODE_INTX;
    if (!strcmp(s, "Polling")) return EHCI_MODE_POLLING;
    return EHCI_MODE_INVALID;
}
void EHCIInterruptLost(EHCIInterruptState *s)
{
    if (!s->mmioLost) s->generation++;
    s->mmioLost = 1; s->stopping = 1; s->active = 0;
    s->interruptDebt = s->dmaDebt = 1;
    s->fenced = s->dmaFenced = 0;
    s->work = 1; s->ops->publish(s->context);
}
static ehci_u32 rd(EHCIInterruptState *s, unsigned reg)
{
    ehci_u32 v;
    if (s->mmioLost) return 0xffffffffU;
    v = s->ops->read(s->context, reg);
    if (v == 0xffffffffU) EHCIInterruptLost(s);
    return v;
}
static int wr(EHCIInterruptState *s, unsigned reg, ehci_u32 v)
{
    if (s->mmioLost) return 0;
    if (!s->ops->write(s->context, reg, v)) { EHCIInterruptLost(s); return 0; }
    return 1;
}
static void acknowledge(EHCIInterruptState *s, unsigned causes)
{
    if (wr(s, EHCI_STS, causes) && rd(s, EHCI_STS) != 0xffffffffU) {
        /* A new pending bit may appear before the next watchdog sample.
         * Completing an acknowledgement is progress even if that sample
         * never observes the brief quiet/fenced interval between IRQs. */
        s->watchdogPending = 0;
    }
}
int EHCIInterruptFence(EHCIInterruptState *s, int dma)
{
    ehci_u32 before, desired, after;
    s->interruptDebt = 1; s->fenced = 0;
    if (dma) { s->dmaDebt = 1; s->dmaFenced = 0; }
    if (!s->ops->pciRead(s->context, &before) || (before & 0xffff) == 0xffff) return 0;
    desired = (before & 0xffff) | EHCI_PCI_INT_DISABLE;
    if (dma) desired &= ~EHCI_PCI_MASTER;
    if (!s->ops->pciWrite(s->context, desired) ||
        !s->ops->pciRead(s->context, &after) || (after & 0xffff) == 0xffff ||
        (after & 0xffff) != desired) return 0;
    s->fenced = 1; s->interruptDebt = 0; s->active = 0;
    if (dma) { s->dmaDebt = 0; s->dmaFenced = 1; }
    return 1;
}
static int rearm(EHCIInterruptState *s, int freshForeign)
{
    if (!s->participant || !s->rearmDebt) return 1;
    if ((!freshForeign || s->mmioLost || s->stopping) && !s->fenced) return 0;
    if (!s->ops->rearm(s->context)) { s->failures++; return 0; }
    s->rearmDebt = 0;
    return 1;
}
static void publish(EHCIInterruptState *s, unsigned cause)
{
    s->pending |= cause; s->work = 1;
    s->ops->publish(s->context);
}
static void examine(EHCIInterruptState *s)
{
    unsigned status, enabled, causes;
    if (s->stopping || s->mmioLost) {
        /* Resolve IRQ proof independently: a BM-clear failure must not hold
         * an otherwise contained shared interrupt vote forever. */
        EHCIInterruptFence(s, 0);
        rearm(s, 0);
        return;
    }
    if (s->fenced) { rearm(s, 0); return; }
    status = rd(s, EHCI_STS); enabled = rd(s, EHCI_INTR);
    if (s->mmioLost) { EHCIInterruptFence(s, 0); rearm(s, 0); return; }
    causes = status & enabled & EHCI_STS_W1C;
    if (!causes) { s->foreign++; rearm(s, 1); return; }
    s->owned++;
    /* EHCI USBINTR=0 alone need not deassert an already asserted interrupt.
     * A verified PCI fence is the proof; W1C acknowledges observed bits only. */
    EHCIInterruptFence(s, 0);
    wr(s, EHCI_INTR, 0);
    acknowledge(s, causes);
    publish(s, causes);
    if (s->mmioLost && !s->fenced) EHCIInterruptFence(s, 0);
    rearm(s, 0);
}
void EHCIInterruptCallback(EHCIInterruptState *s)
{
    if (!s->participant) return;
    s->callbacks++; s->rearmDebt = 1;
    examine(s);
}
void EHCIInterruptRetry(EHCIInterruptState *s)
{
    s->retries++;
    if (s->interruptDebt) EHCIInterruptFence(s, 0);
    if (s->rearmDebt) examine(s);
    if (s->dmaDebt) {
        /* Keep the previously established INTx proof if the independent DMA
         * transition fails. Re-establish it from current PCI readback. */
        if (!EHCIInterruptFence(s, 1)) EHCIInterruptFence(s, 0);
    }
}
unsigned EHCIInterruptTakeWork(EHCIInterruptState *s)
{
    unsigned pending = s->pending;
    s->pending = 0; s->work = 0; s->services++;
    return pending;
}
void EHCIInterruptWatchdog(EHCIInterruptState *s, ehci_u64 now)
{
    unsigned status;
    if (s->mode != EHCI_MODE_INTX || !s->active || s->stopping) {
        s->watchdogPending = 0; return;
    }
    status = rd(s, EHCI_STS);
    if (s->mmioLost) return;
    if (!(status & EHCI_INTR_DEFAULT)) { s->watchdogPending = 0; return; }
    if (!s->watchdogPending || now < s->watchdogSince) {
        s->watchdogSince = now; s->watchdogPending = 1;
    } else if (now - s->watchdogSince >= 100) EHCIInterruptStop(s);
}
int EHCIInterruptRestore(EHCIInterruptState *s)
{
    ehci_u32 before, after, desired;
    unsigned status, generation = s->generation;
    if (s->mode != EHCI_MODE_INTX || !s->participant || s->stopping ||
        s->mmioLost || s->admitted || s->work || s->interruptDebt || s->dmaDebt) return 0;
    if (!s->fenced && !EHCIInterruptFence(s, 0)) return 0;
    if (!rearm(s, 0)) return 0;
    status = rd(s, EHCI_STS);
    if (s->mmioLost) return 0;
    if (status & EHCI_INTR_DEFAULT) {
        acknowledge(s, status & EHCI_INTR_DEFAULT);
        publish(s, status & EHCI_INTR_DEFAULT); return 0;
    }
    if (!wr(s, EHCI_INTR, EHCI_INTR_DEFAULT) || rd(s, EHCI_INTR) != EHCI_INTR_DEFAULT ||
        generation != s->generation || s->mmioLost) goto failed;
    if (!s->ops->pciRead(s->context, &before) || (before & 0xffff) == 0xffff) goto failed;
    desired = (before & 0xffff) & ~EHCI_PCI_INT_DISABLE;
    if (!(desired & EHCI_PCI_MASTER) || !(desired & EHCI_PCI_MEMORY) ||
        !s->ops->pciWrite(s->context, desired) || !s->ops->pciRead(s->context, &after) ||
        (after & 0xffff) != desired || generation != s->generation) goto failed;
    s->active = 1; s->fenced = 0;
    return 1;
failed:
    s->stopping = 1; s->dmaDebt = 1;
    EHCIInterruptFence(s, 0); publish(s, 0);
    return 0;
}
void EHCIInterruptPoll(EHCIInterruptState *s)
{
    unsigned status;
    if (s->mode != EHCI_MODE_POLLING || s->stopping || s->mmioLost) return;
    status = rd(s, EHCI_STS);
    if (s->mmioLost) return;
    if (status & EHCI_STS_W1C) {
        acknowledge(s, status & EHCI_STS_W1C);
    }
    publish(s, status & EHCI_INTR_DEFAULT);
}
void EHCIInterruptStop(EHCIInterruptState *s)
{
    s->stopping = 1; s->active = 0; s->generation++;
    s->interruptDebt = s->dmaDebt = 1;
    EHCIInterruptFence(s, 0); rearm(s, 0);
    if (!EHCIInterruptFence(s, 1)) EHCIInterruptFence(s, 0);
    publish(s, 0);
}
