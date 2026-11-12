/*
 * Minimal text console on the ABL splash framebuffer. With the MMU off
 * every access is Device memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_BASE;
int con_bpp = 32;		/* violet: 32 = a8r8g8b8 (stride W*4), 24 = packed r8g8b8 (stride W*3) */
static uint32_t con_bg;

/*
 * Mirror every console line into the pstore console zone, so Android shows it
 * after a warm reboot as /sys/fs/pstore/console-ramoops-0. violet DT:
 * ramoops@a1600000 size 8M, console-size 4M, pmsg-size 4M, no dmesg/ftrace/ecc
 * -> console zone starts at 0xa1600000. Format: persistent_ram_buffer
 * { u32 sig "DBGC"; u32 start; u32 size; u8 data[]; } with start == size (no wrap).
 */
#define PSTORE_CON	0xa1600000UL
#define PSTORE_CON_SZ	0x400000U
int con_log = 1;
static uint32_t log_pos;
/* xnu's /pram (panic log) starts at pstore+0x10000; keep the shim log below it. */
#define LOG_LIMIT	0xf000U

uint32_t con_log_len(void) { return log_pos; }

static void log_ch(char c)
{
	if (!con_log || log_pos >= LOG_LIMIT)
		return;
	*(volatile uint8_t *)(PSTORE_CON + 12 + log_pos) = (uint8_t)c;
	log_pos++;
	volatile uint32_t *h = (volatile uint32_t *)PSTORE_CON;
	h[0] = 0x43474244;
	h[1] = log_pos;
	h[2] = log_pos;
}

static void log_str(const char *s)
{
	while (*s)
		log_ch(*s++);
}

static void put_px(int x, int y, uint32_t c)
{
	if (con_bpp == 32) {
		con_fb[y * (FB_STRIDE / 4) + x] = c;
	} else {	/* byte writes: MMIO-safe with the MMU off */
		volatile uint8_t *p = (volatile uint8_t *)FB_BASE + (uintptr_t)y * FB_W * 3 + x * 3;
		p[0] = c & 0xff; p[1] = (c >> 8) & 0xff; p[2] = (c >> 16) & 0xff;
	}
}
static int row;

void con_clear(uint32_t bg)
{
	for (int y = 0; y < FB_H; y++)
		for (int x = 0; x < FB_W; x++)
			put_px(x, y, bg);
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > FB_W || y0 + CH_H > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++)
				for (int sx = 0; sx < SCALE; sx++)
					put_px(x0 + gx * SCALE + sx, y0 + gy * SCALE + sy, c);
		}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (*s)
		put_char(col++, *s++, fg);
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	row++;
}

void con_str(const char *label, const char *s)
{
	log_str(label); log_ch(' '); log_str(s); log_ch('\n');
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	log_str(label); log_ch(' '); log_str(a);
	fmt_hex(buf, va, 8); log_str(buf);
	if (b) { log_ch(' '); log_str(b); fmt_hex(buf, vb, 8); log_str(buf); }
	log_ch('\n');
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	row++;
}
