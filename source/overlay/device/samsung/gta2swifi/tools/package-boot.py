#!/usr/bin/env python3
"""Preserve the proven Samsung legacy QCDTv3 boot container, with a new ramdisk."""
import hashlib
import json
import struct
import sys
from pathlib import Path


def package(source, table_path, output):
    b, table = source.read_bytes(), table_path.read_bytes()
    receipt = json.loads((table_path.parent / "receipt.json").read_text())
    approved = json.loads((Path(__file__).parent / "approved-kernels.json").read_text())
    profile = approved.get(receipt.get("diagnostic"))
    if profile is None or receipt["source_sha256"] != profile["boot_sha256"]:
        raise ValueError("Receipt does not identify an approved diagnostic boot image")
    if (receipt["kernel_sha256"] != profile["kernel_sha256"] or
            receipt["dt_sha256"] != profile["dt_sha256"] or
            receipt["charge_inhibited"] != profile["charge_inhibited"] or
            (not profile["charge_inhibited"] and
             (profile.get("charging_mode") != "samsung_stock_autonomous" or
              receipt.get("charging_mode") != "samsung_stock_autonomous"))):
        raise ValueError("Receipt differs from approved payload hashes and charging mode")
    if b[:8] != b"ANDROID!":
        raise ValueError("Not an Android boot image")
    k, _, r, _, second, _, _, page, version, _ = struct.unpack_from("<10I", b, 8)
    if page != 2048 or version != 0 or second != 0:
        raise ValueError("Requires unmodified modern header-v0 input with no second stage")
    align = lambda n: (n + page - 1) // page * page
    kernel = b[page:page + k]
    start = page + align(k)
    ramdisk = b[start:start + r]
    if len(kernel) != k or len(ramdisk) != r or len(b) != start + align(r):
        raise ValueError("Unexpected length or trailer in input")
    if hashlib.sha256(kernel).hexdigest() != receipt["kernel_sha256"]:
        raise ValueError("Kernel differs from immutable approved receipt")
    if hashlib.sha256(table).hexdigest() != receipt["dt_sha256"]:
        raise ValueError("DT receipt mismatch")
    if struct.unpack_from("<3I", table) != (0x54444351, 3, 1):
        raise ValueError("Expected QCDTv3 with one entry")
    entry = struct.unpack_from("<10I", table, 12)
    if entry[:8] != (307, 8, 5, 0, 0x10019, 0, 0, 0) or entry[8:] != (page, len(table) - page):
        raise ValueError("Not the proved Rev05 table geometry")
    fdt_len = struct.unpack_from(">I", table, page + 4)[0]
    if table[page:page + 4] != bytes.fromhex("d00dfeed") or kernel[-fdt_len:] != table[page:page + fdt_len]:
        raise ValueError("Appended FDT and table FDT differ")
    header = bytearray(b[:page])
    struct.pack_into("<II", header, 40, len(table), 0)
    digest = hashlib.sha1()
    for payload in (kernel, ramdisk, b"", table):
        digest.update(payload)
        digest.update(struct.pack("<I", len(payload)))
    header[576:608] = digest.digest() + bytes(12)
    result = bytes(header) + b[page:] + table + b"SEANDROIDENFORCE"
    if len(result) > 33554432:
        raise ValueError("Boot image exceeds confirmed32MiB BOOT partition")
    output.write_bytes(result)
    print("boot_sha256", hashlib.sha256(result).hexdigest(), "bytes", len(result))


if __name__ == "__main__":
    package(*map(Path, sys.argv[1:]))
