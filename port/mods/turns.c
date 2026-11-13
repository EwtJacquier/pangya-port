// Mod 3: network rounds of two players in turns, everyone watching the player whose turn it is.
//
// The golf singleton keeps the game type at +0x13d0 (FUN_0889f798): 1 match against the computer,
// 2-4 the single-player modes, 5 network "single" (everyone plays at once, each with their own
// ball, seeing only their own character), 6 network "team". Type 8 is a turn-based network round
// the menus never offer, left from a version with a server: the player whose turn it is
// (doc+0x161) sends the aim, the power-shot toggles and the shot (messages 0x24 / 0x20) and the
// others replay it with that player's character; the farthest from the pin plays next
// (FUN_088ba518). The setup, the opponent's looks and stats, and keeping the copies in step were
// the server's or never written; this mod does them (see each part below).
//
// PANGYA_NET_TURNS=0 keeps "single" rooms as they are. PANGYA_NET_TRACE=1 logs the turn state.
// PANGYA_GAME_LOG=1 prints the game's own debug messages (FUN_08af9f64, a printf the release
// build left empty; the Korean ones are EUC-KR). PANGYA_AUTOPLAY=1 plays this copy's shots (tests).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "funcs.h"
#include "rt.h"

static bool env_on(const char *name) {
	const char *v = getenv(name);
	return v && atoi(v) != 0;
}

static bool turns(void) {
	static int on = -1;
	if (on < 0) on = !getenv("PANGYA_NET_TURNS") || atoi(getenv("PANGYA_NET_TURNS")) != 0;
	return on;
}

#define GOLF 0x08b60028u     // the golf singleton (FUN_0889f06c)
#define GOLF_TYPE 0x13d0u    // byte: the game type
#define GAMEDATA 0x08b60030u // FUN_08822120
#define ROOM 0x600u          // the network room (FUN_0889d500): +0xc this player's index, +0x10 players
#define LOADED 0x5ccu        // gamedata: characters on the course (FUN_08883940 / FUN_08883980)
#define MAX_PLAYERS 4        // players the messages' tables keep (a room has up to 4)
// Players a round in turns has room for: the round's player list (FUN_0889a154: 2 entries of 0x120
// whatever the count it is given) and the characters' models (the menu avatar's array, FUN_089bce98:
// 2 of 0x410, the first the menu's own) are made for two. A third player wrote past both (tried:
// the game crashed on the first turn). Rooms of 3 or 4 play at once, as a "single" round does.
#define TURN_PLAYERS 2

static int setup_type;  // the real type while FUN_088a04c0 runs as type 5
static bool ended;      // a round in turns ended (its game over runs as type 5)
static bool actor_swapped;  // its end scene shows this copy's character (f_088f10a4)
static uint32_t player_actor;  // the Player actor (FUN_088ed920's a0), in a round in turns

static uint32_t golf(void) { return M32(GOLF); }
static uint32_t my_index(void) { return M32(M32(GAMEDATA) + ROOM + 0xc); }
static uint32_t doc_player(uint32_t doc, uint32_t i) { return M32(doc + 0xa8) + i * 0x120; }

// ---- the players' profiles ---------------------------------------------------------------------
// What a copy needs to show the other player and play their shots exactly: the character, its
// costume, ball, caddie, club set and the stats the shot uses. The game had messages for this in
// its room (0x14-0x16 into gamedata+0x7d4..0x83f, FUN_089b9b1c) but never sends them, so each
// copy sends its own here (net_mod_send) while the players wait in the room.
enum { MSG_PROFILE = 1 };
typedef struct {
	uint8_t index, pad[3];             // room index
	uint32_t chr, set, ball, caddie;   // gamedata +0xe82c, +0xe824 the club set ("m_diClubSet"), +0xe828 the ball
	                                   // ("m_diBall"), +0x5ac (the 0x1c000000 kind: the caddie, it seems)
	uint32_t stats[5];                 // gamedata +0xe8ec.. (FUN_0889b604 order)
	uint32_t parts[24];                // the costume: gamedata + chr * 0x8e4 + 0xaf8
	uint32_t items[10];                // the items brought to the round (see "items" below)
} Profile;
static Profile profiles[4];
static bool have_profile[4];
// The items a player brings to a round: the equipment screen puts this copy's player's in player
// 0's list (G + 0x41b, ten ids; FUN_08a1865c), where a round of one player per copy takes them.
#define ITEM_SLOTS 0x41bu
static uint32_t my_items[10];
static bool items_given[4];  // each player's list set for the round (once: using one takes it out)

static void take_profiles(void) {
	Profile p;
	while (net_mod_recv(MSG_PROFILE, NULL, &p, sizeof p) == (int)sizeof p)
		if (p.index < 4) profiles[p.index] = p, have_profile[p.index] = true;
}

static void send_profile(void) {
	uint32_t gd = M32(GAMEDATA);
	Profile me = {.index = (uint8_t)my_index()};
	me.chr = M32(gd + 0xe82c);
	me.set = M32(gd + 0xe824);
	me.ball = M32(gd + 0xe828);
	me.caddie = M32(gd + 0x5ac);
	for (int i = 0; i < 5; i++) me.stats[i] = M32(gd + 0xe8ec + 4 * (uint32_t)i);
	for (int i = 0; i < 24; i++) me.parts[i] = M32(gd + me.chr * 0x8e4 + 0xaf8 + 4 * (uint32_t)i);
	memcpy(me.items, my_items, sizeof me.items);
	net_mod_send(MSG_PROFILE, &me, sizeof me);
}

// The room (FUN_089b9b1c, every frame while the players wait in it): the copies are in the same
// ad hoc group here (the round has a group of its own, joined after it is set up), so each one
// sends its profile every second and keeps the others'.
void f_089b9b1c(CPU *c) {
	f_089b9b1c_orig(c);
	const char *tc = getenv("PANGYA_TEST_CHAR");  // tests: this copy plays another character
	if (tc && *tc) M32(M32(GAMEDATA) + 0xe82c) = (uint32_t)atoi(tc);
	const char *tb = getenv("PANGYA_TEST_BALL");  // tests: this copy's player has another ball
	if (tb && *tb) M32(M32(GAMEDATA) + 0xe828) = (uint32_t)atoi(tb);
	uint32_t g = golf();
	if (!turns() || !g || (M8(g + GOLF_TYPE) != 5 && M8(g + GOLF_TYPE) != 8)) return;
	const char *ti = getenv("PANGYA_TEST_ITEMS");  // tests: this copy's player brings these items (hex ids)
	for (int i = 0; ti && *ti && i < 10; i++) {
		M32(g + ITEM_SLOTS + 4 * (uint32_t)i) = (uint32_t)strtoul(ti, (char **)&ti, 16);
		if (*ti == ',') ti++;
	}
	for (int i = 0; i < 10; i++) my_items[i] = M32(g + ITEM_SLOTS + 4 * (uint32_t)i);
	memset(items_given, 0, sizeof items_given);
	static uint64_t next;
	uint64_t now = rt_now_us();
	if (now >= next) {
		send_profile();
		next = now + 1000000;
	}
	take_profiles();
}

static void log_profiles(void) {
	uint32_t n = M32(M32(GAMEDATA) + ROOM + 0x10), gd = M32(GAMEDATA);
	rt_log("turns: this copy's player %u: character %u, ball %x, items %08x %08x %08x %08x (saved list %08x %08x %08x %08x)", my_index(),
	       M32(gd + 0xe82c), M32(gd + 0xe828), my_items[0], my_items[1], my_items[2], my_items[3], M32(gd + 0xb408), M32(gd + 0xb40c),
	       M32(gd + 0xb410), M32(gd + 0xb414));
	if (env_on("PANGYA_NET_TRACE")) {  // the items the player owns (id, count: 12 bytes each from gamedata+0xb364)
		char line[400];
		int k = 0;
		for (uint32_t e = 0; e < 14 && k < 360; e++)
			if (M32(gd + 0xb364 + e * 12)) k += snprintf(line + k, sizeof line - (size_t)k, " %08x x%d", M32(gd + 0xb364 + e * 12), (int)M32(gd + 0xb368 + e * 12));
		line[k] = 0;
		rt_log("turns: this copy's player owns:%s", k ? line : " nothing");
	}
	for (uint32_t i = 0; i < n && i < 4; i++) {
		if (i == my_index()) continue;
		if (!have_profile[i]) {
			rt_log("turns: no profile from player %u (default look and stats)", i);
			continue;
		}
		const Profile *p = &profiles[i];
		rt_log("turns: player %u plays character %u (club set %x, ball %x, caddie %x, stats %u %u %u %u %u)", i, p->chr, p->set,
		       p->ball, p->caddie, p->stats[0], p->stats[1], p->stats[2], p->stats[3], p->stats[4]);
	}
}

// The opponent's data where the type-8 setup reads it (FUN_088b0930, FUN_0889b604).
static void put_opponent(const Profile *p) {
	uint32_t gd = M32(GAMEDATA);
	M8(gd + 0x7d4) = (uint8_t)p->chr;
	for (int i = 0; i < 24; i++) M32(gd + 0x7d8 + 4 * (uint32_t)i) = p->parts[i];
	M8(gd + 0x838) = (uint8_t)p->set;
	M8(gd + 0x839) = (uint8_t)p->ball;
	M8(gd + 0x83a) = (uint8_t)p->caddie;
	M8(gd + 0x83b) = (uint8_t)p->stats[0];
	M8(gd + 0x83d) = (uint8_t)p->stats[1];
	M8(gd + 0x83c) = (uint8_t)p->stats[2];
	M8(gd + 0x83e) = (uint8_t)p->stats[3];
	M8(gd + 0x83f) = (uint8_t)p->stats[4];
}

