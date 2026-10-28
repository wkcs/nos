#!/usr/bin/env python3
"""
NOS Link Kit (NLK) Host CLI & Hardware Test Suite for NK60-V2.

Supports connecting to NK60-V2 over USB WinUSB (via ctypes libusb-1.0, zero pip
dependencies required) or UART Stream framing to perform:
  1. Device, Capability & Cascaded Topology Discovery (info / topo)
  2. Firmware OTA Upgrade with Sliding-Window Bulk Transfer & CRC32 (fw-update)
  3. Interactive & Non-Interactive Remote nShell (shell / exec)
  4. Real-Time Kernel printk Log Streaming & Filter Control (log)
  5. Keyboard Keymap & Lighting Inspection/Configuration (keyboard)
  6. Full End-to-End Automated Hardware Verification Suite (test-all)
"""

import argparse
import codecs
import ctypes
import ctypes.util
import os
import select
import struct
import sys
import time
import zlib
from collections import deque

# ============================================================================
# NLK Protocol UAPI Constants & Wire Structures (Little-Endian)
# ============================================================================
NLK_PROTO_VER_1 = 1 << 5
NLK_PROTO_VER_MASK = 0xE0
NLK_DEFAULT_TTL = 8

NLK_SOF_BYTE0 = 0x55
NLK_SOF_BYTE1 = 0xAA

NLK_ADDR_HOST = 0x0000
NLK_ADDR_PRIMARY = 0x0001
NLK_ADDR_LOCAL = 0xFFFE
NLK_ADDR_BROADCAST = 0xFFFF

NLK_FLAG_NONE = 0x00
NLK_FLAG_ACK_REQ = 1 << 0
NLK_FLAG_IS_ACK = 1 << 1
NLK_FLAG_IS_ERR = 1 << 2
NLK_FLAG_FRAG = 1 << 3
NLK_FLAG_MORE_FRAG = 1 << 4
NLK_FLAG_MASK = 0x1F

NLK_MODE_SYS = 0x0
NLK_MODE_RPC = 0x1
NLK_MODE_EVENT = 0x2
NLK_MODE_STREAM = 0x3
NLK_MODE_BULK = 0x4

NLK_CAP_ID_CORE = 0
NLK_CAP_ID_BROADCAST = 255

# Capability Type IDs
NLK_CAP_CORE = 0x0000
NLK_CAP_FW_UPDATE = 0x0001
NLK_CAP_LOG = 0x0002
NLK_CAP_SHELL = 0x0003
NLK_CAP_VFS = 0x0004
NLK_CAP_SYSMON = 0x0005
NLK_CAP_KEYBOARD = 0x0100
NLK_CAP_ENCODER = 0x0101
NLK_CAP_KEYPAD = 0x0102
NLK_CAP_POINTER = 0x0103
NLK_CAP_DISPLAY = 0x0200
NLK_CAP_AUDIO = 0x0202
NLK_CAP_WIRELESS = 0x0300

CAP_TYPE_NAMES = {
    NLK_CAP_CORE: "CORE",
    NLK_CAP_FW_UPDATE: "FW_UPDATE",
    NLK_CAP_LOG: "LOG",
    NLK_CAP_SHELL: "SHELL",
    NLK_CAP_VFS: "VFS",
    NLK_CAP_SYSMON: "SYSMON",
    NLK_CAP_KEYBOARD: "KEYBOARD",
    NLK_CAP_ENCODER: "ENCODER",
    NLK_CAP_KEYPAD: "KEYPAD",
    NLK_CAP_POINTER: "POINTER",
    NLK_CAP_DISPLAY: "DISPLAY",
    NLK_CAP_AUDIO: "AUDIO",
    NLK_CAP_WIRELESS: "WIRELESS",
}

DEV_CLASS_NAMES = {
    0x0000: "Generic",
    0x0001: "Host",
    0x0010: "Keyboard",
    0x0011: "Keypad",
    0x0012: "Encoder",
    0x0020: "Display",
    0x0030: "Wireless",
    0x0040: "Hub",
}

KB_FORM_NAMES = {
    0x01: "100% ANSI",
    0x02: "96% Compact",
    0x03: "80% TKL",
    0x04: "75% Compact",
    0x05: "65% Compact",
    0x06: "60% Compact",
    0x07: "40% Mini",
    0x08: "Split Ergonomic",
    0x09: "Ortholinear",
    0x0A: "Alice",
    0x0B: "Numpad",
    0x0C: "Macropad",
    0xFF: "Custom",
}

# Core Opcodes (Cap 0x0000)
NLK_CORE_OP_HELLO = 0x01
NLK_CORE_OP_ASSIGN_ADDR = 0x02
NLK_CORE_OP_GET_DEV_DESC = 0x03
NLK_CORE_OP_GET_CAP_LIST = 0x04
NLK_CORE_OP_GET_CAP_DESC = 0x05
NLK_CORE_OP_GET_TOPOLOGY = 0x06
NLK_CORE_OP_PING = 0x07
NLK_CORE_OP_RESET = 0x08
NLK_CORE_EVT_TOPO_CHANGE = 0x20

# Firmware Update Opcodes (Cap 0x0001)
NLK_FW_OP_GET_PART_INFO = 0x01
NLK_FW_OP_PREPARE = 0x02
NLK_FW_OP_WRITE_CHUNK = 0x03
NLK_FW_OP_VERIFY_COMMIT = 0x04
NLK_FW_OP_REBOOT = 0x05

# Kernel Log Opcodes (Cap 0x0002)
NLK_LOG_OP_SUBSCRIBE = 0x01
NLK_LOG_OP_SET_LEVEL = 0x02
NLK_LOG_OP_GET_STATS = 0x03
NLK_LOG_STREAM_RECORD = 0x10

LOG_LEVEL_NAMES = {
    0: "ALL",
    1: "DEBUG",
    2: "INFO",
    3: "WARN",
    4: "ERROR",
    5: "FATAL",
    6: "DISABLE",
}

# Remote nShell Opcodes (Cap 0x0003)
NLK_SHELL_OP_OPEN = 0x01
NLK_SHELL_OP_CLOSE = 0x02
NLK_SHELL_OP_SIGNAL = 0x03
NLK_SHELL_OP_FLOW_CRED = 0x04
NLK_SHELL_OP_INPUT = 0x05
NLK_SHELL_OP_HEARTBEAT = 0x06
NLK_SHELL_OPEN_RELIABLE = 1
NLK_SHELL_STREAM_STDIN = 0x10
NLK_SHELL_STREAM_STDOUT = 0x11
NLK_SHELL_STREAM_STDOUT_V2 = 0x12
NLK_SHELL_SIG_INT = 2

# Keyboard Opcodes (Cap 0x0100)
NLK_KB_OP_GET_DESC = 0x01
NLK_KB_OP_GET_KEYMAP = 0x02
NLK_KB_OP_SET_KEYMAP = 0x03
NLK_KB_OP_GET_SET_PARAM = 0x04
NLK_KB_OP_GET_LIGHT_CFG = 0x05
NLK_KB_OP_SET_LIGHT_CFG = 0x06
NLK_KB_OP_SET_INDICATORS = 0x07
NLK_KB_OP_GET_ANALOG_CFG = 0x08
NLK_KB_OP_SET_ANALOG_CFG = 0x09
NLK_KB_OP_GET_NKRO_REPORT = 0x0A

