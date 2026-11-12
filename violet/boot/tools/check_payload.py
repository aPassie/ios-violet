#!/usr/bin/env python3
"""Host-side dry run of shim/loader.c on out/payload.bin: prints where every piece lands and checks it
against violet's memory map (no hardware access)."""
import struct, sys
b = open(sys.argv[1], 'rb').read()
magic, ver, nsegs = struct.unpack_from('<8sII', b, 0)
virt_base, entry_va, adt_off, adt_size, kend, phys_base, mem_size = struct.unpack_from('<7Q', b, 0x10)
cmd = b[0x48:0x48 + 608].split(b'\0')[0].decode()
assert magic == b'SWEETXNU' and ver == 1
d = phys_base - virt_base
ranges = []
for i in range(nsegs):
    va, vs, fo, fs = struct.unpack_from('<4Q', b, 0x2a8 + 32 * i)
    ranges.append((va + d, va + d + vs, f'seg{i}'))
adt_pa = kend + d
ba_pa = (adt_pa + adt_size + 0x3fff) & ~0x3fff
ranges += [(adt_pa, adt_pa + adt_size, 'ADT'), (ba_pa, ba_pa + 0x4000, 'boot_args')]
win = (phys_base, phys_base + mem_size)
print(f'phys_base {phys_base:#x} mem_size {mem_size:#x} window end {win[1]:#x}  entry PA {entry_va + d:#x}')
print(f'cmdline: {cmd}')
for lo, hi, n in ranges:
    ok = win[0] <= lo and hi <= win[1]
    print(f'  {n:10s} {lo:#12x}-{hi:#12x} {"in window" if ok else "OUTSIDE WINDOW"}')
    assert ok
FB = (0x9c000000, 0x9cf00000); PS = (0xa1600000, 0xa1e00000)
assert phys_base % 0x2000000 == 0
assert win[0] >= PS[1] and win[1] <= 0x17dd00000
assert not (win[0] < FB[1] and FB[0] < win[1]), 'window overlaps framebuffer'
print('payload size', len(b), 'OK: window above pstore/FB, inside System RAM, 32 MiB aligned')