// The round / hole setup (CGolfRule "SendHoleData"): builds the players only for types 2-7. A
// type-8 round runs it as type 5 (the room's players), with every player's character on the course.
void f_088a04c0(CPU *c) {
	uint32_t g = golf();
	uint32_t type = g ? M8(g + GOLF_TYPE) : 0;
	ended = actor_swapped = false;
	if (turns() && (type == 5 || type == 8)) {  // a "single" network room: in turns when two play
		uint32_t n = M32(M32(GAMEDATA) + ROOM + 0x10);
		M8(g + GOLF_TYPE) = n == TURN_PLAYERS ? 8 : 5;
		if (M8(g + GOLF_TYPE) != type) rt_log("turns: %u players, %s", n, M8(g + GOLF_TYPE) == 8 ? "they play in turns" : "everyone plays at once");
	}
	const char *th = getenv("PANGYA_TEST_HOLES");  // tests: a shorter round (G+0x11a9 holes)
	if (!g || M8(g + GOLF_TYPE) != 8) {
		f_088a04c0_orig(c);
		if (th && atoi(th) > 0 && golf() && type == 5 && atoi(th) < (int)M8(golf() + 0x11a9)) M8(golf() + 0x11a9) = (uint8_t)atoi(th);
		return;
	}
	log_profiles();
	setup_type = 8;
	M8(g + GOLF_TYPE) = 5;
	f_088a04c0_orig(c);
	M8(golf() + GOLF_TYPE) = 8;
	setup_type = 0;
	uint32_t gd = M32(GAMEDATA);
	rt_log("turns: round set up, %u players, %u characters on the course", M32(gd + ROOM + 0x10), M32(gd + LOADED));
	if (th && atoi(th) > 0 && atoi(th) < (int)M8(golf() + 0x11a9)) M8(golf() + 0x11a9) = (uint8_t)atoi(th);
}

// The characters' models in a network round live in the menu's (gamedata+0x24, +0x90: one model
// of 0x410 bytes per player; player 0's is the menu's own). With two players on the same character
// their models hold the same resources (the bone pet, animations, meshes, textures), and releasing one
// model (FUN_089cac24: the other players' at the end, FUN_088f10a4) destroyed what another still used:
// the game crashed back in the menus. A model whose character another one also has leaves the
// shared resources to it.
static void words_of(uint32_t m, uint32_t *w, int *n, int cap) {
	if (M32(m + 4) && *n < cap) w[(*n)++] = M32(m + 4);
	if (M32(m + 0x10) && *n < cap) w[(*n)++] = M32(m + 0x10);
	uint32_t b = M32(m + 0x14), e = M32(m + 0x18);
	if (b && e > b && e - b < 0x400)
		for (uint32_t a = b; a < e && *n < cap; a += 4)
			if (M32(a)) w[(*n)++] = M32(a);
	for (uint32_t a = m + 0x30; a < m + 0x330 && *n < cap; a += 4)
		if (M32(a)) w[(*n)++] = M32(a);
}

static int unshare(uint32_t m, const uint32_t *w, int n) {
	int k = 0;
	for (int i = 0; i < n; i++) {
		if (M32(m + 4) == w[i]) M32(m + 4) = 0, k++;
		if (M32(m + 0x10) == w[i]) M32(m + 0x10) = 0, k++;
		uint32_t b = M32(m + 0x14), e = M32(m + 0x18);
		if (b && e > b && e - b < 0x400)
			for (uint32_t a = b; a < e; a += 4)
				if (M32(a) == w[i]) M32(a) = 0, k++;
		for (uint32_t a = m + 0x30; a < m + 0x330; a += 4)
			if (M32(a) == w[i]) M32(a) = 0, k++;
	}
	return k;
}

void f_089cac24(CPU *c) {
	uint32_t m = A0, gd = M32(GAMEDATA), av = gd ? M32(gd + 0x24) : 0, arr = av ? M32(av + 0x90) : 0;
	uint32_t n = gd ? M32(gd + LOADED) : 0;  // the models in use: one per character on the course
	if (n > TURN_PLAYERS) n = TURN_PLAYERS;
	if (arr && n > 1 && m >= arr && m < arr + n * 0x410 && (m - arr) % 0x410 == 0 && M32(m)) {
		static uint32_t w[512];
		int k = 0;
		for (uint32_t i = 0; i < n; i++) {
			uint32_t other = arr + i * 0x410;
			if (other == m || M32(other) != M32(m)) continue;
			int nw = 0;
			words_of(other, w, &nw, 512);
			k += unshare(m, w, nw);
		}
		if (k) rt_log("turns: player %u's model is character %x as another's: %d shared resource(s) left to it", (m - arr) / 0x410, M32(m), k);
	}
	f_089cac24_orig(c);
}

// The end scene (set up by FUN_088f10a4, a0 the Player actor) shows player 0's character: the
// second player saw the first one's. Everything the scene keeps per player swaps with this copy's
// player's: the look (doc+0x2a4, 0x68 each), the model (the models' array, +0x1764, 0x410 each), its
// animation state (+0x1770, 0xb8 each), the bone handles (+0x1914, 0x18 each) and the positions
// (+0x17e0, 0x50 each), and the lists of facial expressions (player 0's is the global one at
// 0x08be4e20, shared with the menu's avatar; the others' at +0x199c): an expression missing from a
// list is made without its picture, which the scene then applied with no cache and crashed.
static void swap_mem(uint32_t a, uint32_t b, uint32_t n) {
	uint8_t t[0x410];
	memcpy(t, MP(a), n);
	memmove(MP(a), MP(b), n);
	memcpy(MP(b), t, n);
}

#define FACIALS0 0x08be4e20u
static uint32_t facials_actor;  // the Player actor whose facial lists are swapped
static void swap_facials(uint32_t pa) {
	uint32_t t = M32(FACIALS0);
	M32(FACIALS0) = M32(pa + 0x199c);
	M32(pa + 0x199c) = t;
}

void f_088f10a4(CPU *c) {
	uint32_t pa = A0, g = golf(), doc = g ? M32(g + 0x143c) : 0, me = my_index();
	if (ended && !actor_swapped && doc && pa && me != 0 && me < M8(doc + 0xc1) && M32(pa + 0x1764) && M32(pa + 0x1770)) {
		swap_mem(doc + 0x2a4, doc + 0x2a4 + me * 0x68, 0x68);
		swap_mem(M32(pa + 0x1764), M32(pa + 0x1764) + me * 0x410, 0x410);
		swap_mem(M32(pa + 0x1770), M32(pa + 0x1770) + me * 0xb8, 0xb8);
		swap_mem(pa + 0x1914, pa + 0x1914 + me * 0x18, 0x18);
		swap_mem(pa + 0x17e0, pa + 0x17e0 + me * 0x50, 0x50);
		swap_facials(pa);
		facials_actor = pa;
		rt_log("turns: the end scene shows this copy's character (player %u's)", me);
	}
	if (ended) actor_swapped = true;
	f_088f10a4_orig(c);
}

// The Player actor's destructor (FUN_088ed73c) frees its own list of facial expressions: the lists
// swapped for the end scene go back first (the global one stays the menu avatar's).
void f_088ed73c(CPU *c) {
	if (A0 == player_actor) player_actor = 0;
	if (facials_actor && A0 == facials_actor) {
		swap_facials(facials_actor);
		facials_actor = 0;
	}
	f_088ed73c_orig(c);
}

// The end scene's players (FUN_088f4a34): at the end of a match against the computer it takes one,
// the second's things being gone by then; walking two crashed. A round in turns ends the same way.
void f_088f4a34(CPU *c) {
	uint32_t g = golf();
	if (!ended || !g) {
		f_088f4a34_orig(c);
		return;
	}
	uint8_t type = M8(g + GOLF_TYPE);
	M8(g + GOLF_TYPE) = 1;
	f_088f4a34_orig(c);
	M8(golf() + GOLF_TYPE) = type;
}

// The results after the last hole (FUN_08925708, a0 the rank screen) are laid out by the round's
// type the board kept when it was made (+0x1e58: 8 at the start of a round in turns), and type 8
// has no layout: no panels (experience, pang), so the screen never let the player leave. They are
// laid out as type 5's, and that type is the one the game goes back to after them.
void f_08925708(CPU *c) {
	if (A0 && M32(A0 + 0x1e58) == 8) M32(A0 + 0x1e58) = 5;
	f_08925708_orig(c);
}

// The board after each hole (FUN_089366dc, a0 the board; the round in mode 0x2000) goes on to the
// next hole when the player presses the confirm button once it has shown for a second (+0x18c the
// time it has shown, +0x18 set while it takes buttons): it plays "play_next" and sets +0x3c to 4
// (+0xb4 to 1). In a network round it goes on by itself after a few seconds, everyone at their pace.
#define BOARD_SHOWN 1.2f  // the board takes the button once it has shown a second
#define PLAY_NEXT 0x08b42a3cu  // "play_next" (the button's sound)
static uint32_t pressed_hole;  // the hole (+1) whose board went on (once: it then waits, +0x3c 3)
static uint32_t hole_place(uint32_t hole);
void f_089366dc(CPU *c) {
	uint32_t b = A0;
	f_089366dc_orig(c);
	uint32_t g = golf(), doc = g ? M32(g + 0x143c) : 0;
	uint8_t type = g ? M8(g + GOLF_TYPE) : 0;
	if (doc && M32(doc + 0x144) != 0x2000) pressed_hole = 0;
	if (b && doc && type >= 5 && type <= 8 && M32(doc + 0x144) == 0x2000 && M8(b + 0x18) && M32(b + 0x3c) < 3 &&
	    pressed_hole != M8(doc + 0x156) + 1u) {
		float shown;
		memcpy(&shown, MP(b + 0x18c), 4);
		if (shown > BOARD_SHOWN) {
			pressed_hole = M8(doc + 0x156) + 1u;
			if (!strcmp(rt_str(PLAY_NEXT), "play_next")) rt_call_guest(0x08805730u, PLAY_NEXT, 0, 0);
			M32(b + 0x3c) = 4;  // the button's fade (+0xb4 counts down to 0), shorter
			float fade = 0.2f;
			memcpy(MP(b + 0xb4), &fade, 4);
			rt_log("turns: the board after hole %u goes on by itself", M8(doc + 0x156));
		}
	}
	// Tests (PANGYA_AUTOPLAY): the board after the last hole waits for circle (cross here).
	static int on = -1;
	if (on < 0) on = env_on("PANGYA_AUTOPLAY");
	if (!on || !doc || M32(doc + 0x144) != 0x20000) return;
	static uint64_t next;
	uint64_t now = rt_vblank_count();
	if (now >= next) rt_tap_buttons(0x4000), next = now + 120;
}

