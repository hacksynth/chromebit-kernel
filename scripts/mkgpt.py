#!/usr/bin/env python3
"""Rebuild the backup GPT of a veyron (Chromebook) eMMC.

Why: the primary GPT header on these devices is `IGNOREME` and lives in a
permanently write-protected 8 MiB region, so it can never be fixed.  ChromeOS
firmware (depthcharge) falls back to the *secondary* GPT at the end of the
disk -- but `mmc sanitize` wipes it.  This script lays down a valid secondary
GPT describing the two partitions the device needs.

Run from a rescue system, with the eMMC not mounted.

    python3 mkgpt.py /dev/mmcblk0

Layout produced (LBA, 512-byte sectors):

    p1   20480 .. 40959        ChromeOS kernel slot   priority=15 tries=5 successful=1
    p2   40960 .. last_usable  Linux data "ArchRoot"

Never run `mmc sanitize` before this -- it erases the backup GPT.
"""

import struct
import sys
import uuid
import zlib


def main():
    dev = sys.argv[1] if len(sys.argv) > 1 else "/dev/mmcblk0"

    with open(dev, "rb") as f:
        f.seek(0, 2)
        total_bytes = f.tell()
    total_sectors = total_bytes // 512
    last = total_sectors - 1                 # backup header lives here
    entry_lba = last - 32                    # 128 entries * 128 bytes = 32 sectors
    last_usable = entry_lba - 1
    first_usable = 34

    kernel_type = uuid.UUID("FE3A2A5D-4F32-41A7-B725-ACCC3285A309")  # ChromeOS kernel
    data_type = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")    # Linux filesystem
    disk_guid = uuid.uuid4()

    def entry(ptype, first, last_lba, attrs, name):
        e = bytearray(128)
        e[0:16] = ptype.bytes_le
        e[16:32] = uuid.uuid4().bytes_le
        struct.pack_into("<QQQ", e, 32, first, last_lba, attrs)
        n = name.encode("utf-16le")[:70]
        e[56:56 + len(n)] = n
        return bytes(e)

    # ChromeOS attribute bits: priority 48-51, tries 52-55, successful 56
    attrs_kernel = (15 << 48) | (5 << 52) | (1 << 56)

    entries = bytearray(128 * 128)
    entries[0:128] = entry(kernel_type, 20480, 40959, attrs_kernel, "Kernel")
    entries[128:256] = entry(data_type, 40960, last_usable, 0, "ArchRoot")
    entries = bytes(entries)
    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF

    hdr = bytearray(92)
    hdr[0:8] = b"EFI PART"
    struct.pack_into("<I", hdr, 8, 0x00010000)     # revision 1.0
    struct.pack_into("<I", hdr, 12, 92)            # header size
    struct.pack_into("<I", hdr, 16, 0)             # crc placeholder
    struct.pack_into("<Q", hdr, 24, last)          # MyLBA
    struct.pack_into("<Q", hdr, 32, 1)             # AlternateLBA (broken primary)
    struct.pack_into("<Q", hdr, 40, first_usable)
    struct.pack_into("<Q", hdr, 48, last_usable)
    hdr[56:72] = disk_guid.bytes_le
    struct.pack_into("<Q", hdr, 72, entry_lba)
    struct.pack_into("<I", hdr, 80, 128)           # number of entries
    struct.pack_into("<I", hdr, 84, 128)           # entry size
    struct.pack_into("<I", hdr, 88, entries_crc)
    header_crc = zlib.crc32(bytes(hdr)) & 0xFFFFFFFF
    struct.pack_into("<I", hdr, 16, header_crc)

    with open(dev, "r+b", buffering=0) as f:
        f.seek(entry_lba * 512)
        f.write(entries)
        f.seek(last * 512)
        f.write(bytes(hdr).ljust(512, b"\0"))

    print(f"disk            {dev} ({total_sectors} sectors)")
    print(f"backup entries  LBA {entry_lba}..{entry_lba + 31}")
    print(f"backup header   LBA {last}")
    print(f"p1 Kernel       LBA 20480..40959  attrs=0x{attrs_kernel:016x}")
    print(f"p2 ArchRoot     LBA 40960..{last_usable}")
    print(f"entries crc     0x{entries_crc:08x}")
    print(f"header  crc     0x{header_crc:08x}")


if __name__ == "__main__":
    main()
