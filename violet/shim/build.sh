#!/bin/bash
# Build the violet probe shim with clang/lld and pack it as a trimmed boot.img v2.
set -euo pipefail
cd "$(dirname "$0")"
STOCK=${STOCK:?set STOCK to your stock boot.img dump}
CC="clang --target=aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-builtin -Wall -Wextra"
mkdir -p out
objs=()
for f in start.S probe.c console.c emmc.c touch.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o; objs+=(out/${f%.*}.o)
done
# Runs wherever ABL loads it with no relocation pass: C objects must have no absolute relocations.
abs=$(llvm-readelf -r out/probe.o out/console.o out/emmc.o out/touch.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; llvm-readelf -r out/probe.o out/console.o out/emmc.o out/touch.o | grep ABS64; exit 1; }
ld.lld -T shim.ld -N --build-id=none -static -o out/shim.elf "${objs[@]}"
llvm-objcopy -O binary out/shim.elf out/Image
size=$(llvm-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image
# empty newc cpio (ABL dislikes a zero-size ramdisk on some builds)
python3 - <<'PY'
import gzip
name=b'TRAILER!!!\0'
h=b'070701'+b''.join(b'%08X'%v for v in (0,0,0,0,1,0,0,0,0,0,0,len(name),0))+name
h+=b'\0'*((-len(h))%4)
open('out/ramdisk.bin','wb').write(gzip.compress(h.ljust(512,b'\0')))
PY
python3 mkboot.py "$STOCK" out/Image out/ramdisk.bin out/probe-boot.img "console=null"
head -c 64 out/Image | xxd | head -4
sha256sum out/probe-boot.img