// Tests (PANGYA_NET_TRACE): the rank screen's state after the last hole (FUN_0892a8b8, a0 the
// screen; its panels are 0x130 bytes from +0x28, +0x1dd8 the current one of +0x1ddc, +0x1de4 set
// when it may be left).
void f_0892a8b8(CPU *c) {
	uint32_t r = A0;
	f_0892a8b8_orig(c);
	if (!env_on("PANGYA_NET_TRACE") || !r) return;
	int cur = (int)M32(r + 0x1dd8);
	uint32_t pn = r + (uint32_t)cur * 0x130;
	char now[200];
	snprintf(now, sizeof now, "panel %d of %d, ready %u | 159 %u 15a %u 15c %u 15d %u 15e %u 160 %08x 274 %u 154 %d", cur,
	         (int)M32(r + 0x1ddc), M8(r + 0x1de4), M8(pn + 0x159), M8(pn + 0x15a), M8(pn + 0x15c), M8(pn + 0x15d), M8(pn + 0x15e),
	         M32(pn + 0x160), M8(pn + 0x274), (int)M32(pn + 0x154));
	static char last[200];
	if (strcmp(now, last)) {
		strcpy(last, now);
		rt_log("turns: rank screen %s", now);
	}
}

// Characters on the course: all the room's players (the type-5 setup puts only this one).
void f_08883940(CPU *c) {
	if (setup_type == 8) {
		uint32_t n = M32(M32(GAMEDATA) + ROOM + 0x10);
		A1 = n < 1 ? 1 : n > TURN_PLAYERS ? TURN_PLAYERS : n;
	}
	f_08883940_orig(c);
}

// A player's character: the type-8 branch takes this player's own or the opponent's (room data).
void f_088b0930(CPU *c) {
	uint32_t g = golf(), i = A1 & 0xff;
	bool other = setup_type == 8 && i < 4 && i != my_index() && have_profile[i];
	if (other) put_opponent(&profiles[i]);
	// Each player's items in their own list (this copy's player's were in player 0's), once a round.
	if (setup_type == 8 && i < 4 && !items_given[i]) {
		items_given[i] = true;
		const uint32_t *it = i == my_index() ? my_items : have_profile[i] ? profiles[i].items : NULL;
		for (uint32_t k = 0; k < 10; k++) M32(g + i * 0x270 + ITEM_SLOTS + 4 * k) = it ? it[k] : 0;
	}
	if (setup_type == 8) M8(g + GOLF_TYPE) = 8;
	f_088b0930_orig(c);
	g = golf();
	if (setup_type == 8) M8(g + GOLF_TYPE) = 5;
	if (other) {  // the full item ids (the room data had a byte each)
		uint32_t e = g + i * 0x270;
		M32(e + 0x413) = M32(e + 0x50a) = profiles[i].set | 0x10000000u;
		M32(e + 0x417) = profiles[i].ball | 0x14000000u;
		M32(e + 0x4df) = i;
		M32(e + 0x4e3) = profiles[i].caddie | 0x1c000000u;
	}
}

// The game's end (CGolfRule's mode change FUN_088b5dbc to 0x20000, "modeGameOver"): the results
// and the rank screen are set up for the network types the menus offer (5; type 8 has no branch of
// its own there), so the round ends as a type-5 one. The next round's setup decides again (two
// players: in turns). Open: in the automated tests the rank screen after the last hole did not
// leave, in type 8 or 5.
//
// In a type-5 round each copy has one player, its own, as player 0; the end (the character's pose,
// the results with the experience and pang won) reads player 0. So this copy's player becomes
// player 0 (the two players' entries swap places): the pose and the results are theirs. Both
// players stay for the round's teardown (each character is destroyed once).
//
// The places (1st, 2nd...) at the end of a network round come from each player's record (G+0xfa4: a
// vector of 0x1d0-byte records, one per player of the room): at each hole's board (FUN_088ab3b8)
// FUN_088af1c0 takes +0x11e from the place on that hole (+0x1bc + the hole's place in the round)
// and gives player 0's +0xcc this copy's; the results show this copy's +0x11e, and the end scene
// poses the winner and, in the rain, the others. The network board writes those places when it
// sorts the players (FUN_08931c2c: by score, then +0x110, more is better; the same, the same
// place), and in turns that board never shows: everyone came 9th (the record's 8), in the rain.
// They are worked out here the same way, from the players, before FUN_088af1c0 takes them, and
// once more at the game over (the players' +0xcc, before player 0 and this copy's swap).
static void put_places(uint32_t g, uint32_t doc) {
	uint32_t b = M32(g + 0xfa4), e = M32(g + 0xfa8), n = b && e > b ? (e - b) / 0x1d0 : 0, at = hole_place(M8(doc + 0x156));
	if (n > M8(doc + 0xc1)) n = M8(doc + 0xc1);
	if (n > MAX_PLAYERS || at > 0x13) return;
	int64_t key[MAX_PLAYERS];
	for (uint32_t i = 0; i < n; i++) {
		uint32_t p = doc_player(doc, i);
		key[i] = (int64_t)(int32_t)M32(p + 0xbc) * 100000 - (int16_t)M32(p + 0xc0) + 99999;
	}
	char line[160];
	int k = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t place = 0;
		for (uint32_t j = 0; j < n; j++) place += key[j] < key[i];
		M8(b + i * 0x1d0 + 0x1bc + at) = (uint8_t)place;
		M8(b + i * 0x1d0 + 0x11e) = (uint8_t)place;
		M8(doc_player(doc, i) + 0xcc) = (uint8_t)place;
		k += snprintf(line + k, sizeof line - (size_t)k, "%s player %u %u%s (score %d)", i ? "," : "", i, place + 1,
		              place == 0 ? "st" : place == 1 ? "nd" : place == 2 ? "rd" : "th", (int)M32(doc_player(doc, i) + 0xbc));
	}
	rt_log("turns: the places:%s", line);
}

void f_088af1c0(CPU *c) {
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0;
	if (doc && M32(doc + 0xa8)) put_places(g, doc);
	f_088af1c0_orig(c);
}

void f_088b5dbc(CPU *c) {
	uint32_t g = golf();
	static int trace = -1;  // PANGYA_NET_TRACE=2: who changes the round's mode
	if (trace < 0) trace = getenv("PANGYA_NET_TRACE") ? atoi(getenv("PANGYA_NET_TRACE")) : 0;
	if (trace >= 2 && g && M8(g + GOLF_TYPE) >= 5) rt_log("turns: mode %x from %08x", A1, c->r[31]);
	if (g && A1 == 0x20000 && M8(g + GOLF_TYPE) == 8) {
		uint32_t doc = M32(g + 0x143c), me = my_index();
		if (doc && M32(doc + 0xa8)) {
			put_places(g, doc);
			// The players' entries; their looks and characters swap when the end scene is set up
			// (f_088f10a4).
			if (me != 0 && me < M8(doc + 0xc1)) {
				uint8_t t[0x120];
				memcpy(t, MP(doc_player(doc, 0)), sizeof t);
				memcpy(MP(doc_player(doc, 0)), MP(doc_player(doc, me)), sizeof t);
				memcpy(MP(doc_player(doc, me)), t, sizeof t);
				// and the items left: the round's end keeps player 0's for the next (FUN_088ac3c0)
				swap_mem(g + ITEM_SLOTS, g + me * 0x270 + ITEM_SLOTS, 0x28);
			}
			M8(doc + 0x161) = 0;
		}
		M8(g + GOLF_TYPE) = 5;
		ended = true;
		rt_log("turns: the round is over; it ends as a network round of this copy's player");
	}
	f_088b5dbc_orig(c);
}

// ---- a player's own things (player +0x90 against the golf singleton's +0x77) -------------------
// The game tells this copy's player by an id: the doc player's +0x90 (its index) against G+0x77
// (G+0x73 in some modes), which is 0 on every copy of a network round, where each copy's own
// player is player 0. In turns two things go by it:
//  - after a give-up (the despair pose, FUN_088abdf8) the hole ended for the copy whose "own"
//    player gave up, which waited at the scoreboard for the others (a "single" round's rule, where
//    each copy plays only its own ball): player 0's give-up ended the hole on every copy while the
//    other player still had to play (one copy waited, the other went on to the end). In turns the
//    hole ends when everyone is done (FUN_088aecfc).
//  - a prize won by a shot (FUN_088bb74c) goes to this copy's player when the shooter is its own:
//    the first player's prizes went to everyone, the second's to no one. They go to the shooter's.
static uint32_t id_kept, id_player;
static void set_id(uint32_t doc, uint32_t player, bool own) {
	uint32_t g = golf(), p = doc_player(doc, player), v = 0xffffffffu;
	while (v == M32(g + 0x77) || v == M32(g + 0x73)) v--;
	id_player = p, id_kept = M32(p + 0x90), M32(p + 0x90) = own ? M32(g + 0x77) : v;
}
static void restore_id(void) {
	if (id_player) M32(id_player + 0x90) = id_kept, id_player = 0;
}

void f_088abdf8(CPU *c) {
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0;
	if (doc && M32(doc + 0xa8)) set_id(doc, M8(doc + 0x161), false);
	f_088abdf8_orig(c);
	restore_id();
}

void f_088bb74c(CPU *c) {
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0;
	if (doc && M32(doc + 0xa8)) set_id(doc, M8(doc + 0x161), M8(doc + 0x161) == my_index());
	f_088bb74c_orig(c);
	restore_id();
}

// ---- whose turn it is, as the letters say -----------------------------------------------------
// The letters (FUN_0890d788) show "Your Turn" when the turn is player 0's: in a match against the
// computer that is the player. In a turn-based network round it is this copy's player.
void f_0890d788(CPU *c) {
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0;
	if (!doc) {
		f_0890d788_orig(c);
		return;
	}
	uint8_t turn = M8(doc + 0x161);
	M8(doc + 0x161) = turn == my_index() ? 0 : 1;
	f_0890d788_orig(c);
	M8(doc + 0x161) = turn;
}

