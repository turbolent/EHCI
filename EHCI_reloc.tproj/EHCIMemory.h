#ifndef EHCI_MEMORY_H
#define EHCI_MEMORY_H

#include <string.h>
#ifdef EHCI_HOST_TEST
/* Host tests use standard C equivalents of the BSD memory routines. */
#define bzero(address, length) memset((address), 0, (length))
#define bcopy(source, destination, length) memmove((destination), (source), (length))
#endif

#endif