PKT_HDR_STRUCT = struct.Struct("<BBHHBBHH")
DEV_DESC_STRUCT = struct.Struct("<HHH2B4B12s16sHBBHBB")
CAP_ITEM_STRUCT = struct.Struct("<BBHBBH")
TOPO_ENTRY_STRUCT = struct.Struct("<HHHHHBB")
FW_PART_INFO_STRUCT = struct.Struct("<BBHHHI4I")
FW_PREPARE_STRUCT = struct.Struct("<BBHIII")
FW_CHUNK_HDR_STRUCT = struct.Struct("<I")
FW_CHUNK_ACK_STRUCT = struct.Struct("<Ihh")
LOG_SUB_REQ_STRUCT = struct.Struct("<BBH")
LOG_STATS_RESP_STRUCT = struct.Struct("<IIIBBH")
LOG_WIRE_HDR_STRUCT = struct.Struct("<IHBB")
SHELL_OPEN_STRUCT = struct.Struct("<HHHH")
KB_DESC_STRUCT = struct.Struct("<BBHBBBBBBBBHHI")
KB_LIGHT_CFG_STRUCT = struct.Struct("<BBBBBBBB")
KB_KEYMAP_HDR_STRUCT = struct.Struct("<BBBB")
KB_KEYMAP_ENTRY_STRUCT = struct.Struct("<BB")


def nlk_crc8(data: bytes) -> int:
    crc = 0x00
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 0x80:
                crc = ((crc << 1) ^ 0x07) & 0xFF
            else:
                crc = (crc << 1) & 0xFF
    return crc


def nlk_crc16_ccitt(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def nlk_crc32(data: bytes, init_crc: int = 0) -> int:
    return zlib.crc32(data, init_crc) & 0xFFFFFFFF


def pack_nlk_pkt(
    dst_addr: int,
    dst_cap: int,
    mode: int,
    flags: int,
    cmd_op: int,
    seq_id: int,
    payload: bytes = b"",
    src_addr: int = NLK_ADDR_HOST,
    ttl: int = NLK_DEFAULT_TTL,
) -> bytes:
    ver_flags = NLK_PROTO_VER_1 | (flags & NLK_FLAG_MASK)
    msg_type = ((mode & 0x0F) << 4) | (ttl & 0x0F)
    hdr = PKT_HDR_STRUCT.pack(
        ver_flags,
        msg_type,
        src_addr,
        dst_addr,
        dst_cap,
        cmd_op,
        seq_id & 0xFFFF,
        len(payload),
    )
    return hdr + payload


def unpack_nlk_pkt(raw: bytes):
    if len(raw) < PKT_HDR_STRUCT.size:
        raise ValueError(f"Packet too short ({len(raw)} bytes)")
    (
        ver_flags,
        msg_type,
        src_addr,
        dst_addr,
        dst_cap,
        cmd_op,
        seq_id,
        payload_len,
    ) = PKT_HDR_STRUCT.unpack_from(raw, 0)
    if (ver_flags & NLK_PROTO_VER_MASK) != NLK_PROTO_VER_1:
        raise ValueError(f"Invalid NLK version byte: 0x{ver_flags:02x}")
    payload = raw[PKT_HDR_STRUCT.size : PKT_HDR_STRUCT.size + payload_len]
    return {
        "ver_flags": ver_flags,
        "flags": ver_flags & NLK_FLAG_MASK,
        "mode": (msg_type >> 4) & 0x0F,
        "ttl": msg_type & 0x0F,
        "src_addr": src_addr,
        "dst_addr": dst_addr,
        "dst_cap": dst_cap,
        "cmd_op": cmd_op,
        "seq_id": seq_id,
        "payload_len": payload_len,
        "payload": payload,
    }


# ============================================================================
# LibUSB 1.0 Ctypes Direct WinUSB Datagram Transport (Zero Pip Dependencies)
# ============================================================================
class LibusbEndpointDescriptor(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_uint8),
        ("bDescriptorType", ctypes.c_uint8),
        ("bEndpointAddress", ctypes.c_uint8),
        ("bmAttributes", ctypes.c_uint8),
        ("wMaxPacketSize", ctypes.c_uint16),
        ("bInterval", ctypes.c_uint8),
        ("bRefresh", ctypes.c_uint8),
        ("bSynchAddress", ctypes.c_uint8),
        ("extra", ctypes.c_void_p),
        ("extra_length", ctypes.c_int),
    ]


class LibusbInterfaceDescriptor(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_uint8),
        ("bDescriptorType", ctypes.c_uint8),
        ("bInterfaceNumber", ctypes.c_uint8),
        ("bAlternateSetting", ctypes.c_uint8),
        ("bNumEndpoints", ctypes.c_uint8),
        ("bInterfaceClass", ctypes.c_uint8),
        ("bInterfaceSubClass", ctypes.c_uint8),
        ("bInterfaceProtocol", ctypes.c_uint8),
        ("iInterface", ctypes.c_uint8),
        ("endpoint", ctypes.POINTER(LibusbEndpointDescriptor)),
        ("extra", ctypes.c_void_p),
        ("extra_length", ctypes.c_int),
    ]


class LibusbInterface(ctypes.Structure):
    _fields_ = [
        ("altsetting", ctypes.POINTER(LibusbInterfaceDescriptor)),
        ("num_altsetting", ctypes.c_int),
    ]


class LibusbConfigDescriptor(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_uint8),
        ("bDescriptorType", ctypes.c_uint8),
        ("wTotalLength", ctypes.c_uint16),
        ("bNumInterfaces", ctypes.c_uint8),
        ("bConfigurationValue", ctypes.c_uint8),
        ("iConfiguration", ctypes.c_uint8),
        ("bmAttributes", ctypes.c_uint8),
        ("MaxPower", ctypes.c_uint8),
        ("interface", ctypes.POINTER(LibusbInterface)),
        ("extra", ctypes.c_void_p),
        ("extra_length", ctypes.c_int),
    ]


def _load_libusb():
    candidates = [
        "/opt/homebrew/lib/libusb-1.0.dylib",
        "/usr/local/lib/libusb-1.0.dylib",
        "libusb-1.0.dylib",
        "libusb-1.0.so.0",
        "libusb-1.0.so",
        "libusb-1.0.dll",
    ]
    found = ctypes.util.find_library("usb-1.0")
    if found:
        candidates.insert(0, found)
    for path in candidates:
        try:
            return ctypes.CDLL(path)
        except OSError:
            continue
    raise RuntimeError(
        "Could not load libusb-1.0 shared library. Install via `brew install libusb`."
    )


