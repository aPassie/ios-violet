#!/usr/bin/env python3
"""Write an AOSP bootloader_message ("boot-recovery") image for the misc partition.
Qualcomm ABL honours it on the next power-up, so any reset lands in recovery.
  mkbcb.py out.img            -> boot-recovery
  mkbcb.py --clear out.img    -> zeroed message (normal boot)"""
import sys
args = sys.argv[1:]
clear = '--clear' in args
out = [a for a in args if a != '--clear'][0]
if clear:
    blob = b'\0' * 2048
else:  # char command[32]; char status[32]; char recovery[768]; ...
    blob = (b'boot-recovery'.ljust(32, b'\0') + b'\0' * 32 + b'recovery\n'.ljust(768, b'\0')).ljust(2048, b'\0')
open(out, 'wb').write(blob)
print(f'{out}: {"cleared" if clear else "boot-recovery"} bootloader message, {len(blob)} bytes')