// ---- the HUD ----------------------------------------------------------------------------------
// The round's HUD (FUN_088fe81c, every frame) is the match against the computer's, whose player is
// player 0: the power-shot sign on the gauge shows in player 0's turn and from player 0's power,
// the map view moves only then (doc+0x161 == 0, FUN_088fb284(0)). On the second player's copy the
// sign showed in the other player's turn. In turns the HUD draws as if this copy's player were
// player 0: its turn (doc+0x161) and the players it reads swap 0 and this copy's player (the round's
// player getter FUN_0889a22c, which all its parts use: FUN_088fb284, the pang counter's
// FUN_0896567c(0), the wind's FUN_0894ec54 for Silent Wind...), and so do the things it reads by the
// turn without a getter: each player's things in the round, G + 0x270 * player from +0x2a9 (the
// name, the club set at +0x413 and +0x50a, the ball at +0x417 that the ball's picture, FUN_08978a8c,
// takes every frame: the second player's copy showed each player's ball in the other one's turn),
// and the HUD's own pictures of each player's ball and club set (+0x60 and +0x68, and the spin
// circle's ball, +0x1c's +0x1c: one per player, made with the round from each player's things and
// drawn by the turn: the second player's copy showed each player's ball in the other one's turn).
static uint32_t hud_me;  // while the HUD draws in turns, on the copy of a player other than 0
static uint32_t hud_swap(uint32_t i) { return i == 0 ? hud_me : i == hud_me ? 0 : i; }
#define PLAYER_THINGS 0x2a9u  // G + 0x270 * player: the first of a player's things in the round
static void swap_things(uint32_t g, uint32_t me) {
	uint8_t t[0x270];
	memcpy(t, MP(g + PLAYER_THINGS), sizeof t);
	memmove(MP(g + PLAYER_THINGS), MP(g + PLAYER_THINGS + me * 0x270), sizeof t);
	memcpy(MP(g + PLAYER_THINGS + me * 0x270), t, sizeof t);
}
static void swap_words(uint32_t at, uint32_t me) {
	uint32_t t = M32(at);
	M32(at) = M32(at + 4 * me), M32(at + 4 * me) = t;
}
static void hud_swap_pictures(uint32_t hud, uint32_t me) {
	swap_words(hud + 0x60, me);  // the ball and the club set by the gauge
	swap_words(hud + 0x68, me);
	uint32_t circle = M32(hud + 0x1c);  // the ball in the spin circle (FUN_08978e34): one picture per player at +0x1c
	if (circle && rt_valid(circle, 0x30)) swap_words(circle + 0x1c, me);
}

void f_0889a22c(CPU *c) {
	if (hud_me) A1 = hud_swap(A1 & 0xff);
	f_0889a22c_orig(c);
}

void f_088fe81c(CPU *c) {
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0, me = my_index();
	if (!doc || !M32(doc + 0xa8) || me == 0 || me >= M8(doc + 0xc1) || hud_me) {
		f_088fe81c_orig(c);
		return;
	}
	uint8_t turn = M8(doc + 0x161);
	uint32_t hud = A0;
	hud_me = me;
	M8(doc + 0x161) = (uint8_t)hud_swap(turn);
	swap_things(g, me);
	if (hud && me < TURN_PLAYERS) hud_swap_pictures(hud, me);
	f_088fe81c_orig(c);
	if (hud && me < TURN_PLAYERS) hud_swap_pictures(hud, me);
	swap_things(g, me);
	M8(doc + 0x161) = turn;
	hud_me = 0;
}

// The players' cards (FUN_08906e40, a0 its part of the HUD: each player's picture, name, strokes,
// score and power gauge; player 0's on the left at +0x54 (a float), player 1's on the right at
// 407 minus that) showed both, the second player's own on the right. In turns only the card of
// the player whose turn it is shows, on the left: the others' are drawn as absent (status 3,
// skipped) and the second player's goes where the first one's would.
void f_08906e40(CPU *c) {
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0;
	if (!doc || !M32(doc + 0xa8) || !A0) {
		f_08906e40_orig(c);
		return;
	}
	uint32_t part = A0, me = hud_me, n = M8(doc + 0xc1), kept[MAX_PLAYERS];
	uint8_t shown = M8(doc + 0x161), turn = me ? (uint8_t)hud_swap(shown) : shown;
	hud_me = 0;
	if (me) swap_things(golf(), me);  // the cards read the players as they are
	M8(doc + 0x161) = turn;
	if (n > MAX_PLAYERS) n = MAX_PLAYERS;
	for (uint32_t i = 0; i < n; i++)
		if (i != turn) kept[i] = M32(doc_player(doc, i)), M32(doc_player(doc, i)) = 3;
	uint32_t x = M32(part + 0x54);  // a float
	if (turn == 1) {
		float f;
		memcpy(&f, &x, 4);
		f = 407.0f - f;
		memcpy(MP(part + 0x54), &f, 4);
	}
	f_08906e40_orig(c);
	M32(part + 0x54) = x;
	for (uint32_t i = 0; i < n; i++)
		if (i != turn) M32(doc_player(doc, i)) = kept[i];
	M8(doc + 0x161) = shown;
	if (me) swap_things(golf(), me);
	hud_me = me;
}

// The name on a card (FUN_0896b39c(x, y, its text, player), floats in the FPU registers): player 0's
// is this copy's nickname (gamedata+0xadb), the others' their character's ("mingty"), as against the
// computer. In turns every card shows that player's own nickname, the one the room's list shows (the
// player's record, G+0xfa4, +4).
#define MY_NAME 0xadbu
void f_0896b39c(CPU *c) {
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0, i = A1 & 0xff, gd = M32(GAMEDATA);
	uint32_t rb = g ? M32(g + 0xfa4) : 0, re = g ? M32(g + 0xfa8) : 0;
	if (!doc || !gd || !rb || re <= rb || i >= (re - rb) / 0x1d0 || !M8(rb + i * 0x1d0 + 4)) {
		f_0896b39c_orig(c);
		return;
	}
	uint8_t kept[24];
	memcpy(kept, MP(gd + MY_NAME), sizeof kept);
	char *name = (char *)MP(gd + MY_NAME);
	strncpy(name, rt_str(rb + i * 0x1d0 + 4), sizeof kept - 1);
	name[sizeof kept - 1] = 0;
	A1 = 0;
	f_0896b39c_orig(c);
	memcpy(MP(gd + MY_NAME), kept, sizeof kept);
}

// ---- the distance to the hole --------------------------------------------------------------
// (The ball markers, which turns leave out too, are in mods/spot.c.)
// The distance to the hole under the flag (a HUD label, FUN_0896e4d0's: update FUN_0896e760, a0 the
// label, +0x1c its state, 5 hidden, 0 shown) is hidden when a turn starts (FUN_08967190) and, in a
// network round (doc+0x74 > 0xe), shown again only in modes 0x10 (the stance taken), 0x200 (the view
// from above) or 0x8000: while the player aimed it was not there, and it came with the view from
// above. In turns it shows from the start of every turn.
void f_0896e760(CPU *c) {
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0, label = A0;
	if (doc && label && M32(label + 0x1c) == 5) {
		uint32_t mode = M32(doc + 0x144);
		if (mode == 0x8 || mode == 0x10 || mode == 0x20) M32(label + 0x1c) = 0, M32(label + 0x50) = 0;  // standing, stance, gauge
	}
	f_0896e760_orig(c);
}

// ---- the shots -------------------------------------------------------------------------------
// A shot is worked out in one go when it is hit (FUN_088be948, mode 0x40: the flight, where the
// ball stops, who plays next), on the player's copy from the gauge and on the others from the
// player's message 0x20 (FUN_088bafac fills its 0x31 bytes at GolfRule+0x23f6; FUN_088c20ac takes
// it in). What else the others need goes with it in the mod's own message, sent just before:
//  - the wind: each hole's comes from the room (the same everywhere), but every turn adds a random
//    tenth or so of a meter per second on each copy (FUN_0894f554), and a long shot landed yards
//    apart; and the state of the game's random numbers (FUN_08b14df8 at 0x08b7c110);
//  - the direction exactly as the player's character held it (the Player actor's +0x1760, the
//    aim plus pi, which its message 0xf8 gives the shot as the aim): a putt replayed once the
//    opposite way round;
//  - which of the player's strokes it is, so every copy counts the same.
// The game takes message 0x20 in whenever it comes (FUN_088c45c0), even while that copy still
// shows the shot before or the next turn is being set up, and a copy that fell a little behind
// once missed a shot (left waiting for it while the player had given up). So the shot comes from
// the mod's message instead, in order, once this copy stands ready for that player's shot (the
// game's own message 0x20 is left aside). After the shot the player's copy sends who plays next
// (GolfRule +0x11c): the others take it, should their flight still differ.
#define RNG_STATE 0x08b7c110u
#define WIND 0x08b6c298u  // the wind (FUN_0894ed18): +0xc strength, +0x10 direction, +0xb4 the vector
#define PACKET 0x23f6u    // GolfRule: message 0x20's bytes
#define PACKET_LEN 0x31
#define PACKET_AIM 0x19   // in them: the aim (a float)
#define PLAYER_DIR 0x1760u  // the Player actor: the character's direction (the aim + pi)
#define PI_F 3.1415927f
static const struct {
	uint16_t at, len;
} wind_fields[] = {{0x00, 0x1c}, {0x98, 0x10}, {0xb4, 0x24}, {0xd8, 0x0c}};
#define WIND_BYTES (0x1c + 0x10 + 0x24 + 0x0c)
enum { MODE_STAND = 0x8, MODE_READY = 0x10, MODE_GAUGE = 0x20, MODE_SHOT = 0x40, MODE_FLIGHT = 0x80, MODE_BACK = 0x800 };
enum { MSG_SHOT = 3, MSG_NEXT = 4 };
typedef struct {
	uint8_t player, hole, value, stroke;  // value: who plays next (MSG_NEXT)
	uint32_t rng, dir;
	uint8_t wind[WIND_BYTES];
	uint8_t packet[PACKET_LEN];
} Shot;

static void wind_io(uint8_t *buf, bool save) {
	uint32_t w = M32(WIND);
	if (!w) return;
	size_t k = 0;
	for (size_t i = 0; i < sizeof wind_fields / sizeof wind_fields[0]; i++) {
		if (save) memcpy(buf + k, MP(w + wind_fields[i].at), wind_fields[i].len);
		else memcpy(MP(w + wind_fields[i].at), buf + k, wind_fields[i].len);
		k += wind_fields[i].len;
	}
}
static uint32_t rule;    // the CGolfRule (FUN_088a2070's and FUN_088be948's argument)
static uint32_t my_dir;  // this copy's character's direction when its shot was set (FUN_088bafac)
static bool have_dir;

