#!/bin/bash
# Boot iOS on violet from RAM, safely without volume keys:
#   1. get to fastboot (from Android/recovery via adb, or already there)
#   2. arm misc with an AOSP "boot-recovery" bootloader message: ABL honours it on the
#      next power-up, so ANY reset out of iOS (panic, power long-press) lands in OrangeFox,
#      never in Android (whose fstab has `formattable`: it would wipe the iOS userdata).
#      OrangeFox clears the message when it starts, so this is armed before every boot.
#   3. fastboot boot the XNU image (must stay < ~17 MB: larger single transfers stall here)
#   tools/boot-ios.sh [image]      default: out/ios-boot.img
set -euo pipefail
S=${SERIAL:?set SERIAL to the phone adb/fastboot serial}
IMG=${1:-$(dirname "$0")/../out/ios-boot.img}
BCB=${BCB:-$(dirname "$0")/../out/misc-boot-recovery.img}
[ -f "$IMG" ]
[ -f "$BCB" ] || { mkdir -p "$(dirname "$BCB")"; python3 "$(dirname "$0")/mkbcb.py" "$BCB"; }
size=$(stat -c %s "$IMG"); [ "$size" -lt 17500000 ] || { echo "image $size bytes: too large for fastboot boot here" >&2; exit 1; }
if ! fastboot devices | grep -q "$S"; then
	adb -s "$S" reboot bootloader
	until fastboot devices | grep -q "$S"; do sleep 1; done
fi
fastboot -s "$S" flash misc "$BCB"
fastboot -s "$S" boot "$IMG"
date +%T
