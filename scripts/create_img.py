import os
import sys
import struct

if len(sys.argv) != 7:
    sys.exit(1)

bin_path = sys.argv[1]
ver_major = int(sys.argv[2])
ver_minor = int(sys.argv[3])
ver_patch = int(sys.argv[4])
hdr_type = int(sys.argv[5])
out_path = sys.argv[6]

with open(bin_path, "rb") as img_in:
    img_in_data = img_in.read()

img_in_size = len(img_in_data)
header_bytes = [
    0x30, 0x20, 0x30, 0x20,
    0, 0, 0, 0,
    (img_in_size & 0xFF),
    ((img_in_size >> 8) & 0xFF),
    ((img_in_size >> 16) & 0xFF),
    ((img_in_size >> 24) & 0xFF),
    hdr_type,
    ver_major,
    ver_minor,
    ver_patch,
]

with open(out_path, "wb") as img_out:
    img_out.write(bytes(header_bytes))
    img_out.write(img_in_data)