static Shot inbox[8];    // the other players' shots, in order, until this copy is ready for them
static uint64_t inbox_at[8];  // when each came (PANGYA_TEST_LAG)
static int n_inbox;
static Shot replay;      // the one being replayed (from FUN_088c20ac to the end of FUN_088be948)
static bool replaying, delivering;
static uint64_t waiting_since, replay_since;
// The shot each player last had worked out on this copy (hole and stroke): a player's message after
// a shot (who plays next, where the ball rests) goes in once this copy has replayed that shot.
static struct {
	uint8_t hole, stroke;
	bool any;
} worked[MAX_PLAYERS];
static uint8_t my_stroke, my_hole;  // this copy's player's last shot

// A hole's place in the round (1 for the first; FUN_0889f2f8), to tell earlier holes' messages.
static uint32_t hole_place(uint32_t hole) {
	uint32_t g = golf();
	for (uint32_t u = 0; g && u < M8(g + 0x11a9); u++)
		if (M8(g + 0x10b8 + u) == hole) return u + 1;
	return 0x13;
}

static uint8_t *strokes_of(uint32_t doc, uint32_t player) { return MP(doc_player(doc, player) + 0x1f + M8(doc + 0x156)); }

void f_088be948(CPU *c) {
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0;
	if (!doc) {
		f_088be948_orig(c);
		return;
	}
	rule = A0;
	uint32_t turn = M8(doc + 0x161), me = my_index(), hole = M8(doc + 0x156);
	rt_log("turns: player %u's shot %u on hole %u (item %08x)", turn, *strokes_of(doc, turn), hole, M32(doc_player(doc, turn) + 0x98));
	if (turn == me) {
		Shot m = {.player = (uint8_t)me, .hole = (uint8_t)hole, .stroke = *strokes_of(doc, me), .rng = M32(RNG_STATE)};
		m.dir = have_dir ? my_dir : 0;
		wind_io(m.wind, true);
		memcpy(m.packet, MP(rule + PACKET), PACKET_LEN);
		net_mod_send(MSG_SHOT, &m, sizeof m);
		have_dir = false;
		my_stroke = m.stroke, my_hole = m.hole;
		f_088be948_orig(c);
		m.value = M8(rule + 0x11c);
		net_mod_send(MSG_NEXT, &m, sizeof m);
		return;
	}
	if (replaying && replay.player == turn && replay.hole == hole) {
		M32(RNG_STATE) = replay.rng;
		float was, now;
		uint32_t w = M32(WIND);
		memcpy(&was, MP(w + 0xc), 4);
		wind_io(replay.wind, false);
		memcpy(&now, MP(w + 0xc), 4);
		if (was != now) rt_log("turns: player %u's shot in their wind (%.2f m/s, this copy had %.2f)", turn, now, was);
		if (*strokes_of(doc, turn) != replay.stroke) {
			rt_log("turns: player %u's shot is their stroke %u (this copy counted %u)", turn, replay.stroke, *strokes_of(doc, turn));
			*strokes_of(doc, turn) = replay.stroke;
		}
	} else {
		rt_log("turns: player %u's shot came without the mod's message", turn);
	}
	if (turn < MAX_PLAYERS) worked[turn].hole = (uint8_t)hole, worked[turn].stroke = *strokes_of(doc, turn), worked[turn].any = true;
	f_088be948_orig(c);
	replaying = false;
}

// The game's message 0x20 (FUN_088c20ac from FUN_088c45c0): the mod's message brings the same shot
// (f_088a2070 hands it in when this copy is ready for it).
void f_088c20ac(CPU *c) {
	uint32_t g = golf(), ra = c->r[31];
	if (!delivering && g && M8(g + GOLF_TYPE) == 8 && ra >= 0x088c45c0u && ra < 0x088c54b8u) return;
	f_088c20ac_orig(c);
}

// The Player actor's messages (FUN_088ed920, a0 the actor, a1 the message: +4 its number). 0xf8
// asks for the direction the shot goes in (+0x1760 minus pi): kept from this copy's player's shot
// for the others, and the replay's direction set to the player's.
void f_088ed920(CPU *c) {
	uint32_t g = golf();
	if (g && M8(g + GOLF_TYPE) == 8 && A0) player_actor = A0;
	if (g && M8(g + GOLF_TYPE) == 8 && A0 && A1 && M32(A1 + 4) == 0xf8) {
		if (replaying) {
			if (replay.dir && M32(A0 + PLAYER_DIR) != replay.dir) {
				float had, want, aim;
				memcpy(&had, MP(A0 + PLAYER_DIR), 4);
				memcpy(&want, &replay.dir, 4);
				memcpy(&aim, replay.packet + PACKET_AIM, 4);
				rt_log("turns: player %u's shot goes their way (aim %.4f; this copy's character had %.4f)", replay.player, aim,
				       had - PI_F);
				M32(A0 + PLAYER_DIR) = replay.dir;
			}
		} else {
			my_dir = M32(A0 + PLAYER_DIR), have_dir = true;
		}
	}
	f_088ed920_orig(c);
}

// ---- items -------------------------------------------------------------------------------------
// An item (X on it in the item window, FUN_0890b6ac) takes effect through FUN_0890afb4(window,
// player, item, 1, a random number): it leaves the player's list (G + 0x270 * player + 0x41b, the
// count at the round player's +0x94), becomes the item of the player's next shot (+0x98: more power,
// no wind...), plays its effect on the character and shows on the HUD; one may fail (the random
// number). With 0 instead of 1 the item is found by its id in that list: the way another player's
// item came in from the server of the network game this one was made for. Here nothing sent it,
// and the other copies never saw it. The player's copy sends the item and the random number; the
// others take it in that way once they stand in that player's turn for that stroke, before its shot.
enum { MSG_ITEM = 5 };
typedef struct {
	uint8_t player, hole, stroke, pad;
	uint32_t item, rng;
} Item;
static Item items_in[8];
static int n_items;
static uint32_t item_window;  // the item window (FUN_0890a92c's a0, its update every frame)
static bool applying_item;

void f_0890a92c(CPU *c) {
	item_window = A0;
	f_0890a92c_orig(c);
}

void f_0890afb4(CPU *c) {
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0;
	uint32_t player = A1 & 0xff, item = A2, from_window = A3 & 0xff, rng = A4;
	// the stroke it comes before, as it is now: an item may change it (the Mulligan Rose takes the shot back)
	uint8_t stroke = doc && M32(doc + 0xa8) && player < M8(doc + 0xc1) ? *strokes_of(doc, player) : 0;
	f_0890afb4_orig(c);
	if (!doc || !M32(doc + 0xa8) || applying_item || !from_window || player != my_index() || c->r[2] != 1) return;
	Item m = {.player = (uint8_t)player, .hole = M8(doc + 0x156), .stroke = stroke, .item = item, .rng = rng};
	net_mod_send(MSG_ITEM, &m, sizeof m);
	rt_log("turns: player %u uses item %08x before shot %u", player, item, m.stroke);
}

// Whether an item can be used now (FUN_0890c654, a0 the window, a1 the item): the Mulligan Rose
// asks for player 0's strokes on the hole, as against the computer. In turns, the player whose turn it is.
void f_0890c654(CPU *c) {
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0, turn = doc ? M8(doc + 0x161) : 0;
	if (!doc || !turn || hud_me) {
		f_0890c654_orig(c);
		return;
	}
	hud_me = turn;  // FUN_0889a22c: player 0 is the turn's
	f_0890c654_orig(c);
	hud_me = 0;
}

static void apply_item(CPU *c, uint32_t doc, const Item *m) {
	uint32_t g = golf(), slots = g + m->player * 0x270u + ITEM_SLOTS, p = doc_player(doc, m->player), n = M8(p + 0x94);
	uint32_t k = 0;
	while (k < n && k < 10 && M32(slots + 4 * k) != m->item) k++;
	if (k == n && n < 10) {  // not in this copy's list of that player's items: it goes there first
		M32(slots + 4 * n) = m->item, M8(p + 0x94) = (uint8_t)(n + 1);
		rt_log("turns: player %u's item %08x was not in their list here (%u items)", m->player, m->item, n);
	}
	CPU saved = *c;
	c->r[4] = item_window, c->r[5] = m->player, c->r[6] = m->item, c->r[7] = 0, c->r[8] = m->rng, c->r[31] = 0;
	applying_item = true;
	rt_call(c, 0x0890afb4u);
	applying_item = false;
	uint32_t ok = c->r[2];
	*c = saved;
	rt_log("turns: player %u's item %08x before shot %u %s", m->player, m->item, m->stroke, ok == 1 ? "taken in" : "NOT taken in");
}

// The items the others used, in order, each once this copy stands in that player's turn for that
// stroke (before the shot: deliver_shots asks for them first).
static void deliver_items(CPU *c, uint32_t doc, uint32_t mode, uint32_t turn, uint32_t me) {
	Item m;
	while (net_mod_recv(MSG_ITEM, NULL, &m, sizeof m) == (int)sizeof m)
		if (m.player != me && m.player < MAX_PLAYERS && n_items < (int)(sizeof items_in / sizeof items_in[0])) items_in[n_items++] = m;
	uint32_t hole = M8(doc + 0x156);
	bool standing = mode == MODE_STAND || mode == MODE_READY || mode == MODE_GAUGE;
	while (n_items) {
		const Item *h = &items_in[0];
		bool old = hole_place(h->hole) < hole_place(hole) ||
		           (h->hole == hole && h->player < M8(doc + 0xc1) && *strokes_of(doc, h->player) > h->stroke);
		if (!old) {
			if (h->hole != hole || h->player != turn || *strokes_of(doc, turn) != h->stroke || !standing || !item_window) return;
			apply_item(c, doc, h);
		} else {
			rt_log("turns: player %u's item %08x before shot %u on hole %u dropped (too late here)", h->player, h->item, h->stroke, h->hole);
		}
		memmove(&items_in[0], &items_in[1], sizeof items_in[0] * (size_t)(n_items - 1));
		n_items--;
	}
}

