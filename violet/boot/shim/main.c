/*
 * violet XNU boot shim: entered by Qualcomm ABL via `fastboot boot`
 * (EL1, MMU/caches off, x0 = DTB), finds the XNU payload in the "ramdisk"
 * (loader.c, from dipa-studio/ios-sweet, MIT) and jumps to it.
 * start.S, shim.ld, console.c, font.h, fdt.c, loader.c: dipa-studio/ios-sweet (MIT).
 * main.c: violet specific (reset path and pstore handling from our probe shim).
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"

#define IMEM_RESTART_REASON	0x146aa65cUL	/* qcom,msm-imem@146aa000 + restart_reason@65c */
#define RESTART_BOOTLOADER	0x77665500U
#define PSHOLD			0x0c264000UL
#define PSCI_SYSTEM_RESET	0x84000009UL

/* ramoops@a1600000: console zone 4 MiB, then pmsg 4 MiB. xnu's /pram = console zone + 0x10000. */
#define PSTORE_CON		0xa1600000UL
#define PSTORE_CON_SZ		0x400000UL
#define PRAM_OFF		0x10000UL

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define isb() __asm__ volatile("isb" ::: "memory")

static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

/* clang may emit these even with -ffreestanding */
void *memset(void *d, int c, unsigned long n) { char *p = d; while (n--) *p++ = (char)c; return d; }
void *memcpy(void *d, const void *s, unsigned long n) { char *p = d; const char *q = s; while (n--) *p++ = *q++; return d; }

static const char *stage;	/* set at runtime: no absolute pointers in data */

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
	wr32(IMEM_RESTART_REASON, RESTART_BOOTLOADER);	/* not honoured on violet: lands in Android, fine */
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

void secondary_main(uint64_t idx) { (void)idx; for (;;) __asm__ volatile("wfe"); }

void exc_handler(uint64_t slot, uint64_t *regs)
{
	con_clear(BLUE);
	con_str("PANIC", stage);
	con_hex("SLOT", slot);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("X0", regs[0]);
	con_str("RESET", "in 40s");
	delay_ms(40000);
	reset_to_fastboot();
}

/*
 * Called by loader_boot right before the jump.
 *  1. pstore: leave a valid persistent_ram buffer (sig DBGC, start 0, size = whole zone) whose first
 *     bytes are this shim's log; xnu's /pram (ADT, mkadt_violet.py) sits at zone+0x10000 and is zeroed
 *     here, so a kernel panic log shows up in Android as /sys/fs/pstore/console-ramoops-0 (if the
 *     phone warm-resets and DDR survives).
 *  2. screen: black, with a red bar at the bottom (a row xnu's boot console reaches last). Black + red
 *     bar after the jump = kernel never drew anything; anything else = kernel is running.
 */
void pre_jump(void)
{
	stage = "pre_jump";
	con_str("PSTORE", "arming /pram @a1610000");
	volatile uint64_t *z = (volatile uint64_t *)(PSTORE_CON + PRAM_OFF);
	for (uint64_t i = 0; i < (PSTORE_CON_SZ - PRAM_OFF) / 8; i++)
		z[i] = 0;
	volatile uint32_t *h = (volatile uint32_t *)PSTORE_CON;
	h[0] = 0x43474244;			/* "DBGC" */
	h[1] = 0;				/* start */
	h[2] = (uint32_t)(PSTORE_CON_SZ - 12);	/* size == buffer size: whole zone valid */
	con_log = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	con_clear(BLACK);
	for (int y = FB_H - 48; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++)
			con_fb[y * (FB_STRIDE / 4) + x] = RED;
	__asm__ volatile("dsb sy" ::: "memory");
}

void shim_main(uint64_t dtb)
{
	stage = "banner";
	con_clear(GREEN);
	con_str("VIOLET-XNU", "shim alive");
	con_hex("DTB", dtb);
	con_hex("MIDR", rsr("midr_el1"));
	con_hex("MPIDR", rsr("mpidr_el1"));
	con_hex("EL", rsr("CurrentEL") >> 2);
	con_hex("MMFR0", rsr("id_aa64mmfr0_el1"));	/* bits 23:20 == 0 -> 16K granule supported */

	stage = "loader";
	const char *err = loader_boot((uintptr_t)dtb);
	/* loader_boot only returns on error */
	con_skip();
	con_str("ERROR", err);
	con_str("RESET", "in 40s");
	delay_ms(40000);
	reset_to_fastboot();
}
