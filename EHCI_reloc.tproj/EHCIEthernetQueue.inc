/* The event lock protects both queues. Keep netbuf ownership while the core
 * queue is full; network callbacks cannot sleep waiting for USB completion. */
- (void)flushOutputQueue
{
    EHCIECMState *n = _controller->_state.ecm;
    netbuf_t packet;
    while ((packet = [_outputQueue dequeue]) != 0) {
        if (n) n->txErrors++;
        nb_free(packet);
    }
    if (n) n->txNativeQueued = 0;
}
- (void)drainOutputQueue
{
    EHCIControllerState *c = &_controller->_state;
    EHCIECMState *n = c->ecm;
    netbuf_t packet;
    if (!n || !EHCICoreECMDevice(c, n->generation) || !n->enabled ||
        (n->notifications.linkKnown && !n->notifications.linkUp) ||
        _queueGeneration != n->generation) {
        [self flushOutputQueue];
        _queueGeneration = n ? n->generation : 0;
        return;
    }
    while (n->tx.count < USB_ECM_QUEUE_SIZE && (packet = [_outputQueue dequeue]) != 0) {
        EHCICoreECMTransmit(c, n->generation, (const ehci_u8 *)nb_map(packet), nb_size(packet));
        nb_free(packet);
    }
    n->txNativeQueued = [_outputQueue count];
}
- (void)transmit:(netbuf_t)packet
{
    EHCIECMState *n;
    unsigned length;
    if (!packet) return;
    length = nb_size(packet);
    [_controller->_eventLock lock];
    n = _controller->_state.ecm;
    [self drainOutputQueue];
    if (!n || !EHCICoreECMDevice(&_controller->_state, n->generation) || !n->enabled ||
        (n->notifications.linkKnown && !n->notifications.linkUp)) {
        if (n) n->txErrors++;
        nb_free(packet);
    } else if (length < 14 || length > USB_ECM_FRAME_MAX) {
        n->txErrors++; n->txInvalid++; nb_free(packet);
    } else if ([_outputQueue count] >= [_outputQueue maxCount]) {
        /* IONetbufQueue silently frees overflow: count it before enqueue. */
        n->txErrors++; n->txQueueDrops++; nb_free(packet);
    } else {
        if (n->tx.count == USB_ECM_QUEUE_SIZE) n->txBackpressure++;
        [_outputQueue enqueue:packet];
        if ([_outputQueue count] > n->txNativeHighWater)
            n->txNativeHighWater = [_outputQueue count];
        [self drainOutputQueue];
    }
    [_controller->_eventLock unlock];
}
