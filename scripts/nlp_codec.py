#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors.

NLP v1 DESIGN reference: plaintext wire, bounded framing, TLV and CAN IDs only.
No authentication, discovery, routing, reliable session or capability execution.
Encrypted business frames are deliberately rejected by decode_frame().
"""
import argparse
from dataclasses import asdict, dataclass
import json
import struct

HEADER_BYTES = 52
MAX_PAYLOAD = 1024
MAX_PDU = HEADER_BYTES + MAX_PAYLOAD + 16
MAX_TLVS = 32
RELIABLE = 1
ENCRYPTED = 2
CONTROL, REQUEST, RESPONSE, EVENT, DATA, ACK, CLOSE = range(7)
_HEADER = struct.Struct('<2sBBBBBBI6H4I2H2I')
assert _HEADER.size == HEADER_BYTES


class ProtocolError(ValueError):
    pass


class UnsupportedSecurity(ProtocolError):
    pass


@dataclass(frozen=True)
class Header:
    kind: int = EVENT
    flags: int = 0
    priority: int = 1
    hop_limit: int = 16
    fabric_epoch: int = 1
    src_node: int = 2
    dst_node: int = 1
    src_cap: int = 1
    dst_cap: int = 0
    channel_id: int = 1
    opcode: int = 0x1001
    session_id: int = 1
    packet_counter: int = 1
    reliable_seq: int = 0
    ack: int = 0
    receive_limit: int = 0


def crc32c(data):
    crc = 0xffffffff
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82f63b78 if crc & 1 else 0)
    return crc ^ 0xffffffff


def _uint(value, bits, name):
    if type(value) is not int or not 0 <= value < 1 << bits:
        raise ProtocolError(f'{name} is not a u{bits}')


def _validate(header, payload_len):
    if not isinstance(header, Header):
        raise ProtocolError('expected Header')
    widths = {'kind': 8, 'flags': 8, 'priority': 8, 'hop_limit': 8,
              'fabric_epoch': 32, 'src_node': 16, 'dst_node': 16,
              'src_cap': 16, 'dst_cap': 16, 'channel_id': 16, 'opcode': 16,
              'session_id': 32, 'packet_counter': 32, 'reliable_seq': 32,
              'ack': 32, 'receive_limit': 32}
    for field, bits in widths.items():
        _uint(getattr(header, field), bits, field)
    if type(payload_len) is not int or not 0 <= payload_len <= MAX_PAYLOAD:
        raise ProtocolError('payload exceeds v1 bound')
    if header.kind not in range(7) or header.flags & ~3:
        raise ProtocolError('unknown kind or flags')
    if header.priority > 3 or not 1 <= header.hop_limit <= 16:
        raise ProtocolError('priority or hop_limit outside v1 bound')
    if not header.src_node or not header.dst_node or 0xffff in (header.src_cap, header.dst_cap):
        raise ProtocolError('reserved endpoint')
    if not header.session_id:
        if (header.kind != CONTROL or header.flags or header.channel_id or
            header.src_cap or header.dst_cap or header.packet_counter or
            header.reliable_seq or header.ack or header.receive_limit):
            raise ProtocolError('invalid pre-session envelope')
    elif (not header.packet_counter or not header.fabric_epoch or
          header.src_node >= 0xfffe or header.dst_node >= 0xfffe):
        raise ProtocolError('established session requires counter, epoch and assigned nodes')
    if header.flags & RELIABLE:
        if not header.reliable_seq or header.kind == ACK:
            raise ProtocolError('invalid reliable sequence')
    elif header.reliable_seq:
        raise ProtocolError('unreliable frame has reliable_seq')
    if header.receive_limit < header.ack or header.receive_limit - header.ack > 8:
        raise ProtocolError('invalid cumulative credit')
    if header.kind == ACK:
        if payload_len or header.opcode or not header.session_id:
            raise ProtocolError('ACK must be empty and belong to a session')
    elif not header.flags & RELIABLE and (header.ack or header.receive_limit):
        raise ProtocolError('unreliable data does not advertise reliable credit')


def pack_header(header, payload_len):
    _validate(header, payload_len)
    values = (b'NL', 1, HEADER_BYTES, header.kind, header.flags,
              header.priority, header.hop_limit, header.fabric_epoch,
              header.src_node, header.dst_node, header.src_cap, header.dst_cap,
              header.channel_id, header.opcode, header.session_id,
              header.packet_counter, header.reliable_seq, header.ack,
              payload_len, 0, header.receive_limit, 0)
    raw = _HEADER.pack(*values)
    return raw[:48] + struct.pack('<I', crc32c(raw[:48]))


def canonical_aad(header, payload_len):
    """Construct authenticated header bytes; does not encrypt/authenticate anything."""
    if not header.flags & ENCRYPTED:
        raise ProtocolError('AAD example requires ENCRYPTED flag')
    raw = bytearray(pack_header(header, payload_len))
    raw[7] = 0
    raw[48:52] = bytes(4)
    return bytes(raw)


def encode_frame(header, payload=b''):
    if not isinstance(payload, (bytes, bytearray)):
        raise ProtocolError('payload must be bytes')
    if header.flags & ENCRYPTED:
        raise UnsupportedSecurity('reference codec has no AEAD implementation')
    raw = pack_header(header, len(payload)) + payload
    return raw + struct.pack('<I', crc32c(raw))


def decode_frame(frame):
    if not isinstance(frame, (bytes, bytearray)) or not HEADER_BYTES + 4 <= len(frame) <= MAX_PDU:
        raise ProtocolError('invalid complete PDU length')
    fields = _HEADER.unpack(frame[:HEADER_BYTES])
    (magic, major, size, kind, flags, priority, hops, epoch, src, dst,
     sc, dc, channel, opcode, session, counter, seq, ack, length,
     reserved, limit, checksum) = fields
    if (magic, major, size) != (b'NL', 1, HEADER_BYTES) or reserved:
        raise ProtocolError('invalid magic, version, header length or reserved field')
    if checksum != crc32c(frame[:48]):
        raise ProtocolError('invalid header CRC32C')
    header = Header(kind, flags, priority, hops, epoch, src, dst, sc, dc,
                    channel, opcode, session, counter, seq, ack, limit)
    _validate(header, length)
    trailer = 16 if flags & ENCRYPTED else 4
    if len(frame) != HEADER_BYTES + length + trailer:
        raise ProtocolError('truncated PDU or trailing garbage')
    if flags & ENCRYPTED:
        raise UnsupportedSecurity('opaque encrypted frame is NOT authenticated by this codec')
    if struct.unpack_from('<I', frame, len(frame) - 4)[0] != crc32c(frame[:-4]):
        raise ProtocolError('invalid frame CRC32C')
    return header, bytes(frame[HEADER_BYTES:-4])


def encode_tlvs(items):
    result = bytearray()
    previous = 0
    if len(items) > MAX_TLVS:
        raise ProtocolError('too many TLVs')
    for tag, value in items:
        _uint(tag, 16, 'tag')
        number = tag & 0x7fff
        if number <= previous or not isinstance(value, bytes) or len(value) > MAX_PAYLOAD:
            raise ProtocolError('TLVs must have unique ascending nonzero tags and byte values')
        result.extend(struct.pack('<HH', tag, len(value)))
        result.extend(value)
        previous = number
    if len(result) > MAX_PAYLOAD:
        raise ProtocolError('TLV container exceeds v1 payload')
    return bytes(result)


def decode_tlvs(data, known_tags):
    if len(data) > MAX_PAYLOAD:
        raise ProtocolError('TLV container exceeds v1 payload')
    result, offset, previous, count = {}, 0, 0, 0
    while offset < len(data):
        if len(data) - offset < 4:
            raise ProtocolError('truncated TLV header')
        tag, length = struct.unpack_from('<HH', data, offset)
        offset += 4
        number = tag & 0x7fff
        count += 1
        if count > MAX_TLVS or number <= previous or length > len(data) - offset:
            raise ProtocolError('invalid TLV ordering, count or length')
        if number not in known_tags and tag & 0x8000:
            raise ProtocolError('unknown MUST_UNDERSTAND TLV')
        if number in known_tags:
            result[number] = bytes(data[offset:offset + length])
        previous, offset = number, offset + length
    return result


def cobs_encode(data):
    output = bytearray([0])
    index, code = 0, 1
    for byte in data:
        if byte == 0:
            output[index] = code
            index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(byte)
            code += 1
            if code == 255:
                output[index] = code
                index = len(output)
                output.append(0)
                code = 1
    output[index] = code
    return bytes(output)


def cobs_decode(data, max_decoded=MAX_PDU + 2):
    if not data or 0 in data:
        raise ProtocolError('invalid COBS code')
    output, offset = bytearray(), 0
    while offset < len(data):
        code = data[offset]
        offset += 1
        count = code - 1
        if count > len(data) - offset:
            raise ProtocolError('truncated COBS block')
        if len(output) + count > max_decoded:
            raise ProtocolError('COBS output exceeds bound')
        output.extend(data[offset:offset + count])
        offset += count
        if code != 255 and offset < len(data):
            if len(output) >= max_decoded:
                raise ProtocolError('COBS output exceeds bound')
            output.append(0)
    return bytes(output)


def _pdu_bound(size, max_pdu=MAX_PDU):
    if not HEADER_BYTES + 4 <= size <= max_pdu <= MAX_PDU:
        raise ProtocolError('PDU exceeds binding bound')


def encode_stream(pdu):
    _pdu_bound(len(pdu))
    return struct.pack('<H', len(pdu)) + pdu


def encode_uart(pdu):
    return cobs_encode(encode_stream(pdu)) + b'\0'


class StreamDecoder:
    """Opaque PDU framing only. Invalid length poisons the TCP connection."""
    def __init__(self, max_pdu=MAX_PDU):
        _pdu_bound(max_pdu)
        self.maximum, self.pending, self.expected, self.failed = max_pdu, bytearray(), None, False

    def feed(self, data):
        if self.failed or len(data) > 4096:
            raise ProtocolError('closed parser or oversized input chunk')
        result = []
        for byte in data:
            self.pending.append(byte)
            if self.expected is None and len(self.pending) == 2:
                self.expected = struct.unpack('<H', self.pending)[0]
                try:
                    _pdu_bound(self.expected, self.maximum)
                except ProtocolError:
                    self.failed = True
                    self.pending.clear()
                    raise
                self.pending.clear()
            elif self.expected is not None and len(self.pending) == self.expected:
                result.append(bytes(self.pending))
                self.pending.clear()
                self.expected = None
        return result


class UartDecoder:
    """Bounded resynchronization; partial-frame timer is a caller obligation."""
    def __init__(self, max_pdu=MAX_PDU):
        _pdu_bound(max_pdu)
        self.maximum, self.pending = max_pdu, bytearray()
        size = max_pdu + 2
        self.encoded_bound = size + size // 254 + 1
        self.discarding, self.errors = False, 0

    def cancel_partial(self):
        self.pending.clear()
        self.discarding = True

    def feed(self, data):
        if len(data) > 4096:
            raise ProtocolError('oversized input chunk')
        result = []
        for byte in data:
            if byte == 0:
                if self.discarding:
                    self.discarding = False
                elif self.pending:
                    try:
                        decoded = cobs_decode(self.pending, self.maximum + 2)
                        if len(decoded) < 2:
                            raise ProtocolError('missing PDU length')
                        length = struct.unpack_from('<H', decoded)[0]
                        _pdu_bound(length, self.maximum)
                        if len(decoded) != length + 2:
                            raise ProtocolError('UART length mismatch')
                        result.append(decoded[2:])
                    except ProtocolError:
                        self.errors += 1
                self.pending.clear()
            elif not self.discarding:
                if len(self.pending) >= self.encoded_bound:
                    self.pending.clear()
                    self.discarding = True
                    self.errors += 1
                else:
                    self.pending.append(byte)
        return result


def can_identifier(priority, source, destination, transfer, subtype=0):
    for name, value, bits in [('priority', priority, 3), ('source', source, 8),
                              ('destination', destination, 8), ('transfer', transfer, 6), ('subtype', subtype, 4)]:
        _uint(value, bits, name)
    if priority > 3 or source > 253 or destination == 254 or subtype > 1:
        raise ProtocolError('reserved CAN link identifier')
    return priority << 26 | source << 18 | destination << 10 | transfer << 4 | subtype


def can_commission_reply_id(next_uid_byte, query_token):
    _uint(next_uid_byte, 8, 'UID byte')
    _uint(query_token, 8, 'query token')
    return 255 << 18 | 255 << 10 | next_uid_byte << 2 | query_token & 3


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    decode = commands.add_parser('decode', help='validate a plaintext PDU in hexadecimal')
    decode.add_argument('hex')
    frame = commands.add_parser('frame', help='wrap an opaque hex PDU (not authentication)')
    frame.add_argument('binding', choices=('uart', 'tcp'))
    frame.add_argument('hex')
    args = parser.parse_args()
    try:
        raw = bytes.fromhex(args.hex)
        if args.command == 'decode':
            header, payload = decode_frame(raw)
            print(json.dumps({'header': asdict(header), 'payload_hex': payload.hex(),
                              'authenticated': False, 'scope': 'plaintext CRC checks only'}, indent=2))
        else:
            print((encode_uart(raw) if args.binding == 'uart' else encode_stream(raw)).hex())
    except (ProtocolError, ValueError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
