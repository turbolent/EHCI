/* EHCI 1.0 register/schedule interface, Intel specification chapters 2-4. */
#ifndef EHCI_REGS_H
#define EHCI_REGS_H
#include "EHCITypes.h"
#define EHCI_CAP_LENGTH 0x00U
#define EHCI_CAP_PARAMS 0x04U
#define EHCI_CAP_CAPS 0x08U
#define EHCI_CMD 0x00U
#define EHCI_STS 0x04U
#define EHCI_INTR 0x08U
#define EHCI_FRAME 0x0cU
#define EHCI_SEGMENT 0x10U
#define EHCI_PERIODIC 0x14U
#define EHCI_ASYNC 0x18U
#define EHCI_CONFIG 0x40U
#define EHCI_PORT(n) (0x44U + ((n) - 1U) * 4U)
#define EHCI_CMD_RUN 0x0001U
#define EHCI_CMD_RESET 0x0002U
#define EHCI_CMD_PSE 0x0010U
#define EHCI_CMD_ASE 0x0020U
#define EHCI_CMD_IAAD 0x0040U
#define EHCI_CMD_ITC (1U << 16)
#define EHCI_STS_USB 0x0001U
#define EHCI_STS_ERROR 0x0002U
#define EHCI_STS_PORT 0x0004U
#define EHCI_STS_ROLLOVER 0x0008U
#define EHCI_STS_FATAL 0x0010U
#define EHCI_STS_IAA 0x0020U
#define EHCI_STS_HALTED 0x1000U
#define EHCI_STS_PSS 0x4000U
#define EHCI_STS_ASS 0x8000U
#define EHCI_STS_W1C 0x003fU
#define EHCI_INTR_DEFAULT 0x0037U
#define EHCI_PORT_CONNECT 0x0001U
#define EHCI_PORT_CSC 0x0002U
#define EHCI_PORT_ENABLE 0x0004U
#define EHCI_PORT_PEC 0x0008U
#define EHCI_PORT_OC 0x0010U
#define EHCI_PORT_OCC 0x0020U
#define EHCI_PORT_RESUME 0x0040U
#define EHCI_PORT_SUSPEND 0x0080U
#define EHCI_PORT_RESET 0x0100U
#define EHCI_PORT_LINE 0x0c00U
#define EHCI_PORT_LOW 0x0400U
#define EHCI_PORT_POWER 0x1000U
#define EHCI_PORT_OWNER 0x2000U
#define EHCI_PORT_W1C 0x002aU
#define EHCI_LINK_END 1U
#define EHCI_LINK_QH 2U
#define EHCI_LINK_ADDR 0xffffffe0U
#define EHCI_QTD_ACTIVE 0x80U
#define EHCI_QTD_HALTED 0x40U
#define EHCI_QTD_BUFFER 0x20U
#define EHCI_QTD_BABBLE 0x10U
#define EHCI_QTD_TRANSACTION 0x08U
#define EHCI_QTD_MISSED 0x04U
#define EHCI_QTD_ERRORS 0x7cU
#define EHCI_PID_OUT (0U << 8)
#define EHCI_PID_IN (1U << 8)
#define EHCI_PID_SETUP (2U << 8)
#define EHCI_QTD_CERR (3U << 10)
#define EHCI_QTD_IOC (1U << 15)
#define EHCI_QTD_BYTES(n) ((ehci_u32)(n) << 16)
#define EHCI_QTD_REMAIN(t) (((t) >> 16) & 0x7fffU)
#define EHCI_QTD_TOGGLE (1U << 31)
#define EHCI_SPEED_FULL 0U
#define EHCI_SPEED_LOW 1U
#define EHCI_SPEED_HIGH 2U
#define EHCI_QH_DTC (1U << 14)
#define EHCI_QH_HEAD (1U << 15)
#define EHCI_QH_CONTROL (1U << 27)
#define EHCI_QH_MULT (1U << 30)
#define EHCI_PCI_MEMORY 0x0002U
#define EHCI_PCI_MASTER 0x0004U
#define EHCI_PCI_INT_DISABLE 0x0400U

/* Extended buffer pointers are always present and zero (32-bit DMA).
 * Padding gives every descriptor a 32-byte aligned address in its arena. */
typedef struct EHCIqTD {
    volatile ehci_u32 next, alternate, token, buffer[5], high[5];
    ehci_u32 padding[3];
} EHCIqTD;
typedef struct EHCIQH {
    volatile ehci_u32 link, endpoint, capabilities, current;
    volatile ehci_u32 next, alternate, token, buffer[5], high[5];
    ehci_u32 padding[7];
} EHCIQH;
typedef char ehci_qtd_size[sizeof(EHCIqTD) == 64 ? 1 : -1];
typedef char ehci_qh_size[sizeof(EHCIQH) == 96 ? 1 : -1];
#endif
