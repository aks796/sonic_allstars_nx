#!/usr/bin/env python3
"""make_exefs_override.py -- launch the wrapper from a home-menu icon, no R.

Atmosphere replaces an installed title's whole ExeFS (every NSO plus
main.npdm) with /atmosphere/contents/<title id>/exefs.nsp. Point that at a
title you created for the purpose -- a sphaira forwarder is ideal -- and its
icon starts Angry Birds Space, 32-bit, with full application memory. Nothing
else is touched: hbmenu/the album and every other game behave as before.

The loader requires main.npdm's program ID to match the title being
launched, so this tool rewrites it (ACI0 program id and the ACID range, the
same fields sphaira patches in its own forwarders) and repacks the PFS0.

    python3 tools/make_exefs_override.py 0500ABCDEF012000 -o /Volumes/SWITCH
    python3 tools/make_exefs_override.py 0500ABCDEF012000 -o ../SD_CARD

To undo: delete /atmosphere/contents/<title id>/exefs.nsp (the forwarder then
does what it did before).
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_NSP = os.path.join(os.path.dirname(HERE), "labyrinth2_nx.nsp")


def read_pfs0(data):
    magic, n, strsz, _ = struct.unpack_from("<4sIII", data, 0)
    if magic != b"PFS0":
        sys.exit("error: not a PFS0/NSP container")
    table = 16 + 24 * n
    data_start = table + strsz
    files = {}
    for i in range(n):
        off, size, name_off, _ = struct.unpack_from("<QQII", data, 16 + 24 * i)
        name = data[table + name_off:data.index(b"\0", table + name_off)].decode()
        files[name] = data[data_start + off:data_start + off + size]
    return files


def write_pfs0(files):
    names = list(files)
    strtab = b""
    name_offs = []
    for nm in names:
        name_offs.append(len(strtab))
        strtab += nm.encode() + b"\0"
    header_len = 16 + 24 * len(names) + len(strtab)
    strtab += b"\0" * ((-header_len) % 0x20)  # data starts 0x20-aligned
    out = struct.pack("<4sIII", b"PFS0", len(names), len(strtab), 0)
    off = 0
    for nm, no in zip(names, name_offs):
        out += struct.pack("<QQII", off, len(files[nm]), no, 0)
        off += len(files[nm])
    out += strtab
    for nm in names:
        out += files[nm]
    return out


def patch_npdm(npdm, tid):
    npdm = bytearray(npdm)
    if npdm[:4] != b"META":
        sys.exit("error: main.npdm is not an NPDM")
    flags = npdm[0xC]
    aci0_off, aci0_size, acid_off, acid_size = struct.unpack_from("<IIII", npdm, 0x70)
    if npdm[aci0_off:aci0_off + 4] != b"ACI0" or npdm[acid_off + 0x200:acid_off + 0x204] != b"ACID":
        sys.exit("error: unexpected NPDM layout")
    old = struct.unpack_from("<Q", npdm, aci0_off + 0x10)[0]
    struct.pack_into("<Q", npdm, aci0_off + 0x10, tid)           # ACI0 program id
    struct.pack_into("<QQ", npdm, acid_off + 0x210, tid, tid)     # ACID program id range
    return bytes(npdm), old, flags


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("title_id", help="16 hex digits: the installed title to launch through")
    ap.add_argument("-o", "--out", required=True, help="SD card root (or a staging folder)")
    ap.add_argument("--nsp", default=DEFAULT_NSP, help="the built wrapper (default: labyrinth2_nx.nsp)")
    a = ap.parse_args()

    t = a.title_id.lower().removeprefix("0x")
    if len(t) != 16 or any(c not in "0123456789abcdef" for c in t):
        sys.exit("error: title id must be 16 hex digits, e.g. 0500ABCDEF012000")
    tid = int(t, 16)
    if (tid >> 56) not in (0x01, 0x05):
        print("warning: %016X does not look like an application / forwarder title id" % tid)

    files = read_pfs0(open(a.nsp, "rb").read())
    if "main" not in files or "main.npdm" not in files:
        sys.exit("error: %s lacks main / main.npdm" % a.nsp)
    npdm, old, flags = patch_npdm(files["main.npdm"], tid)
    if flags & 1:
        sys.exit("error: this NPDM is 64-bit; expected the 32-bit wrapper")
    files["main.npdm"] = npdm

    dst_dir = os.path.join(a.out, "atmosphere", "contents", "%016X" % tid)
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, "exefs.nsp")
    with open(dst, "wb") as f:
        f.write(write_pfs0(files))
    print("program id %016X -> %016X (32-bit, address space type %d)" % (old, tid, (flags >> 1) & 7))
    print("wrote %s" % dst)
    print("\nLaunch that title's icon normally. Delete exefs.nsp to undo.")


if __name__ == "__main__":
    main()
