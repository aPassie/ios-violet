/*
 * violet eMMC (SDHC1, sdhci-msm-v5 @ 0x7c4000): polled bring-up from the state
 * ABL leaves (bus power off, SD clock off), card re-init, PIO single-block reads.
 * Sequence follows Linux sdhci-msm (v5 offsets) and U-Boot msm_sdhci (violet/README.md, "Storage").
 * Nothing is written to user data. CMD6 only touches volatile EXT_CSD fields
 * (BUS_WIDTH, HS_TIMING) and, if needed, clears PARTITION_ACCESS bits [2:0].
 */
#include <stdint.h>
#include "console.h"
#include "emmc.h"

#define HC		0x007c4000UL
#define GCC		0x00100000UL
#define TLMM_SDC1	0x0359a000UL

/* standard SDHCI */
#define BLKSZ		0x04	/* 16 */
#define BLKCNT		0x06	/* 16 */
#define ARG		0x08
#define XFERMODE	0x0c	/* 16 */
#define COMMAND		0x0e	/* 16, write launches */
#define RESP		0x10
#define DATAPORT	0x20
#define PSTATE		0x24
#define HCTL1		0x28	/* 8 */
#define POWER		0x29	/* 8 */
#define CLOCK		0x2c	/* 16 */
#define TOUT		0x2e	/* 8 */
#define SWRST		0x2f	/* 8 */
#define INTSTS		0x30
#define INTEN		0x34
#define INTSIG		0x38
#define HCTL2		0x3e	/* 16 */
/* msm v5 vendor (inside hc_mem) */
#define DLL_CONFIG	0x200
#define VENDOR_SPEC	0x20c
#define PWRCTL_STATUS	0x240
#define PWRCTL_MASK	0x244
#define PWRCTL_CLEAR	0x248
#define PWRCTL_CTL	0x24c
#define VENDOR_SPEC3	0x250
#define VENDOR_FUNC4	0x260
#define MCI_VERSION	0x318
/* GCC SDCC1 */
#define SDCC1_BCR	0x12000
#define SDCC1_APPS_CBCR	0x12004
#define SDCC1_AHB_CBCR	0x12008
#define SDCC1_ICE_CBCR	0x1200c
#define SDCC1_RCGR	0x12028
#define SDCC1_CFG	0x1202c
#define SDCC1_M		0x12030
#define SDCC1_N		0x12034
#define SDCC1_D		0x12038
#define GPLL_VOTE	0x52000

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
static inline uint8_t r8(uintptr_t o) { return *(volatile uint8_t *)(HC + o); }
static inline uint16_t r16(uintptr_t o) { return *(volatile uint16_t *)(HC + o); }
static inline uint32_t r32(uintptr_t o) { return *(volatile uint32_t *)(HC + o); }
static inline void w8(uintptr_t o, uint8_t v) { *(volatile uint8_t *)(HC + o) = v; }
static inline void w16(uintptr_t o, uint16_t v) { *(volatile uint16_t *)(HC + o) = v; }
static inline void w32(uintptr_t o, uint32_t v) { *(volatile uint32_t *)(HC + o) = v; }
static inline uint32_t g32(uintptr_t o) { return *(volatile uint32_t *)(GCC + o); }
static inline void gw32(uintptr_t o, uint32_t v) { *(volatile uint32_t *)(GCC + o) = v; }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static uint64_t now(void) { __asm__ volatile("isb" ::: "memory"); return rsr("cntpct_el0"); }
static uint64_t ticks_ms(unsigned ms) { return rsr("cntfrq_el0") / 1000 * ms; }
static void udelay(unsigned us)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000000 * us;
	while (now() < end)
		;
}

static uint32_t rca = 1;
uint32_t emmc_ext_csd[128];

/* ---- PWRCTL handshake ---- */
static uint32_t pwr_wait(uint32_t want, unsigned ms)
{
	uint64_t end = now() + ticks_ms(ms);
	uint32_t s;
	do {
		s = r32(PWRCTL_STATUS) & 0xf;
		if (s & want)
			return s;
	} while (now() < end);
	return s | 0x80000000u;		/* timeout flag */
}

static void pwr_clear_ack(uint32_t s)
{
	s &= 0xf;
	if (!s)
		return;
	for (int i = 0; i < 10; i++) {
		w32(PWRCTL_CLEAR, s);
		dsb();
		udelay(10);
		if (!(r32(PWRCTL_STATUS) & s))
			break;
	}
	uint32_t ctl = 0;
	if (s & 0x3)
		ctl |= 0x1;	/* BUS_SUCCESS */
	if (s & 0xc)
		ctl |= 0x4;	/* IO_SUCCESS */
	w32(PWRCTL_CTL, ctl);
	dsb();
}

