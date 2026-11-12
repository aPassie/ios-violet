/*
 * violet touch probe (shim milestone 3): Novatek NT36672A TDDI on QUPv3 wrapper 0
 * SE1 (I2C, 0x884000), polled GENI FIFO mode, MMU off.
 * Register map: QUPv3 wrapper0 SE1 I2C, GCC/TLMM SM6150, Novatek nt36xxx (violet/README.md, "Touch").
 * Every register block is logged before the next one is touched; SE registers are
 * only accessed once the wrapper AHB clocks and the SE1 clock report CLK_OFF = 0
 * (an access to an unclocked block stalls the CPU: topaz 0087).
 * The IC runs firmware from its own flash and is alive because ABL left the
 * display on; we never reset the panel (GPIO 91) or touch GPIO 88.
 */
#include <stdint.h>
#include "console.h"
#include "touch.h"

#define GCC		0x00100000UL
#define VOTE		(GCC + 0x5200c)
#define CBCR_M_AHB	(GCC + 0x17004)
#define CBCR_S_AHB	(GCC + 0x17008)
#define CBCR_CORE	(GCC + 0x1700c)
#define CBCR_CORE2X	(GCC + 0x17014)
#define CBCR_SE1	(GCC + 0x17274)
#define RCG_CMD		(GCC + 0x17278)
#define RCG_CFG		(GCC + 0x1727c)
#define RCG_DFS		(GCC + 0x1728c)
#define RCG_PERF0	(GCC + 0x17294)
#define VOTE_SE1	0x00000bc0u

#define TLMM_CFG(n)	(0x03500000UL + 0x1000UL * (n))
#define TLMM_IO(n)	(TLMM_CFG(n) + 4)

#define SE		0x00884000UL
#define FORCE_DEF	0x020
#define OUT_CTRL	0x024
#define CGC_CTRL	0x028
#define GENI_STATUS	0x040
#define SER_M_CLK	0x048
#define FIFO_DIS_RO	0x064
#define FW_REV_RO	0x068
#define CLK_SEL		0x07c
#define BYTE_GRAN	0x254
#define DMA_MODE_EN	0x258
#define TX_PACK0	0x260
#define TX_PACK1	0x264
#define TX_LEN		0x26c
#define RX_LEN		0x270
#define SCL_CNT		0x278
#define RX_PACK0	0x284
#define RX_PACK1	0x288
#define M_CMD0		0x600
#define M_CMD_CTRL	0x604
#define M_IRQ_ST	0x610
#define M_IRQ_EN	0x614
#define M_IRQ_CLR	0x618
#define S_IRQ_EN	0x644
#define S_IRQ_CLR	0x648
#define TX_FIFO		0x700
#define RX_FIFO		0x780
#define TX_FIFO_ST	0x800
#define RX_FIFO_ST	0x804
#define TX_WM		0x80c
#define RX_WM		0x810
#define RX_RFR_WM	0x814
#define GSI_EV_EN	0xe18
#define SE_IRQ_EN	0xe1c
#define HW_PARAM0	0xe24
#define HW_PARAM1	0xe28
#define DMA_GEN_CFG	0xe30

#define IRQ_DONE	(1u << 0)
#define IRQ_CANCEL	(1u << 4)
#define IRQ_ABORT	(1u << 5)
#define IRQ_NACK	(1u << 10)
#define IRQ_TX_WM	(1u << 30)
#define IRQ_ERRORS	(0x0eu | (0x1fu << 9) | (3u << 24) | (3u << 28))
#define IRQ_ENABLE	(IRQ_DONE | 0x3eu | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13) | \
			 (3u << 24) | (3u << 28) | (1u << 26) | (1u << 27) | IRQ_TX_WM)

#define NVT_FW		0x01	/* events, page select, FW info */
#define NVT_HW		0x62	/* reset / idle only */

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; __asm__ volatile("dsb sy" ::: "memory"); }
static inline uint32_t se_rd(uint32_t o) { return rd(SE + o); }
static inline void se_wr(uint32_t o, uint32_t v) { wr(SE + o, v); }

static uint64_t now(void) { __asm__ volatile("isb" ::: "memory"); return rsr("cntpct_el0"); }
static uint64_t ms_ticks(unsigned ms) { return rsr("cntfrq_el0") / 1000 * ms; }
static void udelay(unsigned us) { uint64_t e = now() + rsr("cntfrq_el0") / 1000000 * us; while (now() < e) ; }

