#!/bin/bash
# Build out/xnu-boot.img: violet shim + sweet's prebuilt XNU payload, as a trimmed boot.img v2 for
# `fastboot boot` (RAM only). Toolchain: clang/lld/llvm-objcopy only (no gcc cross, no mkbootimg, no cpio).
#   env: STOCK=<stock boot.img>   KERNEL=<sweet kernel Mach-O>   CMDLINE="<xnu boot-args>"
#        MEM_SIZE=0x40000000  (optional smaller kernel RAM window from 0xa2000000, fallback if the full window hangs)
set -euo pipefail
cd "$(dirname "$0")"
STOCK=${STOCK:?set STOCK to your stock boot.img dump}
KDIR=${KDIR:?set KDIR to the directory holding kernel.development.sweet and SHA256SUMS}
KERNEL=${KERNEL:-$KDIR/kernel.development.sweet}
# -v + serial=3 : verbose text on the framebuffer console (sweet's fbcon "serial" device)
# rd=ufsroot0   : no /sweet-ufs node on violet -> deterministic, readable panic at root mount instead of
#                 waiting for a root device forever (sweet_ufs_rootdev returns ENXIO before any MMIO)
# sweet{touch,keys,usb,battery}=0, sweetlog=0 : belt and braces, those drivers are only started from the
#                 UFS success path anyway (touch=Goodix on SE7, usb/log need UFS) and are not violet hardware
# cpus=1        : single CPU, no PSCI CPU_ON yet
# debug=0x10000c: DB_NMI|DB_KPRT|DB_REBOOT_ALWAYS -> after a panic xnu calls the reboot path
#                 (kernel patch 0084: wait panicreset=20 s, then PSCI warm reset; RAM/pstore logs survive)
DEFAULT_CMDLINE="-v debug=0x10000c serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 wdt=-1 wdt_test=no_user_checkin sweettouch=0 sweetkeys=0 sweetusb=0 sweetbattery=0 sweetlog=0 sweetfb_noconsole=0"
CMDLINE=${CMDLINE:-$DEFAULT_CMDLINE}

( cd "$KDIR" && sha256sum -c SHA256SUMS )          # refuse an unverified kernel
[ -f "$STOCK" ] || { echo "stock boot.img not found: $STOCK" >&2; exit 1; }

CC="clang --target=aarch64-none-elf"
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -fno-builtin -Wall -Wextra"
mkdir -p out
objs=(); cobjs=()
for f in start.S main.c console.c fdt.c loader.c; do
	$CC $CFLAGS -c shim/$f -o out/${f%.*}.o; objs+=(out/${f%.*}.o)
	[ "$f" = start.S ] || cobjs+=(out/${f%.*}.o)
done
# No relocation pass at load time: C objects must not contain absolute pointers.
abs=$(llvm-readelf -r "${cobjs[@]}" | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; llvm-readelf -r "${cobjs[@]}" | grep ABS64; exit 1; }
ld.lld -T shim/shim.ld -N --build-id=none -static -o out/shim.elf "${objs[@]}"
llvm-objcopy -O binary out/shim.elf out/Image
size=$(llvm-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image

# ADT (adds /product from the real D22AP tree if tools/im4p.py has produced the raw ADT; see PLAN.md)
python3 -I tools/mkadt_violet.py out/violet.adt ${MEM_SIZE:+--mem-size $MEM_SIZE} ${ADT_FLAGS:-}
# payload: SWEETXNU header + kernel segments with chained fixups resolved + ADT + boot-args
python3 -I tools/mkpayload.py "$KERNEL" out/violet.adt out/payload.bin --cmdline "$CMDLINE"
# ramdisk slot = payload; kernel slot = shim; DTB = stock DTB (header fields copied from the stock image)
python3 -I tools/mkboot.py "$STOCK" out/Image out/payload.bin out/xnu-boot.img "console=null"

bytes=$(stat -c %s out/xnu-boot.img)
[ "$bytes" -lt $((700*1024*1024)) ] || { echo "image too large: $bytes" >&2; exit 1; }
echo "xnu-boot.img: $bytes bytes ($((bytes/1024/1024)) MiB)"
sha256sum out/xnu-boot.img out/payload.bin