// The Mulligan Rose (GolfRule message 0x8f, "PM_NET_CANCEL_SHOT", in FUN_088a33c4): the ball of
// the player whose turn it is goes back (FUN_088b35a0), then the player's statistics count it when
// the turn is player 0's, as against the computer: on the second player's copy the first player's
// counted, its own never did. Between the ball's going back and the stroke limit's check
// (FUN_088c61d8) the turn reads as this copy's player's view of it (0 when it is this copy's), the
// players through the getter as they are (the ball's own setup, before, reads by the real turn).
static bool cancel_window;
static uint8_t cancel_turn;
static uint32_t cancel_doc, cancel_stats, cancel_counts;  // the statistics (rule +0x248): +0x1434/+0x1438 count them
static void close_cancel(void) {
	if (!cancel_window) return;
	M8(cancel_doc + 0x161) = cancel_turn;
	hud_me = 0, cancel_window = false;
	uint32_t now = cancel_stats ? M32(cancel_stats + 0x1434) + M32(cancel_stats + 0x1438) : 0;
	rt_log("turns: player %u's Mulligan %s this copy's statistics", cancel_turn, now != cancel_counts ? "counted in" : "left out of");
}

void f_088b35a0(CPU *c) {
	uint32_t rule_of_cancel = A0;
	f_088b35a0_orig(c);
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0, me = my_index();
	if (!doc || !M32(doc + 0xa8) || me == 0 || me >= M8(doc + 0xc1) || hud_me || cancel_window) return;
	cancel_doc = doc, cancel_turn = M8(doc + 0x161), cancel_window = true;
	cancel_stats = rt_valid(M32(rule_of_cancel + 0x248), 0x1440) ? M32(rule_of_cancel + 0x248) : 0;
	cancel_counts = cancel_stats ? M32(cancel_stats + 0x1434) + M32(cancel_stats + 0x1438) : 0;
	hud_me = me;
	M8(doc + 0x161) = (uint8_t)hud_swap(cancel_turn);
}

void f_088c61d8(CPU *c) {
	close_cancel();
	f_088c61d8_orig(c);
}

// ---- the club in the player's hands ------------------------------------------------------------
// The club (the club object, FUN_08949ce0: +0x28 the club, +4 its data) went to the others only with
// the shot (FUN_088c20ac: FUN_0894c8d8(object, club)): while a player chose another, the others saw
// the one the game picks by itself. The player's copy sends it whenever it changes in its turn; the
// others set it the same way while they stand in that turn (the club's update, FUN_0894a748, then
// changes the character's club and the HUD).
#define CLUB_OBJ 0x08bb4fecu
enum { MSG_CLUB = 6 };
typedef struct {
	uint8_t player, hole, stroke, club;
} Club;
static Club club_in[MAX_PLAYERS];
static bool has_club[MAX_PLAYERS];

static void clubs(uint32_t doc, uint32_t mode, uint32_t turn, uint32_t me) {
	bool standing = mode == MODE_STAND || mode == MODE_READY || mode == MODE_GAUGE;
	uint32_t hole = M8(doc + 0x156);
	static Club sent;
	static bool have_sent;
	if (standing && turn == me && M32(CLUB_OBJ + 4)) {
		Club m = {.player = (uint8_t)me, .hole = (uint8_t)hole, .stroke = *strokes_of(doc, me), .club = M8(CLUB_OBJ + 0x28)};
		if (!have_sent || memcmp(&m, &sent, sizeof m)) {
			net_mod_send(MSG_CLUB, &m, sizeof m);
			sent = m, have_sent = true;
		}
	}
	Club m;
	while (net_mod_recv(MSG_CLUB, NULL, &m, sizeof m) == (int)sizeof m)
		if (m.player < MAX_PLAYERS && m.player != me) club_in[m.player] = m, has_club[m.player] = true;
	if (turn >= MAX_PLAYERS || turn == me || !has_club[turn] || !standing || !M32(CLUB_OBJ + 4)) return;
	const Club *h = &club_in[turn];
	if (h->hole != hole || h->stroke != *strokes_of(doc, turn)) return;
	has_club[turn] = false;
	if (M8(CLUB_OBJ + 0x28) == h->club) return;
	rt_log("turns: player %u holds club %u (this copy had %u)", turn, h->club, M8(CLUB_OBJ + 0x28));
	rt_call_guest(0x0894c8d8u, CLUB_OBJ, h->club, 0);
}

// ---- the shots' results ------------------------------------------------------------------------
// With the same wind the replay lands where the shot did. Should anything still differ, the ball
// is put where it is for the player: once it rests (the camera going back, mode 0x800), the
// player's copy sends where it is and the others put it there, so the next turn (the farthest from
// the pin) and the next shot start from the same place everywhere. A copy that fell behind may get
// a player's result before it has replayed that shot: the result waits for the replay (and goes in
// before that player's next shot, should the replay never come).
enum { MSG_RESULT = 2 };
typedef struct {
	uint8_t player, hole, stroke, pad;
	float pos[3];
} Result;
static Result pending[MAX_PLAYERS];
static bool has_pending[MAX_PLAYERS];
static int results_in[MAX_PLAYERS], shots_in[MAX_PLAYERS];  // tests (PANGYA_TEST_BATCH)

static void apply_result(uint32_t doc, uint32_t i) {
	if (i >= MAX_PLAYERS || !has_pending[i]) return;
	has_pending[i] = false;
	if (pending[i].hole != M8(doc + 0x156)) return;
	uint32_t p = doc_player(doc, i);
	float was[3];
	memcpy(was, MP(p + 0x14), 12);
	memcpy(MP(p + 0x14), pending[i].pos, 12);
	rt_log("turns: player %u's ball after shot %u at %.2f,%.2f,%.2f (the replay had %.2f,%.2f,%.2f)", i, pending[i].stroke,
	       pending[i].pos[0], pending[i].pos[1], pending[i].pos[2], was[0], was[1], was[2]);
}

static void results(uint32_t doc, uint32_t mode, uint32_t turn, uint32_t me) {
	static uint32_t last_mode;
	uint32_t hole = M8(doc + 0x156);
	if (mode == MODE_BACK && last_mode != MODE_BACK && turn == me && my_hole == hole) {
		Result r = {.player = (uint8_t)me, .hole = (uint8_t)hole, .stroke = my_stroke};
		memcpy(r.pos, MP(doc_player(doc, me) + 0x14), 12);
		net_mod_send(MSG_RESULT, &r, sizeof r);
	}
	last_mode = mode;
	Result r;
	while (net_mod_recv(MSG_RESULT, NULL, &r, sizeof r) == (int)sizeof r)
		if (r.player < MAX_PLAYERS && r.player != me) pending[r.player] = r, has_pending[r.player] = true, results_in[r.player]++;
	for (uint32_t i = 0; i < MAX_PLAYERS && i < M8(doc + 0xc1); i++) {
		if (!has_pending[i]) continue;
		if (hole_place(pending[i].hole) < hole_place(hole)) {  // an earlier hole's
			has_pending[i] = false;
			continue;
		}
		bool shooting = turn == i && (replaying || mode == MODE_READY || mode == MODE_GAUGE || mode == MODE_SHOT || mode == MODE_FLIGHT);
		bool replayed = worked[i].any && worked[i].hole == pending[i].hole && worked[i].stroke >= pending[i].stroke;
		if (replayed && !shooting) apply_result(doc, i);  // else its replay is still to come, or running
	}
}

// Who plays after the shot in flight: the player's copy decides (once this copy has worked that
// shot out: the game takes it at the end of the shot, mode 0x800).
static Shot nexts[MAX_PLAYERS];
static bool has_next[MAX_PLAYERS];
static void next_turn(uint32_t doc, uint32_t mode, uint32_t turn, uint32_t me) {
	Shot m;
	while (net_mod_recv(MSG_NEXT, NULL, &m, sizeof m) == (int)sizeof m)
		if (m.player < MAX_PLAYERS && m.player != me) nexts[m.player] = m, has_next[m.player] = true;
	if (turn >= MAX_PLAYERS || turn == me || !has_next[turn] || !rule || (mode != MODE_SHOT && mode != MODE_FLIGHT && mode != MODE_BACK))
		return;
	const Shot *n = &nexts[turn];
	if (!worked[turn].any || worked[turn].hole != n->hole || worked[turn].stroke != n->stroke) return;
	has_next[turn] = false;
	if (M8(rule + 0x11c) != n->value) {
		rt_log("turns: player %u plays next (this copy had player %u)", n->value, M8(rule + 0x11c));
		M8(rule + 0x11c) = n->value;
	}
}