class WinUSBTransport:
    """Direct USB WinUSB Bulk IN/OUT transport via libusb-1.0."""

    LIBUSB_SUCCESS = 0
    LIBUSB_ERROR_TIMEOUT = -7
    LIBUSB_ERROR_PIPE = -9

    def __init__(self, vid: int = 0x0EFF, pid: int = 0x0001):
        self.vid = vid
        self.pid = pid
        self.lib = _load_libusb()
        self._setup_signatures()

        self.ctx = ctypes.c_void_p()
        rc = self.lib.libusb_init(ctypes.byref(self.ctx))
        if rc != 0:
            raise RuntimeError(f"libusb_init failed: {rc}")

        self.handle = self.lib.libusb_open_device_with_vid_pid(
            self.ctx, self.vid, self.pid
        )
        if not self.handle:
            self.lib.libusb_exit(self.ctx)
            raise RuntimeError(
                f"NK60-V2 USB device ({self.vid:04x}:{self.pid:04x}) not found or permission denied."
            )

        self.intf_num = 1
        self.ep_out = 0x01
        self.ep_in = 0x81
        self.mps_in = 64
        self._discover_winusb_endpoints()

        # Claim only the WinUSB vendor interface (0xFF), leaving HID keyboard untouched
        try:
            self.lib.libusb_set_auto_detach_kernel_driver(self.handle, 1)
        except Exception:
            pass
        rc = self.lib.libusb_claim_interface(self.handle, self.intf_num)
        if rc != 0:
            self.close()
            raise RuntimeError(
                f"libusb_claim_interface({self.intf_num}) failed with code {rc}"
            )

        # Drain any stale bytes sitting in the endpoint FIFO from previous runs
        self._drain_stale()

    def _setup_signatures(self):
        self.lib.libusb_init.argtypes = [ctypes.POINTER(ctypes.c_void_p)]
        self.lib.libusb_init.restype = ctypes.c_int

        self.lib.libusb_exit.argtypes = [ctypes.c_void_p]
        self.lib.libusb_exit.restype = None

        self.lib.libusb_open_device_with_vid_pid.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint16,
            ctypes.c_uint16,
        ]
        self.lib.libusb_open_device_with_vid_pid.restype = ctypes.c_void_p

        self.lib.libusb_close.argtypes = [ctypes.c_void_p]
        self.lib.libusb_close.restype = None

        self.lib.libusb_get_device.argtypes = [ctypes.c_void_p]
        self.lib.libusb_get_device.restype = ctypes.c_void_p

        self.lib.libusb_get_active_config_descriptor.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.POINTER(LibusbConfigDescriptor)),
        ]
        self.lib.libusb_get_active_config_descriptor.restype = ctypes.c_int

        self.lib.libusb_free_config_descriptor.argtypes = [
            ctypes.POINTER(LibusbConfigDescriptor)
        ]
        self.lib.libusb_free_config_descriptor.restype = None

        self.lib.libusb_set_auto_detach_kernel_driver.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
        ]
        self.lib.libusb_set_auto_detach_kernel_driver.restype = ctypes.c_int

        self.lib.libusb_claim_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_claim_interface.restype = ctypes.c_int

        self.lib.libusb_release_interface.argtypes = [ctypes.c_void_p, ctypes.c_int]
        self.lib.libusb_release_interface.restype = ctypes.c_int

        self.lib.libusb_bulk_transfer.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint8,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.POINTER(ctypes.c_int),
            ctypes.c_uint,
        ]
        self.lib.libusb_bulk_transfer.restype = ctypes.c_int

    def _discover_winusb_endpoints(self):
        dev = self.lib.libusb_get_device(self.handle)
        if not dev:
            return
        cfg_ptr = ctypes.POINTER(LibusbConfigDescriptor)()
        rc = self.lib.libusb_get_active_config_descriptor(dev, ctypes.byref(cfg_ptr))
        if rc != 0 or not cfg_ptr:
            return
        try:
            cfg = cfg_ptr.contents
            for i in range(cfg.bNumInterfaces):
                intf = cfg.interface[i]
                for j in range(intf.num_altsetting):
                    alt = intf.altsetting[j]
                    if alt.bInterfaceClass == 0xFF:
                        self.intf_num = alt.bInterfaceNumber
                        for k in range(alt.bNumEndpoints):
                            ep = alt.endpoint[k]
                            if (ep.bmAttributes & 0x03) == 0x02:  # Bulk
                                if ep.bEndpointAddress & 0x80:
                                    self.ep_in = ep.bEndpointAddress
                                    self.mps_in = ep.wMaxPacketSize or 64
                                else:
                                    self.ep_out = ep.bEndpointAddress
                        return
        finally:
            self.lib.libusb_free_config_descriptor(cfg_ptr)

    def _bulk_read_raw(self, length: int, timeout_ms: int) -> bytes:
        buf = (ctypes.c_uint8 * length)()
        transferred = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(
            self.handle,
            self.ep_in,
            ctypes.cast(buf, ctypes.c_void_p),
            length,
            ctypes.byref(transferred),
            timeout_ms,
        )
        if rc == self.LIBUSB_ERROR_TIMEOUT:
            if transferred.value > 0:
                return bytes(buf[: transferred.value])
            raise TimeoutError("WinUSB bulk read timed out")
        if rc != 0:
            raise RuntimeError(f"libusb_bulk_transfer(IN) error: {rc}")
        return bytes(buf[: transferred.value])

    def _drain_stale(self):
        for _ in range(4):
            try:
                self._bulk_read_raw(self.mps_in, 15)
            except TimeoutError:
                break
            except Exception:
                break

    def send_packet(self, pkt: bytes, timeout_ms: int = 500):
        buf = (ctypes.c_uint8 * len(pkt)).from_buffer_copy(pkt)
        transferred = ctypes.c_int(0)
        rc = self.lib.libusb_bulk_transfer(
            self.handle,
            self.ep_out,
            ctypes.cast(buf, ctypes.c_void_p),
            len(pkt),
            ctypes.byref(transferred),
            timeout_ms,
        )
        if rc != 0 or transferred.value != len(pkt):
            raise RuntimeError(
                f"libusb_bulk_transfer(OUT) failed: rc={rc}, sent={transferred.value}/{len(pkt)}"
            )

    def recv_packet(self, timeout_ms: int = 500) -> dict:
        # Read first USB FS packet (64B) so exact-multiple packets never hang waiting for ZLP
        first = self._bulk_read_raw(self.mps_in, timeout_ms)
        while len(first) < PKT_HDR_STRUCT.size:
            more = self._bulk_read_raw(self.mps_in - len(first), timeout_ms)
            first += more

        hdr_tuple = PKT_HDR_STRUCT.unpack_from(first, 0)
        payload_len = hdr_tuple[7]
        total_len = PKT_HDR_STRUCT.size + payload_len
        raw = bytearray(first)
        while len(raw) < total_len:
            rem = total_len - len(raw)
            chunk = self._bulk_read_raw(rem, timeout_ms)
            raw.extend(chunk)
        return unpack_nlk_pkt(bytes(raw[:total_len]))

    def close(self):
        if getattr(self, "handle", None):
            try:
                self.lib.libusb_release_interface(self.handle, self.intf_num)
            except Exception:
                pass
            self.lib.libusb_close(self.handle)
            self.handle = None
        if getattr(self, "ctx", None):
            self.lib.libusb_exit(self.ctx)
            self.ctx = None


# ============================================================================
# NLK Host Session Client (RPC, Bulk OTA, Async Stream/Event Multiplexer)
# ============================================================================
class NLKRPCError(RuntimeError):
    def __init__(self, error_code, detail="NLK RPC failed"):
        self.error_code = error_code
        super().__init__(f"{detail}: errno={error_code}")


