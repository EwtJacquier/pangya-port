// The Spot actor's drawing (FUN_088d6574, a0 the actor): the marks on the course around the shot
// (the club's landing mark and its yards, the players' ball markers, the hole).
//
// The yards over the landing mark ("226y", the distance the club reaches): with the camera behind
// the player the game wrote them as letters in the course (FUN_08952344 with the Spot's batch
// +0x1c and the [font_wind texture), small and far away, and their dark outline went pale and
// blurred; the view from above shows the same yards as a label of the interface (the Spot's +0x90,
// drawn by FUN_08969768: the interface's font, at the mark's place on the screen, in the Spot's
// color +0x94). Both views show that label now. PANGYA_MOD_YARDS=0 keeps the letters in the course.
//
// The ball markers (a "player_mark" over each player's ball, drawn while the Spot's +0x1e1 is set)
// show a network round's players where the others' balls are, each copy seeing only its own ball.
// In a round in turns (game type 8, mods/turns.c) everyone watches every ball and the markers stood
// in front of the view (the current player's over the ball in play, just before the shot): there
// they are not drawn.
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "rt.h"

#define GOLF 0x08b60028u  // the golf singleton: +0x13d0 the game type
#define SPOT_BATCH 0x1cu
#define SPOT_POS 0x28u
#define SPOT_LABEL 0x90u
#define SPOT_COLOR 0x94u
#define SPOT_MARKERS 0x1e1u
#define LABEL_RISE 0.012f  // the label's rise in the course, per unit of the mark's distance

static uint32_t drawing;  // the Spot being drawn

void f_088d6574(CPU *c) {
	uint32_t spot = A0, g = M32(GOLF);
	bool turns = spot && g && M8(g + 0x13d0) == 8;
	uint8_t markers = spot ? M8(spot + SPOT_MARKERS) : 0;
	if (turns) M8(spot + SPOT_MARKERS) = 0;
	drawing = spot;
	f_088d6574_orig(c);
	drawing = 0;
	if (turns) M8(spot + SPOT_MARKERS) = markers;
}

// FUN_08952344(f12 the scale, a0 the batch, a1, a2 the text): letters in the course.
void f_08952344(CPU *c) {
	static int on = -1;
	if (on < 0) on = !getenv("PANGYA_MOD_YARDS") || atoi(getenv("PANGYA_MOD_YARDS")) != 0;
	uint32_t spot = drawing, label = spot ? M32(spot + SPOT_LABEL) : 0;
	if (!on || !label || A0 != M32(spot + SPOT_BATCH)) {
		f_08952344_orig(c);
		return;
	}
	rt_call_guest(0x088d9e98u, label, M32(spot + SPOT_COLOR), 0);  // the label's color
	rt_call_guest(0x088d9ea0u, label, spot + SPOT_POS, 0);          // its place in the course
	// From above the label sits over the mark's dot; behind the player the mark is an arrow standing
	// over that place, and the label goes over the arrow: raised in the course by a share of how far
	// the mark is (the farther, the smaller it shows).
	uint32_t g = M32(GOLF), doc = g ? M32(g + 0x143c) : 0;
	if (doc && M32(doc + 0xa8)) {
		uint32_t ball = M32(doc + 0xa8) + M8(doc + 0x161) * 0x120u + 0x14;
		float p[3], b[3];
		memcpy(p, MP(label + 0x18), 12);
		memcpy(b, MP(ball), 12);
		float dx = p[0] - b[0], dz = p[2] - b[2];
		p[1] += sqrtf(dx * dx + dz * dz) * LABEL_RISE;
		memcpy(MP(label + 0x18), p, 12);
	}
	rt_call_guest(0x08969768u, label, 0, 0);  // drawn
}
