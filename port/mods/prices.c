// Mod 5 (prices): everything the shops sell costs a third of what it does in the game, rounded
// down to a round number (133 is 130, 1,333 is 1,300), so that less golf pays for it. The
// shops' lists (FUN_08a5d718 costumes, FUN_08a5e4f4 the other categories: at +0x9fc with the
// count at +0xa00, and at +0xa04 with the count at +0xa08, 0x38 bytes an item, the price at
// +0x24) are the one thing every screen, the "can you afford it" check and the sale itself
// read, so the price is cut there, right after the list is built (mods/unlock.c hooks those two
// functions and calls shop_prices). PANGYA_MOD_PRICES=0 keeps the game's prices, another number
// is the divisor (3 without it).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "rt.h"
#include "prices.h"

#define ITEM_SIZE 0x38u
#define PRICE 0x24u

static int32_t divisor(void) {
	static int32_t d = -1;
	if (d < 0) {
		const char *v = getenv("PANGYA_MOD_PRICES");
		d = v ? atoi(v) : 3;
		if (d < 1) d = 1;
	}
	return d;
}

// A price divided and rounded down to a round number: to hundreds from 1,000 on, tens from 100
// on, fives from 10 on. Never to nothing.
static uint32_t cheaper(uint32_t price) {
	uint32_t p = price / (uint32_t)divisor();
	uint32_t step = p >= 1000 ? 100 : p >= 100 ? 10 : p >= 10 ? 5 : 1;
	p -= p % step;
	return p ? p : price ? 1 : 0;
}

// PANGYA_TEST_PANG=n (tests): the player has n pang once a shop opens.
static void test_pang(void) {
	static bool done;
	const char *v = getenv("PANGYA_TEST_PANG");
	if (done || !v || !M32(0x08b60030u)) return;
	done = true;
	M32(M32(0x08b60030u) + 0xaf0u) = (uint32_t)atoi(v);
}

// Both lists of a shop (the one just built, the other may be as before).
void shop_prices(uint32_t shop, uint32_t list) {
	test_pang();
	if (divisor() < 2) return;
	uint32_t items = M32(shop + list), n = M32(shop + list + 4);
	if (!items || n > 1000) return;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t a = items + i * ITEM_SIZE + PRICE;
		M32(a) = cheaper(M32(a));
	}
	if (getenv("PANGYA_SHOP_LOG"))
		rt_log("shop: %u items at %08x, the first at %u", n, items, M32(items + PRICE));
}

// ---- The Papel Shop: a go at its machine costs 200 pang in the game, as a number in its code ----
// (FUN_08a46420 takes it; the checks whether the player has it are in FUN_08a47784, FUN_08a49b88
// and FUN_08a4cac8) and "Costs 200 Pang each time." written in the picture of its balloon
// ("costinfo"). patches.txt puts the checks and the deduction through these; the balloon's words are
// painted over with white (the atlas's white piece) and written again with the new number, in the
// game's font engine (the scene's text, at +0x488: FUN_089e403c makes a string, FUN_089e3d7c draws it).
uint32_t papel_cost(void) {
	return cheaper(200);
}

// What the check compares with 200: the game has the pang to pay when it has the new cost.
uint32_t papel_pays(uint32_t pang) {
	return pang >= papel_cost() ? 200u : 0u;
}

// What the game takes too much (it takes 200).
uint32_t papel_refund(void) {
	return 200u - papel_cost();
}

#define SCENE_TEXT 0x488u
#define MAKE_STRING 0x089e403cu  // (text, key, words, color, 0)
#define STRING_WIDTH 0x089e4a0cu // (text, key)
#define DRAW_STRING 0x089e3d7cu  // ($f12 = $f13 = 0; text, x, y, key, how long, 1)
#define AS_LONG 0xffffffffu  // as many pixels as the string has
#define BLACK 0xff000000u
#define ORANGE 0xff0052fdu       // ABGR: the balloon's