/* ---------------- clocks, pins, SE init ---------------- */

static int wait_clk_on(uintptr_t cbcr)
{
	/* HW_CTL (bit 1): hardware clock gating; CLK_OFF then reads 1 whenever the bus
	 * is idle. Linux skips the halt check for such branches (branch_in_hwcg_mode). */
	if (rd(cbcr) & 2)
		return 0;
	uint64_t end = now() + ms_ticks(5);
	while (rd(cbcr) & 0x80000000u)
		if (now() > end)
			return -1;
	return 0;
}

static int touch_bringup(void)
{
	/* checklist 1-3: what ABL left */
	con_hex2("T-VOTE", "v=", rd(VOTE), "se1=", rd(CBCR_SE1));
	con_hex2("T-CBCR", "mahb=", rd(CBCR_M_AHB), "sahb=", rd(CBCR_S_AHB));
	con_hex2("T-CBCR", "core=", rd(CBCR_CORE), "c2x=", rd(CBCR_CORE2X));
	con_hex2("T-RCG", "cmd=", rd(RCG_CMD), "cfg=", rd(RCG_CFG));
	con_hex2("T-DFS", "en=", rd(RCG_DFS), "perf0=", rd(RCG_PERF0));
	con_hex2("T-PIN", "g4=", rd(TLMM_CFG(4)), "g5=", rd(TLMM_CFG(5)));
	con_hex2("T-INT", "cfg89=", rd(TLMM_CFG(89)), "io89=", rd(TLMM_IO(89)));

	/* A. clocks: OR the vote, wait for the five branches */
	wr(VOTE, rd(VOTE) | VOTE_SE1);
	int bad = 0;
	if (wait_clk_on(CBCR_M_AHB)) bad |= 1;
	if (wait_clk_on(CBCR_S_AHB)) bad |= 2;
	if (wait_clk_on(CBCR_CORE)) bad |= 4;
	if (wait_clk_on(CBCR_CORE2X)) bad |= 8;
	if (wait_clk_on(CBCR_SE1)) bad |= 16;
	con_hex2("T-CLK", "bad=", bad, "se1=", rd(CBCR_SE1));
	if (bad)
		return -1;
	if (rd(RCG_DFS) & 1) {
		if ((rd(RCG_PERF0) & 0x371f) != 1) {
			con_str("T-CLK", "DFS perf0 is not XO/1: stop");
			return -2;
		}
	} else {
		wr(RCG_CFG, (rd(RCG_CFG) & ~(0x371fu | (1u << 20))) | 1);
		wr(RCG_CMD, rd(RCG_CMD) | 1);
		uint64_t end = now() + ms_ticks(5);
		while (rd(RCG_CMD) & 1)
			if (now() > end) {
				con_str("T-CLK", "RCG update stuck");
				return -3;
			}
	}
	con_hex2("T-RCG", "cfg=", rd(RCG_CFG), "dfs=", rd(RCG_DFS));

	/* B. pins: qup01 function, 2 mA, no pull (external pull-ups) */
	wr(TLMM_CFG(4), 0x4);
	wr(TLMM_CFG(5), 0x4);

	/* C. SE identity (safe now: clocks on) */
	uint32_t fw = se_rd(FW_REV_RO), dis = se_rd(FIFO_DIS_RO);
	uint32_t hp0 = se_rd(HW_PARAM0), hp1 = se_rd(HW_PARAM1);
	con_hex2("T-SE", "fwrev=", fw, "fifodis=", dis);
	con_hex2("T-SE", "hp0=", hp0, "hp1=", hp1);
	con_hex2("T-SE", "status=", se_rd(GENI_STATUS), "rxst=", se_rd(RX_FIFO_ST));
	if (((fw >> 8) & 0xff) != 3) {
		con_str("T-SE", "protocol is not I2C (3): stop");
		return -4;
	}
	if (dis & 1) {
		con_str("T-SE", "FIFO disabled (GSI only): need bit-bang plan B");
		return -5;
	}
	uint32_t rxdepth = (hp1 >> 16) & 0x3f;

	/* D. init as geni_se_init + sweet's polled variant */
	se_wr(GSI_EV_EN, 0);
	se_wr(SE_IRQ_EN, 0);
	se_wr(S_IRQ_EN, 0);
	se_wr(M_IRQ_CLR, 0xffffffff);
	se_wr(S_IRQ_CLR, 0xffffffff);
	se_wr(DMA_MODE_EN, 0);
	se_wr(CGC_CTRL, se_rd(CGC_CTRL) | 0x7f);
	se_wr(DMA_GEN_CFG, se_rd(DMA_GEN_CFG) | 0x0f);
	se_wr(OUT_CTRL, 0x7f);
	se_wr(FORCE_DEF, 1);
	se_wr(CLK_SEL, 0);
	se_wr(SER_M_CLK, 0x71);			/* 19.2 MHz / 7 -> 100 kHz */
	se_wr(SCL_CNT, 0x00a02c1a);
	se_wr(BYTE_GRAN, 0);
	se_wr(TX_PACK0, 0x0007f8fe);
	se_wr(TX_PACK1, 0x000ffefe);
	se_wr(RX_PACK0, 0x0007f8fe);
	se_wr(RX_PACK1, 0x000ffefe);
	se_wr(TX_WM, 0);
	se_wr(RX_WM, 0);
	se_wr(RX_RFR_WM, rxdepth ? rxdepth - 1 : 1);
	se_wr(M_IRQ_EN, IRQ_ENABLE);
	con_hex2("T-INIT", "pk0=", se_rd(TX_PACK0), "rxpk1=", se_rd(RX_PACK1));
	con_hex2("T-INIT", "mclk=", se_rd(SER_M_CLK), "scl=", se_rd(SCL_CNT));
	return 0;
}


