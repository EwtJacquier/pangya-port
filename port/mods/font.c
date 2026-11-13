// The game's font engine: each string it writes into a texture goes to the HD font
// (runtime/fonthd.c: drawn again at the render scale with the upscaled glyphs of the texture pack)
// and to text.h's text drawing, when the build has it.
//
// PSP_font.cpp: AddTextToCache (FUN_089e403c) writes a string into a new cache entry's RGBA strip
// and then calls FUN_089e4824(font, entry), hooked here. At that point:
//   entry +0 the string (ASCII; bytes >= 0x80 start two-byte codes), +0x40 / +0x44 texture
//   width / height, +0x48 row pitch in pixels, +0x58 the strip's pixels;
//   0x08b72c64: one color pointer per string byte (RGB bytes, NULL = white), set by ColorText;
//   font +0x2ee4 glyph records (u32 bitmap offset, u8 width, u8 height), +0x2f08 line height,
//   +0x2f0c glyph count, +0x2f10 sorted u16 codes, +0x2f14 bitmaps (8-bit alpha), +0x2f18
//   spacing after each glyph.
#include <stdlib.h>

#include <string.h>

#include "fonthd.h"
#include "funcs.h"
#include "rt.h"
#include "text.h"

#define F_GLYPHS 0x2ee4u
#define F_HEIGHT 0x2f08u
#define F_COUNT 0x2f0cu
#define F_CODES 0x2f10u
#define F_BITMAPS 0x2f14u
#define F_SPACING 0x2f18u
#define E_TEXW 0x40u
#define E_TEXH 0x44u
#define E_STRIDE 0x48u
#define E_PIXELS 0x58u
#define COLORS 0x08b72c64u

static int glyph_index(uint32_t font, uint16_t code) {
	int lo = 0, hi = (int)M32(font + F_COUNT) - 1;
	uint32_t codes = M32(font + F_CODES);
	while (lo < hi) {  // same lower bound as FUN_089e3814
		int mid = (lo + hi) / 2;
		if (M16(codes + (uint32_t)mid * 2) < code) lo = mid + 1;
		else hi = mid;
	}
	return lo;
}

// Rows of the capital letters in the bitmap font ('H'), to size and place other fonts' letters.
static void cap_rows(uint32_t font, int *top, int *base) {
	static uint32_t cached_font;
	static int ct, cb;
	if (font != cached_font) {
		cached_font = font;
		ct = 3, cb = 13;
		uint32_t rec = M32(font + F_GLYPHS) + (uint32_t)glyph_index(font, 'H') * 8;
		int w = M8(rec + 4), h = M8(rec + 5);
		uint32_t bm = M32(font + F_BITMAPS) + M32(rec);
		int first = -1, last = -1;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
				if (M8(bm + (uint32_t)(y * w + x)) >= 128) {
					if (first < 0) first = y;
					last = y;
				}
		if (first >= 0 && last > first) ct = first, cb = last + 1;
	}
	*top = ct;
	*base = cb;
}

// The font's glyph table, once per font object, for the HD font's key (see fonthd.h).
static void describe(uint32_t font) {
	uint32_t count = M32(font + F_COUNT), glyphs = M32(font + F_GLYPHS), codes = M32(font + F_CODES), bitmaps = M32(font + F_BITMAPS);
	if (!count || count > 20000 || !rt_valid(glyphs, count * 8) || !rt_valid(codes, count * 2)) return;
	uint32_t size = 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t rec = glyphs + i * 8, end = M32(rec) + (uint32_t)M8(rec + 4) * M8(rec + 5);
		if (end > size) size = end;
	}
	if (!rt_valid(bitmaps, size)) return;
	size_t n = 12 + (size_t)count * 10 + size;
	uint8_t *b = malloc(n), *p = b;
	memcpy(p, "PGF1", 4), p += 4;
	memcpy(p, &count, 4), p += 4;
	memcpy(p, MP(codes), count * 2), p += count * 2;
	memcpy(p, MP(glyphs), count * 8), p += count * 8;
	memcpy(p, &size, 4), p += 4;
	memcpy(p, MP(bitmaps), size);
	fonthd_font(font, b, n);
	free(b);
}

static void collect(uint32_t font, uint32_t e) {
	static TextGlyph g[128];
	if (!fonthd_knows(font)) describe(font);
	int n = 0, x = 0;
	int spacing = (int)M32(font + F_SPACING);
	uint32_t glyphs = M32(font + F_GLYPHS), bitmaps = M32(font + F_BITMAPS);
	for (uint32_t i = 0; M8(e + i) && i < 0x40 && n < 128;) {
		uint32_t at = i;
		uint16_t code = M8(e + i);
		i++;
		if (code & 0x80) code = (uint16_t)(code << 8 | M8(e + i)), i++;
		uint32_t rec = glyphs + (uint32_t)glyph_index(font, code) * 8;
		uint32_t cp = M32(COLORS + at * 4);
		TextGlyph *t = &g[n++];
		t->code = code;
		t->x = (int16_t)x;
		t->w = M8(rec + 4);
		t->h = M8(rec + 5);
		t->rgb = cp ? (uint32_t)M8(cp) | (uint32_t)M8(cp + 1) << 8 | (uint32_t)M8(cp + 2) << 16 : 0xFFFFFFu;
		t->bitmap = bitmaps + M32(rec);
		x += t->w + spacing;
	}
	int top, base;
	cap_rows(font, &top, &base);
	text_register(M32(e + E_PIXELS), (int)M32(e + E_STRIDE), (int)M32(e + E_TEXW), (int)M32(e + E_TEXH), top, base, g, n);
	fonthd_register(font, M32(e + E_PIXELS), (int)M32(e + E_STRIDE), (int)M32(e + E_TEXW), (int)M32(e + E_TEXH), g, n);
}

void f_089e4824(CPU *c) {
	collect(c->r[4], c->r[5]);
	f_089e4824_orig(c);
}