static uint32_t scene;  // the Papel Shop being drawn

void f_08a49b88(CPU *c) {
	test_pang();
	scene = c->r[4];
	f_08a49b88_orig(c);
	scene = 0;
}

static uint32_t call3(CPU *c, uint32_t func, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5) {
	c->r[4] = a0, c->r[5] = a1, c->r[6] = a2, c->r[7] = a3, c->r[8] = a4, c->r[9] = a5, c->r[31] = 0;
	c->fpr.f[12] = c->fpr.f[13] = 0;
	rt_call(c, func);
	return c->r[2];
}

static uint32_t guest_strings(void) {  // 32 bytes each: the white piece (the atlas lists it as "item_c_c_I": the last letters are how it fits), then 3 keys and 3 texts
	static uint32_t at;
	if (!at) {
		at = rt_alloc_user(32 * 7, false, "mods: the Papel Shop's price");
		if (!at) return 0;
		memset(MP(at), 0, 32 * 7);
		char price[16];
		snprintf(price, sizeof price, "%u Pang", papel_cost());
		const char *words[7] = {"item_c_c", "mods_papel_1", "mods_papel_2", "mods_papel_3", "Costs ", price, "each time."};
		for (int i = 0; i < 7; i++) strcpy((char *)MP(at + 32u * (uint32_t)i), words[i]);
	}
	return at;
}

// The balloon has just been drawn (its atlas, $f12 and $f13): the words over it.
static void balloon_words(CPU *c, uint32_t atlas, float x, float y) {
	uint32_t at = guest_strings(), text = scene ? M32(scene + SCENE_TEXT) : 0;
	if (!at || !text) return;
	CPU keep = *c;
	// The white over the words. The atlas says how to fit its sprites (its first byte): stretched.
	uint8_t mode = M8(atlas);
	M8(atlas) = 1;
	c->r[4] = atlas, c->r[5] = at, c->r[6] = 1;
	c->fpr.f[12] = x + 14, c->fpr.f[13] = y + 6, c->fpr.f[14] = 135, c->fpr.f[15] = 42;
	f_08983f14_orig(c);
	M8(atlas) = mode;
	for (int i = 0; i < 3; i++) {
		uint32_t key = at + 32u * (uint32_t)(1 + i), word = at + 32u * (uint32_t)(4 + i);
		if (!call3(c, STRING_WIDTH, text, key, 0, 0, 0, 0)) call3(c, MAKE_STRING, text, key, word, i == 1 ? ORANGE : BLACK, 0, 0);
	}
	int w1 = (int)call3(c, STRING_WIDTH, text, at + 32, 0, 0, 0, 0), w2 = (int)call3(c, STRING_WIDTH, text, at + 64, 0, 0, 0, 0);
	int w3 = (int)call3(c, STRING_WIDTH, text, at + 96, 0, 0, 0, 0);
	if (getenv("PANGYA_SHOP_LOG")) {
		static int n;
		if (n++ < 3) rt_log("shop: balloon at %.0f,%.0f mode %u widths %d %d %d text %08x atlas %08x", x, y, mode, w1, w2, w3, text, atlas);
	}
	int mid = (int)x + 81, line1 = (int)y + 9, line2 = (int)y + 26;
	call3(c, DRAW_STRING, text, (uint32_t)(mid - (w1 + w2) / 2), (uint32_t)line1, at + 32, AS_LONG, 1);
	call3(c, DRAW_STRING, text, (uint32_t)(mid - (w1 + w2) / 2 + w1), (uint32_t)line1, at + 64, AS_LONG, 1);
	call3(c, DRAW_STRING, text, (uint32_t)(mid - w3 / 2), (uint32_t)line2, at + 96, AS_LONG, 1);
	*c = keep;
}

void prices_sprite_drawn(CPU *c, uint32_t atlas, float x, float y) {
	if (divisor() > 1) balloon_words(c, atlas, x, y);
}