/* ---- GCC SDCC1 clock ---- */
static uint32_t rcg_rate_khz(void)
{
	static const uint32_t parent_khz[8] = { 19200, 600000, 384000, 0, 0, 0, 300000, 0 };
	uint32_t cfg = g32(SDCC1_CFG);
	uint32_t src = (cfg >> 8) & 7, hid = cfg & 0x1f;
	uint64_t r = parent_khz[src];
	if (hid)
		r = r * 2 / (hid + 1);
	if ((cfg >> 12) & 3) {
		uint32_t m = g32(SDCC1_M) & 0xff;
		uint32_t n = ((~g32(SDCC1_N)) & 0xff) + m;
		if (n)
			r = r * m / n;
	}
	return (uint32_t)r;
}

static int rcg_set(uint32_t cfg, uint32_t m, uint32_t n, uint32_t d, int mnd)
{
	if (mnd) {
		gw32(SDCC1_M, m);
		gw32(SDCC1_N, n);
		gw32(SDCC1_D, d);
	}
	gw32(SDCC1_CFG, cfg);
	dsb();
	gw32(SDCC1_RCGR, g32(SDCC1_RCGR) | 1);	/* UPDATE */
	dsb();
	uint64_t end = now() + ticks_ms(5);
	while (g32(SDCC1_RCGR) & 1)
		if (now() > end)
			return -1;
	return 0;
}

static int sdclk_on(void)
{
	w16(CLOCK, 0);
	w16(CLOCK, 0x0001);			/* INT_CLK_EN, divider 0 (MSM: GCC sets the rate) */
	uint64_t end = now() + ticks_ms(20);
	while (!(r16(CLOCK) & 0x0002))
		if (now() > end)
			return -1;
	w16(CLOCK, r16(CLOCK) | 0x0004);	/* SD_CLK_EN */
	udelay(2000);
	return 0;
}

static void sdclk_off(void) { w16(CLOCK, r16(CLOCK) & ~0x0004); udelay(10); }

/* ---- commands ---- */
static void reset_cmd_data(void)
{
	for (uint8_t b = 2; b <= 4; b += 2) {
		w8(SWRST, b);
		uint64_t end = now() + ticks_ms(100);
		while ((r8(SWRST) & b) && now() < end)
			;
	}
}

struct cmdres { uint32_t sts; uint32_t resp[4]; };

/* flags: low byte of COMMAND. data: 0 none, 1 single-block read into buf. busy: R1b. */
static int cmd(uint32_t idx, uint32_t flags, uint32_t arg, int data, uint32_t *buf, struct cmdres *cr)
{
	uint32_t inhibit = (data || (flags & 3) == 3) ? 3 : 1;
	uint64_t end = now() + ticks_ms(200);
	while (r32(PSTATE) & inhibit)
		if (now() > end) {
			cr->sts = 0xdead0001;
			reset_cmd_data();
			return -1;
		}
	w32(INTSTS, 0xffffffff);
	if (data) {
		w8(TOUT, 0x0e);
		w16(BLKSZ, 0x0200);
		w16(BLKCNT, 1);
	}
	w32(ARG, arg);
	w16(XFERMODE, data ? 0x0012 : 0);
	w16(COMMAND, (uint16_t)((idx << 8) | flags));

	end = now() + ticks_ms(250);
	uint32_t s;
	while (!((s = r32(INTSTS)) & 0x8001))
		if (now() > end) {
			cr->sts = 0xdead0002;
			reset_cmd_data();
			return -1;
		}
	cr->sts = s;
	if (s & 0x8000) {
		w32(INTSTS, s);
		reset_cmd_data();
		return -1;
	}
	if ((flags & 3) == 1) {		/* R2: 136-bit, CRC stripped, shift by 8 */
		for (int i = 0; i < 4; i++) {
			uint32_t hi = r32(RESP + (3 - i) * 4) << 8;
			uint32_t lo = i < 3 ? r8(RESP + (3 - i) * 4 - 1) : 0;
			cr->resp[i] = hi | lo;
		}
	} else {
		cr->resp[0] = r32(RESP);
	}
	w32(INTSTS, 0x1);

	if ((flags & 3) == 3) {		/* R1b: wait DAT0 high, ignore data timeout */
		end = now() + ticks_ms(1000);
		while (!(r32(PSTATE) & (1u << 20)))
			if (now() > end) {
				cr->sts |= 0x40000000;
				break;
			}
		w32(INTSTS, 0xffffffff);
	}
	if (data) {
		end = now() + ticks_ms(500);
		while (!((s = r32(INTSTS)) & 0x8020))
			if (now() > end) {
				cr->sts = 0xdead0003;
				reset_cmd_data();
				return -1;
			}
		if (s & 0x8000) {
			cr->sts = s;
			w32(INTSTS, s);
			reset_cmd_data();
			return -1;
		}
		w32(INTSTS, 0x20);
		for (int i = 0; i < 128; i++)
			buf[i] = r32(DATAPORT);
		end = now() + ticks_ms(500);
		while (!((s = r32(INTSTS)) & 0x8002))
			if (now() > end) {
				cr->sts = 0xdead0004;
				reset_cmd_data();
				return -1;
			}
		cr->sts |= s;
		w32(INTSTS, 0xffffffff);
		if (s & 0x8000) {
			reset_cmd_data();
			return -1;
		}
	}
	udelay(100);
	return 0;
}

