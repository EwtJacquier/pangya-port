// Mod 1 (boot): no autosave notice, no company logos and no opening movie: the game goes straight
// to the title (which the single-save mod then continues from).
//
// CProjectG::Ready (FUN_0887dfc4) starts the loading thread th_load (FUN_0887d24c) in mode 2
// (DAT_08b637b4): after a 1.6 s blank it plays sk_logo_intro.bin (autosave notice, TOMY,
// SK telecom, NTREEV) until its "end_point" label, which sets DAT_08b637bc. The loading-done
// callback FUN_0887007c waits for that flag, then suspends the thread and frees the animation.
// Setting the flag beforehand lets the game continue as soon as its data is loaded (~40 frames).
// The title plays the opening (pangya_op.pmf) through FUN_0887f60c; that movie is reported as
// finished as soon as it starts (the gallery plays it through other code and still can).
// PANGYA_MOD_SKIP_INTRO=0 keeps the original sequence.
#include <stdlib.h>

#include "funcs.h"
#include "rt.h"

#define LOAD_MODE 0x08b637b4u  // th_load mode (2 = boot logos)
#define LOGO_DONE 0x08b637bcu  // set when the logo animation reaches "end_point"

static bool enabled(void) {
	const char *e = getenv("PANGYA_MOD_SKIP_INTRO");
	return !e || atoi(e) != 0;
}

void f_0887007c(CPU *c) {
	if (enabled() && c->r[5] == 0 && M32(LOAD_MODE) == 2 && M32(LOGO_DONE) == 0) {
		M32(LOGO_DONE) = 1;
		rt_log("mod: boot notice and logos skipped");
	}
	f_0887007c_orig(c);
}

void f_0887f60c(CPU *c) {
	if (enabled()) rt_psmf_skip_next();
	f_0887f60c_orig(c);
}
