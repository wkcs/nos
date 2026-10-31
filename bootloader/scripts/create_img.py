#!/usr/bin/env python3
"""Pack bootloader.bin into an NLK OTA image (bootloader.img) with CRC32."""
import struct
import sys
import zlib

NLK_FW_IMG_MAGIC = 0x20302030
IMG_HDR_STRUCT = struct.Struct("<III4B")
MAX_BOOTLOADER_BIN = 16 * 1024


def main() -> int:
    if len(sys.argv) != 7:
        print(
            "Usage: create_img.py <in.bin> <major> <minor> <patch> <type> <out.img>",
            file=sys.stderr,
        )
        return 1

    bin_path = sys.argv[1]
    ver_major = int(sys.argv[2]) & 0xFF
    ver_minor = int(sys.argv[3]) & 0xFF
    ver_patch = int(sys.argv[4]) & 0xFF
    hdr_type = int(sys.argv[5]) & 0xFF
    out_path = sys.argv[6]

    with open(bin_path, "rb") as f:
        payload = f.read()

    if hdr_type == 0 and len(payload) > MAX_BOOTLOADER_BIN:
        print(
            f"Error: bootloader.bin ({len(payload)}B) exceeds 16KB Sector 0 limit!",
            file=sys.stderr,
        )
        return 1

    payload_crc32 = zlib.crc32(payload) & 0xFFFFFFFF
    hdr = IMG_HDR_STRUCT.pack(
        NLK_FW_IMG_MAGIC,
        payload_crc32,
        len(payload),
        hdr_type,
        ver_major,
        ver_minor,
        ver_patch,
    )

    with open(out_path, "wb") as f:
        f.write(hdr)
        f.write(payload)
    return 0


if __name__ == "__main__":
    sys.exit(main())
