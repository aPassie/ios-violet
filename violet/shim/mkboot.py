#!/usr/bin/env python3
"""Pack a boot.img header v2 for violet, copying address/os fields from the stock
boot.img so ABL sees exactly what it is used to. Output is trimmed (no padding/AVB)."""
import hashlib, struct, sys
stock, kernel, ramdisk, out = sys.argv[1:5]
cmdline = (sys.argv[5] if len(sys.argv) > 5 else "console=null").encode()
s = open(stock, 'rb').read()
assert s[:8] == b'ANDROID!'
ks, ka, rs, ra, ss, sa, ta, pg, hv, osv = struct.unpack('<10I', s[8:48])
assert hv == 2 and pg == 4096, (hv, pg)
up = lambda n: (n + pg - 1) // pg * pg
dsz, = struct.unpack('<I', s[1648:1652]); daddr, = struct.unpack('<Q', s[1652:1660])
doff = pg + up(ks) + up(rs) + up(ss)
dtb = s[doff:doff + dsz]
assert dtb[:4] == b'\xd0\x0d\xfe\xed', 'stock dtb not found'
k = open(kernel, 'rb').read(); r = open(ramdisk, 'rb').read()
sha = hashlib.sha1()
for blob in (k, r, b''):
    sha.update(blob); sha.update(struct.pack('<I', len(blob)))
sha.update(b''); sha.update(struct.pack('<I', 0))   # recovery_dtbo (v1+)
sha.update(dtb); sha.update(struct.pack('<I', len(dtb)))  # dtb (v2)
hdr = b'ANDROID!' + struct.pack('<10I', len(k), ka, len(r), ra, 0, sa, ta, pg, 2, osv)
hdr += b'\0' * 16 + cmdline.ljust(512, b'\0') + sha.digest().ljust(32, b'\0') + b'\0' * 1024
hdr += struct.pack('<IQI', 0, 0, 1660) + struct.pack('<IQ', len(dtb), daddr)
assert len(hdr) == 1660, len(hdr)
pad = lambda b: b + b'\0' * (up(len(b)) - len(b))
img = pad(hdr) + pad(k) + pad(r) + pad(dtb)
open(out, 'wb').write(img)
print(f"{out}: {len(img)} bytes  kernel={len(k)} @0x{ka:x} ramdisk={len(r)} @0x{ra:x} dtb={len(dtb)} @0x{daddr:x} os=0x{osv:x}")
