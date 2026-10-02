#ifndef EHCI_USB_ETHERNET_H
#define EHCI_USB_ETHERNET_H
#import <driverkit/IOEthernet.h>
#import <driverkit/IONetbufQueue.h>
@class EHCIController;
@interface EHCIUSBEthernet : IOEthernet
{
    EHCIController *_controller;
    IONetwork *_network;
    id _usbDescription;
    IONetbufQueue *_outputQueue;
    unsigned _queueGeneration;
    unsigned _published, _promiscuous, _multicast, _multicastAddresses;
    unsigned _rxErrorsSeen, _txErrorsSeen, _txPacketsSeen;
}
- initWithController:(EHCIController *)controller address:(const unsigned char *)mac;
- (void)serviceNetwork;
- (void)drainOutputQueue;
- (void)flushOutputQueue;
@end
#endif
