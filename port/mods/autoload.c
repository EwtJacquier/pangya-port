// Mod 2 (single save): the title shows one "Start Game" button instead of "New Game / Load Game".
// Pressing it loads the save when there is one, otherwise starts a new game (name entry). The game
// always saves to the same slot (ms0:/PSP/SAVEDATA/ULUS10438/SECURE.BIN).
//
// Title update FUN_089ec928(float dt, CTitle *t): t+0x68 is the menu state (1 intro, 2 cursor on
// New Game, 3 cursor on Load Game, 4-7 name entry). With the menu up the cursor stays on the
// button that does the right thing, and the d-pad doesn't move it (the update asks
// FUN_088661cc(0, action, 1) for its actions by their Korean names: 좌 우 상 하).
// Title drawing FUN_089edc28(CTitle *t) draws each button and its label through
// FUN_08983f14 / FUN_08984678 (x, y, w, h in $f12-$f15; atlas, sprite name in a0, a1): the other
// button isn't drawn and this one moves to the middle of the screen; neither is the copyright bar
// at the bottom ("cpright_back", "cpright"). Its label (title_profile's
// "New Game" / "Load Game" sprites) reads "Start Game" (runtime/text.c redraws it; at 1x the
// original words stay).
// PANGYA_MOD_AUTOLOAD=0 keeps the original menu.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "funcs.h"
#include "prices.h"
#include "rt.h"
#include "text.h"

#define T_STATE 0x68u
#define SCREEN_MIDDLE 240.f

// the update's d-pad actions: 좌 우 상 하 (EUC-KR strings in the game's data)
#define ACT_LEFT 0x08b4c460u
#define ACT_RIGHT 0x08b4c474u
#define ACT_UP 0x08b4c478u
#define ACT_DOWN 0x08b4c488u

static bool enabled(void) {
	static int on = -1;
	if (on < 0) on = !getenv("PANGYA_MOD_AUTOLOAD") || atoi(getenv("PANGYA_MOD_AUTOLOAD")) != 0;
	return on;
}

static bool save_exists(void) {
	static uint64_t checked_at;
	static bool exists;
	uint64_t now = rt_vblank_count();
	if (checked_at && now - checked_at < 30) return exists;
	checked_at = now;
	const char *ms0 = getenv("PANGYA_MS0");
	char path[600];
	snprintf(path, sizeof path, "%s/PSP/SAVEDATA/ULUS10438/SECURE.BIN", ms0 ? ms0 : "build/ms0");
	struct stat st;
	exists = stat(path, &st) == 0 && st.st_size > 0;
	return exists;
}

__attribute__((constructor)) static void start_game_label(void) {
	if (!enabled()) return;
	uitext_replace("title_profile", "New Game", "Start Game");
	uitext_replace("title_profile", "Load Game", "Start Game");
}

static bool menu_up;  // the title's update runs with its two buttons up

void f_089ec928(CPU *c) {
	uint32_t t = c->r[4], state = M32(t + T_STATE);
	if (!enabled() || (state != 2 && state != 3)) {
		f_089ec928_orig(c);
		return;
	}
	bool load = save_exists();
	static int logged = -1;
	if (logged != load) logged = load, rt_log("mod: Start Game %s", load ? "loads the save" : "starts a new game");
	M32(t + T_STATE) = load ? 3 : 2;
	menu_up = true;
	f_089ec928_orig(c);
	menu_up = false;
}

void f_088661cc(CPU *c) {
	uint32_t act = c->r[5];
	if (menu_up && (act == ACT_LEFT || act == ACT_RIGHT || act == ACT_UP || act == ACT_DOWN)) {
		c->r[2] = 0;  // not pressed
		return;
	}
	f_088661cc_orig(c);
}

// ---- drawing: one button, in the middle --------------------------------------------------------
static bool title_drawing;
static float shift;  // moves the button (and its label and cursor) to the middle

void f_089edc28(CPU *c) {
	uint32_t state = M32(c->r[4] + T_STATE);
	title_drawing = enabled() && state >= 1 && state <= 3;
	shift = 0;
	f_089edc28_orig(c);
	title_drawing = false;
}

// -1: not drawn (the other button), 1: drawn moved (this button), 0: drawn as is.
static int title_sprite(CPU *c) {
	const char *n = (const char *)MP(c->r[5]);
	if (!strcmp(n, "button_big_off") || !strcmp(n, "text_newgame_off") || !strcmp(n, "text_continue_off")) return -1;
	if (!strcmp(n, "cpright_back") || !strcmp(n, "cpright")) return -1;  // the copyright bar at the bottom
	if (!strcmp(n, "button_big_on")) {
		shift = SCREEN_MIDDLE - (c->fpr.f[12] + c->fpr.f[14] / 2.f);
		return 1;
	}
	return !strcmp(n, "button_big_cursor") || !strcmp(n, "text_newgame_on") || !strcmp(n, "text_continue_on");
}

void f_08983f14(CPU *c) {
	if (title_drawing) {
		int k = title_sprite(c);
		if (k < 0) return;
		if (k > 0) c->fpr.f[12] += shift;
	}
	// The Papel Shop's balloon: its price is painted over (mods/prices.c).
	bool balloon = c->r[5] && !strcmp((const char *)MP(c->r[5]), "costinfo");
	uint32_t atlas = c->r[4];
	float x = c->fpr.f[12], y = c->fpr.f[13];
	f_08983f14_orig(c);
	if (balloon) prices_sprite_drawn(c, atlas, x, y);
}

void f_08984678(CPU *c) {
	if (title_drawing) {
		int k = title_sprite(c);
		if (k < 0) return;
		if (k > 0) c->fpr.f[12] += shift;
	}
	f_08984678_orig(c);
}
