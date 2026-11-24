// Mod 4 (unlock): every character playable, every item the shops sell on sale from the start, and
// the whole Pangya Collection gallery (videos, artworks, music) open.
//
// The game data singleton (DAT_08b60030, FUN_08822120) holds:
//   +0xaf4 + i * 0x8e4  character i (0..17) available (0/1; the story sets it, the shop's character
//                       grid shows "?" while it is 0)
//   +0xaec              the player's class (byte; the record room shows it); a shop item is listed
//                       when its required class (IFF record +0x1cc, -1: never) is at most this
//   +0xad8              the profile the gallery reads (gallerymode.cpp), with its bit sets:
//     +0xc564 videos (4)  +0xc566 artworks (15)  +0xc56c music (20); +0xc565/+0xc568/+0xc570 are
//     the "new" marks, left alone.
// The flags are set while the menus run (the out-game scene update FUN_089e9570, every frame), so
// a loaded save gets them too and they are saved with it. The class is raised only while a shop
// builds its lists (FUN_08a5d718 costumes, FUN_08a5e4f4 the other categories): the player's
// class stays theirs. PANGYA_MOD_UNLOCK=0 turns it off.
#include <stdlib.h>

#include "funcs.h"
#include "prices.h"
#include "rt.h"

#define GAMEDATA 0x08b60030u
#define CHAR_AVAILABLE 0xaf4u
#define CHAR_STRIDE 0x8e4u
#define N_CHARS 18
#define CLASS 0xaecu
#define PROFILE 0xad8u
#define GALLERY_VIDEOS 0xc564u
#define GALLERY_ART 0xc566u
#define GALLERY_MUSIC 0xc56cu

static bool enabled(void) {
	static int on = -1;
	if (on < 0) on = !getenv("PANGYA_MOD_UNLOCK") || atoi(getenv("PANGYA_MOD_UNLOCK")) != 0;
	return on;
}

static void unlock_all(void) {
	uint32_t g = M32(GAMEDATA);
	if (!g) return;
	int opened = 0;
	for (int i = 0; i < N_CHARS; i++) {
		uint32_t a = g + CHAR_AVAILABLE + (uint32_t)i * CHAR_STRIDE;
		if (M32(a) != 1) M32(a) = 1, opened++;
	}
	uint32_t p = g + PROFILE;
	uint8_t v = M8(p + GALLERY_VIDEOS);
	uint16_t art = M16(p + GALLERY_ART);
	uint32_t music = M32(p + GALLERY_MUSIC);
	M8(p + GALLERY_VIDEOS) = v | 0x0fu;
	M16(p + GALLERY_ART) = art | 0x7fffu;
	M32(p + GALLERY_MUSIC) = music | 0xfffffu;
	if (opened || (v & 0x0fu) != 0x0fu || (art & 0x7fffu) != 0x7fffu || (music & 0xfffffu) != 0xfffffu)
		rt_log("mod: unlocked %d characters; gallery videos %02x -> 0f, artworks %04x -> 7fff, music %05x -> fffff",
		       opened, v & 0x0fu, art & 0x7fffu, music & 0xfffffu);
}

void f_089e9570(CPU *c) {
	if (enabled()) unlock_all();
	f_089e9570_orig(c);
}

// A shop's list as if the player had the top class.
static void with_top_class(CPU *c, void (*orig)(CPU *)) {
	uint32_t g = M32(GAMEDATA);
	if (!enabled() || !g) {
		orig(c);
		return;
	}
	uint8_t cls = M8(g + CLASS);
	M8(g + CLASS) = 0xff;
	orig(c);
	M8(g + CLASS) = cls;
}

// Both also cut the prices (mods/prices.c).
void f_08a5d718(CPU *c) {
	uint32_t shop = c->r[4];
	with_top_class(c, f_08a5d718_orig);
	shop_prices(shop, 0x9fcu);
}

void f_08a5e4f4(CPU *c) {
	uint32_t shop = c->r[4];
	with_top_class(c, f_08a5e4f4_orig);
	shop_prices(shop, 0xa04u);
}
