#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. NLP physical-link client; never injects OS input.

TCP plaintext is restricted to loopback fixtures. USB needs optional PyUSB;
UART uses the POSIX tty module. For secure networks use the authenticated backend.
"""
import argparse
from dataclasses import replace
import json
import os
from pathlib import Path
import secrets
import select
import socket
import struct
import sys
import time
import uuid
import nlp_codec as wire


class Link:
    def __init__(self, args):
        self.mode = "tcp"
        self.device = self.fd = self.socket = None
        self.decoder = wire.StreamDecoder()
        if args.tcp:
            host, port = args.tcp.rsplit(":", 1)
            if not args.key and host not in ("localhost", "127.0.0.1", "::1"):
                raise ValueError("plaintext TCP is restricted to a loopback fixture")
            self.socket = socket.create_connection((host, int(port)), 3)
            self.socket.settimeout(0.05)
        elif args.uart:
            import termios

            self.fd = os.open(args.uart, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            self.old = termios.tcgetattr(self.fd)
            state = termios.tcgetattr(self.fd)
            state[0] = state[1] = state[3] = 0
            state[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            state[4] = state[5] = termios.B115200
            state[6][termios.VMIN] = state[6][termios.VTIME] = 0
            termios.tcsetattr(self.fd, termios.TCSANOW, state)
            self.mode = "uart"
            self.decoder = wire.UartDecoder()
        else:
            import usb.core
            import usb.util

            vendor, product = (int(value, 16) for value in args.usb.split(":"))
            self.device = usb.core.find(idVendor=vendor, idProduct=product)
            if self.device is None:
                raise ValueError("USB device not found")
            self.interface = next(
                (
                    i
                    for i in self.device.get_active_configuration()
                    if i.bInterfaceClass == 255
                    and i.bInterfaceSubClass == 0x4E
                    and i.bInterfaceProtocol == 1
                ),
                None,
            )
            if self.interface is None:
                raise ValueError(
                    "NLP vendor interface not found; refusing to claim the HID interface"
                )
            usb.util.claim_interface(self.device, self.interface.bInterfaceNumber)

    def write(self, pdu):
        framed = wire.encode_uart(pdu) if self.mode == "uart" else wire.encode_stream(pdu)
        if self.socket:
            self.socket.sendall(framed)
        elif self.fd is not None:
            deadline = time.monotonic() + 3
            while framed:
                if time.monotonic() >= deadline:
                    raise TimeoutError("UART output blocked")
                if select.select([], [self.fd], [], 0.05)[1]:
                    try:
                        framed = framed[os.write(self.fd, framed) :]
                    except BlockingIOError:
                        pass
        else:
            self.device.write(2, framed, timeout=3000)

    def read(self):
        if self.socket:
            try:
                data = self.socket.recv(4096)
            except socket.timeout:
                return []
            if not data:
                raise ConnectionError("NLP peer disconnected")
        elif self.fd is not None:
            if not select.select([self.fd], [], [], 0.05)[0]:
                return []
            data = os.read(self.fd, 4096)
        else:
            import usb.core

            try:
                data = bytes(self.device.read(0x82, 64, timeout=50))
            except usb.core.USBTimeoutError:
                return []
        return self.decoder.feed(data)

    def close(self):
        if self.socket:
            self.socket.close()
        elif self.fd is not None:
            import termios

            termios.tcsetattr(self.fd, termios.TCSANOW, self.old)
            os.close(self.fd)
        elif self.device:
            import usb.util

            usb.util.release_interface(self.device, self.interface.bInterfaceNumber)
            usb.util.dispose_resources(self.device)


class RPCError(wire.ProtocolError):
    def __init__(self, status):
        self.status = status
        super().__init__(f"RPC status {status}")


class Session:
    def __init__(self, identifier, capability=0):
        self.id, self.cap = identifier, capability
        self.counter = 1
        self.tx_seq = self.tx_ack = self.tx_limit = self.rx_seq = 0
        self.rx_limit = 1
        self.pending = None
        self.messages = []
        self.request = 0
        self.closed = None
        self.control_tx_limit = 0
        self.last_tx = 0
        self.ciphers = None


class Client:
    def __init__(self, link, uid=None, key=None, peer_pin=None):
        self.link = link
        self.key, self.peer_pin = key, peer_pin
        self.noise = None
        if key:
            from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey
            import hashlib

            uid = hashlib.sha256(
                X25519PrivateKey.from_private_bytes(key).public_key().public_bytes_raw()
            ).digest()[:16]
        self.uid = uid or uuid.uuid4().bytes
        self.boot = secrets.randbits(64) or 1
        self.sessions = {}
        self.controls = []
        self.epoch = 0
        self.node = 65534
        self.peer = 1
        self.next_id = 1
        self.major = 1

    def _control(self, opcode, body=b"", control=1):
        h = wire.Header(
            kind=0,
            flags=0,
            priority=0,
            hop_limit=1,
            fabric_epoch=self.epoch,
            src_node=self.node,
            dst_node=65535 if opcode == 1 else self.peer,
            src_cap=0,
            dst_cap=0,
            channel_id=0,
            opcode=opcode,
            session_id=0,
            packet_counter=0,
        )
        self.link.write(wire.encode_frame(h, struct.pack("<IHH", control, 0, 0) + body))

    def _header(self, s, kind, opcode=0, reliable=False, channel=1):
        h = wire.Header(
            kind=kind,
            flags=1 if reliable else 0,
            priority=0 if kind == 5 else 1,
            hop_limit=16,
            fabric_epoch=self.epoch,
            src_node=self.node,
            dst_node=self.peer,
            src_cap=0,
            dst_cap=s.cap,
            channel_id=channel,
            opcode=opcode,
            session_id=s.id,
            packet_counter=s.counter,
            reliable_seq=s.tx_seq if reliable else 0,
            ack=s.rx_seq,
            receive_limit=s.rx_limit,
        )
        s.counter += 1
        if s.counter >= 0xFFFFFFFF:
            raise ConnectionError("session counter exhausted")
        return h

    def ack(self, s, channel=1):
        if channel == 0:
            # No application data on channel0. Grant zero control-data slots.
            h = self._header(s, 5, channel=0)
            h = replace(h, ack=0, receive_limit=0)
        else:
            h = self._header(s, 5)
        self.link.write(s.ciphers[channel].encode(h) if s.ciphers else wire.encode_frame(h))
        s.last_tx = time.monotonic()

    def pump(self):
        for pdu in self.link.read():
            h, _ = wire.inspect_frame(pdu)
            session = self.sessions.get(h.session_id)
            if h.session_id and session is None:
                continue
            if h.channel_id >= 2:
                raise wire.ProtocolError("unknown channel")
            if session and session.ciphers:
                h, payload = session.ciphers[h.channel_id].decode(pdu)
            else:
                h, payload = wire.decode_frame(pdu)
            if not h.session_id:
                if len(payload) < 8 or struct.unpack_from("<H", payload, 6)[0]:
                    raise wire.ProtocolError("invalid control response")
                self.controls.append((h, payload))
                if len(self.controls) > 8:
                    raise wire.ProtocolError("control queue full")
                continue
            s = self.sessions.get(h.session_id)
            if s is None:
                continue  # Periodic credit ACK is repeated after OPEN is consumed.
            if (h.fabric_epoch, h.src_node, h.dst_node, h.src_cap, h.dst_cap) != (
                self.epoch,
                self.peer,
                self.node,
                s.cap,
                0,
            ):
                raise wire.ProtocolError("session endpoint mismatch")
            if h.kind == 6:
                if len(payload) != 8 or h.opcode != 15:
                    raise wire.ProtocolError("invalid CLOSE")
                s.closed = struct.unpack_from("<H", payload, 4)[0]
                continue
            if h.channel_id == 0:
                s.control_tx_limit = max(s.control_tx_limit, h.receive_limit)
                continue
            if h.channel_id != 1 or h.ack > s.tx_seq or h.receive_limit - h.ack > 1:
                raise wire.ProtocolError("invalid channel or acknowledgement")
            s.tx_ack = max(s.tx_ack, h.ack)
            s.tx_limit = max(s.tx_limit, h.receive_limit)
            if s.pending and s.tx_ack >= s.tx_seq:
                s.pending = None
            if h.kind == 5:
                continue
            if h.flags & 1:
                if (
                    h.reliable_seq == s.rx_seq + 1
                    and h.reliable_seq <= s.rx_limit
                    and not s.messages
                ):
                    s.messages.append((h, payload))
                    s.rx_seq += 1
                elif h.reliable_seq > s.rx_seq:
                    raise wire.ProtocolError("peer exceeded reserved inbox")
                self.ack(s)
            elif h.kind == 3:
                # Best-effort logging: bounded queue with explicit drop policy.
                if not s.messages:
                    s.messages.append((h, payload))
        now = time.monotonic()
        for s in self.sessions.values():
            if s.pending and now >= s.pending[1]:
                frame, _, retries = s.pending
                if retries >= 5:
                    raise TimeoutError("reliable retries exhausted")
                self.link.write(frame)
                s.pending = (frame, now + min(0.1 * (2 ** (retries + 1)), 2), retries + 1)
            if now - s.last_tx >= 5:
                self.ack(s)

    def wait(self, predicate, timeout=5):
        deadline = time.monotonic() + timeout
        while not predicate():
            if time.monotonic() >= deadline:
                raise TimeoutError("NLP response timeout")
            self.pump()

    def control_reply(self, opcode):
        self.wait(lambda: any(h.opcode == opcode for h, _ in self.controls))
        index = next(i for i, (h, _) in enumerate(self.controls) if h.opcode == opcode)
        h, payload = self.controls.pop(index)
        status = struct.unpack_from("<H", payload, 4)[0]
        if status:
            raise wire.ProtocolError(f"control status {status}")
        return h, wire.decode_tlvs(payload[8:], set(range(1, 23)))

    def connect(self):
        nonce = secrets.randbits(32)
        hello = wire.encode_tlvs(
            [
                (1, struct.pack("<I", nonce)),
                (2, self.uid),
                (3, struct.pack("<Q", self.boot)),
                (4, b"\1\1"),
                (5, struct.pack("<H", 128)),
                (6, b"\1\0"),
                (7, b"\2\0" if self.key else b"\1\0"),
                (8, b"\1\0"),
            ]
        )
        self._control(1, hello)
        h, info = self.control_reply(2)
        if info.get(1) != struct.pack("<I", nonce) or not (
            struct.unpack("<H", info[7])[0] & (2 if self.key else 1)
        ):
            raise wire.ProtocolError("physical profile unavailable or discovery nonce mismatch")
        self.peer = h.src_node
        self.peer_uid = info[2]
        self.fabric = info[9]
        self.epoch = struct.unpack("<I", info[10])[0]
        self.peer_boot = struct.unpack("<Q", info[3])[0]
        proposed = 1 if self.uid < self.peer_uid else 0
        if self.key:
            self.authenticate(nonce, proposed)
        body = wire.encode_tlvs(
            [
                (9, self.fabric),
                (10, info[10]),
                (13, self.peer_uid),
                (17, struct.pack("<I", proposed)),
            ]
        )
        if self.key:
            self._join_control(body)
        else:
            self._control(3, body, 4)
        join_header, join = self.control_reply(4)
        if self.key:
            self._verify_join(join_header, join)
        self.node = struct.unpack("<H", join[11])[0]
        manager = struct.unpack("<I", join[17])[0]
        self.next_id = max(2, manager + 1)
        self.manager = Session(manager)
        self.secure_session(self.manager)
        self.sessions[manager] = self.manager
        self.ack(self.manager)
        return self

    def authenticate(self, nonce, proposed):
        from nlp_crypto import Noise
        import hashlib

        if hashlib.sha256(self.peer_pin).digest()[:16] != self.peer_uid:
            raise ValueError("peer UID is not bound to the pinned public key")
        initiator = self.uid < self.peer_uid
        if not initiator:
            body = wire.encode_tlvs([(15, b"\1"), (17, bytes(4))])
            self._control(18, body, 2)
            _, response = self.control_reply(18)
            proposed = struct.unpack("<I", response[17])[0]
        self.auth_id = proposed
        low, high = (self.uid, self.peer_uid) if initiator else (self.peer_uid, self.uid)
        lowboot, highboot = (
            (self.boot, self.peer_boot) if initiator else (self.peer_boot, self.boot)
        )
        prologue = (
            b"NLP1AUTH"
            + struct.pack("<HHHH", 1, 128, 2, 1)
            + self.fabric
            + struct.pack("<III", self.epoch, nonce, proposed)
            + low
            + high
            + struct.pack("<QQ", lowboot, highboot)
        )
        self.noise = Noise(self.key, self.peer_pin, initiator, prologue)
        if initiator:
            body = wire.encode_tlvs(
                [(15, b"\1"), (16, self.noise.write()), (17, struct.pack("<I", proposed))]
            )
            self._control(18, body, 2)
            _, response = self.control_reply(19)
            self.noise.read(response[16])
            body = wire.encode_tlvs(
                [(15, b"\3"), (16, self.noise.write()), (17, struct.pack("<I", proposed))]
            )
            self._control(20, body, 3)
        else:
            self.noise.read(response[16])
            body = wire.encode_tlvs(
                [(15, b"\2"), (16, self.noise.write()), (17, struct.pack("<I", proposed))]
            )
            self._control(19, body, 3)
            _, response = self.control_reply(20)
            self.noise.read(response[16])

    def _join_mac(self, header, payload, transmit):
        from nlp_crypto import hkdf
        import hmac

        src, dst = (self.uid, self.peer_uid) if transmit else (self.peer_uid, self.uid)
        sb, db = (self.boot, self.peer_boot) if transmit else (self.peer_boot, self.boot)
        info = (
            b"NLP1/join"
            + struct.pack("<I", self.auth_id)
            + src
            + dst
            + struct.pack("<QQ", sb, db)
            + self.fabric
            + struct.pack("<I", self.epoch)
        )
        key = hkdf(self.noise.hash, self.noise.tx if transmit else self.noise.rx, info, 32)
        aad = bytearray(wire.pack_header(header, len(payload) + 36))
        aad[7] = 0
        aad[48:52] = bytes(4)
        return hmac.digest(key, bytes(aad) + payload, "sha256")

    def _join_control(self, body):
        h = wire.Header(
            kind=0,
            flags=0,
            priority=0,
            hop_limit=1,
            fabric_epoch=self.epoch,
            src_node=self.node,
            dst_node=self.peer,
            src_cap=0,
            dst_cap=0,
            channel_id=0,
            opcode=3,
            session_id=0,
            packet_counter=0,
        )
        payload = struct.pack("<IHH", 4, 0, 0) + body
        payload += struct.pack("<HH", 22, 32) + self._join_mac(h, payload, True)
        self.link.write(wire.encode_frame(h, payload))

    def _verify_join(self, header, tlvs):
        import hmac

        if 22 not in tlvs:
            raise ValueError("missing JOIN authentication")
        # Canonical registered singleton TLVs, no padding/duplicates.
        payload = struct.pack("<IHH", 4, 0, 0) + wire.encode_tlvs(
            [(tag, value) for tag, value in sorted(tlvs.items()) if tag != 22]
        )
        if not hmac.compare_digest(tlvs[22], self._join_mac(header, payload, False)):
            raise ValueError("JOIN authentication failed")

    def secure_session(self, session, cap=None):
        if not self.noise:
            return
        from nlp_crypto import Cipher

        kind = (
            bytes.fromhex("04fd1cc6c2ff56d5886551d04d84c748")
            if cap is None
            else uuid.UUID(cap["type"]).bytes
        )
        session.ciphers = [
            Cipher(
                self.noise,
                session.id,
                ch,
                self.uid,
                self.peer_uid,
                self.boot,
                self.peer_boot,
                0,
                session.cap,
                kind,
            )
            for ch in (0, 1)
        ]

    def receive(self, s):
        self.wait(lambda: bool(s.messages) or s.closed is not None)
        if s.closed is not None:
            raise ConnectionError(f"NLP capability closed: status {s.closed}")
        h, payload = s.messages.pop(0)
        if h.flags & 1:
            s.rx_limit += 1
            self.ack(s)
        return h, payload

    def send(self, s, kind, opcode, payload):
        self.wait(lambda: s.tx_seq < s.tx_limit and s.pending is None)
        s.tx_seq += 1
        header = self._header(s, kind, opcode, True)
        frame = (
            s.ciphers[1].encode(header, payload)
            if s.ciphers
            else wire.encode_frame(header, payload)
        )
        self.link.write(frame)
        s.last_tx = time.monotonic()
        s.pending = (frame, time.monotonic() + 0.1, 0)

    def call(self, s, opcode, body=b""):
        if len(body) > 120:
            raise ValueError("RPC exceeds negotiated 128-byte payload")
        self.wait(lambda: s.tx_seq < s.tx_limit and s.pending is None)
        s.request += 1
        s.tx_seq += 1
        payload = struct.pack("<IHBB", s.request, 0, 0, 0) + body
        header = self._header(s, 1, opcode, True)
        frame = (
            s.ciphers[1].encode(header, payload)
            if s.ciphers
            else wire.encode_frame(header, payload)
        )
        self.link.write(frame)
        s.last_tx = time.monotonic()
        s.pending = (frame, time.monotonic() + 0.1, 0)
        h, response = self.receive(s)
        if (
            h.kind != 2
            or h.opcode != opcode
            or len(response) < 8
            or struct.unpack_from("<I", response)[0] != s.request
            or response[6:8] != b"\0\0"
        ):
            raise wire.ProtocolError("RPC response does not match request")
        status = struct.unpack_from("<H", response, 4)[0]
        if status:
            raise RPCError(status)
        return response[8:]

    def close(self):
        if getattr(self, "manager", None) and self.manager.control_tx_limit:
            h = self._header(self.manager, 6, 15, True, 0)
            h = replace(h, reliable_seq=1, ack=0, receive_limit=0)
            payload = struct.pack("<IHH", 1, 13, 0)
            self.link.write(
                self.manager.ciphers[0].encode(h, payload)
                if self.manager.ciphers
                else wire.encode_frame(h, payload)
            )

    def close_session(self, s):
        self.call(self.manager, 15, struct.pack("<I", s.id))
        self.sessions.pop(s.id, None)

    def topology(self):
        cursor = revision = 0
        result = []
        for _ in range(64):
            data = self.call(self.manager, 23, struct.pack("<IIHH", revision, cursor, 2, 0))
            if len(data) < 12:
                raise wire.ProtocolError("short topology page")
            revision, cursor, count, flags = struct.unpack_from("<IIHH", data)
            if len(data) != 12 + count * 40 or flags & ~1:
                raise wire.ProtocolError("invalid topology page")
            for i in range(count):
                uid, boot, node, parent, limit, caps, desc, lease = struct.unpack_from(
                    "<16sQHHHHII", data, 12 + i * 40
                )
                result.append(
                    dict(
                        uid=str(uuid.UUID(bytes=uid)),
                        boot=boot,
                        node=node,
                        parent=parent,
                        payload=limit,
                        capabilities=caps,
                        revision=desc,
                        lease_ms=lease,
                    )
                )
            if not flags:
                return result
            if not cursor or not count:
                raise wire.ProtocolError("nonprogressing topology page")
        raise wire.ProtocolError("topology page limit")

    def capabilities(self):
        cursor = revision = 0
        result = []
        for _ in range(64):
            page = self.call(self.manager, 8, struct.pack("<IIHH", revision, cursor, 4, 0))
            if len(page) < 12:
                raise wire.ProtocolError("short directory page")
            rev, cursor, count, flags = struct.unpack_from("<IIHH", page)
            if (revision and rev != revision) or len(page) != 12 + count * 28 or flags & ~1:
                raise wire.ProtocolError("invalid directory snapshot")
            revision = rev
            for i in range(count):
                cap, roles, kind, major, minor, desc = struct.unpack_from(
                    "<HH16sHHI", page, 12 + i * 28
                )
                result.append(
                    dict(
                        id=cap,
                        roles=roles,
                        type=str(uuid.UUID(bytes=kind)),
                        major=major,
                        minor=minor,
                        revision=desc,
                    )
                )
            if not flags & 1:
                if cursor:
                    raise wire.ProtocolError("unexpected terminal cursor")
                return result
            if not cursor or not count:
                raise wire.ProtocolError("nonprogressing cursor")
        raise wire.ProtocolError("directory page limit")

    def open(self, cap, permissions, reliable=True):
        proposed = self.next_id if self.uid < self.peer_uid else 0
        if proposed:
            self.next_id += 1
        body = struct.pack(
            "<IHH16sHHQIHBBB3xI",
            proposed,
            cap["id"],
            0,
            uuid.UUID(cap["type"]).bytes,
            cap["major"],
            cap["minor"],
            0,
            permissions,
            128,
            int(reliable),
            2,
            1,
            15000,
        )
        response = self.call(self.manager, 14, body)
        if len(response) != 24:
            raise wire.ProtocolError("invalid OPEN response")
        identifier = struct.unpack_from("<I", response)[0]
        s = Session(identifier, cap["id"])
        self.secure_session(s, cap)
        self.sessions[identifier] = s
        self.ack(s)
        return s


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    transport = parser.add_mutually_exclusive_group(required=True)
    transport.add_argument("--tcp", metavar="127.0.0.1:PORT")
    transport.add_argument("--uart", metavar="TTY")
    transport.add_argument("--usb", metavar="VID:PID")
    parser.add_argument(
        "--key", type=Path, help="32-byte raw X25519 static private key file, mode 0600"
    )
    parser.add_argument("--peer-pin", type=Path, help="32-byte raw pinned peer public key file")
    parser.add_argument(
        "--physical",
        action="store_true",
        help="explicitly trust this dedicated physical link; no encryption",
    )
    parser.add_argument(
        "--identity", type=Path, help="persistent host UUID JSON file (created with mode 0600)"
    )
    parser.add_argument(
        "command",
        choices=[
            "discover",
            "caps",
            "topology",
            "state",
            "monitor",
            "logs",
            "config-get",
            "config-put",
            "shell",
        ],
    )
    parser.add_argument(
        "--exec",
        dest="terminal_command",
        help="terminal input; include exit if the remote shell should finish",
    )
    parser.add_argument("--file", type=Path, help="canonical keymap input/output file")
    parser.add_argument(
        "--save", action="store_true", help="request durable keymap save, not just application"
    )
    parser.add_argument(
        "--wait-unlock",
        type=int,
        default=0,
        help="config-put: keep this connection for up to N seconds awaiting physical unlock",
    )
    parser.add_argument("--count", type=int, default=10)
    args = parser.parse_args()
    if args.identity and args.key:
        parser.error("secure identity derives from the key; omit --identity")
    if args.physical == bool(args.key) or bool(args.key) != bool(args.peer_pin):
        parser.error(
            "select either --physical or both --key and --peer-pin; no automatic downgrade"
        )
    if not 1 <= args.count <= 10000:
        parser.error("count must be in 1..10000")
    if not 0 <= args.wait_unlock <= 120 or (args.wait_unlock and args.command != "config-put"):
        parser.error("--wait-unlock is config-put only and must be in 0..120 seconds")
    uid = None
    if args.identity:
        if args.identity.exists():
            uid = uuid.UUID(json.loads(args.identity.read_text())["uid"]).bytes
        else:
            uid = uuid.uuid4().bytes
            fd = os.open(args.identity, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, "w") as stream:
                json.dump({"uid": str(uuid.UUID(bytes=uid))}, stream)
    link = Link(args)
    client = None
    try:
        key = pin = None
        if args.key:
            if args.key.stat().st_mode & 0o077:
                raise ValueError("private key file must not be readable by group/others")
            key, pin = args.key.read_bytes(), args.peer_pin.read_bytes()
            if len(key) != 32 or len(pin) != 32:
                raise ValueError("key and peer pin must contain exactly 32 raw bytes")
        client = Client(link, uid, key, pin).connect()
        if args.command == "topology":
            print(json.dumps(client.topology(), indent=2))
            return
        caps = client.capabilities()
        if args.command in ("discover", "caps"):
            print(
                json.dumps(
                    {
                        "uid": str(uuid.UUID(bytes=client.peer_uid)),
                        "security": (
                            "NOISE_XX_PINNED" if args.key else "PHYSICALLY_TRUSTED_UNENCRYPTED"
                        ),
                        "capabilities": caps,
                    },
                    indent=2,
                )
            )
            return
        types = {
            cap["name"]: cap["type_uuid"]
            for cap in json.loads(
                (Path(__file__).resolve().parents[1] / "protocols/nlp/v1/registry.json").read_text()
            )["capabilities"]
        }
        name = (
            "shell"
            if args.command == "shell"
            else (
                "keymap"
                if args.command.startswith("config-")
                else "log" if args.command == "logs" else "keyboard"
            )
        )
        cap = next((c for c in caps if c["type"] == types[name]), None)
        if cap is None:
            raise ValueError(f"{name} capability not advertised")
        permissions = (
            32
            if name == "shell"
            else (
                (9 if args.command == "config-put" else 1)
                if name == "keymap"
                else 16 if name == "log" else 2
            )
        )
        deadline = time.monotonic() + args.wait_unlock
        announced = False
        while True:
            try:
                session = client.open(cap, permissions, reliable=True)
                break
            except RPCError as error:
                if error.status != 3 or not args.wait_unlock or time.monotonic() >= deadline:
                    raise
                if not announced:
                    print(
                        "Connected: hold Esc+PN+Fn for two seconds to unlock this peer.",
                        file=sys.stderr,
                    )
                    announced = True
                retry_at = min(deadline, time.monotonic() + 0.25)
                while time.monotonic() < retry_at:
                    client.pump()
        if name == "shell":
            response = client.call(session, 1, struct.pack("<HHHH", 80, 24, 0, 5) + b"xterm")
            terminal_id = struct.unpack_from("<I", response)[0]
            if args.terminal_command:
                data = args.terminal_command.encode() + b"\n"
                for offset in range(0, len(data), 124):
                    client.send(
                        session,
                        4,
                        0x2001,
                        struct.pack("<I", terminal_id) + data[offset : offset + 124],
                    )
            while session.closed is None:
                if session.messages:
                    h, data = client.receive(session)
                    if len(data) < 4 or struct.unpack_from("<I", data)[0] != terminal_id:
                        raise wire.ProtocolError("terminal ID mismatch")
                    if h.kind == 4 and h.opcode == 0x2001:
                        sys.stdout.buffer.write(data[4:])
                        sys.stdout.buffer.flush()
                    elif h.kind == 3 and h.opcode == 0x1001 and len(data) == 8:
                        return struct.unpack_from("<i", data, 4)[0] & 255
                    else:
                        raise wire.ProtocolError("unexpected terminal message")
                else:
                    if not args.terminal_command and select.select([sys.stdin], [], [], 0)[0]:
                        data = os.read(sys.stdin.fileno(), 120)
                        if not data:
                            client.close_session(session)
                            return
                        client.send(session, 4, 0x2001, struct.pack("<I", terminal_id) + data)
                    client.pump()
            raise ConnectionError(f"terminal disconnected: {session.closed}")
        if name == "keymap":
            if args.file is None:
                raise ValueError("config-get/config-put require --file")
            info = client.call(session, 1)
            generation, length = struct.unpack_from("<II", info)
            if length != 1056:
                raise ValueError("unsupported keymap schema size")
            if args.command == "config-get":
                blob = bytearray()
                while len(blob) < length:
                    offset = len(blob)
                    size = min(108, length - offset)
                    response = client.call(session, 3, struct.pack("<QIHH", 0, offset, size, 0))
                    gen, at, count, reserved = struct.unpack_from("<IIHH", response)
                    if (gen, at, count, reserved) != (generation, offset, size, 0) or len(
                        response
                    ) != 12 + size:
                        raise wire.ProtocolError("keymap changed or malformed page")
                    blob.extend(response[12:])
                with args.file.open("xb") as output:
                    output.write(blob)
                print(json.dumps({"generation": generation, "bytes": len(blob)}))
                return
            import hashlib

            blob = args.file.read_bytes()
            if len(blob) != length:
                raise ValueError("keymap input must contain exactly 1056 canonical bytes")
            tx = struct.unpack(
                "<Q",
                client.call(
                    session,
                    2,
                    struct.pack("<II", generation, length) + hashlib.sha256(blob).digest(),
                ),
            )[0]
            for offset in range(0, length, 104):
                data = blob[offset : offset + 104]
                client.call(session, 4, struct.pack("<QIHH", tx, offset, len(data), 0) + data)
            client.call(session, 5, struct.pack("<Q", tx))
            response = client.call(
                session, 6, struct.pack("<QIB3x", tx, generation, int(args.save))
            )
            committed, state = struct.unpack_from("<IB", response)
            print(
                json.dumps(
                    {
                        "generation": committed,
                        "persistence": ["APPLIED_VOLATILE", "SAVE_PENDING", "PERSISTED"][state],
                    }
                )
            )
            return
        if args.command == "state":
            print(client.call(session, 1).hex())
            return
        if name == "keyboard":
            client.call(session, 2, struct.pack("<HH", 300, 100))
        else:
            client.call(session, 1, struct.pack("<BBHHH", 63, 0, 0, 16, 1))
        for _ in range(args.count):
            h, payload = client.receive(session)
            print(json.dumps({"opcode": h.opcode, "payload_hex": payload.hex()}), flush=True)
    finally:
        if client is not None:
            try:
                client.close()
            except (OSError, ValueError):
                pass
        link.close()


if __name__ == "__main__":
    try:
        sys.exit(main() or 0)
    except (OSError, ValueError, TimeoutError, ConnectionError) as error:
        print(f"nlp-host: {error}", file=sys.stderr)
        sys.exit(1)