/* ---------------- one I2C message ---------------- */

static uint32_t last_err;

/*
 * Bus timing variants (m3g). gap_us != 0: wait gap_us between the index write's DONE and
 * the READ, and 1 ms after every transaction that ends in a STOP. stop_index: the index
 * write ends in a normal STOP (no STOP_STRETCH) and the read follows 500 us later.
 * stop_wait = 0 suppresses the 1 ms post-STOP wait (used where the caller samples INT
 * right after the STOP).
 */
static uint32_t gap_us;
static int stop_index;
static int stop_wait = 1;

/* error counters by kind */
static struct { uint32_t nack, to, sh, oth, retry, drain; } E;

#define MAX_RETRY	5

static void set_timing(int t)	/* T0 tight, T1 gap 200 us, T2 index with STOP + 500 us */
{
	gap_us = t == 0 ? 0 : t == 1 ? 200 : 500;
	stop_index = t == 2;
}

static void se_recover(void)
{
	se_wr(M_CMD_CTRL, 4);			/* cancel */
	uint64_t end = now() + ms_ticks(1);
	while (!(se_rd(M_IRQ_ST) & IRQ_CANCEL) && now() < end)
		;
	if (!(se_rd(M_IRQ_ST) & IRQ_CANCEL)) {
		se_wr(M_CMD_CTRL, 2);		/* abort */
		end = now() + ms_ticks(1);
		while (!(se_rd(M_IRQ_ST) & IRQ_ABORT) && now() < end)
			;
	}
	end = now() + ms_ticks(5);
	while ((se_rd(GENI_STATUS) & 1) && now() < end)
		;
	while (se_rd(RX_FIFO_ST) & 0x1ffffff)
		(void)se_rd(RX_FIFO);
	se_wr(M_IRQ_CLR, 0xffffffff);
}

static int wait_idle(void)
{
	uint64_t end = now() + ms_ticks(5);
	while (se_rd(GENI_STATUS) & 1)
		if (now() > end)
			return -1;
	return 0;
}

static void drain_rx(void)
{
	uint32_t n = 0;
	while ((se_rd(RX_FIFO_ST) & 0x1ffffff) && n < 64) {
		(void)se_rd(RX_FIFO);
		n++;
	}
	E.drain += n;
}

