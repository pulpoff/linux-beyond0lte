#!/usr/bin/env python3
"""Replace the ramdisk (and with --kernel, the kernel) of an Android boot image (header v0-v2).

Kernel, second stage, recovery_dtbo, dtb and every header field stay as they
are; the output is padded with zeros to --pad bytes (the partition size), so
the old AVB footer at the end of the partition is cleared too.
"""
import argparse, struct

ap = argparse.ArgumentParser()
ap.add_argument('src'); ap.add_argument('ramdisk'); ap.add_argument('out')
ap.add_argument('--pad', type=int, default=0)
ap.add_argument('--kernel', help='replace the kernel too (e.g. u-boot.bin)')
a = ap.parse_args()

d = open(a.src, 'rb').read()
assert d[:8] == b'ANDROID!'
ks, ka, rs, ra, ss, sa, ta, ps, hv = struct.unpack_from('<9I', d, 8)
assert hv <= 2, hv
pg = lambda n: (n + ps - 1) // ps * ps
o = ps
kernel = d[o:o + ks]; o += pg(ks)
o += pg(rs)
second = d[o:o + ss]; o += pg(ss)
rdtbo = b''; dtb = b''
if hv >= 1:
    rds, rdo = struct.unpack_from('<IQ', d, 1632)
    rdtbo = d[rdo:rdo + rds]; o = rdo + pg(rds)
if hv >= 2:
    dbs, = struct.unpack_from('<I', d, 1648)
    dtb = d[o:o + dbs]

rd = open(a.ramdisk, 'rb').read()
if a.kernel:
    kernel = open(a.kernel, 'rb').read()
hdr = bytearray(d[:ps])
struct.pack_into('<I', hdr, 8, len(kernel))
struct.pack_into('<I', hdr, 16, len(rd))
hdr[576:608] = b'\0' * 32                      # id: a checksum nobody checks; cleared
pad = lambda b: b + b'\0' * (pg(len(b)) - len(b))
body = pad(kernel) + pad(rd) + pad(second)
if hv >= 1:
    struct.pack_into('<IQ', hdr, 1632, len(rdtbo), ps + len(body))
    body += pad(rdtbo)
if hv >= 2:
    body += pad(dtb)
img = bytes(hdr) + body
if a.pad:
    assert len(img) <= a.pad, (len(img), a.pad)
    img += b'\0' * (a.pad - len(img))
open(a.out, 'wb').write(img)
print(f'{a.out}: {len(img)} bytes, kernel {len(kernel)}, ramdisk {len(rd)}, recovery_dtbo {len(rdtbo)}')
