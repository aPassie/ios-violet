/*
 * violet probe shim, milestone 1.
 * Entered by Qualcomm ABL via `fastboot boot` (EL1, MMU off, x0 = DTB).
 * Answers, on screen, the questions that decide the port:
 *   - CPU: MIDR/MPIDR, 16K granule (TGran16), GIC sysreg iface, timer freq
 *   - PSCI / SMCCC versions
 *   - framebuffer format: draws in a8r8g8b8 (32bpp) first, then packed 24bpp
 *   - eMMC host (sdhci-msm-v5 @ 0x7c4000): register state ABL left behind,
 *     command-queue engine state, then READ-ONLY CMD13 (status) and CMD17 of
 *     LBA 1 (GPT header, expect "EFI PART") by polled PIO.
 * Nothing is written to the card. Ends with reboot-to-bootloader.
 * start.S, shim.ld, console.c, font.h come from dipa-studio/ios-sweet (MIT).
 */
#include <stdint.h>
#include "console.h"
#include "emmc.h"
#include "touch.h"

#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem@146aa000 + restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x0c264000UL

#define PSCI_VERSION		0x84000000UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define SMCCC_VERSION		0x80000000UL

#define SDHC			0x007c4000UL	/* hc_mem: standard SDHCI register file */
#define CQ			0x007c5000UL	/* cmdq_mem */
#define R_ARG			0x08
#define R_XFER_CMD		0x0c	/* lo16 transfer mode, hi16 command */
#define R_RESP0			0x10
#define R_DATA			0x20
#define R_PSTATE		0x24
#define R_HCTL			0x28
#define R_CLK			0x2c
#define R_INTSTS		0x30
#define R_INTSTSEN		0x34
#define R_INTSIGEN		0x38
#define R_HCTL2			0x3c
#define R_CAPS			0x40
#define R_CAPS1			0x44
#define R_ADMAERR		0x54
#define R_VER			0xfc
#define R_BLK			0x04	/* lo16 block size, hi16 block count */

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* clang may emit these even with -ffreestanding */
void *memset(void *d, int c, unsigned long n) { char *p = d; while (n--) *p++ = (char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }

static const char *stage;

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void) { isb(); return rsr("cntpct_el0"); }
static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) reset_to_fastboot(void)
{
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- milestone 2: eMMC bring-up, GPT walk, throughput ---- */
static uint32_t sect[128];

static int is_efi_part(const uint32_t *w) { return w[0] == 0x20494645 && w[1] == 0x54524150; }

static int name_is(const uint32_t *e, const char *want)	/* GPT entry name is UTF-16LE at +56 */
{
	const uint8_t *b = (const uint8_t *)e + 56;
	int i = 0;
	for (; want[i]; i++)
		if (b[2 * i] != (uint8_t)want[i] || b[2 * i + 1])
			return 0;
	return b[2 * i] == 0 && b[2 * i + 1] == 0;
}

static void __attribute__((unused)) emmc_tests(void)
{
	stage = "emmc init";
	int rc = emmc_init();
	con_hex2("INIT", "rc=", (uint32_t)rc, 0, 0);
	if (rc)
		return;

	stage = "lba1";
	rc = emmc_read(1, sect);
	con_hex2("LBA1", "rc=", (uint32_t)rc, "w0=", sect[0]);
	if (rc || !is_efi_part(sect)) {
		con_str("RESULT", "no GPT header at LBA1");
		return;
	}
	con_str("RESULT", "EFI PART -> eMMC READ WORKS (400k 1-bit)");
	const uint8_t *h = (const uint8_t *)sect;
	uint32_t ent_lba = h[72] | h[73] << 8 | h[74] << 16 | (uint32_t)h[75] << 24;
	uint32_t n_ent = h[80] | h[81] << 8 | h[82] << 16 | (uint32_t)h[83] << 24;
	uint32_t ent_sz = h[84] | h[85] << 8;
	uint32_t last_lo = h[48] | h[49] << 8 | h[50] << 16 | (uint32_t)h[51] << 24;
	con_hex2("GPT", "entlba=", ent_lba, "n=", n_ent);
	con_hex2("GPT", "entsz=", ent_sz, "lastusable=", last_lo);

	stage = "fast";
	rc = emmc_fast();
	con_hex2("FAST", "rc=", (uint32_t)rc, 0, 0);

	stage = "gpt walk";
	int found = 0;
	if (ent_sz == 128)
		for (uint32_t i = 0; i < n_ent && i < 256 && !found; i += 4) {
			if (emmc_read(ent_lba + i / 4, sect)) {
				con_hex2("GPTRD", "FAIL lba=", ent_lba + i / 4, 0, 0);
				break;
			}
			for (int j = 0; j < 4; j++) {
				const uint32_t *e = sect + j * 32;
				if (name_is(e, "userdata")) {
					con_hex2("USERDATA", "first=", e[8], "last=", e[10]);
					con_hex2("USERDATA", "first_hi=", e[9], "last_hi=", e[11]);
					found = 1;
				}
			}
		}
	if (!found)
		con_str("USERDATA", "not found");

	stage = "throughput";
	uint64_t t = now();
	int bad = 0;
	for (uint32_t i = 0; i < 2048; i++)	/* 1 MiB from LBA 34 on, single-block PIO */
		if (emmc_read(34 + i, sect))
			bad++;
	uint64_t us = (now() - t) * 10 / 192;
	con_hex2("1MiB", "us=", (uint32_t)us, "errs=", (uint32_t)bad);
	stage = "reread";
	rc = emmc_read(1, sect);
	con_str("LBA1@fast", (!rc && is_efi_part(sect)) ? "EFI PART ok" : "FAIL");
}

/* ---- entry points used by start.S ---- */
void secondary_main(uint64_t idx) { (void)idx; for (;;) __asm__ volatile("wfe"); }

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_bpp = 32;
	con_clear(BLUE);
	con_str("PANIC", stage);
	con_hex("SLOT", slot);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("X0", regs[0]);
	con_str("RESET", "to fastboot in 40s");
	delay_ms(40000);
	reset_to_fastboot();
}

void shim_main(uint64_t dtb)
{
	stage = "touch";
	con_clear(BLACK);
	con_str("VIOLET-PROBE-M3G", "touch begin");
	con_hex2("DTB", "lo=", dtb, "magic=", dtb ? __builtin_bswap32(rd32(dtb)) : 0);
	touch_test(0);
	con_str("VIOLET-PROBE-M3G", "end; reset in 10 s");
	delay_ms(10000);
	reset_to_fastboot();
}
