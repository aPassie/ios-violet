#pragma once
#include <stdint.h>
/* violet: reserved-memory cont_splash_region@9c000000 (0xf00000 bytes); panel 1080x2340 */
#define FB_BASE		0x9c000000UL
#define FB_W		1080
#define FB_H		2340
#define FB_STRIDE	(FB_W * 4)	/* assumed a8r8g8b8 like sweet; probe also redraws as 24bpp */

#define RED	0xffff0000U
#define BLUE	0xff0000a0U
#define WHITE	0xffffffffU
#define YELLOW	0xffffff00U
#define GREEN	0xff00ff00U
#define BLACK	0xff000000U

extern volatile uint32_t *con_fb;
void con_clear(uint32_t bg);
void con_skip(void);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
extern int con_bpp;
extern int con_log;
