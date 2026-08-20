#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
mkbootscr.py - generate boot.scr for the DVR card (no mkimage needed).

The standalone-boot design keeps U-Boot on SPI-NOR untouched (it remains
the dev/recovery path) and stores the DVR firmware at NOR offset 0x100000.
U-Boot's distro-boot scans the SD card for /boot.scr; this script makes it
load and start the firmware automatically. Every DVR card should carry the
generated boot.scr in its root.

Legacy uImage script format: 64-byte header (CRC'd) + payload, where a
script payload is two 32-bit big-endian words (text size, 0) + the text.
"""
import struct
import sys
import zlib

SCRIPT = b"""echo Booting f1c200s-dvr from SPI-NOR
sf probe
sf read 0x80000000 0x100000 0x40000
go 0x80000000
"""

payload = struct.pack(">II", len(SCRIPT), 0) + SCRIPT

IH_MAGIC = 0x27051956
IH_OS_UBOOT = 17       # firmware
IH_ARCH_ARM = 2
IH_TYPE_SCRIPT = 6
IH_COMP_NONE = 0

name = b"f1c200s-dvr boot"
hdr = struct.pack(
    ">IIIIIIIBBBB32s",
    IH_MAGIC,
    0,                      # hcrc placeholder
    0,                      # timestamp (board has no clock anyway)
    len(payload),
    0,                      # load
    0,                      # entry
    zlib.crc32(payload) & 0xFFFFFFFF,
    IH_OS_UBOOT,
    IH_ARCH_ARM,
    IH_TYPE_SCRIPT,
    IH_COMP_NONE,
    name.ljust(32, b"\x00"),
)
hcrc = zlib.crc32(hdr) & 0xFFFFFFFF
hdr = hdr[:4] + struct.pack(">I", hcrc) + hdr[8:]

out = sys.argv[1] if len(sys.argv) > 1 else "boot.scr"
with open(out, "wb") as f:
    f.write(hdr + payload)
print(f"{out}: {len(hdr + payload)} bytes")
