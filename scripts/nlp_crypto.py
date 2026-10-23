#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Noise XX and NLP KDF using cryptography primitives.

No implicit TOFU: the caller supplies a pinned peer static public key.
"""
import hashlib
import hmac
import secrets
import struct
from dataclasses import replace
from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.asymmetric.x25519 import X25519PrivateKey, X25519PublicKey
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
import nlp_codec as wire


def hkdf(salt, ikm, info=b"", length=64):
    if length > 64:
        raise ValueError("HKDF bound")
    prk = hmac.digest(salt, ikm, "sha256")
    first = hmac.digest(prk, info + b"\1", "sha256")
    second = hmac.digest(prk, first + info + b"\2", "sha256")
    return (first + second)[:length]


class Noise:
    def __init__(self, key, peer_pin, initiator, prologue, ephemeral=None):
        if len(key) != 32 or len(peer_pin) != 32:
            raise ValueError("X25519 keys must be 32 raw bytes")
        self.static = X25519PrivateKey.from_private_bytes(key)
        self.ephemeral = X25519PrivateKey.from_private_bytes(ephemeral or secrets.token_bytes(32))
        self.pin = peer_pin
        self.initiator = initiator
        self.ck = b"Noise_XX_25519_ChaChaPoly_SHA256"
        self.hash = hashlib.sha256(self.ck + prologue).digest()
        self.key = None
        self.nonce = 0
        self.step = 0
        self.complete = False

    def mix(self, data):
        self.hash = hashlib.sha256(self.hash + data).digest()

    def dh(self, key, remote):
        shared = key.exchange(X25519PublicKey.from_public_bytes(remote))
        keys = hkdf(self.ck, shared)
        self.ck, self.key = keys[:32], keys[32:]
        self.nonce = 0

    def crypt(self, encrypt, data):
        nonce = bytes(4) + struct.pack("<Q", self.nonce)
        self.nonce += 1
        cipher = ChaCha20Poly1305(self.key)
        try:
            result = (
                cipher.encrypt(nonce, data, self.hash)
                if encrypt
                else cipher.decrypt(nonce, data, self.hash)
            )
        except InvalidTag:
            raise ValueError("Noise authentication failed") from None
        self.mix(result if encrypt else data)
        return result

    def verify(self, key):
        if not hmac.compare_digest(key, self.pin):
            raise ValueError("Noise peer static key differs from pin")

    def finish(self):
        keys = hkdf(self.ck, b"")
        self.tx, self.rx = (keys[:32], keys[32:]) if self.initiator else (keys[32:], keys[:32])
        self.complete = True
        self.ck = self.key = None

    def write(self):
        if self.complete:
            raise ValueError("handshake already complete")
        if self.initiator and self.step == 0:
            result = self.ephemeral.public_key().public_bytes_raw()
            self.mix(result)
        elif not self.initiator and self.step == 1:
            result = self.ephemeral.public_key().public_bytes_raw()
            self.mix(result)
            self.dh(self.ephemeral, self.remote_ephemeral)
            result += self.crypt(True, self.static.public_key().public_bytes_raw())
            self.dh(self.static, self.remote_ephemeral)
            result += self.crypt(True, b"")
        elif self.initiator and self.step == 2:
            result = self.crypt(True, self.static.public_key().public_bytes_raw())
            self.dh(self.static, self.remote_ephemeral)
            result += self.crypt(True, b"")
            self.finish()
        else:
            raise ValueError("wrong Noise write phase")
        self.step += 1
        return result

    def read(self, data):
        if not self.initiator and self.step == 0 and len(data) == 32:
            self.remote_ephemeral = data
            self.mix(data)
        elif self.initiator and self.step == 1 and len(data) == 96:
            self.remote_ephemeral = data[:32]
            self.mix(data[:32])
            self.dh(self.ephemeral, self.remote_ephemeral)
            remote_static = self.crypt(False, data[32:80])
            self.verify(remote_static)
            self.dh(self.ephemeral, remote_static)
            if self.crypt(False, data[80:]) != b"":
                raise ValueError("unexpected Noise payload")
        elif not self.initiator and self.step == 2 and len(data) == 64:
            remote_static = self.crypt(False, data[:48])
            self.verify(remote_static)
            self.dh(self.ephemeral, remote_static)
            if self.crypt(False, data[48:]) != b"":
                raise ValueError("unexpected Noise payload")
            self.finish()
        else:
            raise ValueError("wrong Noise read phase")
        self.step += 1


class Cipher:
    def __init__(
        self,
        noise,
        session,
        channel,
        src_uid,
        dst_uid,
        src_boot,
        dst_boot,
        src_cap,
        dst_cap,
        kind,
        major=1,
        minor=0,
    ):
        def info(src, dst, sboot, dboot, scap, dcap):
            return (
                b"NLP1/channel"
                + struct.pack("<IH", session, channel)
                + src
                + dst
                + struct.pack("<QQHH", sboot, dboot, scap, dcap)
                + kind
                + struct.pack("<HH", major, minor)
            )

        tx = hkdf(
            noise.hash, noise.tx, info(src_uid, dst_uid, src_boot, dst_boot, src_cap, dst_cap), 38
        )
        rx = hkdf(
            noise.hash, noise.rx, info(dst_uid, src_uid, dst_boot, src_boot, dst_cap, src_cap), 38
        )
        self.tx_key, self.tx_prefix = tx[:32], tx[32:]
        self.rx_key, self.rx_prefix = rx[:32], rx[32:]

    def encode(self, h, payload=b""):
        h = replace(h, flags=h.flags | wire.ENCRYPTED)
        header = wire.pack_header(h, len(payload))
        aad = bytearray(header)
        aad[7] = 0
        aad[48:52] = bytes(4)
        nonce = self.tx_prefix + struct.pack("<HI", h.channel_id, h.packet_counter)
        return header + ChaCha20Poly1305(self.tx_key).encrypt(nonce, payload, bytes(aad))

    def decode(self, frame):
        h, length = wire.inspect_frame(frame)
        if not h.flags & wire.ENCRYPTED:
            raise ValueError("refusing plaintext in authenticated session")
        nonce = self.rx_prefix + struct.pack("<HI", h.channel_id, h.packet_counter)
        aad = bytearray(frame[:52])
        aad[7] = 0
        aad[48:52] = bytes(4)
        try:
            payload = ChaCha20Poly1305(self.rx_key).decrypt(nonce, frame[52:], bytes(aad))
        except InvalidTag:
            raise ValueError("NLP frame authentication failed") from None
        if len(payload) != length:
            raise ValueError("AEAD plaintext length mismatch")
        return h, payload
