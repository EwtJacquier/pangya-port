// Platform layer (SDL2 + OpenGL ES 3): window, framebuffer presentation, input. Also sceDisplay,
// sceCtrl and the sceGe list queue (command walking; drawing is in ge.c).
//
// Environment:
//   PANGYA_HEADLESS=1          hidden window (automated runs; rendering still happens)
//   PANGYA_DUMP=dir:N          write the displayed framebuffer every N vblanks to dir/NNNNNN.ppm
//   PANGYA_INPUT="300:start,420:cross"  press buttons at given vblank counts (scripted tests)
//   PANGYA_FRAMES=N            exit after N vblanks
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ge.h"
#include "hle.h"
#include "platform.h"
#include "rt.h"
#include "texpack.h"
#include "text.h"


static bool headless;
static GLuint present_fbo;
static uint32_t buttons;
static uint32_t inject_mask;  // rt_tap_buttons: buttons a mod presses for the next vblanks
static int inject_left;
static uint8_t analog_x = 128, analog_y = 128;
static uint32_t fb_addr, fb_width = 512, fb_format = 3;
static unsigned long fps_frames;  // sceDisplaySetFrameBuf calls (game frames)
static char dump_dir[256];
static unsigned dump_every;
static unsigned long dump_from;  // PANGYA_DUMP_FROM: first vblank to dump
static unsigned long max_frames;

typedef struct {
	unsigned long from, to, every;  // every == 0: held from..to
	uint32_t buttons;
} ScriptedPress;
static ScriptedPress script[256];
static void ge_stats_dump(void);
static int script_len;

static uint32_t parse_button(const char *s) {
	static const struct {
		const char *n;
		uint32_t b;
	} map[] = {{"start", BTN_START}, {"select", BTN_SELECT}, {"up", BTN_UP}, {"down", BTN_DOWN},
	           {"left", BTN_LEFT}, {"right", BTN_RIGHT}, {"l", BTN_L}, {"r", BTN_R},
	           {"triangle", BTN_TRIANGLE}, {"circle", BTN_CIRCLE}, {"cross", BTN_CROSS}, {"square", BTN_SQUARE}};
	for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
		if (!strcmp(s, map[i].n)) {
			return map[i].b;
		}
	}
	return 0;
}

void platform_init(void) {
	headless = getenv("PANGYA_HEADLESS") != NULL;
	const char *d = getenv("PANGYA_DUMP");
	if (d) {
		const char *colon = strrchr(d, ':');
		snprintf(dump_dir, sizeof dump_dir, "%.*s", colon ? (int)(colon - d) : (int)strlen(d), d);
		dump_every = colon ? (unsigned)atoi(colon + 1) : 60;
		mkdir(dump_dir, 0755);
		const char *from = getenv("PANGYA_DUMP_FROM");
		dump_from = from ? strtoul(from, NULL, 10) : 0;
	}
	if (getenv("PANGYA_GESTATS")) {
		atexit(ge_stats_dump);
	}
	const char *f = getenv("PANGYA_FRAMES");
	max_frames = f ? strtoul(f, NULL, 10) : 0;
	const char *in = getenv("PANGYA_INPUT");
	if (in) {
		char buf[1024];
		snprintf(buf, sizeof buf, "%s", in);
		// "F:btn" tap at F; "F1-F2:btn" hold; "F1-F2/N:btn" tap every N frames in the range.
		for (char *tok = strtok(buf, ","); tok && script_len < 256; tok = strtok(NULL, ",")) {
			char name[32];
			unsigned long a, b, n;
			ScriptedPress *sp = &script[script_len];
			if (sscanf(tok, "%lu-%lu/%lu:%31s", &a, &b, &n, name) == 4) {
				*sp = (ScriptedPress){a, b, n, 0};
			}
			else if (sscanf(tok, "%lu-%lu:%31s", &a, &b, name) == 3) {
				*sp = (ScriptedPress){a, b, 0, 0};
			}
			else if (sscanf(tok, "%lu:%31s", &a, name) == 2) {
				*sp = (ScriptedPress){a, a, 1, 0};
			}
			else {
				continue;
			}
			sp->buttons = parse_button(name);
			script_len++;
		}
	}
	plat_open(headless);
	rt_log("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	glGenFramebuffers(1, &present_fbo);
	ge_init();
}

// Reads the displayed image (480x272, top row first) from a GL texture whose visible area is
// sw x sh texels (larger when rendering above the PSP resolution).
// Reads the displayed image at 480x272, or at the internal resolution when full (debug dumps).
static void read_display(GLuint tex, int sw, int sh, uint32_t *px, bool full) {
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, present_fbo);
	glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	if (full) {
		glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px);
	}
	else if (sw != 480 || sh != 272) {
		static GLuint small_fbo, small_tex;
		if (!small_fbo) {
			glGenTextures(1, &small_tex);
			glBindTexture(GL_TEXTURE_2D, small_tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 480, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glGenFramebuffers(1, &small_fbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, small_fbo);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, small_tex, 0);
		}
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, small_fbo);
		glBlitFramebuffer(0, 0, sw, sh, 0, 0, 480, 272, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, small_fbo);
	}
	if (!full) glReadPixels(0, 0, 480, 272, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	ge_unbind_target();
}

