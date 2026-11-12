#!/usr/bin/env python3
"""Minimal IM4P (DER) unwrapper: im4p.py IN.im4p OUT.bin [--raw | --lzfse]
  (default) refuse encrypted or compressed payloads
  --raw     write the payload as stored, even if bvx2-compressed
  --lzfse   if bvx2, decode with an `lzfse` binary from PATH (not installed here)

Parses SEQUENCE { IA5String "IM4P", IA5String type, IA5String version,
OCTET STRING data, [OCTET STRING kbag], [SEQUENCE compression info] }.
Writes the payload. Stops with an error if the payload is encrypted (KBAG)
or compressed (bvx2/lzfse, complzss, ...)."""
import sys


def der(buf, off):
    tag = buf[off]
    ln = buf[off + 1]
    off += 2
    if ln & 0x80:
        n = ln & 0x7f
        ln = int.from_bytes(buf[off:off + n], 'big')
        off += n
    return tag, off, off + ln


def parse(buf):
    tag, s, e = der(buf, 0)
    assert tag == 0x30, 'not a DER SEQUENCE'
    items, off = [], s
    while off < e:
        t, vs, ve = der(buf, off)
        items.append((t, bytes(buf[vs:ve])))
        off = ve
    assert items[0][1] == b'IM4P', 'not an IM4P'
    return items


if __name__ == '__main__':
    buf = open(sys.argv[1], 'rb').read()
    items = parse(buf)
    payload = items[3][1]
    print('type', items[1][1], 'desc', items[2][1], 'payload bytes', len(payload),
          'extra items', [(hex(t), len(v)) for t, v in items[4:]], file=sys.stderr)
    if any(t == 0x04 for t, _ in items[4:]):
        sys.exit('payload is encrypted (KBAG present): stop')
    head = payload[:8]
    if '--raw' in sys.argv:
        pass
    elif '--lzfse' in sys.argv and head[:4] == b'bvx2':
        import subprocess
        payload = subprocess.run(['lzfse', '-decode'], input=payload, capture_output=True, check=True).stdout
    elif head[:4] in (b'bvx2', b'bvxn', b'bvx-', b'bvx1') or head.startswith(b'complzss'):
        sys.exit(f'payload is compressed ({head[:8]!r}): stop')
    if len(items) > 4:  # a trailing SEQUENCE carries the compression algorithm/size
        print('note: trailing items present', file=sys.stderr)
    open(sys.argv[2], 'wb').write(payload)
    print(f'wrote {sys.argv[2]}: {len(payload)} bytes; first 16 = {payload[:16].hex()}', file=sys.stderr)
