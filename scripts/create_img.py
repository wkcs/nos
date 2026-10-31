import struct
import sys
import zlib

NLK_FW_IMG_MAGIC = 0x20302030
IMG_HDR_STRUCT = struct.Struct("<III4B")


def build_image(bin_path: str, ver_major: int, ver_minor: int, ver_patch: int,
                hdr_type: int, out_path: str) -> None:
    with open(bin_path, "rb") as img_in:
        payload = img_in.read()

    payload_size = len(payload)
    payload_crc32 = zlib.crc32(payload) & 0xFFFFFFFF
    hdr = IMG_HDR_STRUCT.pack(
        NLK_FW_IMG_MAGIC,
        payload_crc32,
        payload_size,
        hdr_type & 0xFF,
        ver_major & 0xFF,
        ver_minor & 0xFF,
        ver_patch & 0xFF,
    )

    with open(out_path, "wb") as img_out:
        img_out.write(hdr)
        img_out.write(payload)


def merge_factory_bin(bl_bin: str, kernel_bin: str, kernel_offset: int,
                      out_path: str) -> None:
    with open(bl_bin, "rb") as f:
        bl_data = f.read()
    with open(kernel_bin, "rb") as f:
        krn_data = f.read()
    if len(bl_data) > kernel_offset:
        raise ValueError(
            f"Bootloader size ({len(bl_data)}B) exceeds kernel offset (0x{kernel_offset:x})"
        )
    pad = b"\xFF" * (kernel_offset - len(bl_data))
    with open(out_path, "wb") as f:
        f.write(bl_data)
        f.write(pad)
        f.write(krn_data)


def main() -> int:
    if len(sys.argv) == 6 and sys.argv[1] == "--merge":
        merge_factory_bin(sys.argv[2], sys.argv[3], int(sys.argv[4], 0), sys.argv[5])
        return 0
    if len(sys.argv) != 7:
        sys.exit(1)

    build_image(
        bin_path=sys.argv[1],
        ver_major=int(sys.argv[2]),
        ver_minor=int(sys.argv[3]),
        ver_patch=int(sys.argv[4]),
        hdr_type=int(sys.argv[5]),
        out_path=sys.argv[6],
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