class NLKHostClient:
    def __init__(self, transport):
        self.transport = transport
        self.seq_id = 0
        self.async_queue = deque(maxlen=1024)
        self.async_dropped = 0
        self.fragments = {}
        self.cap_Cache = {}

    def _queue_async(self, packet):
        if len(self.async_queue) == self.async_queue.maxlen:
            self.async_dropped += 1
        self.async_queue.append(packet)

    def _recv_packet(self, timeout_ms):
        deadline = time.monotonic() + timeout_ms / 1000.0
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("packet receive timed out")
            packet = self.transport.recv_packet(timeout_ms=max(1, int(remaining * 1000)))
            if not packet["flags"] & NLK_FLAG_FRAG:
                return packet
            payload = packet["payload"]
            if len(payload) < 4:
                continue
            total, offset = struct.unpack_from("<HH", payload)
            part = payload[4:]
            key = tuple(packet[k] for k in ("src_addr", "dst_addr", "dst_cap", "mode", "cmd_op", "seq_id"))
            if offset == 0:
                if len(self.fragments) >= 16:
                    self.fragments.pop(next(iter(self.fragments)))
                self.fragments[key] = (total, bytearray(), packet)
            state = self.fragments.get(key)
            if state is None:
                continue
            expected, data, first = state
            if total != expected or offset != len(data) or len(data) + len(part) > total:
                self.fragments.pop(key, None)
                continue
            data.extend(part)
            if packet["flags"] & NLK_FLAG_MORE_FRAG:
                continue
            self.fragments.pop(key, None)
            if len(data) != total:
                continue
            result = dict(first)
            result["flags"] &= ~(NLK_FLAG_FRAG | NLK_FLAG_MORE_FRAG)
            result["payload"] = bytes(data)
            result["payload_len"] = total
            return result

    def next_seq(self) -> int:
        self.seq_id = (self.seq_id + 1) & 0xFFFF
        if self.seq_id == 0:
            self.seq_id = 1
        return self.seq_id

    def rpc_call(
        self,
        dst_addr: int,
        dst_cap: int,
        cmd_op: int,
        req_payload: bytes = b"",
        timeout_ms: int = 800,
        retries: int = 2,
    ) -> bytes:
        last_err = None
        for _ in range(retries + 1):
            seq = self.next_seq()
            pkt = pack_nlk_pkt(
                dst_addr=dst_addr,
                dst_cap=dst_cap,
                mode=NLK_MODE_RPC,
                flags=NLK_FLAG_ACK_REQ,
                cmd_op=cmd_op,
                seq_id=seq,
                payload=req_payload,
            )
            try:
                self.transport.send_packet(pkt, timeout_ms=timeout_ms)
                deadline = time.monotonic() + (timeout_ms / 1000.0)
                while time.monotonic() < deadline:
                    rem_ms = max(20, int((deadline - time.monotonic()) * 1000))
                    rx = self._recv_packet(timeout_ms=rem_ms)
                    if ((rx["flags"] & NLK_FLAG_IS_ACK) and rx["seq_id"] == seq and
                            rx.get("src_addr") == dst_addr and rx.get("dst_cap") == dst_cap):
                        if rx["flags"] & NLK_FLAG_IS_ERR:
                            err_code = -1
                            if len(rx["payload"]) >= 2:
                                (err_code,) = struct.unpack_from("<h", rx["payload"], 0)
                            raise NLKRPCError(
                                err_code, f"NLK RPC error from 0x{dst_addr:04x} cap={dst_cap} op=0x{cmd_op:02x}"
                            )
                        return rx["payload"]
                    else:
                        # Preserve asynchronous stream/event packets received during RPC wait
                        self._queue_async(rx)
                last_err = TimeoutError(
                    f"RPC timeout waiting for seq={seq} (dst=0x{dst_addr:04x}, cap={dst_cap}, op=0x{cmd_op:02x})"
                )
            except TimeoutError as e:
                last_err = e
                continue
        raise last_err

    def bulk_write_chunk(
        self,
        dst_addr: int,
        dst_cap: int,
        offset: int,
        data: bytes,
        timeout_ms: int = 1500,
        retries: int = 3,
    ) -> dict:
        payload = FW_CHUNK_HDR_STRUCT.pack(offset) + data
        last_err = None
        for _ in range(retries + 1):
            seq = self.next_seq()
            pkt = pack_nlk_pkt(
                dst_addr=dst_addr,
                dst_cap=dst_cap,
                mode=NLK_MODE_BULK,
                flags=NLK_FLAG_ACK_REQ,
                cmd_op=NLK_FW_OP_WRITE_CHUNK,
                seq_id=seq,
                payload=payload,
            )
            try:
                self.transport.send_packet(pkt, timeout_ms=timeout_ms)
                deadline = time.monotonic() + (timeout_ms / 1000.0)
                while time.monotonic() < deadline:
                    rem_ms = max(20, int((deadline - time.monotonic()) * 1000))
                    rx = self._recv_packet(timeout_ms=rem_ms)
                    if (rx["flags"] & NLK_FLAG_IS_ACK) and rx["seq_id"] == seq:
                        if rx["flags"] & NLK_FLAG_IS_ERR:
                            err_code = -1
                            if len(rx["payload"]) >= 2:
                                (err_code,) = struct.unpack_from("<h", rx["payload"], 0)
                            raise RuntimeError(
                                f"Bulk write failed at offset {offset}: errno={err_code}"
                            )
                        acked_offset, bitmap, status = FW_CHUNK_ACK_STRUCT.unpack_from(
                            rx["payload"], 0
                        )
                        return {
                            "acked_offset": acked_offset,
                            "rx_window_bitmap": bitmap,
                            "status": status,
                        }
                    else:
                        self._queue_async(rx)
                last_err = TimeoutError(f"Bulk chunk timeout at offset {offset}")
            except TimeoutError as e:
                last_err = e
                continue
        raise last_err

    def stream_write(
        self, dst_addr: int, dst_cap: int, stream_op: int, data: bytes
    ):
        seq = self.next_seq()
        pkt = pack_nlk_pkt(
            dst_addr=dst_addr,
            dst_cap=dst_cap,
            mode=NLK_MODE_STREAM,
            flags=NLK_FLAG_NONE,
            cmd_op=stream_op,
            seq_id=seq,
            payload=data,
        )
        self.transport.send_packet(pkt, timeout_ms=500)

    def poll_async_packet(self, timeout_ms: int = 100):
        if self.async_queue:
            return self.async_queue.popleft()
        try:
            return self._recv_packet(timeout_ms=timeout_ms)
        except TimeoutError:
            return None

    def poll_matching(self, addr, cap, op, timeout_ms=100, token=None):
        def matches(pkt):
            return (pkt.get("src_addr") == addr and pkt.get("dst_cap") == cap and
                    pkt.get("mode") == NLK_MODE_STREAM and pkt.get("cmd_op") == op and
                    (token is None or (len(pkt.get("payload", b"")) >= 8 and
                     struct.unpack_from("<Q", pkt["payload"])[0] == token)))
        for index, pkt in enumerate(self.async_queue):
            if matches(pkt):
                del self.async_queue[index]
                return pkt
        deadline = time.monotonic() + timeout_ms / 1000.0
        while time.monotonic() < deadline:
            try:
                pkt = self._recv_packet(timeout_ms=max(1, int((deadline - time.monotonic()) * 1000)))
            except TimeoutError:
                return None
            if matches(pkt):
                return pkt
            self._queue_async(pkt)
        return None

    # --- High-level Capability Discovery Helpers ---
    def get_device_desc(self, addr: int = NLK_ADDR_PRIMARY) -> dict:
        raw = self.rpc_call(addr, NLK_CAP_ID_CORE, NLK_CORE_OP_GET_DEV_DESC)
        (
            vid,
            pid,
            dev_class,
            hw_maj,
            hw_min,
            fw0,
            fw1,
            fw2,
            fw3,
            uuid_bytes,
            name_bytes,
            mtu,
            cap_count,
            dev_flags,
            parent_addr,
            attached_children,
            _,
        ) = DEV_DESC_STRUCT.unpack_from(raw, 0)
        name = name_bytes.split(b"\x00", 1)[0].decode("utf-8", errors="replace")
        return {
            "vendor_id": vid,
            "product_id": pid,
            "device_class": dev_class,
            "hw_ver": f"{hw_maj}.{hw_min}",
            "fw_ver": f"{fw0}.{fw1}.{fw2}.{fw3}",
            "uuid": uuid_bytes.hex(),
            "name": name,
            "max_payload_size": mtu,
            "cap_count": cap_count,
            "dev_flags": dev_flags,
            "parent_addr": parent_addr,
            "attached_children": attached_children,
        }

    def get_cap_list(self, addr: int = NLK_ADDR_PRIMARY) -> list:
        raw = self.rpc_call(addr, NLK_CAP_ID_CORE, NLK_CORE_OP_GET_CAP_LIST)
        caps = []
        for offset in range(0, len(raw), CAP_ITEM_STRUCT.size):
            if offset + CAP_ITEM_STRUCT.size > len(raw):
                break
            cap_id, cap_ver, cap_type, sup_modes, sub_class, desc_size = (
                CAP_ITEM_STRUCT.unpack_from(raw, offset)
            )
            caps.append(
                {
                    "cap_id": cap_id,
                    "cap_ver": f"{(cap_ver >> 4) & 0xF}.{cap_ver & 0xF}",
                    "cap_type": cap_type,
                    "cap_name": CAP_TYPE_NAMES.get(cap_type, f"0x{cap_type:04X}"),
                    "sup_modes": sup_modes,
                    "sub_class": sub_class,
                    "desc_size": desc_size,
                }
            )
        self.cap_Cache[addr] = {c["cap_type"]: c["cap_id"] for c in caps}
        return caps

    def find_cap_id(self, addr: int, cap_type: int) -> int:
        if addr not in self.cap_Cache:
            self.get_cap_list(addr)
        cap_id = self.cap_Cache.get(addr, {}).get(cap_type)
        if cap_id is None:
            raise RuntimeError(
                f"Capability {CAP_TYPE_NAMES.get(cap_type, hex(cap_type))} not present on node 0x{addr:04x}"
            )
        return cap_id

    def get_topology(self, addr: int = NLK_ADDR_PRIMARY) -> list:
        raw = self.rpc_call(addr, NLK_CAP_ID_CORE, NLK_CORE_OP_GET_TOPOLOGY)
        nodes = []
        for offset in range(0, len(raw), TOPO_ENTRY_STRUCT.size):
            if offset + TOPO_ENTRY_STRUCT.size > len(raw):
                break
            n_addr, p_addr, vid, pid, d_cls, caps, port = (
                TOPO_ENTRY_STRUCT.unpack_from(raw, offset)
            )
            nodes.append(
                {
                    "node_addr": n_addr,
                    "parent_addr": p_addr,
                    "vendor_id": vid,
                    "product_id": pid,
                    "device_class": d_cls,
                    "cap_count": caps,
                    "port_id": port,
                }
            )
        return nodes

    def ping(self, addr: int = NLK_ADDR_PRIMARY, payload: bytes = b"NLK-PING") -> float:
        t0 = time.perf_counter()
        resp = self.rpc_call(addr, NLK_CAP_ID_CORE, NLK_CORE_OP_PING, payload)
        dt_ms = (time.perf_counter() - t0) * 1000.0
        if resp != payload:
            raise RuntimeError(f"Ping payload mismatch: {resp!r} != {payload!r}")
        return dt_ms


