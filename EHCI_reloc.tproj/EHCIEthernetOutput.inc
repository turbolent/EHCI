/* Shared with the native shim regression so the actual override is tested.
 * IOEthernet grows short frames before transmit:, but does not clear the
 * added bytes. Pad here while the original length is still available. */
- (int)outputPacket:(netbuf_t)packet address:(void *)address
{
    unsigned length = nb_size(packet);
    if (length < USB_ECM_FRAME_MIN) {
        if (nb_grow_bot(packet, USB_ECM_FRAME_MIN - length)) {
            nb_free(packet);
            return ENOBUFS;
        }
        bzero(nb_map(packet) + length, USB_ECM_FRAME_MIN - length);
    }
    return [super outputPacket:packet address:address];
}