int emmc_read(uint32_t lba, uint32_t *buf)
{
	struct cmdres cr;
	return cmd(17, 0x3a, lba, 1, buf, &cr);
}

/* ---- bring-up ---- */
#define LOG(label, a, va, b, vb) con_hex2(label, a, va, b, vb)

int emmc_init(void)
{
	struct cmdres cr = { 0 };

	/* 0. read-only dump */
	LOG("V-DLL", "cfg=", r32(DLL_CONFIG), "vspec=", r32(VENDOR_SPEC));
	LOG("V-PWR", "sts=", r32(PWRCTL_STATUS), "msk=", r32(PWRCTL_MASK));
	LOG("V-PWR", "ctl=", r32(PWRCTL_CTL), "vs3=", r32(VENDOR_SPEC3));
	LOG("V-MCI", "ver=", r32(MCI_VERSION), "fn4=", r32(VENDOR_FUNC4));
	LOG("G-CBCR", "apps=", g32(SDCC1_APPS_CBCR), "ahb=", g32(SDCC1_AHB_CBCR));
	LOG("G-ICE", "cbcr=", g32(SDCC1_ICE_CBCR), "gpll=", g32(GPLL_VOTE));
	LOG("G-RCG", "cmd=", g32(SDCC1_RCGR), "cfg=", g32(SDCC1_CFG));
	LOG("G-MND", "m=", g32(SDCC1_M), "n=", g32(SDCC1_N));
	LOG("G-RATE", "khz=", rcg_rate_khz(), "tlmm=", *(volatile uint32_t *)TLMM_SDC1);

	/* A. stale PWRCTL */
	uint32_t s = r32(PWRCTL_STATUS) & 0xf;
	pwr_clear_ack(s);
	w32(PWRCTL_MASK, 0xf);
	/* B. vendor spec / DLL */
	w32(VENDOR_SPEC, 0x00000a9c);
	w32(VENDOR_SPEC, r32(VENDOR_SPEC) | (1u << 15) | (1u << 16));
	w32(DLL_CONFIG, r32(DLL_CONFIG) | (1u << 30));
	w32(DLL_CONFIG, r32(DLL_CONFIG) | (1u << 29));
	w32(VENDOR_SPEC3, r32(VENDOR_SPEC3) & ~((1u << 3) | (1u << 10)));
	w16(HCTL2, 0);
	dsb();
	LOG("A/B", "stale=", s, "vspec=", r32(VENDOR_SPEC));

	/* C. bus power */
	w8(POWER, 0x0d);
	dsb();
	s = pwr_wait(0x2, 100);
	pwr_clear_ack(s);
	w32(VENDOR_SPEC, r32(VENDOR_SPEC) | (1u << 15) | (1u << 16));
	udelay(10000);
	LOG("C-PWR", "sts=", s, "pwr=", r8(POWER));

	/* D. 1.8 V signalling */
	w16(HCTL2, r16(HCTL2) | 0x0008);
	dsb();
	s = pwr_wait(0x4, 100);
	pwr_clear_ack(s);
	udelay(5000);
	LOG("D-1V8", "sts=", s, "hctl2=", r16(HCTL2));

	/* E. status plumbing */
	w32(INTSIG, 0);
	w32(INTEN, 0x03bf003b);
	w32(INTSTS, 0xffffffff);
	w8(TOUT, 0x0e);
	w8(HCTL1, 0x00);

	/* F. 400 kHz identification clock from XO via GCC RCG (SD clock gated while switching) */
	sdclk_off();
	int rc = rcg_set(0x2017, 1, 0xfc, 0xfb, 1);
	LOG("F-CLK", "rcg=", (uint32_t)rc, "khz=", rcg_rate_khz());
	if (sdclk_on()) {
		LOG("F-CLK", "INT_CLK not stable", 0, 0, 0);
		return -1;
	}

	/* 3. card init */
	cmd(0, 0x00, 0, 0, 0, &cr);
	udelay(10000);
	LOG("CMD0", "st=", cr.sts, "ps=", r32(PSTATE));
	uint32_t ocr = 0;
	int tries;
	for (tries = 0; tries < 100; tries++) {
		if (cmd(1, 0x02, 0x40ff8080, 0, 0, &cr) == 0) {
			ocr = cr.resp[0];
			if (ocr & 0x80000000u)
				break;
		}
		udelay(10000);
	}
	LOG("CMD1", "ocr=", ocr, "tries=", tries);
	if (!(ocr & 0x80000000u)) {
		LOG("CMD1", "st=", cr.sts, "ps=", r32(PSTATE));
		return -2;
	}
	if (cmd(2, 0x09, 0, 0, 0, &cr))
		{ LOG("CMD2", "FAIL st=", cr.sts, 0, 0); return -3; }
	LOG("CID", "w0=", cr.resp[0], "w1=", cr.resp[1]);
	if (cmd(3, 0x1a, rca << 16, 0, 0, &cr))
		{ LOG("CMD3", "FAIL st=", cr.sts, 0, 0); return -4; }
	if (cmd(7, 0x1b, rca << 16, 0, 0, &cr))
		{ LOG("CMD7", "FAIL st=", cr.sts, 0, 0); return -5; }
	LOG("CMD3/7", "r1=", cr.resp[0], "st=", cr.sts);
	if (cmd(8, 0x3a, 0, 1, emmc_ext_csd, &cr))
		{ LOG("CMD8", "FAIL st=", cr.sts, 0, 0); return -6; }
	const uint8_t *ec = (const uint8_t *)emmc_ext_csd;
	uint32_t sec = ec[212] | ec[213] << 8 | ec[214] << 16 | (uint32_t)ec[215] << 24;
	LOG("EXTCSD", "rev=", ec[192], "sectors=", sec);
	LOG("EXTCSD", "pcfg179=", ec[179], "bw183=", ec[183]);
	LOG("EXTCSD", "hs185=", ec[185], "type196=", ec[196]);
	if (ec[179] & 7) {	/* make sure CMD17 sees the user area: CLEAR_BITS access mode */
		rc = cmd(6, 0x1b, (2u << 24) | (179u << 16) | (0x07u << 8), 0, 0, &cr);
		LOG("CMD6-PA", "rc=", (uint32_t)rc, "st=", cr.sts);
	}
	return 0;
}

