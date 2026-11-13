// Network play fix: every player's per-hole score lands on the hole it belongs to.
//
// In a network round each copy sends its player's state every 0.05 s (FUN_088c45c0 "[ADHOC] SEND
// INFO", message 0x23; 0x22 at events): status, the strokes and score of the hole being played,
// and that hole's index (payload byte 0x18). The receiver stores the strokes and score at the
// hole IT is playing (FUN_0889f4c0()+0x156), not the sender's. When the copies change holes at
// different moments (a player still on the scoreboard while the other already plays the next
// hole; over the internet, messages also arrive late) the other hole's numbers overwrite this
// one's: a finished hole shows 0 strokes (a "hole in one") or the next hole's strokes.
//
// During the game's network update (FUN_088c45c0) this mod reads the messages just received into
// the players' slots (FUN_08b04438; each slot keeps the latest message of one player), and for
// those about another hole puts back what the game overwrote and stores the numbers at the
// sender's hole instead.
// PANGYA_MOD_NETFIX=0 keeps the game's behaviour; PANGYA_NET_TRACE=1 logs the state messages.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "rt.h"

#define GOLF 0x08b60028u         // the golf singleton (FUN_08822078)
#define GOLF_PLAYERS 0xfa4u      // -> player records, 0x1d0 bytes each (FUN_088c91b0)
#define GOLF_DOC 0x143cu         // -> the shared document (FUN_0889f4c0)
#define DOC_HOLE 0x156u          // byte: the hole being played
#define GAMEDATA 0x08b60030u     // + 0x600 the room (FUN_0889d500), + 0xc this player's index
#define REC_SIZE 0x1d0u
#define REC_STROKES 0xb3u        // byte per hole
#define REC_SCORE 0xc4u          // int per hole
#define SLOTS 0x08be7d90u        // ad hoc players (FUN_08b03010): 16 slots of 0x13c bytes
#define SLOT_SIZE 0x13cu
#define SLOT_MAC 4u              // MAC as text ("" when the slot is free)
#define SLOT_MAIL 0x97u          // the latest message received from that player (0x80 bytes)
#define MAX_PLAYERS 4
#define MAX_HOLES 18

static bool enabled(void) {
	static int on = -1;
	if (on < 0) on = !getenv("PANGYA_MOD_NETFIX") || atoi(getenv("PANGYA_MOD_NETFIX")) != 0;
	return on;
}

static bool tracing(void) {
	static int on = -1;
	if (on < 0) on = getenv("PANGYA_NET_TRACE") && atoi(getenv("PANGYA_NET_TRACE")) != 0;
	return on;
}

typedef struct {
	int player, hole;
	uint8_t strokes;
	int16_t score;
	uint8_t kept_strokes;  // what the record held at this copy's hole before the game's update
	uint32_t kept_score;
} Fix;

static bool in_update;  // FUN_088c45c0 runs
static Fix fixes[16];
static int n_fixes, here;
static uint32_t recs;

// The round's network update: takes in the players' messages (FUN_08b04438, below), then applies
// them; the fixes go in after it.
void f_088c45c0(CPU *c) {
	uint32_t golf = M32(GOLF);
	uint32_t doc = golf ? M32(golf + GOLF_DOC) : 0;
	recs = golf ? M32(golf + GOLF_PLAYERS) : 0;
	if ((!enabled() && !tracing()) || !doc || !recs || !M32(GAMEDATA)) {
		f_088c45c0_orig(c);
		return;
	}
	here = M8(doc + DOC_HOLE);
	n_fixes = 0;
	in_update = true;
	f_088c45c0_orig(c);
	in_update = false;
	if (tracing()) {  // the players' records for this hole and the one before, when they change
		static char last[256];
		char now[256];
		int k = 0;
		for (int p = 0; p < 2; p++) {
			uint32_t rec = recs + (uint32_t)p * REC_SIZE;
			k += snprintf(now + k, sizeof now - (size_t)k, " | player %d: status %d", p, M8(rec + 0x68));
			for (int h = here > 0 ? here - 1 : 0; h <= here; h++)
				k += snprintf(now + k, sizeof now - (size_t)k, ", hole %d strokes %d score %d", h, M8(rec + REC_STROKES + h),
				              (int)M32(rec + REC_SCORE + h * 4));
		}
		if (strcmp(now, last)) {
			strcpy(last, now);
			rt_log("netplay: records on hole %d%s", here, now);
		}
	}
	for (int k = 0; k < n_fixes; k++) {
		Fix *f = &fixes[k];
		uint32_t rec = recs + (uint32_t)f->player * REC_SIZE;
		M8(rec + REC_STROKES + here) = f->kept_strokes;
		M32(rec + REC_SCORE + here * 4) = f->kept_score;
		M8(rec + REC_STROKES + f->hole) = f->strokes;
		M32(rec + REC_SCORE + f->hole * 4) = (uint32_t)(int32_t)f->score;
		static int logged;
		if (logged++ < 40)
			rt_log("netplay: player %d's numbers for hole %d (strokes %d, score %d) kept there, not on hole %d", f->player,
			       f->hole, f->strokes, f->score, here);
	}
}

// The messages just received sit in the players' slots until the update applies them: one per
// player in each call (net_recv_round), or a second one would replace the first unread.
void f_08b04438(CPU *c) {
	net_recv_round(true);
	f_08b04438_orig(c);
	net_recv_round(false);
	if (!in_update) return;
	int me = (int)M32(M32(GAMEDATA) + 0x600 + 0xc);
	for (uint32_t i = 0; i < 16; i++) {
		uint32_t slot = SLOTS + i * SLOT_SIZE, m = slot + SLOT_MAIL;
		if (M8(slot + SLOT_MAC) < '0') continue;
		uint8_t type = M8(m);
		int player = (int8_t)M8(m + 1);
		const uint8_t *pl = MP(m + 8);
		int sum = (int8_t)pl[0] + (int8_t)pl[1] + (int8_t)pl[2] + (int8_t)pl[3];
		int check = sum < 0 ? -(-sum & 0x3ff) : sum & 0x3ff;
		if ((type != 0x22 && type != 0x23) || (int)M32(m + 4) != check) continue;
		if (player < 0 || player >= MAX_PLAYERS || player == me) continue;
		int hole = pl[0x18];
		int16_t score = (int16_t)(pl[6] | pl[7] << 8);
		if (tracing()) {
			static uint32_t last[MAX_PLAYERS];
			uint32_t key = (uint32_t)pl[0] | (uint32_t)pl[4] << 8 | (uint32_t)hole << 16 | ((uint32_t)here << 24 ^ (uint32_t)score << 20);
			if (key != last[player] || type == 0x22) {
				last[player] = key;
				rt_log("netplay: %s from player %d: status %d, hole %d (this copy: %d), strokes %d, score %d",
				       type == 0x22 ? "ENV" : "UPDATE", player, pl[0], hole, here, pl[4], score);
			}
		}
		if (!enabled() || hole == here || hole >= MAX_HOLES || here >= MAX_HOLES || n_fixes == 16) continue;
		uint32_t rec = recs + (uint32_t)player * REC_SIZE;
		fixes[n_fixes++] = (Fix){player, hole, pl[4], score, M8(rec + REC_STROKES + here), M32(rec + REC_SCORE + here * 4)};
	}
}
