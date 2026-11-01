#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#
"""Build a Samsung Exynos boot or recovery image: Android boot header v0, kernel, ramdisk, a DTBH v2 device
tree table and the SEANDROIDENFORCE trailer, the layout S-Boot loads on universal8895.

The header has no DTB field: S-Boot finds the DTBH table at the page after the ramdisk (second_size is always
0) and the header's id covers it, as Samsung's mkbootimg computed it. AOSP mkbootimg can only place a DTB in
a header v2+ image, which S-Boot does not parse.

Accepts the arguments build/make passes to $(MKBOOTIMG) (--kernel, --ramdisk, --dtb, --cmdline, --base,
--pagesize, --output); --second, --recovery_dtbo and header versions above 0 are refused.
"""
import argparse
import hashlib
import struct
import sys

DTBH_MAGIC = b"DTBH"
DTBH_VERSION = 2
FDT_MAGIC = b"\xd0\x0d\xfe\xed"
TRAILER = b"SEANDROIDENFORCE"


def roundup(n, page):
    return (n + page - 1) // page * page


def pad(data, page):
    return data + b"\0" * (roundup(len(data), page) - len(data))


def split_fdts(blob):
    """The FDTs concatenated in a dtb.img, each cut at its header's totalsize."""
    fdts = []
    off = 0
    while off < len(blob):
        if blob[off:off + 4] != FDT_MAGIC:
            sys.exit(f"mkdtbhbootimg: no FDT magic at offset {off} of the DTB image")
        size = struct.unpack_from(">I", blob, off + 4)[0]
        fdts.append(blob[off:off + size])
        off += size
    return fdts


def make_dtbh(fdts, page, chip, platform, subtype, hw_rev, hw_rev_end, space):
    """DTBH v2: magic, version, entry count, one 8-word entry per FDT, a zero end-of-table word, the table
    padded to a page, then each FDT padded to a page at the offset its entry records."""
    entry_words = 8
    table = 12 + len(fdts) * entry_words * 4 + 4
    offset = roundup(table, page)
    hdr = DTBH_MAGIC + struct.pack("<II", DTBH_VERSION, len(fdts))
    body = b""
    for fdt in fdts:
        size = roundup(len(fdt), page)
        hdr += struct.pack("<8I", chip, platform, subtype, hw_rev, hw_rev_end, offset, size, space)
        body += pad(fdt, page)
        offset += size
    hdr += struct.pack("<I", 0)
    return pad(hdr, page) + body


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--ramdisk", required=True)
    ap.add_argument("--dtb", required=True, help="one FDT or several concatenated (dtb.img)")
    ap.add_argument("--output", "-o", required=True)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--board", default="")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0x10000000)
    ap.add_argument("--pagesize", type=lambda x: int(x, 0), default=2048)
    ap.add_argument("--kernel_offset", type=lambda x: int(x, 0), default=0x00008000)
    ap.add_argument("--ramdisk_offset", type=lambda x: int(x, 0), default=0x01000000)
    ap.add_argument("--second_offset", type=lambda x: int(x, 0), default=0x00f00000)
    ap.add_argument("--tags_offset", type=lambda x: int(x, 0), default=0x00000100)
    ap.add_argument("--header_version", type=int, default=0)
    ap.add_argument("--dtbh_chip", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--dtbh_platform", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--dtbh_subtype", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--dtbh_hw_rev", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--dtbh_hw_rev_end", type=lambda x: int(x, 0), required=True)
    ap.add_argument("--dtbh_space", type=lambda x: int(x, 0), default=0x20)
    ap.add_argument("--second")
    ap.add_argument("--recovery_dtbo")
    a = ap.parse_args()

    if a.header_version != 0:
        sys.exit("mkdtbhbootimg: S-Boot reads boot header version 0 only")
    if a.second or a.recovery_dtbo:
        sys.exit("mkdtbhbootimg: --second and --recovery_dtbo have no place in this layout")
    cmdline = a.cmdline.encode()
    if len(cmdline) >= 512:
        sys.exit("mkdtbhbootimg: --cmdline does not fit the 512-byte header field")
    board = a.board.encode()
    if len(board) > 16:
        sys.exit("mkdtbhbootimg: --board is longer than 16 bytes")

    page = a.pagesize
    kernel = open(a.kernel, "rb").read()
    ramdisk = open(a.ramdisk, "rb").read()
    dtbh = make_dtbh(split_fdts(open(a.dtb, "rb").read()), page, a.dtbh_chip, a.dtbh_platform,
                     a.dtbh_subtype, a.dtbh_hw_rev, a.dtbh_hw_rev_end, a.dtbh_space)

    # Samsung's id: SHA-1 over each part followed by its size, the empty second stage included.
    sha = hashlib.sha1()
    for part in (kernel, ramdisk, b"", dtbh):
        sha.update(part)
        sha.update(struct.pack("<I", len(part)))
    image_id = sha.digest() + b"\0" * 12

    hdr = b"ANDROID!" + struct.pack(
        "<10I", len(kernel), a.base + a.kernel_offset, len(ramdisk), a.base + a.ramdisk_offset, 0,
        a.base + a.second_offset, a.base + a.tags_offset, page, len(dtbh), 0)
    hdr += board.ljust(16, b"\0") + cmdline.ljust(512, b"\0") + image_id

    with open(a.output, "wb") as f:
        f.write(pad(hdr, page) + pad(kernel, page) + pad(ramdisk, page) + dtbh + TRAILER)


if __name__ == "__main__":
    main()