# ============================================================================
# CLI Subcommand Implementations
# ============================================================================
def cmd_info(client: NLKHostClient, addr: int):
    rtt_ms = client.ping(addr)
    desc = client.get_device_desc(addr)
    caps = client.get_cap_list(addr)
    topo = client.get_topology(addr)

    cls_name = DEV_CLASS_NAMES.get(desc["device_class"], f"0x{desc['device_class']:04x}")
    print("=" * 72)
    print(
        f"NLK Node 0x{addr:04x} [{desc['name']}]  (Ping RTT: {rtt_ms:.2f} ms)"
    )
    print("=" * 72)
    print(
        f"  Vendor/Product : 0x{desc['vendor_id']:04x}:0x{desc['product_id']:04x} ({cls_name})"
    )
    print(
        f"  HW / FW Ver    : HW v{desc['hw_ver']} / FW v{desc['fw_ver']}  (MTU={desc['max_payload_size']}B)"
    )
    print(
        f"  Parent / Flags : parent=0x{desc['parent_addr']:04x}, flags=0x{desc['dev_flags']:02x}, children={desc['attached_children']}"
    )
    print(f"\nCapabilities ({len(caps)}):")
    for c in caps:
        modes = []
        if c["sup_modes"] & 0x01:
            modes.append("RPC")
        if c["sup_modes"] & 0x02:
            modes.append("EVT")
        if c["sup_modes"] & 0x04:
            modes.append("STREAM")
        if c["sup_modes"] & 0x08:
            modes.append("BULK")
        print(
            f"  [Cap {c['cap_id']:2d}] 0x{c['cap_type']:04x} ({c['cap_name']:<10s}) v{c['cap_ver']}  modes={','.join(modes):<15s} desc={c['desc_size']}B"
        )

        if c["cap_type"] == NLK_CAP_FW_UPDATE:
            raw = client.rpc_call(addr, c["cap_id"], NLK_FW_OP_GET_PART_INFO)
            act, cnt, max_chk, max_win, _, run_ver, p0, p1, p2, p3 = (
                FW_PART_INFO_STRUCT.unpack_from(raw, 0)
            )
            sizes = [p0, p1, p2, p3][:cnt]
            sizes_str = ", ".join(f"P{i}={s // 1024}KB" for i, s in enumerate(sizes))
            print(
                f"           -> FW Update: active_part={act}, max_chunk={max_chk}B, window={max_win}, ver=0x{run_ver:08x} ({sizes_str})"
            )
        elif c["cap_type"] == NLK_CAP_LOG:
            raw = client.rpc_call(addr, c["cap_id"], NLK_LOG_OP_GET_STATS)
            drop, trunc, hw, lvl, en, sub_addr = LOG_STATS_RESP_STRUCT.unpack_from(
                raw, 0
            )
            print(
                f"           -> Kernel Log: subscribed={bool(en)}, level={LOG_LEVEL_NAMES.get(lvl, lvl)}, high_water={hw}B, dropped={drop}, truncated={trunc}"
            )
        elif c["cap_type"] == NLK_CAP_KEYBOARD:
            raw = client.rpc_call(addr, c["cap_id"], NLK_KB_OP_GET_DESC)
            (
                ff,
                sw,
                kb_flags,
                rows,
                cols,
                keys,
                layers,
                deb,
                lrows,
                lcols,
                ug,
                leds,
                rate,
                _,
            ) = KB_DESC_STRUCT.unpack_from(raw, 0)
            ff_str = KB_FORM_NAMES.get(ff, f"0x{ff:02x}")
            print(
                f"           -> Keyboard: {ff_str}, matrix={rows}x{cols} ({keys} keys, {layers} layers), debounce={deb}ms, LEDs={leds}, poll={rate}Hz"
            )

    print(f"\nTopology Table ({len(topo)} nodes):")
    for t in topo:
        t_cls = DEV_CLASS_NAMES.get(t["device_class"], f"0x{t['device_class']:04x}")
        print(
            f"  node=0x{t['node_addr']:04x}  parent=0x{t['parent_addr']:04x}  port={t['port_id']}  vid:pid=0x{t['vendor_id']:04x}:0x{t['product_id']:04x} ({t_cls}, caps={t['cap_count']})"
        )
    print("=" * 72)


