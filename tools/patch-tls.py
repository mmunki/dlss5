#!/usr/bin/env python3
"""Bionic requires PT_TLS to be 64-byte aligned on arm64.

lld emits `p_align = 8` for statically linked executables because no TLS
variable in the program asks for more.  A static bionic program aborts at
startup with

    executable's TLS segment is underaligned: alignment is 8 (skew 0),
    needs to be at least 64 for ARM64 Bionic

so the program header is fixed up after linking.  The change is one field in
PT_TLS (p_align 8 -> 64); no code, section or offset moves.
"""
import struct
import sys

PT_TLS = 7


def patch(path: str) -> int:
    with open(path, "rb") as handle:
        data = bytearray(handle.read())
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise SystemExit(f"{path}: not an ELF64 file")
    if struct.unpack_from("<H", data, 0x12)[0] != 183:  # EM_AARCH64
        raise SystemExit(f"{path}: not an AArch64 file")
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    patched = 0
    for index in range(phnum):
        offset = phoff + index * phentsize
        if struct.unpack_from("<I", data, offset)[0] != PT_TLS:
            continue
        align_offset = offset + 0x30
        current = struct.unpack_from("<Q", data, align_offset)[0]
        if current < 64:
            struct.pack_into("<Q", data, align_offset, 64)
            patched += 1
            print(f"{path}: PT_TLS p_align {current} -> 64")
    with open(path, "wb") as handle:
        handle.write(data)
    return patched


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: tools/patch-tls.py <aarch64-elf>...", file=sys.stderr)
        return 2
    for path in sys.argv[1:]:
        if patch(path) == 0:
            print(f"{path}: already aligned")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