/* one attempt; -5 = NACK (retryable) */
static int i2c_write1(uint32_t addr, const uint8_t *b, uint32_t n, int stretch, int stopwait)
{
	if (n == 0 || n > 4)
		return -1;
	if (wait_idle()) {
		E.oth++;
		return -1;
	}
	drain_rx();
	se_wr(M_IRQ_CLR, 0xffffffff);
	se_wr(TX_WM, 1);
	se_wr(TX_LEN, n);
	se_wr(M_CMD0, (1u << 27) | (addr << 9) | (stretch ? 4u : 0u));
	uint32_t word = 0;
	for (uint32_t i = 0; i < n; i++)
		word |= (uint32_t)b[i] << (8 * i);
	int sent = 0;
	uint64_t end = now() + ms_ticks(10);
	for (;;) {
		uint32_t st = se_rd(M_IRQ_ST);
		if (st & IRQ_ERRORS) {
			last_err = st;
			se_recover();
			if (st & IRQ_NACK) {
				E.nack++;
				return -5;
			}
			E.oth++;
			return -2;
		}
		if (!sent && (st & IRQ_TX_WM)) {
			se_wr(TX_FIFO, word);
			se_wr(TX_WM, 0);
			se_wr(M_IRQ_CLR, IRQ_TX_WM);
			sent = 1;
		}
		if (st & IRQ_DONE) {
			se_wr(M_IRQ_CLR, st);
			if (!sent) {
				E.sh++;
				return -3;
			}
			if (!stretch && stopwait && stop_wait && gap_us)
				udelay(1000);	/* after a STOP */
			return 0;
		}
		if (now() > end) {
			last_err = st | 0x80000000u;
			se_recover();
			E.to++;
			return -4;
		}
	}
}

static int i2c_write_r(uint32_t addr, const uint8_t *b, uint32_t n, int stretch, int stopwait)
{
	int rc = 0;
	for (int i = 0; i <= MAX_RETRY; i++) {
		rc = i2c_write1(addr, b, n, stretch, stopwait);
		if (rc != -5)
			break;
		E.retry++;
		udelay(1000);
	}
	return rc;
}

static int i2c_write(uint32_t addr, const uint8_t *b, uint32_t n, int stretch)
{
	return i2c_write_r(addr, b, n, stretch, 1);
}

/* -5 = NACK (the caller retries the whole index+read pair) */
static int i2c_read(uint32_t addr, uint8_t *buf, uint32_t n)
{
	if (wait_idle()) {
		E.oth++;
		return -1;
	}
	drain_rx();
	se_wr(M_IRQ_CLR, 0xffffffff);
	se_wr(RX_LEN, n);
	se_wr(M_CMD0, (2u << 27) | (addr << 9));
	uint32_t got = 0;
	int done = 0;
	uint64_t end = now() + ms_ticks(20);
	for (;;) {
		uint32_t st = se_rd(M_IRQ_ST);
		if (st & IRQ_ERRORS) {
			last_err = st;
			se_recover();
			if (st & IRQ_NACK) {
				E.nack++;
				return -5;
			}
			E.oth++;
			return -2;
		}
		uint32_t words = se_rd(RX_FIFO_ST) & 0x1ffffff;
		while (words--) {
			uint32_t w = se_rd(RX_FIFO);
			for (int k = 0; k < 4 && got < n; k++)
				buf[got++] = (uint8_t)(w >> (8 * k));
		}
		if (st & IRQ_DONE)
			done = 1;
		if (done && !(se_rd(RX_FIFO_ST) & 0x1ffffff)) {
			se_wr(M_IRQ_CLR, 0xffffffff);
			if (got != n) {
				E.sh++;
				return -3;
			}
			if (stop_wait && gap_us)
				udelay(1000);	/* after a STOP */
			return 0;
		}
		if (now() > end) {
			last_err = st | 0x80000000u;
			se_recover();
			E.to++;
			return -4;
		}
	}
}

/*
 * register read: index write + read. Normal: index write held with STOP_STRETCH
 * (repeated start), gap_us between its DONE and the READ. stop_index (T2): index
 * write with a normal STOP, 500 us, then the read.
 */
static int rd_at(uint32_t addr, uint8_t reg, uint8_t *buf, uint32_t n)
{
	int rc = 0;
	for (int i = 0; i <= MAX_RETRY; i++) {
		rc = i2c_write_r(addr, &reg, 1, !stop_index, 0);
		if (!rc) {
			uint32_t g = stop_index ? 500 : gap_us;
			if (g)
				udelay(g);
			rc = i2c_read(addr, buf, n);
		}
		if (rc != -5)
			break;
		E.retry++;
		udelay(1000);
	}
	return rc;
}

static int wr2(uint32_t addr, uint8_t a, uint8_t b)
{
	uint8_t v[2] = { a, b };
	return i2c_write(addr, v, 2, 0);
}

static int page(uint8_t hi, uint8_t mid)
{
	uint8_t v[3] = { 0xff, hi, mid };
	return i2c_write(NVT_FW, v, 3, 0);
}

/* ---------------- drawing feedback / logging helpers ---------------- */

