#define MACH_USER_API 1
#import "EHCIUSBEthernet.h"
#import "EHCIController.h"
#import <driverkit/generalFuncs.h>
#import <driverkit/kernelDriver.h>
#import <kernserv/prototypes.h>
#import <string.h>
#import <bsd/sys/errno.h>

extern void thread_wakeup(int);
static void networkThread(void *context)
{ [(EHCIController *)context runNetworkLoop]; IOExitThread(); }

/* OPENSTEP 4.2 IODirectDevice requires its description's delegate/device to
 * accept the Mach port used by IOEthernet's DriverCmd thread, even with zero
 * hardware interrupts. A bare IODeviceDescription has a nil delegate and
 * fails attachInterruptPort. This software description accepts that port;
 * it owns no PCI resource, interrupt source, or controller description.
 * _delegate/device/attachInterruptPort: were checked in the target kernel. */
@interface EHCIECMDeviceDescription : IODeviceDescription
- _delegate;
- device;
- (BOOL)attachInterruptPort:(port_t)port;
@end
@implementation EHCIECMDeviceDescription
- _delegate { return self; }
- device { return self; }
- (BOOL)attachInterruptPort:(port_t)port { return port != PORT_NULL; }
@end

@implementation EHCIUSBEthernet
- initWithController:(EHCIController *)controller address:(const unsigned char *)mac
{
    enet_addr_t address;
    [self setUnit:0x7fffffffU];
    _controller = controller;
    _outputQueue = [[IONetbufQueue alloc] initWithMaxCount:128];
    if (!_outputQueue) return nil;
    /* A separate, resource-free description: never give IOEthernet the PCI
     * controller's description, IRQ list, device port or memory ranges. */
    _usbDescription = [[EHCIECMDeviceDescription alloc] init];
    /* IOEthernet's free assumes its multicast queue was initialized. */
    if (!_usbDescription) { [_outputQueue free]; _outputQueue = nil; return nil; }
    _published = 1; /* superclass may publish an I/O thread before returning */
    if (![super initFromDeviceDescription:_usbDescription]) return nil;
    bcopy(mac, &address, 6);
    _network = [super attachToNetworkWithAddress:address];
    if (!_network) {
        IOLog("EHCI: cannot register ECM Ethernet interface; retained offline until reboot\n");
        return nil;
    }
    [self setRunning:NO];
    IOLog("EHCI: ECM Ethernet interface %s attached, MTU 1500\n", [self name]);
    return self;
}
- (BOOL)resetAndEnable:(BOOL)enable
{
    unsigned filter;
    [_controller->_eventLock lock];
    filter = USB_ECM_FILTER_DEFAULT |
        (_promiscuous ? USB_ECM_FILTER_PROMISCUOUS : 0) |
        ((_multicast || _multicastAddresses) ? USB_ECM_FILTER_ALL_MULTICAST : 0);
    EHCICoreECMEnable(&_controller->_state, enable, filter);
    if (!enable) [self flushOutputQueue];
    [_controller->_eventLock unlock];
    [self setRunning:enable];
    return YES;
}
#include "EHCIEthernetQueue.inc"
#include "EHCIEthernetOutput.inc"
- (void)updateFilter
{
    EHCIECMState *n = _controller->_state.ecm;
    if (n) EHCICoreECMEnable(&_controller->_state, n->enabled, USB_ECM_FILTER_DEFAULT |
        (_promiscuous ? USB_ECM_FILTER_PROMISCUOUS : 0) |
        ((_multicast || _multicastAddresses) ? USB_ECM_FILTER_ALL_MULTICAST : 0));
}
- (BOOL)enablePromiscuousMode
{
    [_controller->_eventLock lock]; _promiscuous = 1; [self updateFilter];
    [_controller->_eventLock unlock]; return YES;
}
- (void)disablePromiscuousMode
{
    [_controller->_eventLock lock]; _promiscuous = 0; [self updateFilter];
    [_controller->_eventLock unlock];
}
- (BOOL)enableMulticastMode
{
    [_controller->_eventLock lock]; _multicast = 1; [self updateFilter];
    [_controller->_eventLock unlock]; return YES;
}
- (void)disableMulticastMode
{
    [_controller->_eventLock lock]; _multicast = 0; [self updateFilter];
    [_controller->_eventLock unlock];
}
- (void)addMulticastAddress:(enet_addr_t *)address
{
    (void)address;
    [_controller->_eventLock lock]; _multicastAddresses++; [self updateFilter];
    [_controller->_eventLock unlock];
}
- (void)removeMulticastAddress:(enet_addr_t *)address
{
    (void)address;
    [_controller->_eventLock lock];
    if (_multicastAddresses) _multicastAddresses--;
    [self updateFilter]; [_controller->_eventLock unlock];
}
- (void)serviceNetwork
{
    USBECMFrame frame;
    EHCIECMState *n;
    unsigned count, running = 0, rxErrors, txErrors, txPackets;
    [_controller->_eventLock lock];
    [self drainOutputQueue];
    [_controller->_eventLock unlock];
    for (count = 0; count < USB_ECM_QUEUE_SIZE; count++) {
        int have = 0;
        netbuf_t packet;
        [_controller->_eventLock lock];
        n = _controller->_state.ecm;
        if (n && EHCICoreECMDevice(&_controller->_state, n->generation) && n->enabled)
            have = USBECMQueuePop(&n->rx, &frame);
        [_controller->_eventLock unlock];
        if (!have) break;
        packet = nb_alloc(frame.length);
        if (!packet) {
            [_controller->_eventLock lock];
            n = _controller->_state.ecm;
            if (n) n->rxNoBuffer++;
            [_controller->_eventLock unlock];
            [_network incrementInputErrors]; continue;
        }
        bcopy(frame.bytes, nb_map(packet), frame.length);
        /* Network delivery can immediately transmit (ARP); no controller lock. */
        [_network handleInputPacket:packet extra:0];
    }
    [_controller->_eventLock lock];
    n = _controller->_state.ecm;
    rxErrors = txErrors = txPackets = 0;
    if (n) {
        running = n->enabled && EHCICoreECMDevice(&_controller->_state, n->generation) != 0;
        rxErrors = n->rxErrors; txErrors = n->txErrors; txPackets = n->txPackets;
    }
    [_controller->_eventLock unlock];
    [self setRunning:running];
    [_network incrementInputErrorsBy:rxErrors - _rxErrorsSeen];
    [_network incrementOutputErrorsBy:txErrors - _txErrorsSeen];
    [_network incrementOutputPacketsBy:txPackets - _txPacketsSeen];
    _rxErrorsSeen = rxErrors; _txErrorsSeen = txErrors; _txPacketsSeen = txPackets;
}
- free
{
    if (_published) { [self setRunning:NO]; return self; }
    [_usbDescription free];
    [_outputQueue free];
    return [super free];
}
@end

