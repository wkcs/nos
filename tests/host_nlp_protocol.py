#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors.

NLP DESIGN reference checks: not a firmware, routing, reliability or crypto test.
Uses fixed vectors, an independent CRC oracle and bounded transport models.
"""
from dataclasses import asdict, replace
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import random
import re
import struct
import sys
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import nlp_codec as nlp

REGISTRY = json.loads((ROOT / 'protocols/nlp/v1/registry.json').read_text())
VECTORS = json.loads((ROOT / 'protocols/nlp/v1/vectors.json').read_text())


def oracle_crc(data):
    """Independent non-reflected polynomial division, reversing input/output."""
    crc = 0xffffffff
    for byte in data:
        reverse = int(f'{byte:08b}'[::-1], 2)
        crc ^= reverse << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1edc6f41 if crc & 0x80000000 else 0)) & 0xffffffff
    return int(f'{crc:032b}'[::-1], 2) ^ 0xffffffff


def repair_crc(raw):
    raw = bytearray(raw)
    struct.pack_into('<I', raw, 48, oracle_crc(raw[:48]))
    struct.pack_into('<I', raw, len(raw) - 4, oracle_crc(raw[:-4]))
    return bytes(raw)


class ProtocolTests(unittest.TestCase):
    def test_registry_offsets_and_types(self):
        wire = REGISTRY['wire']
        offset = 0
        for field in wire['fields']:
            self.assertEqual(field['offset'], offset)
            offset += field['bytes']
        self.assertEqual(offset, nlp.HEADER_BYTES)
        self.assertEqual(wire['kinds'], dict(zip(('CONTROL','REQUEST','RESPONSE','EVENT','DATA','ACK','CLOSE'),range(7))))
        self.assertEqual(wire['max_payload'], nlp.MAX_PAYLOAD)
        for table in ('core_opcodes','statuses','control_tags','radio_tags','firmware_manifest_tags','capability_descriptor_tags'):
            values = list(REGISTRY[table].values())
            self.assertEqual(len(values), len(set(values)), table)
        types = set()
        for cap in REGISTRY['capabilities']:
            expected = uuid.uuid5(uuid.NAMESPACE_URL, 'https://github.com/wkcs/nos/nlp/capability/' + cap['name'])
            self.assertEqual(str(expected), cap['type_uuid'])
            self.assertNotIn(cap['type_uuid'], types)
            types.add(cap['type_uuid'])
            self.assertEqual(len(cap['operations']), len(set(cap['operations'].values())))
        profile = REGISTRY['mcu_profile']
        self.assertEqual(profile['max_encrypted_pdu_bytes'], 52 + profile['payload_bytes'] + 16)
        self.assertLessEqual(profile['reliable_window'], REGISTRY['limits']['max_reliable_window'])
        self.assertLess(profile['pdu_pool_slots'], profile['sessions'] * profile['reliable_window'])
        self.assertEqual(REGISTRY['status'], 'EXPERIMENTAL_IMPLEMENTATION')

    def test_docs_and_local_links(self):
        docs = list((ROOT / 'docs/protocols/nlp').glob('*.md'))
        self.assertEqual(len(docs), 6)
        for path in docs:
            body = path.read_text()
            self.assertIn('Copyright', body)
            for link in re.findall(r'\]\(([^)]+)\)', body):
                if '://' not in link and not link.startswith('#'):
                    self.assertTrue((path.parent / link.split('#')[0]).exists(), (path, link))
        wire = (ROOT / 'docs/protocols/nlp/wire.md').read_text()
        for field in REGISTRY['wire']['fields']:
            self.assertIn('| ' + str(field['offset']) + ' | ' + field['name'] + ' | ' + str(field['bytes']) + ' |', wire)
        for opcode in REGISTRY['core_opcodes']:
            self.assertIn(opcode, wire)
        self.assertIn('实验实现', (ROOT / 'docs/protocols/nlp/README.md').read_text())

    def test_crc_oracle(self):
        self.assertEqual(nlp.crc32c(b'123456789'), 0xe3069283)
        self.assertEqual(nlp.crc32c(b''), 0)
        rng = random.Random(0xc32c)
        for size in (0,1,7,48,52,128,254,1024,1088):
            data = rng.randbytes(size)
            self.assertEqual(nlp.crc32c(data), oracle_crc(data))
        for vector in VECTORS['crc32c']:
            crc = oracle_crc(bytes.fromhex(vector['input_hex']))
            self.assertEqual(f'{crc:08x}', vector['result_hex'])
            self.assertEqual(struct.pack('<I', crc).hex(), vector['wire_hex'])

    def test_fixed_frames(self):
        self.assertEqual(len(VECTORS['frames']), 7)
        for vector in VECTORS['frames']:
            with self.subTest(name=vector['name']):
                header = nlp.Header(**vector['header'])
                payload, pdu = bytes.fromhex(vector['payload_hex']), bytes.fromhex(vector['pdu_hex'])
                self.assertEqual(nlp.encode_frame(header, payload), pdu)
                self.assertEqual(nlp.decode_frame(pdu), (header, payload))
                self.assertEqual(nlp.encode_stream(pdu).hex(), vector['tcp_hex'])
                self.assertEqual(nlp.encode_uart(pdu).hex(), vector['uart_hex'])
                self.assertEqual(pdu[48:52].hex(), vector['header_crc_hex'])
                self.assertEqual(pdu[-4:].hex(), vector['frame_crc_hex'])
                self.assertEqual(struct.unpack_from('<I', pdu, 48)[0], oracle_crc(pdu[:48]))
                self.assertEqual(struct.unpack_from('<I', pdu, len(pdu)-4)[0], oracle_crc(pdu[:-4]))
                self.assertEqual(struct.unpack_from('<H', pdu, 40)[0], len(payload))
                self.assertEqual(struct.unpack_from('<I', pdu, 24)[0], header.session_id)
                self.assertEqual(struct.unpack_from('<I', pdu, 44)[0], header.receive_limit)

    def test_profile_layouts(self):
        for fmt, size in [('<IQBBH32sHH',52),('<IQHH',16),('<IQHh',16),
                          ('<I5HBBH',18),('<QIHH',16),
                          ('<HHBBH',8),('<IHBB',8),('<HH16sHHI',28),
                          ('<IHH16sHHQIHBBB3xI',52),
                          ('<IQHBBB3xI',24),('<HBBHHHHI',16),('<HBBiiHH',16),('<HHHHQI',20)]:
            self.assertEqual(struct.calcsize(fmt), size, fmt)
        state = bytes.fromhex(VECTORS['frames'][3]['payload_hex'])
        seq, stamp, modifiers, flags, length, bitmap, consumer, reserved = struct.unpack('<IQBBH32sHH', state)
        self.assertEqual((seq,stamp,modifiers,flags,length,consumer,reserved), (9,1234567890,2,0,32,0,0))
        self.assertEqual(bitmap, b'\x10' + bytes(31))
        hello = bytes.fromhex(VECTORS['frames'][0]['payload_hex'])
        tlvs = nlp.decode_tlvs(hello[8:], set(range(1,9)))
        self.assertEqual(len(tlvs),8)
        self.assertEqual(tlvs[2], uuid.UUID('00112233-4455-6677-8899-aabbccddeeff').bytes)
        self.assertLessEqual(len(hello),128)

    def test_truncation_corruption_and_garbage(self):
        for vector in VECTORS['frames']:
            pdu = bytes.fromhex(vector['pdu_hex'])
            for cut in range(len(pdu)):
                with self.assertRaises(nlp.ProtocolError):
                    nlp.decode_frame(pdu[:cut])
            for index in range(len(pdu)):
                corrupt = bytearray(pdu); corrupt[index] ^= 1
                with self.assertRaises(nlp.ProtocolError):
                    nlp.decode_frame(corrupt)
            with self.assertRaises(nlp.ProtocolError):
                nlp.decode_frame(pdu+b'\0')

    def test_semantic_header_rejection_after_crc(self):
        base = bytes.fromhex(VECTORS['frames'][1]['pdu_hex'])
        # Correct both CRCs to prove rejection is semantic, not accidental checksum failure.
        for offset, fmt, value in [(0,'H',0),(2,'B',2),(3,'B',51),(4,'B',7),
                                  (5,'B',4),(6,'B',4),(7,'B',0),(7,'B',17),
                                  (8,'I',0),(12,'H',0),(14,'H',65535),
                                  (16,'H',65535),(28,'I',0),(32,'I',0),
                                  (40,'H',1025),(42,'H',1),(44,'I',9)]:
            raw = bytearray(base); struct.pack_into('<'+fmt,raw,offset,value)
            with self.subTest(offset=offset,value=value), self.assertRaises(nlp.ProtocolError):
                nlp.decode_frame(repair_crc(raw))
        for changes in [dict(kind=5,opcode=1),dict(flags=0,reliable_seq=1),dict(ack=3,receive_limit=2),dict(packet_counter=True),dict(session_id=-1),dict(packet_counter=1<<32)]:
            with self.assertRaises(nlp.ProtocolError):
                nlp.encode_frame(replace(nlp.Header(),**changes))
        with self.assertRaises(nlp.ProtocolError):
            nlp.encode_frame(nlp.Header(),bytes(1025))
        with self.assertRaises(nlp.ProtocolError):
            nlp.encode_frame(nlp.Header(), 'not bytes')

    def test_bootstrap_and_credit_shapes(self):
        hello = nlp.Header(**VECTORS['frames'][0]['header'])
        for field, value in [('flags',1),('channel_id',1),('src_cap',1),('packet_counter',1),('ack',1),('kind',1)]:
            with self.assertRaises(nlp.ProtocolError):
                nlp.encode_frame(replace(hello,**{field:value}))
        ack = nlp.Header(kind=nlp.ACK,opcode=0,priority=0,receive_limit=2)
        self.assertEqual(nlp.decode_frame(nlp.encode_frame(ack))[0],ack)
        with self.assertRaises(nlp.ProtocolError):
            nlp.encode_frame(ack,b'not-empty')
        with self.assertRaises(nlp.ProtocolError):
            nlp.encode_frame(replace(ack,flags=nlp.RELIABLE,reliable_seq=1))
        with self.assertRaises(nlp.ProtocolError):
            nlp.encode_frame(replace(nlp.Header(),receive_limit=1))

    def test_encryption_is_never_reported_authenticated(self):
        example = VECTORS['aad_example']
        header = nlp.Header(**example['header'])
        aad = nlp.canonical_aad(header,example['payload_len'])
        self.assertEqual(aad.hex(),example['hex'])
        self.assertEqual((aad[7],aad[48:52]),(0,bytes(4)))
        self.assertEqual(aad,nlp.canonical_aad(replace(header,hop_limit=2),8))
        self.assertNotEqual(aad,nlp.canonical_aad(replace(header,dst_node=3),8))
        with self.assertRaises(nlp.UnsupportedSecurity):
            nlp.encode_frame(header,bytes(8))
        with self.assertRaises(nlp.UnsupportedSecurity):
            nlp.decode_frame(nlp.pack_header(header,8)+bytes(8)+bytes(16))
        with self.assertRaises(nlp.ProtocolError):
            nlp.canonical_aad(nlp.Header(),0)

    def test_tlv_extensions_and_rejection(self):
        wire = nlp.encode_tlvs([(1,b'\x00'),(2,b'known'),(0x8003,b'required')])
        self.assertEqual(nlp.decode_tlvs(wire,{2,3}),{2:b'known',3:b'required'})
        with self.assertRaises(nlp.ProtocolError):
            nlp.decode_tlvs(wire,{2})
        for items in [[(0,b'')],[(1,b'a'),(1,b'b')],[(0x8001,b'a'),(1,b'b')],[(2,b''),(1,b'')],[(1,'text')],[(1,bytes(1025))],[(x,b'') for x in range(1,34)]]:
            with self.assertRaises(nlp.ProtocolError):
                nlp.encode_tlvs(items)
        for wire in [b'\1',b'\1\0\xff\xff',struct.pack('<HH',1,2)+b'x',struct.pack('<HHHH',1,0,1,0),bytes(1025),b''.join(struct.pack('<HH',x,0) for x in range(1,34))]:
            with self.assertRaises(nlp.ProtocolError):
                nlp.decode_tlvs(wire,set(range(100)))
        full=nlp.encode_tlvs([(1,bytes(1020))])
        self.assertEqual(nlp.decode_tlvs(full,{1}),{1:bytes(1020)})

    def test_cobs_vectors_and_bounds(self):
        for vector in VECTORS['cobs']:
            data, encoded = bytes.fromhex(vector['input_hex']),bytes.fromhex(vector['output_hex'])
            self.assertEqual(nlp.cobs_encode(data),encoded)
            self.assertEqual(nlp.cobs_decode(encoded),data)
            self.assertNotIn(0,encoded)
        self.assertEqual(nlp.cobs_encode(b'\0'),b'\1\1')
        self.assertEqual(nlp.cobs_encode(b'\1\0\2'),b'\2\1\2\2')
        for bad in (b'',b'\0',b'\4ab',b'\2\0'):
            with self.assertRaises(nlp.ProtocolError):
                nlp.cobs_decode(bad)
        with self.assertRaises(nlp.ProtocolError):
            nlp.cobs_decode(b'\3ab',1)

    def test_all_stream_split_positions(self):
        for vector in VECTORS['frames']:
            pdu = bytes.fromhex(vector['pdu_hex'])
            for cls, key in [(nlp.StreamDecoder,'tcp_hex'),(nlp.UartDecoder,'uart_hex')]:
                wire = bytes.fromhex(vector[key])
                for split in range(len(wire)+1):
                    parser=cls()
                    self.assertEqual(parser.feed(wire[:split])+parser.feed(wire[split:]),[pdu])
                parser=cls()
                result=[]
                for byte in wire+wire:
                    result.extend(parser.feed(bytes([byte])))
                    self.assertLessEqual(len(parser.pending),nlp.MAX_PDU+8)
                self.assertEqual(result,[pdu,pdu])

    def test_uart_noise_timeout_and_recovery(self):
        pdu=bytes.fromhex(VECTORS['frames'][1]['pdu_hex']); wire=nlp.encode_uart(pdu)
        parser=nlp.UartDecoder()
        self.assertEqual(parser.feed(b'\xffnoise\0'+wire),[pdu])
        self.assertGreater(parser.errors,0)
        self.assertEqual(parser.feed(bytes([1])*(parser.encoded_bound+50)),[])
        self.assertTrue(parser.discarding)
        self.assertEqual(len(parser.pending),0)
        self.assertEqual(parser.feed(b'\0'+wire),[pdu])
        self.assertEqual(parser.feed(wire[:6]),[])
        parser.cancel_partial()
        self.assertEqual(parser.feed(wire[6:]+wire),[pdu])
        for bad in (nlp.cobs_encode(b'\1\0x')+b'\0',nlp.cobs_encode(struct.pack('<H',56)+bytes(55))+b'\0'):
            self.assertEqual(parser.feed(bad),[])
        self.assertEqual(parser.feed(wire),[pdu])

    def test_stream_rejects_invalid_length_and_poison(self):
        for size in (0,1,55,1093,65535):
            parser=nlp.StreamDecoder()
            with self.assertRaises(nlp.ProtocolError):
                parser.feed(struct.pack('<H',size))
            self.assertTrue(parser.failed)
            self.assertEqual(len(parser.pending),0)
            with self.assertRaises(nlp.ProtocolError):
                parser.feed(b'')
        for cls in (nlp.StreamDecoder,nlp.UartDecoder):
            with self.assertRaises(nlp.ProtocolError):
                cls(55)
            with self.assertRaises(nlp.ProtocolError):
                cls().feed(bytes(4097))
        # Framing accepts opaque maximum encrypted PDU, but does not authenticate it.
        opaque=bytes(nlp.MAX_PDU)
        self.assertEqual(nlp.StreamDecoder().feed(nlp.encode_stream(opaque)),[opaque])
        self.assertEqual(nlp.UartDecoder().feed(nlp.encode_uart(opaque)),[opaque])
        with self.assertRaises(nlp.ProtocolError):
            nlp.encode_stream(bytes(nlp.MAX_PDU+1))

    def test_seeded_roundtrips_and_mutation_bound(self):
        rng=random.Random(0x4e4c5001)
        for _ in range(600):
            payload=rng.randbytes(rng.randrange(1025))
            header=replace(nlp.Header(),packet_counter=rng.randrange(1,1<<32),opcode=rng.randrange(65536),priority=rng.randrange(4),hop_limit=rng.randrange(1,17))
            frame=nlp.encode_frame(header,payload)
            self.assertEqual(nlp.decode_frame(frame),(header,payload))
            self.assertEqual(nlp.cobs_decode(nlp.cobs_encode(frame)),frame)
            self.assertLessEqual(len(nlp.encode_uart(frame)),len(frame)+2+(len(frame)+2)//254+2)
        parser=nlp.UartDecoder()
        for _ in range(1000):
            noise=rng.randbytes(rng.randrange(257))
            parser.feed(noise)
            self.assertLessEqual(len(parser.pending),parser.encoded_bound)
            try:
                nlp.decode_frame(noise)
            except nlp.ProtocolError:
                pass
        parser.feed(b'\0')
        frame=nlp.encode_frame(nlp.Header(),b'recovered')
        self.assertEqual(parser.feed(nlp.encode_uart(frame)),[frame])

    def test_can_identifier_fields_and_commissioning_ids(self):
        for priority in range(4):
            for source in (0,1,253):
                for destination in (0,1,253,255):
                    for transfer in (0,1,63):
                        value=nlp.can_identifier(priority,source,destination,transfer)
                        self.assertLess(value,1<<29)
                        self.assertEqual((value>>26,(value>>18)&255,(value>>10)&255,(value>>4)&63,value&15),(priority,source,destination,transfer,0))
        for args in [(4,1,2,0),(0,254,1,0),(0,255,1,0),(0,1,254,0),(0,1,2,64),(0,1,2,0,2)]:
            with self.assertRaises(nlp.ProtocolError):
                nlp.can_identifier(*args)
        for token in range(256):
            identifiers=[nlp.can_commission_reply_id(byte,token) for byte in range(256)]
            self.assertEqual(len(set(identifiers)),256)
            for byte,value in enumerate(identifiers):
                self.assertEqual((value>>26,(value>>18)&255,(value>>10)&255,(value>>2)&255,value&3),(0,255,255,byte,token&3))

    def test_uid_prefix_enumeration_model(self):
        # Mathematical prefix discovery only: no arbitration/timing/hardware claims.
        rng=random.Random(41)
        for uids in [set(),{bytes(16)},{rng.randbytes(16) for _ in range(253)}, {bytes(15)+bytes([i]) for i in range(253)}]:
            pending=[b''];found=set();queries=0
            while pending:
                prefix=pending.pop();queries+=1
                self.assertLessEqual(len(prefix),15)
                next_bytes={uid[len(prefix)] for uid in uids if uid.startswith(prefix)}
                self.assertEqual(len({nlp.can_commission_reply_id(byte,queries&255) for byte in next_bytes}),len(next_bytes))
                for byte in next_bytes:
                    extended=prefix+bytes([byte])
                    if len(extended)==16:
                        found.add(extended)
                    else:
                        pending.append(extended)
            self.assertEqual(found,uids)
            self.assertLessEqual(queries,1+15*len(uids))
            self.assertLessEqual(queries,4096)


def freeze_inputs():
    paths=[ROOT/'scripts/nlp_codec.py',Path(__file__).resolve(),ROOT/'docs/MICROKERNEL.md',ROOT/'docs/SOURCE_LAYOUT.md']
    paths+=sorted((ROOT/'docs/protocols/nlp').glob('*.md'))
    paths+=sorted((ROOT/'protocols/nlp/v1').glob('*.json'))
    return {str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in paths}


def main():
    before=freeze_inputs()
    stamp=datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    output=ROOT/'out/microkernel/host-nlp-protocol'/stamp
    output.mkdir(parents=True)
    stream=io.StringIO()
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(ProtocolTests)
    result=unittest.TextTestRunner(stream=stream,verbosity=2).run(suite)
    log=stream.getvalue();print(log,end='')
    after=freeze_inputs()
    markers=['NLP_REFERENCE_FORMAT_OK','NLP_INPUTS_FROZEN'] if result.wasSuccessful() and before==after else []
    report={'schema':'nos-nlp-design-check-1','scope':'host DESIGN reference format and bounded models only; NO firmware/crypto/session/routing/hardware validation','python':sys.version,'tests_run':result.testsRun,'failures':len(result.failures),'errors':len(result.errors),'skipped':len(result.skipped),'source_unchanged':before==after,'source_sha256':before,'markers':markers,'exit_status':0 if len(markers)==2 else 1}
    (output/'test.log').write_text(log)
    (output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
    print(' '.join(markers))
    print('REPORT '+str(output/'results.json'))
    return report['exit_status']


if __name__=='__main__':
    sys.exit(main())