// The next of the others' shots goes in once this copy stands ready for it: the same hole, that
// player's turn, before the shot (standing, or the gauge on the screen). The ball is first put where
// it rests for the player (the result of their shot before, should it not be in yet).
static void deliver_shots(CPU *c, uint32_t doc, uint32_t mode, uint32_t turn, uint32_t me) {
	deliver_items(c, doc, mode, turn, me);
	static int lag = -1;  // PANGYA_TEST_LAG=s (tests): a shot waits that long here, as on a copy that fell behind
	if (lag < 0) lag = getenv("PANGYA_TEST_LAG") ? atoi(getenv("PANGYA_TEST_LAG")) : 0;
	Shot m;
	while (net_mod_recv(MSG_SHOT, NULL, &m, sizeof m) == (int)sizeof m) {
		if (n_inbox == (int)(sizeof inbox / sizeof inbox[0])) {
			memmove(&inbox[0], &inbox[1], sizeof inbox[0] * (size_t)(n_inbox - 1));
			memmove(&inbox_at[0], &inbox_at[1], sizeof inbox_at[0] * (size_t)(n_inbox - 1));
			n_inbox--;
		}
		inbox_at[n_inbox] = rt_now_us();
		inbox[n_inbox++] = m;
	}
	uint32_t hole = M8(doc + 0x156);
	while (n_inbox && (inbox[0].player == me || hole_place(inbox[0].hole) < hole_place(hole))) {
		rt_log("turns: player %u's shot %u on hole %u dropped (this copy is on hole %u)", inbox[0].player, inbox[0].stroke,
		       inbox[0].hole, hole);
		memmove(&inbox[0], &inbox[1], sizeof inbox[0] * (size_t)(n_inbox - 1));
		memmove(&inbox_at[0], &inbox_at[1], sizeof inbox_at[0] * (size_t)(n_inbox - 1));
		n_inbox--;
	}
	// A shot goes in once this copy has stood a second in that player's turn (a copy that fell behind
	// finds it there when the turn starts, while the game still sets the turn up), and once more
	// should the game still stand after it.
	bool standing = mode == MODE_STAND || mode == MODE_READY || mode == MODE_GAUGE;
	static uint32_t stood_turn = 0xff;
	static uint64_t stood_since;
	uint64_t now = rt_now_us();
	if (!standing) stood_turn = 0xff;
	else if (stood_turn != turn) stood_turn = turn, stood_since = now;
	static int retries;
	if (replaying && mode == MODE_STAND && now - replay_since > 1500000 && replay.hole == hole && replay.player == turn &&
	    rule && retries < 3) {  // handed in, and the game still stands: once more
		rt_log("turns: player %u's shot %u handed in again (the game had not taken it)", replay.player, replay.stroke);
		memcpy(MP(rule + PACKET), replay.packet, PACKET_LEN);
		replay_since = now, retries++;
		delivering = true;
		rt_call_guest(0x088c20acu, rule, rule + PACKET, 0);
		delivering = false;
	}
	if (replaying && now - replay_since > 10000000) {  // handed in, but the shot never started
		rt_log("turns: player %u's shot %u never started here (mode %x)", replay.player, replay.stroke, mode);
		replaying = false;
	}
	if (!n_inbox || replaying || !rule) {
		waiting_since = 0;
		return;
	}
	const Shot *h = &inbox[0];
	static int batch = -1;  // PANGYA_TEST_BATCH=1 (tests): a shot waits for its result, as when both come at once
	if (batch < 0) batch = env_on("PANGYA_TEST_BATCH");
	bool ready = h->hole == hole && h->player == turn && standing && now - stood_since >= 1000000 &&
	             rt_now_us() - inbox_at[0] >= (uint64_t)lag * 1000000 &&
	             (!batch || h->player >= MAX_PLAYERS || results_in[h->player] > shots_in[h->player]);
	if (!ready) {
		if (!waiting_since) waiting_since = now;
		else if (now - waiting_since > 5000000) {
			rt_log("turns: player %u's shot %u on hole %u waits (this copy: mode %x, player %u's turn, hole %u)", h->player,
			       h->stroke, h->hole, mode, turn, hole);
			waiting_since = now;
		}
		return;
	}
	waiting_since = 0;
	if (h->player < MAX_PLAYERS) shots_in[h->player]++;
	if (h->player < MAX_PLAYERS && has_pending[h->player] && pending[h->player].hole == hole && pending[h->player].stroke < h->stroke)
		apply_result(doc, h->player);
	replay = *h;
	replaying = true;
	replay_since = now, retries = 0;
	memmove(&inbox[0], &inbox[1], sizeof inbox[0] * (size_t)(n_inbox - 1));
	memmove(&inbox_at[0], &inbox_at[1], sizeof inbox_at[0] * (size_t)(n_inbox - 1));
	n_inbox--;
	memcpy(MP(rule + PACKET), replay.packet, PACKET_LEN);
	rt_log("turns: player %u's shot %u handed in (mode %x)", replay.player, replay.stroke, mode);
	delivering = true;
	rt_call_guest(0x088c20acu, rule, rule + PACKET, 0);
	delivering = false;
}

// ---- the state every copy sends (0x23, "SEND INFO", every 0.05 s) ------------------------------
// FUN_088c59d0 fills it from player 0 and from the ball in play: in a "single" round both are this
// copy's player. In turns player 0 is the room's first player and the ball in play is the
// player's whose turn it is, so the second player sent the first one's strokes and status (a hole
// out showed for both) and everyone's ball where the current player stood (the others' ball
// markers jumped there). The state comes from this copy's player; the ball where it rests (while
// this player's own shot flies, where it was hit from: the marker doesn't tell where it lands).
void f_088c59d0(CPU *c) {
	uint32_t buf = A1;
	f_088c59d0_orig(c);
	uint32_t g = golf(), doc = g && M8(g + GOLF_TYPE) == 8 ? M32(g + 0x143c) : 0;
	if (!doc || !M32(doc + 0xa8)) return;
	uint32_t me = my_index(), turn = M8(doc + 0x161), mode = M32(doc + 0x144), hole = M8(doc + 0x156);
	uint32_t p = doc_player(doc, me);
	M8(buf + 0) = (uint8_t)M32(p);
	M8(buf + 4) = M8(p + 0x1f + hole);
	M16(buf + 6) = (uint16_t)M32(p + 0x40 + hole * 4);
	M16(buf + 8) = (uint16_t)M32(p + 0xc4);
	M8(buf + 0x19) = M8(p + 0xb9);
	M8(buf + 0x1a) = (uint8_t)M32(p + 0xbc);
	M16(buf + 0x1c) = (uint16_t)M32(p + 0xc0);
	static uint8_t rest[12];
	static bool have_rest;
	bool shooting = turn == me && (mode == MODE_READY || mode == MODE_GAUGE || mode == MODE_SHOT || mode == MODE_FLIGHT);
	if (!shooting || !have_rest) memcpy(rest, MP(p + 0x14), 12), have_rest = true;
	memcpy(MP(buf + 0xc), rest, 12);
}

// Test helper (PANGYA_AUTOPLAY=1): plays this player's shots by itself: cross to take the stance,
// then power at three quarters of the bar and impact on the mark, read from the gauge (doc+0xcc:
// +0x4 the impact mark, +0x8 where the bar is); cross on the round's last board.
static void autoplay(uint32_t doc, uint32_t mode, uint32_t turn, uint32_t me) {
	static int on = -1;
	if (on < 0) on = env_on("PANGYA_AUTOPLAY");
	static uint32_t last_mode;
	static uint64_t since, tapped;
	static int taps;
	static float prev;
	uint64_t now = rt_vblank_count();
	if (mode != last_mode) since = now, last_mode = mode, taps = 0;
	static int ff = -1;  // PANGYA_TEST_FF=1 (tests): speeds the others' shots up (cross), as a player may
	if (ff < 0) ff = env_on("PANGYA_TEST_FF");
	if (ff && turn != me && mode == MODE_FLIGHT && now % 20 == 0) rt_tap_buttons(0x4000);
	if (!on || tapped == now || (turn != me && mode != 0x20000)) return;
	if (mode == 0x2000000 && now - since > 120) rt_tap_buttons(0x2000), since = now;  // the board
	uint64_t t = now - since;
	float mark, bar;
	memcpy(&mark, MP(doc + 0xcc + 4), 4);
	memcpy(&bar, MP(doc + 0xcc + 8), 4);
	bool tap = false;
	static int use_items = -1;  // PANGYA_TEST_ITEM=1 (tests): the first item before every shot
	if (use_items < 0) use_items = env_on("PANGYA_TEST_ITEM");
	if (use_items && mode == MODE_STAND && t == 150 && item_window && M8(doc_player(doc, me) + 0x94) > 0) {
		rt_log("turns: test: player %u uses their first item (%08x)", me, M32(golf() + me * 0x270u + ITEM_SLOTS));
		rt_call_guest(0x0890b6acu, item_window, 0, 0);
	}
	if (mode == MODE_STAND) {  // a window open (items, the board) keeps the stance: circle closes it
		if (t % 600 == 450) rt_tap_buttons(0x2000), tapped = now;
		static int test_club = -1;  // PANGYA_TEST_CLUB=1 (tests): L (the next club, "LKEY") before each stance
		if (test_club < 0) test_club = env_on("PANGYA_TEST_CLUB");
		if (test_club && t == 200) {
			rt_log("turns: test: player %u presses L (club %u)", me, M8(CLUB_OBJ + 0x28));
			rt_tap_buttons(0x100), tapped = now;
		}
		if (test_club && t == 260) rt_log("turns: test: player %u now holds club %u", me, M8(CLUB_OBJ + 0x28));
		tap = t % 600 == (uint64_t)(use_items || test_club ? 300 : 90);
	}
	else if (mode == 0x20000) tap = t % 180 == 179;
	else if (mode == MODE_GAUGE && t > 8) {
		static float power = -1;  // PANGYA_AUTOPLAY_POWER: the share of the bar (tests: weak shots, a give-up)
		if (power < 0) power = getenv("PANGYA_AUTOPLAY_POWER") ? (float)atof(getenv("PANGYA_AUTOPLAY_POWER")) : 0.75f;
		// Near the pin (on the green: the putter) the share follows the distance, learnt from the
		// putts before (how far a share of the bar took the ball), so that the players hole out.
		static float per_share = 25.0f, putt_share, putt_from[3];
		static bool putted;
		static uint64_t putt_gauge;
		float use = power;
		uint32_t p = doc_player(doc, me), h = M32(doc + 0xc4) + (M8(doc + 0x156) - 1u) * 0x24;
		float ball[3], pin[3];
		memcpy(ball, MP(p + 0x14), 12);
		memcpy(pin, MP(h + 0xc), 12);
		float dx = pin[0] - ball[0], dz = pin[2] - ball[2], d = sqrtf(dx * dx + dz * dz);
		if (d < 48) {  // 15 yards
			if (putted && putt_gauge != since) {  // the last putt: how far it went for its share
				float mx = ball[0] - putt_from[0], mz = ball[2] - putt_from[2], went = sqrtf(mx * mx + mz * mz);
				if (went > 0.5f && putt_share > 0.02f) per_share = went / putt_share;
				putted = false;
			}
			use = (d * 1.15f + 3) / per_share;  // a little past the cup
			if (use < 0.03f) use = 0.03f;
			if (use > 1) use = 1;
		}
		if (taps == 0 && bar >= 66 + use * (343 - 66)) {
			tap = true;
			if (d < 48) putted = true, putt_share = use, putt_gauge = since, memcpy(putt_from, ball, 12);
		}
		else if (taps == 1) tap = bar < prev && bar <= mark + 3;
	}
	prev = bar;
	if (tap) rt_tap_buttons(0x4000), tapped = now, taps++;
}

