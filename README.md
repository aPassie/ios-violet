# iOS 16 on the Redmi Note 7 Pro (violet)

Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) running the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X** on the Xiaomi **Redmi Note 7 Pro** (`violet`, Qualcomm SM6150 / Snapdragon 675).

This is a port of [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet) (Redmi Note 10 Pro, SM7150) to a different phone. Sweet's kernel patches `0001`-`0081` are used as they are. Violet adds its own storage, touch and backlight drivers plus boot tooling: patches `0082` and up, and the `violet/` directory. Sweet's original README is kept as [README.sweet.md](README.sweet.md).

> Experimental bring-up, not a usable phone OS. It boots to the iOS lock screen and home screen and responds to touch and the power key. Most hardware is not supported (see below). It installs into the `userdata` partition and so **erases Android's user data**.

## Status

| Area | State on violet |
| --- | --- |
| Boot | `fastboot boot` from RAM: bare-metal shim -> XNU. Nothing is flashed to boot partitions |
| CPU | All 8 cores (6x Kryo 460 silver + 2x gold) through PSCI |
| Storage | New polled eMMC driver (SDHC1, sdhci-msm-v5). iOS root is a journaled HFS+ volume in `userdata`, read/write |
| Display | CPU-drawn 1125x2436 iPhone X image, centered on the 1080x2340 panel (continuous-splash framebuffer) |
| Boot time | About 3.5 minutes to the lock screen on 8 cores |
| Touch | Novatek NT36672A (in-cell TDDI) on QUP0 SE1 I2C, reset at start, one event read per INT pulse |
| Edge gestures | Touch mapping is stretched at the cropped edges for the home swipe and Control Center (patch 0087, being tested) |
| Buttons | Power key works (sweet's PMIC driver, same PM6150). Volume keys untested: broken hardware on the test unit |
| Backlight | PM6150L WLED5 switched off/on with the display over SPMI |
| Logs | Console mirrored to the Linux pstore zone, plus sweet's on-disk log ring at the end of `userdata` |
| Not working | Wi-Fi, Bluetooth, cellular, audio, camera, GPU (no Adreno 612 driver), sensors, battery gauge, USB, real-time clock, brightness slider |

Device identity, activation bypass and the userland tweaks are sweet's: the phone presents itself as an iPhone X (`iPhone10,3` / `D22AP`).

## Hardware notes (verified on the device)

| Block | Details |
| --- | --- |
| SoC | SM6150, MIDR 0x51DF805E, GICv3, PSCI 1.1, 19.2 MHz timer, 16K pages supported |
| RAM | ~3.7 GB; XNU gets 0xA2000000 + 3.43 GiB |
| eMMC | Samsung eMMC 5.1 (CID 0x15 "DH6DAB"), 62.5 GB, host at 0x7c4000. ABL powers the host down before jumping, so the driver does the full bring-up: PWRCTL handshake, 1.8 V pads, GCC SDCC1 clock (400 kHz, then 25 MHz 8-bit) |
| Display | NT36672A "shenchao" video-mode DSI panel, framebuffer 0x9c000000, a8r8g8b8, stride 4320 |
| Touch | NT36672A TDDI at I2C 0x01 (0x62 for reset), event buffer 0x21C00, INT GPIO89 (active low, ~120 Hz). The firmware does not report until the host sends a bootloader reset (`00 69` @0x62), and reads are valid only when timed to INT |
| Backlight | PM6150L (SPMI SID 5) WLED5: ctrl 0xd800, sink 0xd900, 12-bit brightness |
| Logs | Linux ramoops at 0xa1600000 (console 4 MiB) and 0xa1a00000 (pmsg 4 MiB) |

## Repository layout (violet additions)

| Path | Contents |
| --- | --- |
| `xnu/patches/0082-0087` | Violet kernel patches (below), applied after sweet's series by the same CI |
| `violet/boot/` | Boot-image builder: shim (from sweet, adapted), `tools/mkadt_violet.py` (device tree), `mkpayload.py`, `mkboot.py`, `build.sh` |
| `violet/boot/tools/boot-ios.sh` | Safe boot: arms `misc` with a "boot-recovery" message, then `fastboot boot` |
| `violet/boot/tools/violet-log-pull.sh` | Reads the kernel/iOS log from the end of `userdata` through a root adb shell in recovery |
| `violet/shim/` | Bare-metal probe shim used for bring-up (CPU, eMMC and touch tests). It logs to pstore and resets |

| Patch | What it does |
| --- | --- |
| 0082 | eMMC root backend for `rd=ufsroot0` (sdhci-msm-v5, polled PIO, CMD18/25 with auto-CMD12), selected by ADT node `/violet-emmc` |
| 0083 | Mirrors the console into the Linux pstore pmsg zone (`/violet-pstore`), so a run's whole log survives a warm reset |
| 0084 | Warm reset after a panic (PSCI SYSTEM_RESET, boot-arg `panicreset=`), eMMC bring-up step log |
| 0085 | Novatek NT36672A touch path (`/violet-touch`), panel-size-aware touch geometry, PM6150L WLED backlight off/on (`/violet-wled`) |
| 0086 | Touch: bootloader reset at start, one event read per INT pulse, gap-timed register reads |
| 0087 | Touch: stretched mapping at the cropped display edges, 30-second touch statistics in the log |

## Requirements

- A Redmi Note 7 Pro (violet) with an **unlocked bootloader**, a Linux PC, and a GitHub account for the kernel build.
- A recovery with a root adb shell for reading logs. The test unit uses OrangeFox, installed in the recovery partition.
- Back up the boot chain first (at least `boot dtbo vbmeta recovery misc persist modemst1 modemst2 fsg fsc`). Bootloaders, modem and persist are never written; `misc` and `userdata` are.

## Build and boot

### 1. Kernel

The kernel builds on GitHub's macOS runners (`macos-15`, Xcode 16). Fork this repo, enable Actions, and push or run:

```sh
gh workflow run xnu.yml -R YOU/ios-violet
gh run download RUN_ID -R YOU/ios-violet -n xnu-kernel -D kernel/
```

### 2. iOS root image

As in sweet: download the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW from Apple, extract the OS, SystemOS and AppOS disk images (names are in `BuildManifest.plist`), then run `tools/mkiosroot.sh`, `fsck.hfsplus -fy`, mount, `tools/sweet-root-tweaks.py`, unmount, and `img2simg`. See [README.sweet.md](README.sweet.md) section 2.

Linux's `fsck_hfs` keeps reporting "Volume header needs minor repair (1, 0)" even right after a repair. This is harmless.

### 3. Flash the root

```sh
fastboot -S 16M flash userdata ios16-root.simg
```

Use **16 MB** sparse chunks. On the test setup, single transfers above roughly 17 MB stall: with `-S 512M` the flash hung and nothing was written.

### 4. Boot image

`violet/boot/tools/mkadt_violet.py` needs the raw iPhone X device tree (`DeviceTree.d22ap.im4p` from the IPSW, decompressed with `lzfse`) for `/product`. Point it there with `D22AP_ADT=`.

```sh
cd violet/boot
STOCK=/path/to/stock-boot.img KDIR=/path/to/kernel ADT_FLAGS=--emmc D22AP_ADT=/path/to/d22ap.adt \
CMDLINE="-v debug=0x10000c serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 \
 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 \
 wdt=-1 sweetfb_noconsole=1 sweettouch=1 sweetkeys=1 sweetusb=0 sweetbattery=0 sweetlog=1 panicreset=30" \
./build.sh
cp out/xnu-boot.img out/ios-boot.img
```

Keep the image under about 17 MB, for the same transfer limit.

### 5. Boot

```sh
SERIAL=<your-serial> tools/boot-ios.sh out/ios-boot.img
```

The first boot sits on a black screen for about 3.5 minutes. Tap the power key to wake the lock screen and swipe up to unlock.

**Important: Android's `userdata` fstab entry is `formattable`.** If the phone ever boots Android normally with the iOS volume in `userdata`, Android may format it. `boot-ios.sh` therefore writes an AOSP "boot-recovery" message to `misc` before every boot. Qualcomm ABL honours it, so any reset out of iOS (panic, long power press) goes to recovery instead of Android. The recovery clears the message when it starts. To go back to Android, clear `misc` with `tools/mkbcb.py --clear` and wipe `userdata`.

### 6. Logs

```sh
SERIAL=<your-serial> violet/boot/tools/violet-log-pull.sh boot.txt      # from recovery (root adb)
adb shell cat /sys/fs/pstore/pmsg-ramoops-0                             # console mirror, after a warm reset
```

## Bring-up notes

- **ABL state.** ABL leaves the display on (continuous splash), but the eMMC host, the touch I2C engine and the touch firmware's reporting are all off. Every driver here starts from that state.
- **Method.** Every device was first driven from the bare-metal shim (`violet/shim`, a one-minute test loop with results in pstore), then moved into the kernel.
- **Touch.** Touch took the longest. Polling the event buffer without INT reads random IC memory: the firmware does not apply the register index outside its report window. The fix that matched Linux was a bootloader reset, then one read per INT pulse.

## Credits

- **Base port:** [dipa-studio/ios-sweet](https://github.com/dipa-studio/ios-sweet), whose kernel patches, userland tooling and drivers this port builds on (MIT).
- **Open source used:** Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build) and [ipsw](https://github.com/blacktop/ipsw).
- **Hardware references:** Xiaomi/Qualcomm's published Linux kernel sources for violet (sdhci-msm, nt36xxx, qcom-spmi-wled, GCC/TLMM), mainline Linux and U-Boot.
- **Recovery:** [OrangeFox Recovery](https://orangefox.download).
- **Not distributed:** iOS itself is Apple's and is not included here.

License: MIT (see [LICENSE](LICENSE)). The XNU patches are derived from APSL 2.0 sources.
