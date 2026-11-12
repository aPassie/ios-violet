#!/bin/bash
# Pull the XNU console log ("sweetlog", kernel 0028/0047 + violet eMMC 0082) from the end of
# violet's userdata, via a root adb shell (OrangeFox recovery). Prints the log in order.
#   tools/violet-log-pull.sh [out.txt]
# Layout (bsd/dev/sweet_ufs.c): partition seen as nblocks = floor(sectors/8) 4 KiB blocks;
# log area = last 1 + 1024 blocks: block 0 = header "SWEETLOG" u32 ver, u32 ring size,
# u64 total bytes written, u64 seq; then the 4 MiB ring, byte n at n % ring size.
set -euo pipefail
S=${SERIAL:?set SERIAL to the phone adb/fastboot serial}
OUT=${1:-sweetlog-$(date +%H%M%S).txt}
bytes=$(adb -s "$S" shell blockdev --getsize64 /dev/block/by-name/userdata | tr -d '\r')
nblocks=$(( bytes / 4096 ))
base=$(( nblocks - 1025 ))
echo "userdata $bytes bytes, $nblocks 4K blocks, log at block $base" >&2
raw=$(mktemp)
adb -s "$S" exec-out "dd if=/dev/block/by-name/userdata bs=4096 skip=$base count=1025 2>/dev/null" > "$raw"
python3 -I - "$raw" "$OUT" <<'EOF'
import struct, sys
d = open(sys.argv[1], 'rb').read()
if d[:8] != b'SWEETLOG':
    sys.exit(f"no SWEETLOG header (got {d[:8]!r}): kernel never flushed the log (needs ufsrw=1, sweetlog=1, ~10 s uptime)")
ver, size, total, seq = struct.unpack('<IIQQ', d[8:32])
ring = d[4096:4096 + size]
start = total - size if total > size else 0
out = bytearray(ring[i % size] for i in range(start, total))
open(sys.argv[2], 'wb').write(out)
print(f"version {ver}, {total} bytes logged, flush #{seq} -> {sys.argv[2]}", file=sys.stderr)
EOF
rm -f "$raw"
tail -n 40 "$OUT"