// ---- the round's end: this copy's character ---------------------------------------------------
// After the last hole the game kept showing the player whose turn it was (the last one to finish)
// on every copy: the board's view and "Loading results" showed that player's character from
// behind until the end scene, which shows this copy's (f_088f10a4). Once the last hole is over (the
// board, mode 0x2000; the last hole is the round's list's last, G+0x10b7 + the count G+0x11a9) the
// turn goes to this copy's player and the Player actor gets the message a turn's start sends it
// (0xc9), which shows that player's character (the others' hidden) in its stance. The message also
// puts the actor at the ball in play, in the cup by then: it stays where the last player stood.
static bool own_shown;
static void show_own(CPU *c, uint32_t g, uint32_t doc, uint32_t mode, uint32_t me) {
	uint32_t holes = M8(g + 0x11a9);
	if (own_shown || mode != 0x2000 || !player_actor || !holes || M8(doc + 0x156) != M8(g + 0x10b7 + holes)) return;
	own_shown = true;
	if (me >= M8(doc + 0xc1) || M8(doc + 0x161) == me) return;
	rt_log("turns: the last hole is over; this copy shows its own character (player %u's, not %u's)", me, M8(doc + 0x161));
	M8(doc + 0x161) = (uint8_t)me;
	uint8_t stand[0x10];  // where the character stands (+0x1754) and the way it faces (+0x1760)
	memcpy(stand, MP(player_actor + 0x1754), sizeof stand);
	CPU saved = *c;
	c->r[29] = (c->r[29] - 0x40) & ~0xfu;  // the message (0x20 bytes: +4 its number) below the stack
	uint32_t m = c->r[29];
	memset(MP(m), 0, 0x20);
	M32(m + 4) = 0xc9;
	c->r[4] = player_actor, c->r[5] = m, c->r[31] = 0;
	rt_call(c, 0x088ed920u);
	*c = saved;
	memcpy(MP(player_actor + 0x1754), stand, sizeof stand);
}

// The check whether the character can stand where it is (FUN_088f5064, a0 the Player actor; when it
// cannot, +0x179e "NoPlay" and the game draws it see-through) is skipped from then on: the
// character only shows there, and in the cup or with another character's stance the check failed.
void f_088f5064(CPU *c) {
	if ((own_shown || ended) && A0 && A0 == player_actor) {
		M8(A0 + 0x179e) = 0;
		return;
	}
	f_088f5064_orig(c);
}

// The round's rule update (CGolfRule, every frame).
void f_088a2070(CPU *c) {
	uint32_t rl = A0;
	f_088a2070_orig(c);
	uint32_t g = golf();
	if (g && M8(g + GOLF_TYPE) == 5 && M32(g + 0x143c) && M32(M32(g + 0x143c) + 0xa8)) {  // tests: everyone at once
		uint32_t doc = M32(g + 0x143c);
		autoplay(doc, M32(doc + 0x144), 0, 0);  // each copy's player is player 0
	}
	if (!g || M8(g + GOLF_TYPE) != 8) {
		n_inbox = 0, n_items = 0, replaying = false, own_shown = false, my_hole = 0;
		memset(has_club, 0, sizeof has_club);
		memset(worked, 0, sizeof worked);
		memset(has_pending, 0, sizeof has_pending);
		memset(has_next, 0, sizeof has_next);
		return;
	}
	uint32_t doc = M32(g + 0x143c);
	if (!doc || !M32(doc + 0xa8)) return;
	if (rl) rule = rl;
	uint32_t mode = M32(doc + 0x144), turn = M8(doc + 0x161), me = my_index();
	// The hole's end (FUN_088ab3b8, mode 0x2000): a network round stays there ten seconds at least
	// (rule+0x2ac counts them, after a first two), then asks the board whether it is done and loads
	// the next hole. Once the board has gone on (f_089366dc), the wait ends.
	if (mode == 0x2000 && pressed_hole == M8(doc + 0x156) + 1u && rule) {
		float t;
		memcpy(&t, MP(rule + 0x2ac), 4);
		if (t >= 2 && t < 10) t = 10, memcpy(MP(rule + 0x2ac), &t, 4);
	}
	static int stall = -1;  // PANGYA_TEST_STALL=s (tests): this copy freezes that long when another
	if (stall < 0) stall = getenv("PANGYA_TEST_STALL") ? atoi(getenv("PANGYA_TEST_STALL")) : 0;  // player's turn starts
	static uint32_t stall_mode;
	if (stall > 0 && mode == MODE_STAND && stall_mode != MODE_STAND && turn != me) {
		rt_log("turns: test: this copy stalls %d s at player %u's turn", stall, turn);
		usleep((useconds_t)stall * 1000000);
	}
	static int stall_back = -1;  // PANGYA_TEST_STALL_BACK=s: the same after each shot (the next turn's shot comes first)
	if (stall_back < 0) stall_back = getenv("PANGYA_TEST_STALL_BACK") ? atoi(getenv("PANGYA_TEST_STALL_BACK")) : 0;
	if (stall_back > 0 && mode == MODE_BACK && stall_mode != MODE_BACK) {
		rt_log("turns: test: this copy stalls %d s after player %u's shot", stall_back, turn);
		usleep((useconds_t)stall_back * 1000000);
	}
	stall_mode = mode;
	close_cancel();  // (should the stroke limit's check not have come)
	next_turn(doc, mode, turn, me);
	results(doc, mode, turn, me);
	clubs(doc, mode, turn, me);
	deliver_shots(c, doc, mode, turn, me);
	// A hole out (mode 0x1000, FUN_088ab05c) shows until +0x30, the time in the mode, reaches 1.3
	// times +0x94 (at least 3.5 s), on the player's copy and on the others alike. A copy that got
	// through it sooner (its player sped the ball up with cross) let the next player play while the
	// holed-out player's copy still showed it, and that shot showed there only afterwards. Once the
	// next shot is waiting here, the hole out ends after a second and a half.
	if (mode == 0x1000 && n_inbox && inbox[0].hole == M8(doc + 0x156) && inbox[0].player != turn && rule) {
		float t, len;
		memcpy(&t, MP(rule + 0x30), 4);
		memcpy(&len, MP(rule + 0x94), 4);
		if (len < 3.5f) len = 3.5f;
		if (t >= 1.5f && t < len * 1.3f) {
			rt_log("turns: player %u's hole out ends at %.1f s (player %u's shot is waiting)", turn, t, inbox[0].player);
			t = len * 1.3f;
			memcpy(MP(rule + 0x30), &t, 4);
		}
	}
	show_own(c, g, doc, mode, me);
	autoplay(doc, mode, turn, me);
	static int on = -1;  // PANGYA_NET_TRACE=1: the turn state when it changes
	if (on < 0) on = env_on("PANGYA_NET_TRACE");
	if (!on) return;
	static uint32_t items_hole;
	if (mode == MODE_STAND && items_hole != M8(doc + 0x156)) {
		items_hole = M8(doc + 0x156);
		for (uint32_t i = 0; i < M8(doc + 0xc1) && i < MAX_PLAYERS; i++) {
			uint32_t sl = g + i * 0x270u + ITEM_SLOTS;
			rt_log("turns: player %u: %u items %08x %08x %08x %08x, ball %08x, club set %08x", i, M8(doc_player(doc, i) + 0x94), M32(sl),
			       M32(sl + 4), M32(sl + 8), M32(sl + 12), M32(g + i * 0x270u + 0x417), M32(g + i * 0x270u + 0x413));
		}
	}
	char now[256];
	int k = snprintf(now, sizeof now, "mode %x turn %u (me %u, id %x) hole %d", mode, turn, me, M32(g + 0x77), M8(doc + 0x156));
	for (uint32_t i = 0; i < M8(doc + 0xc1) && i < 4; i++) {
		uint32_t p = doc_player(doc, i);
		float x, z;
		memcpy(&x, MP(p + 0x14), 4);
		memcpy(&z, MP(p + 0x1c), 4);
		k += snprintf(now + k, sizeof now - (size_t)k, " | p%u st %d ord %d id %x ball %.1f,%.1f", i, M32(p), M8(p + 0x9c),
		              M32(p + 0x90), x, z);
	}
	static char last[256];
	if (strcmp(now, last)) {
		strcpy(last, now);
		rt_log("turns: %s", now);
	}
}

// The game's debug printf: a0 the format, then a1-a3, t0-t3 (doubles take an aligned pair).
void f_08af9f64(CPU *c) {
	static int on = -1;
	if (on < 0) on = env_on("PANGYA_GAME_LOG");
	if (on && A0) {
		const char *f = rt_str(A0);
		static const int regs[] = {5, 6, 7, 8, 9, 10, 11};
		int ai = 0;
		char out[512];
		size_t k = 0;
		for (const char *p = f; *p && k < sizeof out - 64; p++) {
			if (*p != '%') {
				out[k++] = *p;
				continue;
			}
			char spec[16];
			int n = 0;
			spec[n++] = '%';
			p++;
			while (*p && strchr("-+ #0123456789.l", *p) && n < 12) spec[n++] = *p++;
			if (!*p) break;
			char conv = *p;
			spec[n++] = conv;
			spec[n] = 0;
			if (conv == '%') {
				out[k++] = '%';
				continue;
			}
			if (conv == 'f' || conv == 'g' || conv == 'e') {
				if (ai & 1) ai++;
				if (ai + 1 >= 7) break;
				uint64_t bits = (uint64_t)c->r[regs[ai]] | (uint64_t)c->r[regs[ai + 1]] << 32;
				ai += 2;
				double d;
				memcpy(&d, &bits, sizeof d);
				k += (size_t)snprintf(out + k, sizeof out - k, spec, d);
				if (k >= sizeof out) k = sizeof out - 1;
				continue;
			}
			if (ai >= 7) break;
			uint32_t v = c->r[regs[ai++]];
			if (conv == 's') {
				char s2[16];
				snprintf(s2, sizeof s2, "%%.%ds", 120);
				k += (size_t)snprintf(out + k, sizeof out - k, s2, rt_valid(v, 1) ? rt_str(v) : "(bad)");
			} else if (strchr("dicuxXp", conv)) {
				spec[n - 1] = conv == 'p' ? 'x' : conv;
				k += (size_t)snprintf(out + k, sizeof out - k, spec, conv == 'd' || conv == 'i' ? (int)v : v);
			}
			if (k >= sizeof out) k = sizeof out - 1;
		}
		while (k && (out[k - 1] == '\n' || out[k - 1] == ' ')) k--;
		out[k] = 0;
		static char last[512];
		if (k && strcmp(out, last)) {
			strcpy(last, out);
			rt_log("game: %s", out);
		}
	}
	f_08af9f64_orig(c);
}
