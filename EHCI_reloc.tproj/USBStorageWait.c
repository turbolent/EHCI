#include "USBStorageWait.h"

int USBStorageWaitTicks(ehci_u64 now, ehci_u64 deadline, unsigned hz)
{
    ehci_u64 remaining;
    if (now >= deadline) return 0;
    if (!hz) return 1;
    remaining = deadline - now;
    /* Avoid overflow even for a malformed, very distant deadline. */
    if (remaining >= (0x7fffffffULL * 1000) / hz) return 0x7fffffff;
    return (int)((remaining * hz + 999) / 1000);
}

int USBStorageWait(void *context, const USBStorageWaitOps *ops,
                   ehci_u64 deadline, unsigned hz)
{
    int rc, ticks;
    for (;;) {
        ops->lock(context);
        rc = ops->result(context);
        if (rc == USB_TRANSFER_PENDING) {
            ticks = USBStorageWaitTicks(ops->milliseconds(context), deadline, hz);
            if (!ticks) rc = USB_TRANSFER_TIMEOUT;
            else ops->prepare(context, ticks);
        }
        ops->unlock(context);
        if (rc != USB_TRANSFER_PENDING) return rc;
        ops->block(context);
        /* Recheck software state after completion, timeout or a spurious
         * wake. The absolute whole-command deadline is never extended. */
    }
}
