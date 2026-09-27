#ifndef EHCI_TYPES_H
#define EHCI_TYPES_H
#include "EHCIUSBNames.h"
typedef unsigned char ehci_u8;
typedef unsigned short ehci_u16;
typedef unsigned int ehci_u32;
typedef unsigned long long ehci_u64;
typedef signed char ehci_s8;
typedef char ehci_word_is_32_bits[sizeof(ehci_u32) == 4 ? 1 : -1];
#define EHCI_PAGE_SIZE 4096U
#define EHCI_MAX_DEVICES 16U
#define EHCI_MAX_PORTS 15U
#define EHCI_MAX_HUB_PORTS 15U
#define EHCI_TIMEOUT_MS 5000U
typedef struct EHCIDMA {
    void *allocation;
    void *virtualAddress;
    ehci_u32 physicalAddress;
    ehci_u32 allocationBytes;
    ehci_u32 bytes;
} EHCIDMA;
#endif