static void write_ppm(const char *path, const uint32_t *px, int w, int h) {
	FILE *f = fopen(path, "wb");
	if (!f) {
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++) {
		uint8_t rgb[3] = {(uint8_t)px[i], (uint8_t)(px[i] >> 8), (uint8_t)(px[i] >> 16)};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

static void dump_frame(unsigned long frame, const uint32_t *px, int w, int h) {
	char path[400];
	snprintf(path, sizeof path, "%s/%06lu.ppm", dump_dir, frame);
	write_ppm(path, px, w, h);
}

void platform_vblank(void) {
	unsigned long frame = (unsigned long)rt_vblank_count();
	audio_pump();
	plat_poll(&buttons, &analog_x, &analog_y);
	for (int i = 0; i < script_len; i++) {
		ScriptedPress *sp = &script[i];
		if (frame < sp->from || frame > sp->to + 6) continue;
		if (sp->every == 0) {
			if (frame <= sp->to) buttons |= sp->buttons;  // held
		}
		else if ((frame - sp->from) % sp->every < 6 && frame <= sp->to + 5) {
			buttons |= sp->buttons;  // taps last 6 vblanks
		}
	}
	int new_scale, new_filter;
	// Test aid: PANGYA_RESCALE_AT=frame:scale changes the render scale like the settings menu.
	static long rescale_at = -2;
	static int rescale_to;
	if (rescale_at == -2) {
		rescale_at = -1;
		if (getenv("PANGYA_RESCALE_AT")) sscanf(getenv("PANGYA_RESCALE_AT"), "%ld:%d", &rescale_at, &rescale_to);
	}
	if (rescale_at >= 0 && (long)frame == rescale_at) ge_set_render_scale(rescale_to);
	if (plat_poll_settings(&new_scale, &new_filter)) {
		ge_set_render_scale(new_scale);
		ge_set_ui_filter(new_filter);
	}
	int vec_strings, vec_ui, hd;
	if (plat_poll_text(&vec_strings, &vec_ui) && text_set_vector(vec_strings, vec_ui)) ge_forget_textures();
	if (plat_poll_hd(&hd) && texpack_set_enabled(hd)) ge_forget_textures();
	if (inject_left > 0) {
		inject_left--;
		buttons |= inject_mask;
	}
	bool want_dump = dump_every && frame >= dump_from && frame % dump_every == 0;
	int sw, sh;
	GLuint tex = ge_display_texture(fb_addr, (int)fb_width, (int)fb_format, &sw, &sh);
	if (want_dump) {
		// PANGYA_DUMP_FULL=1 keeps the internal resolution.
		static int full = -1;
		static uint32_t *px;
		if (full < 0) full = getenv("PANGYA_DUMP_FULL") != NULL;
		if (!px) px = malloc(sizeof *px * 480 * 272 * 36);
		bool f = full && sw <= 480 * 6 && sh <= 272 * 6;
		read_display(tex, sw, sh, px, f);
		dump_frame(frame, px, f ? sw : 480, f ? sh : 272);
	}
#ifdef __EMSCRIPTEN__
	if (ge_dump_finished()) {  // debug capture (F9): send the draw log and the frame it produced
		static uint32_t px[480 * 272];
		read_display(tex, sw, sh, px, false);
		write_ppm("/tmp/frame.ppm", px, 480, 272);
		plat_debug_upload("/tmp/gedump.txt", "gedump.txt");
		plat_debug_upload("/tmp/frame.ppm", "frame.ppm");
	}
#endif
	if (!headless) {
		int ww, wh;
		plat_drawable_size(&ww, &wh);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, present_fbo);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0, 0, 0, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		// Keep 480:272; the texture's row 0 is the top of the PSP screen, so flip vertically.
		int vw = ww, vh = ww * 272 / 480;
		if (vh > wh) vh = wh, vw = wh * 480 / 272;
		int x0 = (ww - vw) / 2, y0 = (wh - vh) / 2;
		glBlitFramebuffer(0, 0, sw, sh, x0, y0 + vh, x0 + vw, y0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		ge_unbind_target();
		plat_swap();
	}
	static int fps_log = -1;
	static uint64_t fps_t0;
	static unsigned long fps_v0, fps_f0;
	if (fps_log < 0) fps_log = getenv("PANGYA_FPS") != NULL;
	if (fps_log && rt_now_us() - fps_t0 >= 5000000) {
		double secs = (rt_now_us() - fps_t0) / 1e6;
		uint64_t under, mixed, draws, batches;
		uint32_t margin;
		audio_stats(&under, &mixed, &margin);
		ge_draw_stats(&draws, &batches);
		rt_log("perf: %.1f vblanks/s, %.1f game frames/s, %.0f draws/s in %.0f GL draws/s, audio %.0f Hz, %.1f%% underrun, "
		       "min margin %u",
		       (frame - fps_v0) / secs, (fps_frames - fps_f0) / secs, draws / secs, batches / secs, mixed / secs,
		       mixed ? 100.0 * under / mixed : 0.0, margin);
		fps_t0 = rt_now_us();
		fps_v0 = frame;
		fps_f0 = fps_frames;
	}
	if (max_frames && frame >= max_frames) {
		rt_log("reached PANGYA_FRAMES=%lu, exiting", max_frames);
		exit(0);
	}
}

void platform_idle(uint64_t us) {
	audio_pump();
	plat_sleep(us);
}

uint32_t platform_buttons(void) { return buttons; }

// Mods press buttons like a player would: held for 6 vblanks, starting at the next one.
void rt_tap_buttons(uint32_t mask) {
	inject_mask = mask;
	inject_left = 6;
}

// ---- sceDisplay ------------------------------------------------------------------------
void hle_sceDisplaySetMode(CPU *c) { RET(0); }
void hle_sceDisplaySetFrameBuf(CPU *c) {
	fps_frames++;
	ge_frame_mark();
	if (A0) {
		fb_addr = A0;
		fb_width = A1 ? A1 : 512;
		fb_format = A2;
	}
	TRACE("sceDisplaySetFrameBuf(%08x, %d, %d, %d)", A0, A1, A2, A3);
	RET(0);
}
void hle_sceDisplayWaitVblankStartCB(CPU *c) {
	rt_wait_vblank();
	RET(0);
}
void hle_sceDisplayGetVcount(CPU *c) { RET((uint32_t)rt_vblank_count()); }

// ---- sceCtrl ---------------------------------------------------------------------------
static uint64_t last_sample_vblank = (uint64_t)-1;
void hle_sceCtrlSetSamplingMode(CPU *c) { RET(0); }
void hle_sceCtrlReadBufferPositive(CPU *c) {
	// Reads block until a new sample is available (one per vblank), which paces some loops.
	if (rt_vblank_count() == last_sample_vblank) {
		rt_wait_vblank();
	}
	last_sample_vblank = rt_vblank_count();
	int n = (int)A1 < 1 ? 1 : (int)A1;
	for (int i = 0; i < n; i++) {
		uint32_t p = A0 + (uint32_t)i * 16;
		M32(p) = (uint32_t)rt_now_us();
		M32(p + 4) = buttons;
		M8(p + 8) = analog_x;
		M8(p + 9) = analog_y;
		memset(MP(p + 10), 0, 6);
	}
	RET(n);
}

// ---- sceGe -----------------------------------------------------------------------------
// Stage 2: lists are walked for control flow (jumps, calls, stalls, FINISH/SIGNAL) so that
// callbacks fire; drawing commands are only counted. Stage 3 plugs the renderer in ge_command.
typedef struct {
	bool used;
	uint32_t start, pc, stall, base;
	uint32_t stack[32], stack_off[32];
	int sp;
	bool done;
	uint32_t cbid;
} GeList;
#define MAX_GE_LISTS 64
static GeList ge_lists[MAX_GE_LISTS];
static uint32_t ge_signal_func, ge_signal_arg, ge_finish_func, ge_finish_arg;
static uint64_t ge_prims;
static uint64_t ge_cmd_hist[256];
static uint64_t ge_prim_hist[8];

void ge_command(uint32_t cmd, uint32_t data, uint32_t addr);  // renderer hook (weak default)
__attribute__((weak)) void ge_command(uint32_t cmd, uint32_t data, uint32_t addr) {
	(void)addr;
	if (cmd == 0x04) {
		ge_prims++;
		ge_prim_hist[(data >> 16) & 7]++;
	}
}

static void ge_stats_dump(void) {
	fprintf(stderr, "GE command histogram (cmd: count):\n");
	for (int i = 0; i < 256; i++) {
		if (ge_cmd_hist[i]) fprintf(stderr, "  %02x: %llu\n", i, (unsigned long long)ge_cmd_hist[i]);
	}
	fprintf(stderr, "primitive types: ");
	for (int i = 0; i < 8; i++) fprintf(stderr, "%d:%llu ", i, (unsigned long long)ge_prim_hist[i]);
	fprintf(stderr, "\n");
}

static void ge_run(GeList *l) {
	while (!l->done && l->pc != l->stall) {
		uint32_t w = M32(l->pc);
		uint32_t cmd = w >> 24, data = w & 0xFFFFFF;
		uint32_t at = l->pc;
		l->pc += 4;
		ge_cmd_hist[cmd]++;
		switch (cmd) {
		case 0x10:  // BASE (also needed by the renderer for vertex/index addresses)
			l->base = (data << 8) & 0x0F000000u;
			ge_command(cmd, data, at);
			break;
		// Addresses in JUMP/CALL/VADDR/IADDR are relative: (BASE high bits | data) + offset, where
		// the offset comes from ORIGIN (address of that command) or OFFSETADDR (data << 8).
		case 0x13: ge_offset_addr = data << 8; break;  // OFFSETADDR
		case 0x14: ge_offset_addr = at; break;         // ORIGIN
		case 0x08: l->pc = (ge_offset_addr + (l->base | (data & 0xFFFFFC))) & 0x0FFFFFFCu; break;  // JUMP
		case 0x09: break;  // BJUMP: bounding-box culling not emulated, so always "visible"
		case 0x0A:         // CALL
			if (l->sp < 32) {
				l->stack_off[l->sp] = ge_offset_addr;
				l->stack[l->sp++] = l->pc;
			}
			l->pc = (ge_offset_addr + (l->base | (data & 0xFFFFFC))) & 0x0FFFFFFCu;
			break;
		case 0x0B:  // RET
			if (l->sp > 0) {
				l->pc = l->stack[--l->sp];
				ge_offset_addr = l->stack_off[l->sp];
			}
			break;
		case 0x0E: {  // SIGNAL (next word is END)
			uint32_t behaviour = (data >> 16) & 0xFF;
			if (ge_signal_func && (behaviour == 1 || behaviour == 2 || behaviour == 3)) {
				rt_call_guest(ge_signal_func, data & 0xFFFF, ge_signal_arg, l->pc);
			}
			break;
		}
		case 0x0F:  // FINISH (next word is END)
			if (ge_finish_func) {
				rt_call_guest(ge_finish_func, data & 0xFFFF, ge_finish_arg, l->pc);
			}
			break;
		case 0x0C:  // END
			l->done = true;
			break;
		default: ge_command(cmd, data, at); break;
		}
	}
}

static uint32_t ge_enqueue(uint32_t list, uint32_t stall) {
	for (int i = 0; i < MAX_GE_LISTS; i++) {
		if (!ge_lists[i].used || ge_lists[i].done) {
			GeList *l = &ge_lists[i];
			memset(l, 0, sizeof *l);
			l->used = true;
			l->start = l->pc = list & 0x0FFFFFFFu;
			l->stall = stall ? (stall & 0x0FFFFFFFu) : 0xFFFFFFFFu;
			ge_run(l);
			return (uint32_t)i;
		}
	}
	return 0x80000021u;
}

void hle_sceGeListEnQueue(CPU *c) {
	uint32_t id = ge_enqueue(A0, A1);
	TRACE("sceGeListEnQueue(%08x, stall=%08x) -> %d", A0, A1, id);
	RET(id);
}
void hle_sceGeListEnQueueHead(CPU *c) { RET(ge_enqueue(A0, A1)); }
void hle_sceGeListUpdateStallAddr(CPU *c) {
	if (A0 < MAX_GE_LISTS && ge_lists[A0].used) {
		ge_lists[A0].stall = A1 & 0x0FFFFFFFu;
		ge_run(&ge_lists[A0]);
	}
	RET(0);
}
void hle_sceGeListSync(CPU *c) { RET(0); }
void hle_sceGeDrawSync(CPU *c) { RET(0); }
void hle_sceGeSetCallback(CPU *c) {
	uint32_t p = A0;
	ge_signal_func = M32(p);
	ge_signal_arg = M32(p + 4);
	ge_finish_func = M32(p + 8);
	ge_finish_arg = M32(p + 12);
	TRACE("sceGeSetCallback(signal=%08x, finish=%08x)", ge_signal_func, ge_finish_func);
	RET(0);
}
void hle_sceGeUnsetCallback(CPU *c) {
	ge_signal_func = ge_finish_func = 0;
	RET(0);
}
void hle_sceGeBreak(CPU *c) { RET(0); }
void hle_sceGeContinue(CPU *c) { RET(0); }
void hle_sceGeEdramGetAddr(CPU *c) { RET(PSP_VRAM_BASE); }
void hle_sceGeEdramGetSize(CPU *c) { RET(PSP_VRAM_SIZE); }

uint64_t ge_prim_count(void) { return ge_prims; }
