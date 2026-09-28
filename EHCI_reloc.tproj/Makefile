NAME = EHCI_reloc
PROJECTVERSION = 1.1
LANGUAGE = English
CLASSES = EHCIController.m EHCIStorage.m EHCIUSBKeyboard.m EHCIUSBPointer.m
HFILES = EHCIController.h EHCICore.h EHCIRegs.h EHCITypes.h EHCIMemory.h EHCIUSBNames.h EHCIInterruptState.h EHCIPCI.h EHCIInputQueue.h EHCIUSBKeyboard.h EHCIUSBPointer.h EHCIVersion.h OpenStepInputCompat.h USBMassStorage.h USBStorageSCSI.h USBStorageBuffer.h USBStorageWait.h EHCIStorageStats.h USBCore.h USBHID.h
CFILES = EHCICore.c EHCIInterruptState.c EHCIPCI.c EHCIInputQueue.c USBCore.c USBHID.c USBMassStorage.c USBStorageSCSI.c USBStorageBuffer.c USBStorageWait.c EHCICompletionWait.c
HFILES += EHCICompletionWait.h
OTHERSRCS = EHCIEnumeration.inc Makefile.preamble Makefile Makefile.postamble Load_Commands.sect
MAKEFILEDIR = /NextDeveloper/Makefiles/app
MAKEFILE = tool.make
SOURCEMODE = 444
LIBS =
DEBUG_LIBS = $(LIBS)
PROF_LIBS = $(LIBS)
-include Makefile.preamble
include $(MAKEFILEDIR)/$(MAKEFILE)
-include Makefile.postamble
-include Makefile.dependencies