static void dot(uint32_t x, uint32_t y, uint32_t c)
{
	for (int dy = -12; dy <= 12; dy++)
		for (int dx = -12; dx <= 12; dx++) {
			int px = (int)x + dx, py = (int)y + dy;
			if (px < 0 || py < 0 || px >= FB_W || py >= FB_H)
				continue;
			con_fb[py * (FB_STRIDE / 4) + px] = c;
		}
}

#define GREENBG		0xff00c000u	/* bright green = touch now */

static void screen(uint32_t bg, const char *label, const char *msg)
{
	con_clear(bg);		/* fill + reset console row + text background */
	con_str(label, msg);
}

/* every result line twice: pstore flips the same bits each boot */
static void L(const char *l, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	con_hex2(l, a, va, b, vb);
	con_hex2(l, a, va, b, vb);
}

static uint32_t be32(const uint8_t *b)	/* bytes in wire order: 17 E8 12 24 -> 17E81224 */
{
	return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static uint32_t us_of(uint64_t ticks) { return (uint32_t)(ticks * 1000000ULL / rsr("cntfrq_el0")); }
static uint32_t ms_of(uint64_t ticks) { return (uint32_t)(ticks * 1000ULL / rsr("cntfrq_el0")); }

static uint64_t t_start;
static void stamp(const char *what) { con_hex2("TIME", what, ms_of(now() - t_start), 0, 0); }

static void dump65(const char *label, const uint8_t *b)	/* 65 bytes, wire order */
{
	for (int i = 0; i < 64; i += 8)
		L(label, "", be32(b + i), "", be32(b + i + 4));
	L(label, "b64=", b[64], 0, 0);
}

/* ---------------- INT watch ---------------- */

#define INT_IO		0x03559004UL	/* GPIO 89 IN_OUT, bit0 = level */
#define INT_CFG		0x03559008UL	/* TLMM_GPIO_INTR_CFG(89) */
#define INT_STS		0x0355900cUL	/* TLMM_GPIO_INTR_STATUS(89) */
#define NFR		12

static const uint8_t EXP8[8] = { 0x17, 0xe8, 0x12, 0x24, 0x04, 0x38, 0x09, 0x24 };

struct watch {
	uint32_t idle, up, dn, reads, rderr, valid, invalid, contact, minw, maxw, latch;
};
static struct watch W[4];		/* P1, P3, P4, P5 */
static uint8_t fr[NFR][65];
static uint32_t fr_t[NFR], fr_lv[NFR];

/* 0 = invalid frame, 1 = valid, no contact, 2 = valid with >= 1 record.
 * Draws a dark dot per slot that decodes as a touch (id 1..10, status 1/2, in range). */
static int frame_decode(const uint8_t *b)
{
	int bad = 0, recs = 0;
	for (int i = 0; i < 10; i++) {
		const uint8_t *e = b + 6 * i;
		if (e[0] == 0xff && e[1] == 0xff && e[2] == 0xff && e[3] == 0xff && e[4] == 0xff && e[5] == 0xff)
			continue;
		uint32_t id = e[0] >> 3, st = e[0] & 7;
		uint32_t x = (uint32_t)e[1] << 4 | e[3] >> 4;
		uint32_t y = (uint32_t)e[2] << 4 | (e[3] & 0x0f);
		if (id < 1 || id > 10 || st < 1 || st > 3 || x >= FB_W || y >= FB_H) {
			bad = 1;
			continue;
		}
		recs++;
		if (st == 1 || st == 2)
			dot(x, y, 0xff000000u);
	}
	return bad ? 0 : recs ? 2 : 1;
}

static void int_watch(const char *lbl, unsigned ms, int mode, struct watch *w)
{
	w->idle = w->up = w->dn = w->reads = w->rderr = w->valid = w->invalid = w->contact = w->maxw = w->latch = 0;
	w->minw = 0xffffffffu;
	uint32_t nfr = 0, nts = 0, ts[8], exc_t = 0;
	int inexc = 0;
	uint8_t b[68];

	wr(INT_CFG, 0x1e);	/* RAW_STATUS_EN | both edges | polarity */
	wr(INT_STS, 0);		/* clear the latch */
	uint32_t idle = rd(INT_IO) & 1, last = idle;
	w->idle = idle;
	uint64_t t0 = now(), end = t0 + ms_ticks(ms);
	while (now() < end) {
		uint32_t in = rd(INT_IO) & 1;
		if (in == last)
			continue;
		uint32_t tu = us_of(now() - t0);
		if (in) w->up++; else w->dn++;
		if (nts < 8)
			ts[nts++] = tu;
		if (in != idle) {
			exc_t = tu;
			inexc = 1;
			if (mode == 1) {
				/* one event read per excursion, T1 timing */
				uint32_t g0 = gap_us; int s0 = stop_index, w0 = stop_wait;
				gap_us = 200; stop_index = 0; stop_wait = 0;
				int rc = rd_at(NVT_FW, 0x00, b, 65);
				uint32_t after = rd(INT_IO) & 1;
				gap_us = g0; stop_index = s0; stop_wait = w0;
				udelay(1000);
				w->reads++;
				if (rc) {
					w->rderr++;
				} else {
					int v = frame_decode(b);
					if (v) w->valid++; else w->invalid++;
					if (v == 2) w->contact++;
					if (nfr < NFR) {
						for (int k = 0; k < 65; k++)
							fr[nfr][k] = b[k];
						fr_t[nfr] = tu;
						fr_lv[nfr] = in << 4 | after;
						nfr++;
					}
				}
			}
		} else if (inexc) {
			uint32_t wd = tu - exc_t;
			if (wd < w->minw) w->minw = wd;
			if (wd > w->maxw) w->maxw = wd;
			inexc = 0;
		}
		last = in;
	}
	w->latch = rd(INT_STS) & 1;
	if (w->minw == 0xffffffffu)
		w->minw = 0;

	L(lbl, "idle=", w->idle, "latch=", w->latch);
	L(lbl, "up=", w->up, "dn=", w->dn);
	L(lbl, "minw_us=", w->minw, "maxw_us=", w->maxw);
	for (uint32_t k = 0; k < nts; k += 2)
		L(lbl, "ts_us=", ts[k], k + 1 < nts ? "ts_us=" : 0, k + 1 < nts ? ts[k + 1] : 0);
	if (mode != 1)
		return;
	L(lbl, "reads=", w->reads, "rderr=", w->rderr);
	L(lbl, "valid=", w->valid, "invalid=", w->invalid);
	L(lbl, "contact=", w->contact, "frames=", nfr);
	for (uint32_t i = 0; i < nfr; i++) {
		char lb[8];
		lb[0] = lbl[0]; lb[1] = lbl[1]; lb[2] = 'F';
		lb[3] = "0123456789ABCDEF"[i];
		lb[4] = ':';
		lb[5] = '0' + (fr_lv[i] >> 4);	/* INT before / after the read */
		lb[6] = '0' + (fr_lv[i] & 1);
		lb[7] = 0;
		const uint8_t *f = fr[i];
		L(lb, "b0-3=", be32(f), "b4-7=", be32(f + 4));
		L(lb, "b8-11=", be32(f + 8), "b60-63=", be32(f + 60));
		L(lb, "t_us=", fr_t[i], "b64=", f[64]);
	}
}

/* ---------------- P6 addressing matrix ---------------- */

struct cell {
	uint32_t m78, mpid, pidtries, mst, tmin, tmax, has_fail;
	uint8_t fail[8];
};

static int trd(struct cell *c, uint8_t reg, uint8_t *b, uint32_t n)
{
	uint64_t t = now();
	int rc = rd_at(NVT_FW, reg, b, n);
	uint32_t us = us_of(now() - t);
	if (us < c->tmin) c->tmin = us;
	if (us > c->tmax) c->tmax = us;
	return rc;
}

static void chk78(struct cell *c, int rc, const uint8_t *b)
{
	if (rc)
		return;
	int ok = 1;
	for (int i = 0; i < 8; i++)
		if (b[i] != EXP8[i])
			ok = 0;
	if (ok) {
		c->m78++;
	} else if (!c->has_fail) {
		for (int i = 0; i < 8; i++)
			c->fail[i] = b[i];
		c->has_fail = 1;
	}
}

#define CAP(v, m)	((v) > (m) ? (m) : (v))

static uint32_t run_cell(int t, int s)
{
	uint8_t b[68];
	struct cell c;
	c.m78 = c.mpid = c.pidtries = c.mst = c.has_fail = 0;
	c.tmin = 0xffffffffu; c.tmax = 0;
	uint32_t n0 = E.nack, o0 = E.to, s0 = E.sh, x0 = E.oth, r0 = E.retry;
	char lb[8] = { 'P', '6', '-', 'T', (char)('0' + t), 'S', (char)('0' + s), 0 };

	if (s != 1)
		page(0x02, 0x1c);	/* set once, then no page writes in this cell */
	for (int i = 0; i < 16; i++) {
		int rc;
		switch (s) {
		case 1:
			page(0x02, 0x1c);
			rc = trd(&c, 0x78, b, 17);
			chk78(&c, rc, b);
			break;
		case 2:
			rc = trd(&c, 0x78, b, 17);
			chk78(&c, rc, b);
			break;
		case 3:
			rc = trd(&c, 0x60, b, 5);
			if (!rc && (b[0] & 0xf0) == 0xa0)
				c.mst++;
			rc = trd(&c, 0x78, b, 17);
			chk78(&c, rc, b);
			rc = trd(&c, 0x9a, b, 2);
			c.pidtries++;
			if (!rc && b[0] == 0x13 && b[1] == 0x59)
				c.mpid++;
			break;
		default:
			trd(&c, 0x00, b, 65);
			rc = trd(&c, 0x78, b, 17);
			chk78(&c, rc, b);
			break;
		}
		udelay(5000);
	}
	uint32_t nk = E.nack - n0, to = E.to - o0, sh = E.sh - s0, ot = E.oth - x0;
	L(lb, "m78/pid=", c.m78 << 16 | (c.pidtries ? c.mpid : 0xffff), "nk/to=", CAP(nk, 0xffff) << 16 | CAP(to, 0xffff));
	L(lb, "sh/ot/st=", CAP(sh, 255) << 24 | CAP(ot, 255) << 16 | c.mst, "tmin/max=", CAP(c.tmin, 0xffff) << 16 | CAP(c.tmax, 0xffff));
	if (c.has_fail)
		L(lb, "fail8=", be32(c.fail), "", be32(c.fail + 4));
	(void)r0;
	return c.m78 + c.mpid;
}

/* ---------------- test ---------------- */

static void log_states(const char *l, const uint8_t *s, uint32_t n)
{
	for (uint32_t i = 0; i < n; i += 8)
		L(l, "st=", be32(s + i), "", be32(s + i + 4));
}

static void phase_summary(const char *l, const struct watch *w, int mode)
{
	con_hex2(l, "up=", w->up, "dn=", w->dn);
	if (mode)
		con_hex2(l, "valid=", w->valid, "invalid=", w->invalid);
}

void touch_test(unsigned seconds)
{
	(void)seconds;
	t_start = now();
	int rc = touch_bringup();
	con_hex2("T-UP", "rc=", (uint32_t)rc, 0, 0);
	if (rc)
		return;

	/* ---- P0: SE bring-up done, no I2C traffic yet: pin state ---- */
	stamp("P0");
	for (int g = 88; g <= 91; g++) {
		char lb[4] = { 'P', '0', (char)('0' + (g - 88)), 0 };
		L(lb, "cfg=", rd(TLMM_CFG(g)), "io=", rd(TLMM_IO(g)));
	}
	con_str("READY", "P1 starts in 5 s: green = touch and move");
	udelay(5000000);

	/* ---- P1: as left, quiet bus ---- */
	stamp("P1");
	screen(GREENBG, "P1", "TOUCH AND MOVE");
	int_watch("P1", 5000, 0, &W[0]);

	/* ---- P2: first NVT read of the boot, T1 timing, no page write ---- */
	stamp("P2");
	screen(BLACK, "P2", "HANDS OFF");
	set_timing(1);
	uint8_t b[68];
	{
		uint32_t lv0 = rd(INT_IO) & 1;
		stop_wait = 0;		/* sample INT right at the STOP */
		rc = rd_at(NVT_FW, 0x00, b, 65);
		stop_wait = 1;
		udelay(100);
		uint32_t l100 = rd(INT_IO) & 1;
		udelay(900);
		uint32_t l1m = rd(INT_IO) & 1;
		udelay(19000);
		uint32_t l20m = rd(INT_IO) & 1;
		L("P2", "rc=", (uint32_t)rc, "int_pre=", lv0);
		L("P2", "int_100us=", l100, "int_1ms=", l1m);
		L("P2", "int_20ms=", l20m, 0, 0);
		dump65("P2-RAW", b);
		L("P2", "frame=", (uint32_t)frame_decode(b), 0, 0);
	}

	/* ---- P3: as left, INT-driven ---- */
	stamp("P3");
	screen(GREENBG, "P3", "TOUCH AND MOVE");
	int_watch("P3", 5000, 1, &W[1]);

	/* ---- P4: reset, quiet bus ---- */
	stamp("P4");
	screen(GREENBG, "P4", "TOUCH AND MOVE");
	rc = wr2(NVT_HW, 0x00, 0x69);
	udelay(100000);
	L("P4", "reset rc=", (uint32_t)rc, 0, 0);
	int_watch("P4", 5000, 0, &W[2]);

	/* ---- P5: reset, Linux shape ---- */
	stamp("P5");
	screen(BLACK, "P5", "HANDS OFF (reset)");
	uint64_t tr = now();
	rc = wr2(NVT_HW, 0x00, 0x69);
	uint32_t rrc = (uint32_t)rc;
	udelay(35000);
	uint8_t states[24] = { 0 };
	uint32_t ns = 0, hit_ms = 0xffffffffu;
	for (int i = 0; i < 20; i++) {
		udelay(10000);
		int r = page(0x02, 0x1c);
		if (!r)
			r = rd_at(NVT_FW, 0x60, b, 5);
		states[ns++] = r ? 0xee : b[0];
		if (!r && (b[0] & 0xf0) == 0xa0) {
			hit_ms = ms_of(now() - tr);
			break;
		}
	}
	L("P5", "reset rc=", rrc, "polls=", ns);
	L("P5", "hit_ms=", hit_ms, 0, 0);
	log_states("P5", states, ns);
	rc = page(0x02, 0x1c);
	if (!rc)
		rc = rd_at(NVT_FW, 0x78, b, 17);
	L("P5-78", "rc=", (uint32_t)rc, "b16=", b[16]);
	L("P5-78", "b0-3=", be32(b), "b4-7=", be32(b + 4));
	L("P5-78", "b8-11=", be32(b + 8), "b12-15=", be32(b + 12));
	rc = page(0x02, 0x1c);
	if (!rc)
		rc = rd_at(NVT_FW, 0x9a, b, 2);
	L("P5-PID", "rc=", (uint32_t)rc, "b0-1=", (uint32_t)b[0] << 8 | b[1]);
	screen(GREENBG, "P5", "TOUCH AND MOVE");
	int_watch("P5", 6000, 1, &W[3]);

	/* ---- P6: addressing matrix ---- */
	stamp("P6");
	screen(BLACK, "P6", "HANDS OFF (matrix)");
	uint32_t score[3];
	for (int t = 0; t < 3; t++) {
		set_timing(t);
		score[t] = 0;
		for (int s = 1; s <= 4; s++)
			score[t] += run_cell(t, s);
	}
	int best = 1;			/* ties go to T1 */
	for (int t = 0; t < 3; t++)
		if (score[t] > score[best])
			best = t;
	L("P6-SUM", "T0=", score[0], "T1=", score[1]);
	L("P6-SUM", "T2=", score[2], "win=", (uint32_t)best);
	L("P6-ERR", "nack=", E.nack, "to=", E.to);
	L("P6-ERR", "short=", E.sh, "oth=", E.oth);
	L("P6-ERR", "retry=", E.retry, "drain=", E.drain);
	L("P6-ERR", "last_err=", last_err, 0, 0);

	/* ---- P7: state over time with the winning timing ---- */
	stamp("P7");
	screen(BLACK, "P7", "HANDS OFF (state)");
	set_timing(best);
	uint8_t st7[16] = { 0 };
	uint32_t bad7 = 0;
	for (int i = 0; i < 12; i++) {
		udelay(250000);
		int r = page(0x02, 0x1c);
		if (!r)
			r = rd_at(NVT_FW, 0x60, b, 5);
		st7[i] = r ? 0xee : b[0];
		bad7 += r != 0;
	}
	log_states("P7", st7, 12);
	L("P7", "errs=", bad7, "timing=", (uint32_t)best);
	stamp("END");

	/* ---- on-screen summary ---- */
	con_clear(BLACK);
	con_str("M3G", "summary (details in pstore)");
	phase_summary("P1", &W[0], 0);
	phase_summary("P3", &W[1], 1);
	phase_summary("P4", &W[2], 0);
	phase_summary("P5", &W[3], 1);
	con_hex2("P6", "win=", (uint32_t)best, "score=", score[best]);
	con_hex2("P7", "st0-3=", be32(st7), "st4-7=", be32(st7 + 4));
}