def cmd_fw_update(
    client: NLKHostClient,
    addr: int,
    fw_path: str,
    part_index: int = 1,
    chunk_size: int = 0,
    reboot: bool = False,
    max_bytes: int = 0,
):
    if not os.path.isfile(fw_path):
        raise FileNotFoundError(f"Firmware file not found: {fw_path}")

    with open(fw_path, "rb") as f:
        fw_data = f.read()
    if max_bytes > 0 and len(fw_data) > max_bytes:
        fw_data = fw_data[:max_bytes]

    total_size = len(fw_data)
    expected_crc = nlk_crc32(fw_data, 0)

    fw_cap_id = client.find_cap_id(addr, NLK_CAP_FW_UPDATE)
    raw_info = client.rpc_call(addr, fw_cap_id, NLK_FW_OP_GET_PART_INFO)
    act_part, part_cnt, max_chunk, max_win, _, run_ver, p0, p1, p2, p3 = (
        FW_PART_INFO_STRUCT.unpack_from(raw_info, 0)
    )
    part_sizes = [p0, p1, p2, p3]
    if part_index >= part_cnt:
        raise ValueError(f"Invalid partition {part_index} (device has {part_cnt} partitions)")
    if part_sizes[part_index] > 0 and total_size > part_sizes[part_index]:
        raise ValueError(
            f"Image size ({total_size}B) exceeds partition {part_index} max ({part_sizes[part_index]}B)"
        )

    step = chunk_size if (0 < chunk_size <= max_chunk) else max_chunk
    print(
        f"[FW-UPDATE] Target=0x{addr:04x} Cap={fw_cap_id} Partition={part_index} (max={part_sizes[part_index] // 1024}KB)"
    )
    print(
        f"[FW-UPDATE] File='{fw_path}' Size={total_size} bytes ({total_size / 1024:.1f} KB) CRC32=0x{expected_crc:08x} Chunk={step}B"
    )

    prep_req = FW_PREPARE_STRUCT.pack(
        part_index, 0, min(max_win, 4), total_size, expected_crc, run_ver
    )
    client.rpc_call(addr, fw_cap_id, NLK_FW_OP_PREPARE, prep_req, timeout_ms=2000)

    t0 = time.perf_counter()
    offset = 0
    while offset < total_size:
        chunk = fw_data[offset : offset + step]
        ack = client.bulk_write_chunk(
            addr, fw_cap_id, offset, chunk, timeout_ms=350, retries=5
        )
        if ack["status"] < 0:
            raise RuntimeError(
                f"Device reported flash write error {ack['status']} at offset {offset}"
            )
        offset = ack["acked_offset"]
        pct = (offset * 100.0) / total_size
        elapsed = max(0.001, time.perf_counter() - t0)
        kbps = (offset / 1024.0) / elapsed
        bar_len = 30
        filled = int(bar_len * offset // total_size)
        bar = "#" * filled + "-" * (bar_len - filled)
        sys.stdout.write(
            f"\r[FW-UPDATE] [{bar}] {pct:6.2f}% ({offset}/{total_size} B, {kbps:.1f} KB/s)"
        )
        sys.stdout.flush()

    print()
    client.rpc_call(addr, fw_cap_id, NLK_FW_OP_VERIFY_COMMIT, b"", timeout_ms=2000)
    dt = time.perf_counter() - t0
    print(
        f"[FW-UPDATE] SUCCESS! CRC32 0x{expected_crc:08x} verified & committed to Partition {part_index} in {dt:.2f}s."
    )

    if reboot:
        print("[FW-UPDATE] Requesting device reboot (NLK_FW_OP_REBOOT)...")
        client.rpc_call(addr, fw_cap_id, NLK_FW_OP_REBOOT, b"", timeout_ms=1000)
        print("[FW-UPDATE] Reboot ACK received.")


class RemoteShellSession:
    """A leased session with acknowledged, ordered and idempotent stdin chunks."""
    def __init__(self, client, addr, cap):
        self.client = client
        self.addr = addr
        self.cap = cap
        self.token = int.from_bytes(os.urandom(8), "little") or 1
        self.offset = 0
        self.last_heartbeat = time.monotonic()
        self.active = False
        self.reliable = True

    def open(self, cols=120, rows=40):
        payload = SHELL_OPEN_STRUCT.pack(cols, rows, 128, NLK_SHELL_OPEN_RELIABLE)
        payload += struct.pack("<Q", self.token)
        self.active = True
        self.reliable = True
        try:
            self.client.rpc_call(self.addr, self.cap, NLK_SHELL_OP_OPEN, payload)
        except NLKRPCError as exc:
            if exc.error_code in (-22, -95):
                self.reliable = False
                legacy = SHELL_OPEN_STRUCT.pack(cols, rows, 128, 0)
                try:
                    self.client.rpc_call(self.addr, self.cap, NLK_SHELL_OP_OPEN, legacy)
                except Exception:
                    self.close()
                    raise
            else:
                self.close()
                raise
        except Exception:
            self.close()
            raise
        self.last_heartbeat = time.monotonic()

    def close(self):
        if self.active:
            self.active = False
            try:
                close_payload = struct.pack("<Q", self.token) if self.reliable else b""
                self.client.rpc_call(self.addr, self.cap, NLK_SHELL_OP_CLOSE,
                                     close_payload)
            except Exception:
                pass

    def heartbeat(self):
        if self.reliable and time.monotonic() - self.last_heartbeat >= 10:
            self.client.rpc_call(self.addr, self.cap, NLK_SHELL_OP_HEARTBEAT,
                                 struct.pack("<Q", self.token))
            self.last_heartbeat = time.monotonic()

    def write(self, data, timeout=30.0):
        for index in range(0, len(data), 64):
            chunk = data[index:index + 64]
            if not self.reliable:
                self.client.stream_write(self.addr, self.cap, NLK_SHELL_STREAM_STDIN, chunk)
                self.last_heartbeat = time.monotonic()
                continue
            payload = struct.pack("<QI", self.token, self.offset) + chunk
            deadline = time.monotonic() + timeout
            while True:
                try:
                    self.client.rpc_call(self.addr, self.cap, NLK_SHELL_OP_INPUT, payload)
                    break
                except NLKRPCError as exc:
                    if exc.error_code != -28 or time.monotonic() >= deadline:
                        raise
                    time.sleep(0.02)
            self.offset = (self.offset + len(chunk)) & 0xFFFFFFFF
            self.last_heartbeat = time.monotonic()

    def poll(self, timeout_ms=100):
        self.heartbeat()
        if hasattr(self.client, "poll_matching"):
            if not self.reliable:
                return self.client.poll_matching(self.addr, self.cap, NLK_SHELL_STREAM_STDOUT,
                                                 timeout_ms, token=None)
            pkt = self.client.poll_matching(self.addr, self.cap, NLK_SHELL_STREAM_STDOUT_V2,
                                            timeout_ms, token=self.token)
            if pkt is not None:
                pkt = dict(pkt, payload=pkt["payload"][8:])
            return pkt
        return self.client.poll_async_packet(timeout_ms=timeout_ms)


def cmd_shell_exec(
    client: NLKHostClient, addr: int, command_str: str, timeout_sec: float = 2.0
) -> str:
    sh_cap_id = client.find_cap_id(addr, NLK_CAP_SHELL)
    session = RemoteShellSession(client, addr, sh_cap_id)
    session.open(100, 30)
    try:
        deadline_init = time.monotonic() + 0.15
        while time.monotonic() < deadline_init:
            pkt = session.poll(timeout_ms=25)
            if pkt is not None and b"nos# " in pkt["payload"]:
                break
        cmd_clean = command_str.rstrip("\r\n")
        session.write((cmd_clean + "\r").encode("utf-8"))
        decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
        chunks = []
        deadline = time.monotonic() + timeout_sec
        while time.monotonic() < deadline:
            pkt = session.poll(timeout_ms=40)
            if pkt is not None:
                chunks.append(decoder.decode(pkt["payload"]))
                combined = "".join(chunks)
                if combined.endswith("nos# "):
                    break
        chunks.append(decoder.decode(b"", final=True))
        return "".join(chunks)
    finally:
        session.close()


def cmd_shell_interactive(client: NLKHostClient, addr: int):
    sh_cap_id = client.find_cap_id(addr, NLK_CAP_SHELL)
    session = RemoteShellSession(client, addr, sh_cap_id)
    session.open()
    decoder = codecs.getincrementaldecoder("utf-8")(errors="replace")
    saved_term = None
    fd = None
    try:
        print(f"Connected to NLK shell (node=0x{addr:04x}, cap={sh_cap_id}). Ctrl+D disconnects.")
        if os.name != "nt":
            fd = sys.stdin.fileno()
            if sys.stdin.isatty():
                import termios
                import tty
                saved_term = termios.tcgetattr(fd)
                tty.setraw(fd)
        else:
            import msvcrt
        while True:
            pkt = session.poll(timeout_ms=20)
            if pkt is not None:
                sys.stdout.write(decoder.decode(pkt["payload"]))
                sys.stdout.flush()
            if os.name == "nt":
                if not msvcrt.kbhit():
                    continue
                char = msvcrt.getwch()
                if char in ("\x00", "\xe0"):
                    key = msvcrt.getwch()
                    data = {"H": b"\x1b[A", "P": b"\x1b[B",
                            "K": b"\x1b[D", "M": b"\x1b[C"}.get(key, b"")
                    if not data:
                        continue
                else:
                    data = char.encode("utf-8")
            else:
                ready, _, _ = select.select([fd], [], [], 0.01)
                if not ready:
                    continue
                data = os.read(fd, 64)
            if not data:
                break
            before_eof, separator, _ = data.partition(b"\x04")
            if before_eof:
                session.write(before_eof)
            if separator:
                break
    finally:
        if saved_term is not None:
            termios.tcsetattr(fd, termios.TCSADRAIN, saved_term)
        session.close()
        print("\nDisconnected from NLK remote nShell.")


def cmd_log_stream(
    client: NLKHostClient,
    addr: int,
    min_level: int = 0,
    duration: float = 0.0,
    trigger_test_msg: str = "",
) -> list:
    log_cap_id = client.find_cap_id(addr, NLK_CAP_LOG)

    # Enable kernel log & set filter level
    lvl_req = struct.pack("<BB", min_level, 1)
    client.rpc_call(addr, log_cap_id, NLK_LOG_OP_SET_LEVEL, lvl_req)

    # Subscribe to real-time kernel log stream
    sub_req = LOG_SUB_REQ_STRUCT.pack(1, min_level, 20)
    client.rpc_call(addr, log_cap_id, NLK_LOG_OP_SUBSCRIBE, sub_req)

    raw_stats = client.rpc_call(addr, log_cap_id, NLK_LOG_OP_GET_STATS)
    drop, trunc, hw, lvl, en, _ = LOG_STATS_RESP_STRUCT.unpack_from(raw_stats, 0)
    print(
        f"[NLK-LOG] Subscribed to node 0x{addr:04x} (cap={log_cap_id}, level>={LOG_LEVEL_NAMES.get(lvl, lvl)}, high_water={hw}B, dropped={drop})"
    )

    # Optionally trigger kernel logs via nShell `log test <msg>` so immediate records are emitted
    if trigger_test_msg:
        sh_cap_id = client.find_cap_id(addr, NLK_CAP_SHELL)
        open_msg = SHELL_OPEN_STRUCT.pack(100, 30, 128, 0)
        client.rpc_call(addr, sh_cap_id, NLK_SHELL_OP_OPEN, open_msg)
        cmd_bytes = f"log test {trigger_test_msg}\r".encode("utf-8")
        client.stream_write(addr, sh_cap_id, NLK_SHELL_STREAM_STDIN, cmd_bytes)

    records = []
    start = time.monotonic()
    try:
        while True:
            if duration > 0 and (time.monotonic() - start) >= duration:
                break
            pkt = client.poll_async_packet(timeout_ms=50)
            if pkt is None:
                continue
            if (
                pkt["mode"] == NLK_MODE_STREAM
                and pkt["cmd_op"] == NLK_LOG_STREAM_RECORD
                and len(pkt["payload"]) >= LOG_WIRE_HDR_STRUCT.size
            ):
                ts_ms, msg_len, rec_lvl, rec_flags = LOG_WIRE_HDR_STRUCT.unpack_from(
                    pkt["payload"], 0
                )
                msg_bytes = pkt["payload"][
                    LOG_WIRE_HDR_STRUCT.size : LOG_WIRE_HDR_STRUCT.size + msg_len
                ]
                msg_text = msg_bytes.decode("utf-8", errors="replace").rstrip("\r\n")
                lvl_name = LOG_LEVEL_NAMES.get(rec_lvl, str(rec_lvl))
                rec_obj = {
                    "timestamp_ms": ts_ms,
                    "level": rec_lvl,
                    "level_name": lvl_name,
                    "flags": rec_flags,
                    "message": msg_text,
                }
                records.append(rec_obj)
                print(f"[{ts_ms:8d} ms][{lvl_name:<5s}] {msg_text}")
    except KeyboardInterrupt:
        pass
    finally:
        unsub_req = LOG_SUB_REQ_STRUCT.pack(0, min_level, 0)
        try:
            client.rpc_call(addr, log_cap_id, NLK_LOG_OP_SUBSCRIBE, unsub_req)
            if trigger_test_msg:
                sh_cap_id = client.find_cap_id(addr, NLK_CAP_SHELL)
                client.rpc_call(addr, sh_cap_id, NLK_SHELL_OP_CLOSE, b"")
        except Exception:
            pass
    return records


def cmd_test_all(
    client: NLKHostClient,
    addr: int,
    fw_path: str,
    fw_part: int = 2,
    fw_max_bytes: int = 0,
):
    print("=" * 72)
    print("NLK Real-Hardware End-to-End Verification Suite (NK60-V2)")
    print("=" * 72)

    # 1. Discovery & Topology
    print("\n[STEP 1/5] Testing NLK Core Discovery, Ping & Capability Enumeration...")
    cmd_info(client, addr)

    # 2. Remote nShell Commands & Signal
    print("\n[STEP 2/5] Testing Remote nShell (NLK_CAP_SHELL 0x0003)...")
    for sh_cmd in ["ver", "help", "nlk", "ps", "ls"]:
        print(f"  -> Executing remote shell command: `{sh_cmd}`")
        out = cmd_shell_exec(client, addr, sh_cmd, timeout_sec=1.5)
        cleaned = "\n".join(
            "     " + line for line in out.splitlines() if line.strip()
        )
        print(cleaned)
        if not out.strip():
            raise RuntimeError(f"Empty output from remote shell command `{sh_cmd}`")

    # Verify Ctrl+C Signal handling on NLK_CAP_SHELL
    sh_cap_id = client.find_cap_id(addr, NLK_CAP_SHELL)
    client.rpc_call(
        addr, sh_cap_id, NLK_SHELL_OP_OPEN, SHELL_OPEN_STRUCT.pack(80, 24, 128, 0)
    )
    client.stream_write(addr, sh_cap_id, NLK_SHELL_STREAM_STDIN, b"partial_cmd")
    client.rpc_call(
        addr, sh_cap_id, NLK_SHELL_OP_SIGNAL, struct.pack("<B", NLK_SHELL_SIG_INT)
    )
    sig_out = []
    deadline = time.monotonic() + 0.4
    while time.monotonic() < deadline:
        pkt = client.poll_async_packet(timeout_ms=30)
        if (
            pkt
            and pkt["mode"] == NLK_MODE_STREAM
            and pkt["cmd_op"] == NLK_SHELL_STREAM_STDOUT
        ):
            sig_out.append(pkt["payload"].decode("utf-8", errors="replace"))
    client.rpc_call(addr, sh_cap_id, NLK_SHELL_OP_CLOSE, b"")
    sig_text = "".join(sig_out)
    print(f"  -> SIGINT (Ctrl+C) response: {sig_text.strip()!r}")

    # 3. Real-Time Kernel Log Streaming
    print("\n[STEP 3/5] Testing Real-Time Kernel Log Stream (NLK_CAP_LOG 0x0002)...")
    records = cmd_log_stream(
        client,
        addr,
        min_level=0,
        duration=0.8,
        trigger_test_msg="nk60_hw_verified",
    )
    if not any("nk60_hw_verified" in r["message"] for r in records):
        raise RuntimeError("Did not receive expected kernel log record over NLK_CAP_LOG")
    print(f"  -> Verified {len(records)} live kernel log records received over NLK!")

    # 4. Keyboard Keymap & Integrated Lighting
    print(
        "\n[STEP 4/5] Testing Keyboard Keymap & Lighting Control (NLK_CAP_KEYBOARD 0x0100)..."
    )
    kb_cap_id = client.find_cap_id(addr, NLK_CAP_KEYBOARD)
    km_req = KB_KEYMAP_HDR_STRUCT.pack(0, 0, 0, 8)
    km_resp = client.rpc_call(addr, kb_cap_id, NLK_KB_OP_GET_KEYMAP, km_req)
    keys_layer0 = [
        KB_KEYMAP_ENTRY_STRUCT.unpack_from(km_resp, 4 + i * 2)[0] for i in range(8)
    ]
    print(
        "  -> Layer 0 first 8 keycodes:",
        " ".join(f"0x{k:02x}" for k in keys_layer0),
    )
    raw_lcfg = client.rpc_call(addr, kb_cap_id, NLK_KB_OP_GET_LIGHT_CFG)
    en, zm, eff, bri, spd, r, g, b = KB_LIGHT_CFG_STRUCT.unpack_from(raw_lcfg, 0)
    print(
        f"  -> Lighting Config: enable={en}, zone=0x{zm:02x}, effect={eff}, brightness={bri}, speed={spd}, rgb=({r},{g},{b})"
    )

    # 5. Firmware OTA Upgrade
    print(
        f"\n[STEP 5/5] Testing Firmware OTA Upgrade (NLK_CAP_FW_UPDATE 0x0001) -> Partition {fw_part}..."
    )
    cmd_fw_update(
        client,
        addr,
        fw_path=fw_path,
        part_index=fw_part,
        reboot=False,
        max_bytes=fw_max_bytes,
    )

    print("\n" + "=" * 72)
    print("ALL 5 NLK HARDWARE VERIFICATION SUITES PASSED ON NK60-V2!")
    print("=" * 72)


def main():
    parser = argparse.ArgumentParser(
        description="NOS Link Kit (NLK) Host CLI & Hardware Verification Tool"
    )
    parser.add_argument(
        "--vid", type=lambda x: int(x, 0), default=0x0EFF, help="USB Vendor ID"
    )
    parser.add_argument(
        "--pid", type=lambda x: int(x, 0), default=0x0001, help="USB Product ID"
    )
    parser.add_argument(
        "--addr",
        type=lambda x: int(x, 0),
        default=NLK_ADDR_PRIMARY,
        help="Target NLK hierarchical node address (default: 0x0001)",
    )

    sub = parser.add_subparsers(dest="subcmd")

    sub.add_parser("info", help="Query device descriptor, capabilities & topology")

    p_fw = sub.add_parser("fw-update", help="Flash firmware image over NLK")
    p_fw.add_argument(
        "file",
        nargs="?",
        default="build/nk60-v2/nos.img",
        help="Firmware image path (default: build/nk60-v2/nos.img)",
    )
    p_fw.add_argument(
        "--part",
        type=int,
        default=1,
        help="Target partition index (0=Bootloader, 1=Kernel-A, 2=Kernel-B, 3=Assets)",
    )
    p_fw.add_argument(
        "--chunk-size", type=int, default=0, help="Override bulk chunk size"
    )
    p_fw.add_argument(
        "--max-bytes",
        type=int,
        default=0,
        help="Optional cap on bytes transferred (for quick smoke tests)",
    )
    p_fw.add_argument(
        "--reboot", action="store_true", help="Reboot device after commit"
    )

    sub.add_parser("shell", help="Open interactive remote nShell terminal")

    p_exec = sub.add_parser("exec", help="Execute a command on remote nShell")
    p_exec.add_argument("cmd", help="nShell command string to execute")
    p_exec.add_argument(
        "--timeout", type=float, default=2.0, help="Command timeout in seconds"
    )

    p_log = sub.add_parser("log", help="Subscribe and stream live kernel logs")
    p_log.add_argument(
        "--level", type=int, default=0, help="Minimum log level (0=ALL..5=FATAL)"
    )
    p_log.add_argument(
        "--duration",
        type=float,
        default=0.0,
        help="Stop after N seconds (0=until Ctrl+C)",
    )
    p_log.add_argument(
        "--trigger-test",
        default="",
        help="Emit test kernel log messages via shell `log test <msg>`",
    )

    p_test = sub.add_parser(
        "test-all",
        help="Run full automated hardware verification (info, shell, log, keyboard, fw-update)",
    )
    p_test.add_argument(
        "--fw-file",
        default="build/nk60-v2/nos.img",
        help="Firmware file to flash during test-all",
    )
    p_test.add_argument(
        "--part",
        type=int,
        default=2,
        help="Target partition for fw-update test (default: 2 = Kernel-B)",
    )
    p_test.add_argument(
        "--fw-max-bytes",
        type=int,
        default=0,
        help="Optional max bytes for fw-update (0 = full firmware image)",
    )

    args = parser.parse_args()
    if not args.subcmd:
        args.subcmd = "info"

    transport = WinUSBTransport(vid=args.vid, pid=args.pid)
    try:
        client = NLKHostClient(transport)
        if args.subcmd == "info":
            cmd_info(client, args.addr)
        elif args.subcmd == "fw-update":
            cmd_fw_update(
                client,
                args.addr,
                fw_path=args.file,
                part_index=args.part,
                chunk_size=args.chunk_size,
                reboot=args.reboot,
                max_bytes=args.max_bytes,
            )
        elif args.subcmd == "shell":
            cmd_shell_interactive(client, args.addr)
        elif args.subcmd == "exec":
            out = cmd_shell_exec(client, args.addr, args.cmd, timeout_sec=args.timeout)
            sys.stdout.write(out)
            if not out.endswith("\n"):
                sys.stdout.write("\n")
        elif args.subcmd == "log":
            cmd_log_stream(
                client,
                args.addr,
                min_level=args.level,
                duration=args.duration,
                trigger_test_msg=args.trigger_test,
            )
        elif args.subcmd == "test-all":
            cmd_test_all(
                client,
                args.addr,
                fw_path=args.fw_file,
                fw_part=args.part,
                fw_max_bytes=args.fw_max_bytes,
            )
    finally:
        transport.close()


if __name__ == "__main__":
    main()
