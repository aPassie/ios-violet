#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the (sweet-built) xnu needs to boot on violet.

Derived from dipa-studio/ios-sweet tools/mkadt.py (MIT, see ../LICENSE.sweet) at HEAD (includes the
/pram panic-log fix). Violet changes, each with the violet evidence:
  - RAM window 0xa2000000..0x17dd00000 (iomem: System RAM a1600000-17dcfffff; ramoops below)
  - framebuffer 1080x2340 (panel dsi_nt36672a, cont_splash_region@9c000000 size 0xf00000)
  - NO sweet-ufs node (violet storage is eMMC sdhci@7c4000; UFS host 0x1d84000 has no device)
  - /pram placed inside the pstore console zone so a panic log can be read from Android
  - 'xiaomi,sweet' stays in compatible: SweetPlatformExpert matches IONameMatch 'xiaomi,sweet'
Usage:  mkadt_violet.py OUT.adt [--dump] [--pram-top] [--product FILE] [--mem-size BYTES]
"""
import os
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM. violet /proc/iomem: one contiguous System RAM run a1600000-17dcfffff, above the
# last fixed carve-out (cdsp_sec_region 9f800000+1e00000 = a1600000) and the ramoops region
# (a1600000+800000 = a1e00000). xnu's start.s maps 32 MiB blocks from physBase, so start on the next
# 32 MiB boundary (0xa2000000) and leave ramoops alone.
PHYS_BASE = 0xa2000000
RAM_END = 0x17dd00000
MEM_SIZE = RAM_END - PHYS_BASE
# Panic log buffer, carved from the TOP of the dram range and exposed to XNU as
# /pram + /chosen/embedded-panic-log-size. Without it, pe_init.c's
# check_for_panic_log() leaves gPanicBase == 0, debug_log_init() bails out twice
# and the kernel hangs right after "kdp_core zlib memory" — reproduced on QEMU
# and independently on ios-fajita.
PANIC_LOG_SIZE = 0x100000
# Default on violet: /pram lives in the pstore console zone (ramoops@a1600000, console-size 4 MiB) at
# +0x10000 so that, if the phone warm-resets after a panic and DDR survives, Android shows xnu's panic
# log in /sys/fs/pstore/console-ramoops-0. The shim (main.c pre_jump) arms the persistent_ram header and
# zeroes this range. --pram-top restores sweet's behaviour (carve 1 MiB off the top of the dram range).
PRAM_PSTORE_BASE = 0xa1610000
PRAM_PSTORE_SIZE = 0x3f0000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17a60000, 0x100000
FB, FB_SIZE = 0x9c000000, 0xf00000     # violet cont_splash_region@9c000000 size 0xf00000
VIOLET_ECID = 0x56494f4c45540001  # 'VIOLET' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 2340, 1080 * 4
TIMEBASE = 19200000            # measured CNTFRQ 19.2 MHz (probe m1)
VTIMER_INTID = 19              # DT timer interrupts <1 3 ..> = PPI 3

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   tools/im4p.py DIR/Firmware/all_flash/DeviceTree.d22ap.im4p d22ap.adt --lzfse   (payload is bvx2)
D22AP_ADT = os.environ.get('D22AP_ADT', 'd22ap.adt')   # raw (decompressed) ADT; see tools/im4p.py


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, emmc=False, pram_top=False, product_path=D22AP_ADT, mem_size=None):
    phys_base, mem_size = PHYS_BASE, mem_size or MEM_SIZE
    if pram_top:
        # Carve the panic log off the top so the kernel never allocates over it.
        panic_base, panic_size = phys_base + mem_size - PANIC_LOG_SIZE, PANIC_LOG_SIZE
        mem_size -= PANIC_LOG_SIZE
    else:
        panic_base, panic_size = PRAM_PSTORE_BASE, PRAM_PSTORE_SIZE
    gicd, gicr, timebase = GICD, GICR, TIMEBASE
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(panic_size)),
        ('firmware-version', cstr('violet-shim')),
        ('system-firmware-version', cstr('violet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        ('display-scale', u32(3)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', VIOLET_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # violet: same topology as sweet (live DT cpu@0..cpu@700, MPIDR core<<8; 0-5 Cortex-A55 part 0x805,
    # 6-7 Cortex-A76 part 0x804). The kernel defaults to one CPU and uses cpus=N for a prefix.
    cpu_nodes = []
    for core in range(8):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,kryo470') + cstr('ARM,v8')),  # name only; xnu does not match on it
            ('reg', u32(core << 8)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2016000000 if core >= 6 else 1708800000)),  # SDM675: descriptive only
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(VTIMER_INTID)),
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1125)),
            ('canvas-height', u32(2436)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    # sweet's 'sweet-ufs' node is deliberately NOT emitted. violet has eMMC (sdhci@7c4000, which ABL powers
    # down); the UFS host block at 0x1d84000 exists in the SoC DT but has no device. In the prebuilt kernel
    # sweet_ufs_rootdev() (rd=ufsroot0) returns ENXIO ("no /sweet-ufs node") before any MMIO access and
    # IOFindBSDRoot then panics with a readable message, instead of waiting forever for a root device.
    # The keys/touch/USB-log/persistent-log drivers are only started from that function's success path.

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
    ])

    # violet eMMC root (kernel patch 0082): reg = <hc_mem 0x1000, GCC 16K page holding SDCC1 clocks (+0x2000)>.
    # Absolute physical addresses, like sweet's top-level /sweet-ufs node. Only with --emmc.
    emmc_node = [node('violet-emmc', [ph(), ('compatible', cstr('qcom,sdhci-msm-v5')),
                                      ('reg', u64(0x7c4000, 0x1000, 0x110000, 0x4000))]),
                  # kernel patch 0083: console mirror into the Linux ramoops pmsg zone (Android: pmsg-ramoops-0)
                  node('violet-pstore', [ph(), ('reg', u64(0xa1a00000, 0x400000))]),
                  # kernel patch 0085: Novatek NT36672A touch on QUP0 SE1, PM6150L WLED backlight
                  node('violet-touch', [ph(), ('compatible', cstr('novatek,nt36672a'))]),
                  node('violet-wled', [ph(), ('compatible', cstr('qcom,pm6150l-wled5'))])] if emmc else []

    product_props = d22ap_product(product_path)
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Panic log region. /pram reg = <phys size>; it sits just above the
    # dram-size we expose, so XNU maps it but never allocates into it.
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(panic_size))])

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet') + cstr('xiaomi,violet') + cstr('qcom,sm6150') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product + emmc_node)


if __name__ == '__main__':
    args = sys.argv[1:]
    product = args[args.index('--product') + 1] if '--product' in args else D22AP_ADT
    mem = int(args[args.index('--mem-size') + 1], 0) if '--mem-size' in args else None   # smaller window = fallback
    root = build(emmc='--emmc' in args, pram_top='--pram-top' in args, product_path=product, mem_size=mem)
    blob = adt.serialize(root)
    open(args[0], 'wb').write(blob)
    if '--dump' in args:
        adt.dump(adt.parse(blob)[0])
    print(f'{args[0]}: {len(blob)} bytes', file=sys.stderr)