/* After a successful emmc_init(): 8-bit bus, 25 MHz legacy timing. */
int emmc_fast(void)
{
	struct cmdres cr = { 0 };
	int rc = cmd(6, 0x1b, 0x03b70200, 0, 0, &cr);		/* BUS_WIDTH = 8 */
	cmd(13, 0x1a, rca << 16, 0, 0, &cr);
	LOG("CMD6-BW", "rc=", (uint32_t)rc, "r1=", cr.resp[0]);
	if (rc || ((cr.resp[0] >> 9) & 0xf) != 4 || (cr.resp[0] & 0x80))
		return -1;
	w8(HCTL1, (r8(HCTL1) & ~0x02) | 0x20);
	if (!(g32(GPLL_VOTE) & 1)) {
		LOG("FAST", "GPLL0 not voted, stay 400k", 0, 0, 0);
		return -2;
	}
	sdclk_off();
	rc = rcg_set(0x260b, 1, 0xfe, 0xfd, 1);		/* GPLL0_AUX2 300 MHz /6 *1/2 = 25 MHz */
	sdclk_on();
	cmd(13, 0x1a, rca << 16, 0, 0, &cr);
	LOG("FAST", "khz=", rcg_rate_khz(), "r1=", cr.resp[0]);
	return rc;
}