@implementation EHCIController (Network)
- (BOOL)startNetworkWorker
{
    return IOForkThread(networkThread, self) != 0;
}
- (void)runNetworkLoop
{
    for (;;) {
        EHCIECMState *n;
        unsigned attach = 0;
        ehci_u8 mac[6];
        [_eventLock lock];
        EHCICoreECMPump(&_state);
        n = _state.ecm;
        if (!_ethernet && !_networkAttachFailed && n && n->macValid &&
            EHCICoreECMDevice(&_state, n->generation)) {
            bcopy(n->mac, mac, 6); attach = 1;
        }
        [_eventLock unlock];
        if (attach) {
            _ethernet = [[EHCIUSBEthernet alloc] initWithController:self address:mac];
            if (!_ethernet) {
                _networkAttachFailed = 1;
                IOLog("EHCI: ECM network attachment failed; reboot required to retry\n");
                [_eventLock lock]; EHCICoreECMEnable(&_state, 0, 0); [_eventLock unlock];
            }
        }
        [_ethernet serviceNetwork];
        /* Completion/queue publishers hold _eventLock. Register before
         * unlocking so wakes cannot be lost; one tick bounds TX timeouts. */
        [_eventLock lock];
        n = _state.ecm;
        if (n) {
            EHCIDevice *d = EHCICoreECMDevice(&_state, n->generation);
            if (d && ((n->enabled && (n->rx.count || (n->tx.count && !n->txActive))) ||
                (n->rxActive && !d->endpoints[2].waiting) ||
                (n->txActive && !d->endpoints[3].waiting) ||
                (n->notifyActive && !d->endpoints[1].waiting) ||
                n->appliedFilter != (n->enabled ? n->filter : 0))) {
                [_eventLock unlock]; continue;
            }
        }
        assert_wait((int)&_networkEvent, FALSE); thread_set_timeout(1);
        [_eventLock unlock]; thread_block();
    }
}
@end

void EHCIPlatformNetworkWake(void *context)
{ thread_wakeup((int)&((EHCIController *)context)->_networkEvent); }
