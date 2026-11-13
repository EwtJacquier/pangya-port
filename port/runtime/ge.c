// PSP GE (graphics engine) command translation to OpenGL ES 3.0 / WebGL2.
//
// Vertex work (decode, morph, skinning, transform, lighting, fog, texgen) is done here in C;
// the GPU only rasterizes with one small shader. Every PSP framebuffer address the game draws
// into becomes an FBO, so render-to-texture and display both work by address.
#include <GLES3/gl3.h>
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/stack.h>
#include <unistd.h>
#endif
#include "ge.h"
#include "fonthd.h"
#include "text.h"
#include "texpack.h"
#include "platform.h"
#include "rt.h"

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
static uint32_t reg[256];
static float world[12], view[12], proj[16], tgen[12], bone[8 * 12];
static int world_i, view_i, proj_i, tgen_i, bone_i;
static uint8_t clut[4096];
static bool clut_dirty = true;
static uint64_t clut_h64, clut_h1k;

static inline float f24(uint32_t d) {
	uint32_t u = d << 8;
	float f;
	memcpy(&f, &u, 4);
	return f;
}
static int gedebug;  // PANGYA_GEDEBUG
uint32_t ge_offset_addr;  // relative-address offset (ORIGIN/OFFSETADDR), maintained by the list walker
static uint32_t vertex_addr, index_addr;
static inline uint32_t rel_addr(uint32_t data) {
	return (ge_offset_addr + (((reg[0x10] & 0x000F0000u) << 8) | (data & 0x00FFFFFFu))) & 0x0FFFFFFFu;
}

// ---------------------------------------------------------------------------------------------
// GL objects
// ---------------------------------------------------------------------------------------------
typedef struct {
	float x, y, z, w;
	float u, v;
	uint8_t c0[4];
	uint8_t c1[4];
	float fog;
	float box[4];  // a sprite's texture rectangle, half a texel in (u0 v0 u1 v1): sampling stays inside
} GVert;

static GLuint prog, vbo, vao;
static GLuint samplers[32];  // texture filter/wrap combinations (see RenderState.sampler)
static int rs = 1;
static int max_scale(void);
static int ui_filter = 0;  // filter for the 2D interface: 0 GE's own (the default), 1 xBR, 2 bicubic (PANGYA_UI_FILTER)
// The game's own textures above 1x (not the pack's, not redrawn text), 2D and 3D: 0 as the GE
// samples them (the default), 1 xBR, 2 sharp pixels (PANGYA_TEX_FILTER; the page's menu)
static int tex_filter = 0;           // render scale: framebuffers are rs times the PSP resolution
static GLint u_tex, u_texEnable, u_tfunc, u_tfuncRGBA, u_colorDouble, u_envColor, u_alphaTest, u_atFunc, u_atRef,
	u_fogEnable, u_fogColor, u_clearMode, u_uvScale, u_blendDouble, u_ctFunc, u_ctRef, u_ctMask, u_xbr, u_lodBias;

typedef struct {
	uint32_t addr;  // VRAM offset
	int width, height;
	GLuint fbo, tex, depth;
	uint64_t last_draw;  // GE draw counter when last rendered into
	bool used;
	// Copy target: VRAM that a block transfer filled from a framebuffer (e.g. the static
	// background the game keeps while the shot gauge runs). The scaled picture lives here; the
	// hash of the 1x pixels written to VRAM tells whether the CPU has replaced them since.
	bool copy;
	uint64_t copy_hash, copy_checked;
} Fbo;
#define MAX_FBOS 32
static Fbo fbos[MAX_FBOS];
static Fbo *cur_fbo;
static uint64_t draw_counter;

typedef struct {
	uint64_t key, hash;
	GLuint tex;
	int w, h;
	uint64_t last_use;
	uint64_t checked;  // frame in which the data hash was last verified
	float scale;       // texels per GE texel: the render scale for redrawn text (text.c), a
	                   // replacement's own scale (texpack.c), 1 for the texture as the GE has it
	int pack1;         // a replacement still on its way (web): its index + 1, else 0
	uint32_t *ui_orig; // ... for a UI atlas with words: its pixels as the game has them (uitext_over)
	bool mips;         // a replacement with its mipmaps (sampled smoothly when it shows smaller)
	uint64_t pack_frame, changed_frame;
	uint8_t changes;   // contents replaced in consecutive frames (movies): not looked up
} TexEntry;
#define MAX_TEX 512
static TexEntry texcache[MAX_TEX];
static uint64_t frame_no;

// GE capture for debugging: every draw of one whole game frame (sceDisplaySetFrameBuf to
// sceDisplaySetFrameBuf), as one line per draw (hashes, NDC bounding box, smallest w) plus the
// vertices of small draws that cover a large area. Started by PANGYA_GEDUMP_FRAME=N (first frame
// at or after vblank N) or by plat_debug_dump_requested (F9 in the browser). Written to
// PANGYA_GEDUMP_FILE (default /tmp/gedump.txt in the browser, stderr natively).
static int ge_dump_state;  // 0 idle, 1 capturing
static int ge_dump_draws;
static int ge_dump_mtx_lines;
static bool ge_dump_done;
static FILE *ge_dump_file;
void rt_dump_threads(FILE *f);
static int ctx_dumped;

void ge_frame_mark(void) {
	static long at = -2;
	if (at == -2) at = getenv("PANGYA_GEDUMP_FRAME") ? atol(getenv("PANGYA_GEDUMP_FRAME")) : -1;
	if (ge_dump_state == 1) {
		if (!ge_dump_draws) return;  // the frame hasn't started drawing yet
		ge_dump_state = 0;
		ge_dump_done = true;
#ifdef __EMSCRIPTEN__
		fprintf(ge_dump_file, "ADDR stack base=%p end=%p current=%p sbrk=%p\n", (void *)emscripten_stack_get_base(),
		        (void *)emscripten_stack_get_end(), (void *)emscripten_stack_get_current(), sbrk(0));
#endif
		rt_dump_threads(ge_dump_file);
		fflush(ge_dump_file);
		rt_log("GE dump: end of frame (%d draws)", ge_dump_draws);
		return;
	}
	bool want = plat_debug_dump_requested();
	if (at >= 0 && (long)rt_vblank_count() >= at) want = true, at = -1;
	if (!want) return;
	const char *path = getenv("PANGYA_GEDUMP_FILE");
#ifdef __EMSCRIPTEN__
	if (!path) path = "/tmp/gedump.txt";
#endif
	if (ge_dump_file && ge_dump_file != stderr) fclose(ge_dump_file);
	ge_dump_file = path ? fopen(path, "w") : stderr;
	if (!ge_dump_file) ge_dump_file = stderr;
	ge_dump_state = 1;
	ctx_dumped = 0;
	ge_dump_draws = 0;
	ge_dump_mtx_lines = 0;
	rt_log("GE dump: frame starts at vblank %ld", (long)rt_vblank_count());
}

// True once after a capture finished (the displayed frame can then be saved too).
bool ge_dump_finished(void) {
	bool d = ge_dump_done;
	ge_dump_done = false;
	return d;
}

static const char *VS =
	"#version 300 es\n"
	"layout(location=0) in vec4 a_pos; layout(location=1) in vec2 a_uv;\n"
	"layout(location=2) in vec4 a_c0; layout(location=3) in vec4 a_c1; layout(location=4) in float a_fog;\n"
	"layout(location=5) in vec4 a_box;\n"
	"out vec2 v_uv; out vec4 v_c0; out vec3 v_c1; out float v_fog; flat out vec4 v_box;\n"
	"void main() { gl_Position = a_pos; v_uv = a_uv; v_c0 = a_c0; v_c1 = a_c1.rgb; v_fog = a_fog; v_box = a_box; }\n";

static const char *FS =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform sampler2D u_tex; uniform int u_texEnable, u_tfunc, u_tfuncRGBA, u_colorDouble, u_alphaTest, u_atFunc;\n"
	"uniform float u_lodBias;\n"
	"uniform int u_fogEnable, u_clearMode; uniform vec4 u_envColor; uniform float u_atRef; uniform vec3 u_fogColor;\n"
	"uniform vec2 u_uvScale; uniform int u_blendDouble, u_ctFunc, u_xbr; uniform ivec3 u_ctRef, u_ctMask;\n"
	"in vec2 v_uv; in vec4 v_c0; in vec3 v_c1; in float v_fog; flat in vec4 v_box; out vec4 o;\n"
	// xBR (Hyllian's xBR-lv2 rules, one pass, any scale) for the 2D interface: the PSP draws it at
	// 1:1 from small textures, so above 480x272 plain filtering only blurs it. u_xbr = 1 + clamp s
	// + 2 * clamp t (texel addressing of the draw); colors are compared premultiplied, so the
	// arbitrary RGB of transparent texels doesn't create edges.
	"ivec2 xsz; ivec2 xe;\n"
	"vec4 X(int dx, int dy) {\n"
	"  ivec2 p = xe + ivec2(dx, dy);\n"
	"  p.x = (u_xbr & 1) == 1 ? clamp(p.x, 0, xsz.x - 1) : (p.x % xsz.x + xsz.x) % xsz.x;\n"
	"  p.y = (u_xbr & 2) == 2 ? clamp(p.y, 0, xsz.y - 1) : (p.y % xsz.y + xsz.y) % xsz.y;\n"
	"  return texelFetch(u_tex, p, 0);\n"
	"}\n"
	"float Y(vec4 c) { return dot(c.rgb * c.a, vec3(14.352, 28.176, 5.472)) + 32.0 * c.a; }\n"
	"vec4 wd(vec4 a, vec4 b, vec4 c, vec4 d, vec4 e, vec4 f, vec4 g, vec4 h) {\n"
	"  return abs(a - b) + abs(a - c) + abs(d - e) + abs(d - f) + 4.0 * abs(g - h);\n"
	"}\n"
	"bvec4 ne(vec4 a, vec4 b) { return greaterThanEqual(abs(a - b), vec4(15.0)); }\n"
	"bvec4 eq(vec4 a, vec4 b) { return lessThan(abs(a - b), vec4(15.0)); }\n"
	"bvec4 and4(bvec4 a, bvec4 b) { return bvec4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w); }\n"
	"bvec4 or4(bvec4 a, bvec4 b) { return bvec4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w); }\n"
	// Catmull-Rom bicubic on premultiplied colors (sharper than bilinear, no blocky look).
	"vec4 cubic(vec2 uv) {\n"
	"  xsz = textureSize(u_tex, 0);\n"
	"  vec2 tc = uv * vec2(xsz) - 0.5;\n"
	"  xe = ivec2(floor(tc)); vec2 f = tc - floor(tc);\n"
	"  vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);\n"
	"  vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);\n"
	"  float wx[4] = float[4](w0.x, w1.x, w2.x, w3.x), wy[4] = float[4](w0.y, w1.y, w2.y, w3.y);\n"
	"  vec4 acc = vec4(0.0);\n"
	"  for (int j = 0; j < 4; j++) for (int k = 0; k < 4; k++) {\n"
	"    vec4 t = X(k - 1, j - 1); acc += vec4(t.rgb * t.a, t.a) * (wx[k] * wy[j]);\n"
	"  }\n"
	"  acc = clamp(acc, 0.0, 1.0);\n"
	"  return acc.a > 0.0 ? vec4(min(acc.rgb / acc.a, 1.0), acc.a) : vec4(0.0);\n"
	"}\n"
	// Sharp pixels: each texel a flat square, its edge blended over one screen pixel (the GPU's
	// bilinear does the blend: the sample moves to the texel's center except at its edge).
	"vec4 sharp(vec2 uv) {\n"
	"  vec2 sz = vec2(textureSize(u_tex, 0)), tc = uv * sz, k = 1.0 / max(fwidth(tc), vec2(1e-4));\n"
	"  vec2 cd = fract(tc) - 0.5, rr = max(0.5 - 0.5 / k, 0.0);\n"
	"  return texture(u_tex, (floor(tc) + (cd - clamp(cd, -rr, rr)) * k + 0.5) / sz);\n"
	"}\n"
	"vec4 xbr(vec2 uv) {\n"
	"  xsz = textureSize(u_tex, 0);\n"
	"  vec2 tc = uv * vec2(xsz);\n"
	"  xe = ivec2(floor(tc)); vec2 fp = tc - floor(tc);\n"
	"  vec4 A1 = X(-1,-2), B1 = X(0,-2), C1 = X(1,-2);\n"
	"  vec4 A0 = X(-2,-1), A = X(-1,-1), B = X(0,-1), C = X(1,-1), C4 = X(2,-1);\n"
	"  vec4 D0 = X(-2, 0), D = X(-1, 0), E = X(0, 0), F = X(1, 0), F4 = X(2, 0);\n"
	"  vec4 G0 = X(-2, 1), G = X(-1, 1), H = X(0, 1), I = X(1, 1), I4 = X(2, 1);\n"
	"  vec4 G5 = X(-1, 2), H5 = X(0, 2), I5 = X(1, 2);\n"
	"  vec4 b = vec4(Y(B), Y(D), Y(H), Y(F)), c = vec4(Y(C), Y(A), Y(G), Y(I));\n"
	"  vec4 e = vec4(Y(E)), d = b.yzwx, f = b.wxyz, g = c.zwxy, h = b.zwxy, i = c.wxyz;\n"
	"  vec4 i4 = vec4(Y(I4), Y(C1), Y(A0), Y(G5)), i5 = vec4(Y(I5), Y(C4), Y(A1), Y(G0));\n"
	"  vec4 h5 = vec4(Y(H5), Y(F4), Y(B1), Y(D0)), f4 = h5.yzwx;\n"
	"  vec4 fx = vec4(1.0, -1.0, -1.0, 1.0) * fp.y + vec4(1.0, 1.0, -1.0, -1.0) * fp.x;\n"
	"  vec4 fxl = vec4(1.0, -1.0, -1.0, 1.0) * fp.y + vec4(0.5, 2.0, -0.5, -2.0) * fp.x;\n"
	"  vec4 fxu = vec4(1.0, -1.0, -1.0, 1.0) * fp.y + vec4(2.0, 0.5, -2.0, -0.5) * fp.x;\n"
	"  bvec4 r0 = and4(ne(e, f), ne(e, h));\n"
	"  bvec4 r1 = and4(r0, or4(or4(and4(ne(f, b), ne(h, d)), and4(and4(eq(e, i), ne(f, i4)), ne(h, i5))), or4(eq(e, g), eq(e, c))));\n"
	"  bvec4 r2l = and4(ne(e, g), ne(d, g)), r2u = and4(ne(e, c), ne(b, c));\n"
	"  vec2 px_out = fwidth(tc); float dl = clamp(max(px_out.x, px_out.y), 0.05, 1.0);\n"
	"  vec4 delta = vec4(dl), deltaL = vec4(0.5, 1.0, 0.5, 1.0) * dl, deltaU = deltaL.yxwz;\n"
	"  vec4 fx45i = clamp((fx + delta - vec4(1.5, 0.5, -0.5, 0.5) - 0.25) / (2.0 * delta), 0.0, 1.0);\n"
	"  vec4 fx45 = clamp((fx + delta - vec4(1.5, 0.5, -0.5, 0.5)) / (2.0 * delta), 0.0, 1.0);\n"
	"  vec4 fx30 = clamp((fxl + deltaL - vec4(1.0, 1.0, -0.5, 0.0)) / (2.0 * deltaL), 0.0, 1.0);\n"
	"  vec4 fx60 = clamp((fxu + deltaU - vec4(2.0, 0.0, -1.0, 0.5)) / (2.0 * deltaU), 0.0, 1.0);\n"
	"  vec4 wd1 = wd(e, c, g, i, h5, f4, h, f), wd2 = wd(h, d, i5, f, i4, b, e, i);\n"
	"  bvec4 edri = and4(lessThanEqual(wd1, wd2), r0), edr = and4(lessThan(wd1, wd2), r1);\n"
	"  bvec4 edrl = and4(and4(lessThanEqual(2.0 * abs(f - g), abs(h - c)), r2l), edr);\n"
	"  bvec4 edru = and4(and4(greaterThanEqual(abs(f - g), 2.0 * abs(h - c)), r2u), edr);\n"
	"  fx45 *= vec4(edr); fx30 *= vec4(edrl); fx60 *= vec4(edru); fx45i *= vec4(edri);\n"
	"  bvec4 px = lessThanEqual(abs(e - f), abs(e - h));\n"
	"  vec4 m = max(max(fx30, fx60), max(fx45, fx45i));\n"
	"  vec4 r1c = mix(E, px.x ? F : H, m.x); r1c = mix(r1c, px.z ? D : B, m.z);\n"
	"  vec4 r2c = mix(E, px.y ? B : F, m.y); r2c = mix(r2c, px.w ? H : D, m.w);\n"
	"  vec4 d1 = abs(E - r1c), d2 = abs(E - r2c);\n"
	"  return dot(d1, vec4(1.0)) >= dot(d2, vec4(1.0)) ? r1c : r2c;\n"
	"}\n"
	"void main() {\n"
	"  vec4 c = v_c0;\n"
	"  if (u_clearMode == 0) {\n"
	"    if (u_texEnable == 1) {\n"
	// a sprite samples within its own rectangle: above 1x, filtering at its edge would blend in the
	// next sprite of the atlas (lines along the edges)
	"      vec2 uv = clamp(v_uv, v_box.xy, v_box.zw);\n"
	// (a slight bias toward the full-size picture, -0.25: a tutorial page the game draws at 0.6x
	// stays sharp instead of mixing in the half-size picture, and the 3D yards keep their outlines
	// letter by letter; -1 lost them again, 0 blurred the page)
	// xBR and the sharp pixels only where the texture shows larger than itself: smaller (far away)
	// they would shimmer, and the plain sampling stands
	"      vec2 tfw = fwidth(uv * u_uvScale * vec2(textureSize(u_tex, 0))); bool mag = max(tfw.x, tfw.y) < 1.0;\n"
	"      vec4 t = u_xbr >= 12 && mag ? sharp(uv * u_uvScale) : u_xbr >= 12 ? texture(u_tex, uv * u_uvScale, u_lodBias)\n"
	"             : u_xbr >= 8 ? cubic(uv * u_uvScale) : u_xbr >= 4 && mag ? xbr(uv * u_uvScale) : texture(u_tex, uv * u_uvScale, u_lodBias);\n"
	"      vec3 rgb; float a = c.a;\n"
	"      if (u_tfunc == 0) { rgb = c.rgb * t.rgb; if (u_tfuncRGBA == 1) a = c.a * t.a; }\n"
	"      else if (u_tfunc == 1) { rgb = u_tfuncRGBA == 1 ? mix(c.rgb, t.rgb, t.a) : t.rgb; }\n"
	"      else if (u_tfunc == 2) { rgb = mix(c.rgb, u_envColor.rgb, t.rgb); if (u_tfuncRGBA == 1) a = c.a * t.a; }\n"
	"      else if (u_tfunc == 3) { rgb = t.rgb; if (u_tfuncRGBA == 1) a = t.a; }\n"
	"      else { rgb = c.rgb + t.rgb; if (u_tfuncRGBA == 1) a = c.a * t.a; }\n"
	"      if (u_colorDouble == 1) rgb *= 2.0;\n"
	"      c = vec4(rgb, a);\n"
	"    }\n"
	"    c.rgb += v_c1;\n"
	"    if (u_alphaTest == 1) {\n"
	"      float A = floor(clamp(c.a, 0.0, 1.0) * 255.0 + 0.5);\n"
	"      bool pass = u_atFunc == 0 ? false : u_atFunc == 1 ? true : u_atFunc == 2 ? A == u_atRef :\n"
	"                  u_atFunc == 3 ? A != u_atRef : u_atFunc == 4 ? A < u_atRef : u_atFunc == 5 ? A <= u_atRef :\n"
	"                  u_atFunc == 6 ? A > u_atRef : A >= u_atRef;\n"
	"      if (!pass) discard;\n"
	"    }\n"
	// Color test (0 never, 1 always, 2 equal, 3 not equal): games use it as a color key, e.g. the
	// ball shadow is a dark disc on an opaque black square whose black pixels must be dropped.
	"    if (u_ctFunc != 1) {\n"
	"      ivec3 C = ivec3(floor(clamp(c.rgb, 0.0, 1.0) * 255.0 + 0.5));\n"
	"      bool eq = all(equal(C & u_ctMask, u_ctRef & u_ctMask));\n"
	"      if (u_ctFunc == 0 || (u_ctFunc == 2) != eq) discard;\n"
	"    }\n"
	"    if (u_fogEnable == 1) c.rgb = mix(u_fogColor, c.rgb, clamp(v_fog, 0.0, 1.0));\n"
	"  }\n"
	"  o = clamp(c, 0.0, 1.0);\n"
	"  if (u_blendDouble == 1) o.rgb = min(o.rgb * 2.0, vec3(1.0));\n"
	"}\n";

static GLuint compile(GLenum type, const char *src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	GLint ok;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[2048];
		glGetShaderInfoLog(s, sizeof log, NULL, log);
		rt_fatal("shader compile: %s", log);
	}
	return s;
}

void ge_init(void) {
	texpack_init();
	prog = glCreateProgram();
	glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
	glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
	glLinkProgram(prog);
	GLint ok;
	glGetProgramiv(prog, GL_LINK_STATUS, &ok);
	if (!ok) {
		rt_fatal("shader link failed");
	}
#define U(n) n = glGetUniformLocation(prog, #n)
	U(u_tex); U(u_texEnable); U(u_tfunc); U(u_tfuncRGBA); U(u_colorDouble); U(u_envColor); U(u_alphaTest);
	U(u_atFunc); U(u_atRef); U(u_fogEnable); U(u_fogColor); U(u_clearMode); U(u_uvScale); U(u_blendDouble);
	U(u_ctFunc); U(u_ctRef); U(u_ctMask); U(u_xbr); U(u_lodBias);
#undef U
	glGenVertexArrays(1, &vao);
	glBindVertexArray(vao);
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(GVert), (void *)offsetof(GVert, x));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(GVert), (void *)offsetof(GVert, u));
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GVert), (void *)offsetof(GVert, c0));
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GVert), (void *)offsetof(GVert, c1));
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(GVert), (void *)offsetof(GVert, fog));
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(GVert), (void *)offsetof(GVert, box));
	glUseProgram(prog);
	glUniform1i(u_tex, 0);
	// mipmap level bias (only textures with mipmaps, the pack's, are affected); PANGYA_LOD_BIAS
	const char *bias = getenv("PANGYA_LOD_BIAS");
	glUniform1f(u_lodBias, bias ? (float)atof(bias) : -0.25f);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	reg[0xC5] = 0xFF00;  // CLUT mask
	gedebug = getenv("PANGYA_GEDEBUG") != NULL;
	// Texture sampling modes (filter and wrap per axis), as sampler objects.
	glGenSamplers(32, samplers);
	for (int i = 0; i < 32; i++) {
		glSamplerParameteri(samplers[i], GL_TEXTURE_MIN_FILTER, (i & 16) ? GL_LINEAR_MIPMAP_LINEAR : (i & 1) ? GL_LINEAR : GL_NEAREST);
		glSamplerParameteri(samplers[i], GL_TEXTURE_MAG_FILTER, (i & 2) ? GL_LINEAR : GL_NEAREST);
		glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_S, (i & 4) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
		glSamplerParameteri(samplers[i], GL_TEXTURE_WRAP_T, (i & 8) ? GL_CLAMP_TO_EDGE : GL_REPEAT);
	}
	// Render scale: framebuffers are rs times the PSP's 480x272 (sharper 3D, smoother edges once
	// scaled down to the window). PANGYA_SCALE=1..6.
	const char *sc = getenv("PANGYA_SCALE");
	rs = sc ? atoi(sc) : 4;
	if (rs < 1) rs = 1;
	if (rs > max_scale()) rs = max_scale();
	if (getenv("PANGYA_TEX_FILTER")) ge_set_tex_filter(atoi(getenv("PANGYA_TEX_FILTER")));
	const char *uf = getenv("PANGYA_UI_FILTER");
	if (uf) ui_filter = atoi(uf) >= 0 && atoi(uf) <= 2 ? atoi(uf) : 0;
	static const char *const ui_names[] = {"off", "xBR", "bicubic"};
	rt_log("GE: rendering at %dx (%dx%d), interface filter %s", rs, 480 * rs, 272 * rs, ui_names[rs > 1 ? ui_filter : 0]);
}

// ---------------------------------------------------------------------------------------------
// Framebuffers
// ---------------------------------------------------------------------------------------------
static Fbo *fbo_find(uint32_t vaddr) {
	for (int i = 0; i < MAX_FBOS; i++) {
		if (fbos[i].used && fbos[i].addr == vaddr) {
			return &fbos[i];
		}
	}
	return NULL;
}

static void batch_flush(void);
static void gl_invalidate(void);
static uint64_t hash_bytes(const uint8_t *p, size_t n);

static void fbo_free(Fbo *f) {
	glDeleteFramebuffers(1, &f->fbo);
	glDeleteTextures(1, &f->tex);
	glDeleteRenderbuffers(1, &f->depth);
	memset(f, 0, sizeof *f);
}

static uint64_t copy_hash(const Fbo *f) {
	uint32_t a = PSP_VRAM_BASE + f->addr, n = (uint32_t)(f->width * f->height * 4);
	return rt_valid(a, n) ? hash_bytes(MP(a), n) : 0;
}

// A 1x scratch color target of at least w x h, for moving pixels between PSP-resolution memory
// and the scaled framebuffers.
static GLuint tmp_fbo, tmp_tex;
static int tmp_w, tmp_h;
static void tmp_target(int w, int h) {
	if (w <= tmp_w && h <= tmp_h) return;
	tmp_w = w > tmp_w ? w : tmp_w;
	tmp_h = h > tmp_h ? h : tmp_h;
	if (!tmp_tex) glGenTextures(1, &tmp_tex), glGenFramebuffers(1, &tmp_fbo);
	glBindTexture(GL_TEXTURE_2D, tmp_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tmp_w, tmp_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glBindFramebuffer(GL_FRAMEBUFFER, tmp_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp_tex, 0);
}

// Copies PSP-resolution RGBA8 pixels (rows of row_len pixels) into a framebuffer rectangle.
static void fb_upload(Fbo *f, int x, int y, int w, int h, const void *px, int row_len) {
	batch_flush();
	glDisable(GL_SCISSOR_TEST);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, row_len);
	if (rs == 1) {
		glBindTexture(GL_TEXTURE_2D, f->tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	}
	else {
		tmp_target(w, h);
		glBindTexture(GL_TEXTURE_2D, tmp_tex);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, tmp_fbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->fbo);
		glBlitFramebuffer(0, 0, w, h, x * rs, y * rs, (x + w) * rs, (y + h) * rs, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	gl_invalidate();
}

// Reads a framebuffer rectangle back at PSP resolution.
static void fb_read(GLuint fbo, int x, int y, int w, int h, void *dst, int row_len) {
	batch_flush();
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	if (rs != 1) {
		tmp_target(w, h);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, tmp_fbo);
		glBlitFramebuffer(x * rs, y * rs, (x + w) * rs, (y + h) * rs, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, tmp_fbo);
		x = y = 0;
	}
	glPixelStorei(GL_PACK_ROW_LENGTH, row_len);
	glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
	glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	gl_invalidate();
}

// GL objects of a framebuffer at the current render scale (depth cleared, contents undefined).
static void fbo_alloc(Fbo *f) {
	glGenTextures(1, &f->tex);
	glBindTexture(GL_TEXTURE_2D, f->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f->width * rs, f->height * rs, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glGenRenderbuffers(1, &f->depth);
	glBindRenderbuffer(GL_RENDERBUFFER, f->depth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, f->width * rs, f->height * rs);
	glGenFramebuffers(1, &f->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->tex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, f->depth);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		rt_fatal("FBO incomplete for %08x %dx%d", f->addr, f->width, f->height);
	}
	glDisable(GL_SCISSOR_TEST);
	glDepthMask(GL_TRUE);
	glClearDepthf(0);
	glClear(GL_DEPTH_BUFFER_BIT);
}

static int max_scale(void) {
	GLint max_rb = 0;
	glGetIntegerv(GL_MAX_RENDERBUFFER_SIZE, &max_rb);
	int n = 6;
	while (n > 1 && 512 * n > max_rb) n--;
	return n;
}

// Changes the render scale while running: every framebuffer is recreated at the new size with
// its picture scaled over (the depth buffers start cleared; the next frame redraws them anyway).
void ge_set_render_scale(int n) {
	if (n < 1) n = 1;
	if (n > max_scale()) n = max_scale();
	if (n == rs) return;
	batch_flush();
	int old = rs;
	rs = n;
	for (int i = 0; i < MAX_FBOS; i++) {
		Fbo *f = &fbos[i];
		if (!f->used) continue;
		GLuint ofbo = f->fbo, otex = f->tex, odepth = f->depth;
		fbo_alloc(f);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, ofbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->fbo);
		glBlitFramebuffer(0, 0, f->width * old, f->height * old, 0, 0, f->width * rs, f->height * rs, GL_COLOR_BUFFER_BIT,
		                  GL_LINEAR);
		glDeleteFramebuffers(1, &ofbo);
		glDeleteTextures(1, &otex);
		glDeleteRenderbuffers(1, &odepth);
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	// Every texture is decoded again: what was drawn for the old scale is drawn for the new one.
	for (int i = 0; i < MAX_TEX; i++) texcache[i].hash = 0, texcache[i].checked = 0;
	gl_invalidate();
	rt_log("GE: rendering at %dx (%dx%d)", rs, 480 * rs, 272 * rs);
}

int ge_tex_filter(void) { return tex_filter; }

void ge_set_tex_filter(int mode) {
	if (mode < 0 || mode > 2) mode = 0;
	if (mode != tex_filter) rt_log("GE: the game's own textures %s", mode == 1 ? "through xBR" : mode == 2 ? "as sharp pixels" : "as the GE samples them");
	tex_filter = mode;
}

void ge_set_ui_filter(int mode) {
	if (mode < 0 || mode > 2) mode = 2;
	ui_filter = mode;
}

static Fbo *fbo_get(uint32_t vaddr, int width, int height) {
	Fbo *f = fbo_find(vaddr);
	if (f && f->width == width && f->height >= height) {
		return f;
	}
	batch_flush();  // queued draws may target or sample the framebuffer being replaced
	if (!f) {
		for (int i = 0; i < MAX_FBOS && !f; i++) {
			if (!fbos[i].used) f = &fbos[i];
		}
		if (!f) {  // evict the least recently drawn
			f = &fbos[0];
			for (int i = 1; i < MAX_FBOS; i++) {
				if (fbos[i].last_draw < f->last_draw) f = &fbos[i];
			}
		}
	}
	if (f->used) {
		glDeleteFramebuffers(1, &f->fbo);
		glDeleteTextures(1, &f->tex);
		glDeleteRenderbuffers(1, &f->depth);
	}
	memset(f, 0, sizeof *f);
	f->used = true;
	f->addr = vaddr;
	f->width = width;
	f->height = height;
	fbo_alloc(f);
	// Start from VRAM so whatever the CPU put there is preserved.
	uint8_t *init = calloc((size_t)width * height, 4);
	for (int y = 0; y < height; y++) {
		uint32_t row = PSP_VRAM_BASE + vaddr + (uint32_t)(y * width * 4);
		if (rt_valid(row, (uint32_t)width * 4)) memcpy(init + (size_t)y * width * 4, MP(row), (size_t)width * 4);
	}
	fb_upload(f, 0, 0, width, height, init, width);
	free(init);
	gl_invalidate();
	TRACE("GE: new framebuffer %08x %dx%d", vaddr, width, height);
	return f;
}

static void bind_target(void) {
	uint32_t vaddr = (((reg[0x9D] & 0xFF0000u) << 8) | (reg[0x9C] & 0xFFFFFFu)) & 0x001FFFFFu;
	int stride = (int)(reg[0x9D] & 0x7FF);
	if (stride <= 0) stride = 512;
	int sy2 = (int)((reg[0xD5] >> 10) & 0x3FF);
	int height = sy2 >= 272 ? 512 : 272;
	cur_fbo = fbo_get(vaddr, stride, height);
	cur_fbo->last_draw = ++draw_counter;
	cur_fbo->copy = false;
}

// ---------------------------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------------------------
static inline uint32_t expand565(uint16_t v) {
	uint32_t r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
	return 0xFF000000u | ((b << 3 | b >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (r << 3 | r >> 2);
}
static inline uint32_t expand5551(uint16_t v) {
	uint32_t r = v & 31, g = (v >> 5) & 31, b = (v >> 10) & 31;
	return ((v >> 15) ? 0xFF000000u : 0) | ((b << 3 | b >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (r << 3 | r >> 2);
}
static inline uint32_t expand4444(uint16_t v) {
	uint32_t r = v & 15, g = (v >> 4) & 15, b = (v >> 8) & 15, a = v >> 12;
	return ((a * 17) << 24) | ((b * 17) << 16) | ((g * 17) << 8) | (r * 17);
}
static uint32_t conv16(uint16_t v, int fmt) {
	return fmt == 0 ? expand565(v) : fmt == 1 ? expand5551(v) : expand4444(v);
}

static uint32_t clut_lookup(uint32_t index) {
	uint32_t cm = reg[0xC5];
	int fmt = cm & 3, shift = (cm >> 2) & 0x1F;
	uint32_t mask = (cm >> 8) & 0xFF, start = ((cm >> 16) & 0x1F) << 4;
	uint32_t i = ((index >> shift) & mask) | start;
	if (fmt == 3) {
		uint32_t v;
		memcpy(&v, clut + (i & 0xFF) * 4, 4);
		return v;
	}
	uint16_t v;
	memcpy(&v, clut + (i & 0x1FF) * 2, 2);
	return conv16(v, fmt);
}

static uint64_t hash_bytes(const uint8_t *p, size_t n) {
	uint64_t h = 1469598103934665603ull;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t w;
		memcpy(&w, p + i, 8);
		h = (h ^ w) * 1099511628211ull;
		h ^= h >> 29;
	}
	for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

// Returns a GL texture for texture level 0 and sets *w/*h.
static bool tex_from_fbo;  // the last texture_get returned a render target
static float tex_scale;    // ... or a texture this many times finer than the GE's (TexEntry.scale)
static bool tex_mips;      // ... with its mipmaps (TexEntry.mips)

// A replacement texture (pw x ph for the iw x ih texels of a w x h texture) into e->tex at its own
// scale; texels past the picture repeat its last column and row. False if the GPU can't hold it.
static bool pack_upload(TexEntry *e, const uint32_t *px, int pw, int ph, int iw, int ih, int w, int h) {
	static GLint max_size;
	if (!max_size) glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
	int s = pw / iw, W = w * s, H = h * s;
	if (W > max_size || H > max_size) return false;
	uint32_t *full = NULL;
	if (W != pw || H != ph) {
		full = malloc((size_t)W * H * 4);
		for (int y = 0; y < H; y++) {
			const uint32_t *row = px + (size_t)(y < ph ? y : ph - 1) * pw;
			uint32_t *d = full + (size_t)y * W;
			memcpy(d, row, (size_t)pw * 4);
			for (int x = pw; x < W; x++) d[x] = row[pw - 1];
		}
	}
	if (!e->tex) glGenTextures(1, &e->tex);
	glBindTexture(GL_TEXTURE_2D, e->tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, full ? full : px);
	// Shown smaller than its own size (3D billboards, far sprites), a 4x picture sampled without
	// mipmaps skips texels: thin outlines come and go letter by letter.
	glGenerateMipmap(GL_TEXTURE_2D);
	gl_invalidate();
	free(full);
	e->scale = (float)s;
	e->mips = true;
	return true;
}

// A replacement that was on its way: shown once it has arrived (checked once a frame). A big one
// takes a while to go to the GPU (a 2048x2048 picture is 16 MB, with its mipmaps), and a new place
// asks for many at once: past PACK_FRAME_US in a frame, the rest wait for the next frames (their
// originals show until then) instead of holding this one up.
#define PACK_FRAME_US 6000
static void pack_poll(TexEntry *e) {
	static uint64_t spent_frame, spent_us;
	if (e->pack_frame == frame_no) return;
	if (spent_frame != frame_no) spent_frame = frame_no, spent_us = 0;
	if (spent_us >= PACK_FRAME_US) return;
	e->pack_frame = frame_no;
	uint64_t t0 = rt_now_us();
	int pw, ph, iw, ih;
	bool failed;
	uint32_t *px = texpack_load(e->pack1 - 1, &pw, &ph, &iw, &ih, &failed);
	if (!px) {
		if (failed) e->pack1 = 0, free(e->ui_orig), e->ui_orig = NULL;
		return;
	}
	uint64_t t1 = rt_now_us();
	batch_flush();  // queued draws still show the original
	const uint32_t *over = e->ui_orig ? uitext_over(e->ui_orig, e->w, e->h, rs, px, pw, ph, iw, ih) : NULL;
	if (over) {  // a UI atlas: the picture with text.h's words over it
		glBindTexture(GL_TEXTURE_2D, e->tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, e->w * rs, e->h * rs, 0, GL_RGBA, GL_UNSIGNED_BYTE, over);
		gl_invalidate();
		e->scale = (float)rs;
		e->mips = false;
	} else {  // (text.h stopped drawing the atlas meanwhile: the picture as it is)
		pack_upload(e, px, pw, ph, iw, ih, e->w, e->h);
	}
	free(e->ui_orig);
	e->ui_orig = NULL;
	free(px);
	e->pack1 = 0;
	uint64_t t2 = rt_now_us();
	spent_us += t2 - t0;
	static int slow_log = -1;  // PANGYA_TEXPACK_SLOW=ms: the pictures that took longer than that
	if (slow_log < 0) slow_log = getenv("PANGYA_TEXPACK_SLOW") ? atoi(getenv("PANGYA_TEXPACK_SLOW")) : 0;
	if (slow_log && t2 - t0 >= (uint64_t)slow_log * 1000)
		rt_log("texpack: %dx%d in %.1f ms (read %.1f, to the GPU %.1f)", pw, ph, (t2 - t0) / 1000.0, (t1 - t0) / 1000.0, (t2 - t1) / 1000.0);
}

static GLuint texture_get(int *tw, int *th, float *uvsx, float *uvsy) {
	uint32_t addr = ((reg[0xA8] & 0x0F0000u) << 8) | (reg[0xA0] & 0xFFFFFFu);
	int stride = (int)(reg[0xA8] & 0x7FF);
	int w = 1 << (reg[0xB8] & 0xF), h = 1 << ((reg[0xB8] >> 8) & 0xF);
	int fmt = (int)(reg[0xC3] & 0xF);
	bool swz = reg[0xC2] & 1;
	*tw = w;
	*th = h;
	*uvsx = *uvsy = 1.f;
	tex_from_fbo = false;
	tex_scale = 1.f;
	tex_mips = false;
	if (w > 1024) w = 1024;
	if (h > 1024) h = 1024;
	// The buffer width (row pitch) may be narrower than the power-of-two texture size; texels
	// past it come from the following rows, exactly as the GE reads them.
	if (stride <= 0) stride = w;

	// Render-to-texture: the address is a framebuffer we drew into.
	if ((addr & 0x0F000000u) == PSP_VRAM_BASE || (addr & 0x0FE00000u) == 0x04000000u) {
		Fbo *f = fbo_find(addr & 0x001FFFFFu);
		if (f && f->copy && f->copy_checked != frame_no) {  // the CPU may have reused that VRAM
			f->copy_checked = frame_no;
			if (copy_hash(f) != f->copy_hash) {
				batch_flush();
				fbo_free(f);
				f = NULL;
			}
		}
		if (f && f->last_draw) {
			tex_from_fbo = true;
			*uvsx = (float)*tw / (float)f->width;
			*uvsy = (float)*th / (float)f->height;
			return f->tex;
		}
	}

	static const int bpp_tab[16] = {16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8, 0, 0, 0, 0, 0};
	int bpp = bpp_tab[fmt];
	if (!bpp || !rt_valid(addr, 1)) {
		return 0;
	}
	size_t row_bytes = (size_t)stride * bpp / 8;
	size_t bytes = fmt >= 8 ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * (fmt == 8 ? 8 : 16) : row_bytes * h;
	if (!rt_valid(addr, (uint32_t)bytes)) {
		// Textures may extend past the end of VRAM/RAM (e.g. a 512x512 texture over a 480x272
		// movie frame near the top of VRAM); only the rows that exist are read.
		uint32_t a = addr & MEM_MASK;
		uint32_t limit = (a >= PSP_VRAM_BASE && a < PSP_VRAM_BASE + PSP_VRAM_SIZE) ? PSP_VRAM_BASE + PSP_VRAM_SIZE : PSP_RAM_END;
		if (fmt >= 8 || a >= limit || row_bytes == 0) return 0;
		int rows = (int)((limit - a) / row_bytes);
		if (rows <= 0) return 0;
		bytes = row_bytes * (size_t)(rows < h ? rows : h);  // missing rows decode as zero
	}
	const uint8_t *src = MP(addr);
	uint64_t key = ((uint64_t)addr << 32) ^ ((uint64_t)fmt << 28) ^ ((uint64_t)w << 16) ^ ((uint64_t)h << 4) ^ (swz ? 1 : 0) ^
	               ((uint64_t)stride << 40);
	uint64_t clut_hash = 0;
	if (fmt >= 4 && fmt <= 7) {
		if (clut_dirty) {  // hashed again only after a CLUT load
			clut_h64 = hash_bytes(clut, 64);
			clut_h1k = hash_bytes(clut, 1024);
			clut_dirty = false;
		}
		clut_hash = (fmt == 4 ? clut_h64 : clut_h1k) * 31 + reg[0xC5];
		key ^= clut_hash * 0x9E3779B97F4A7C15ull;
	}
	// Consecutive draws usually use the same texture.
	static int last_slot = -1;
	if (last_slot >= 0) {
		TexEntry *e = &texcache[last_slot];
		if (e->tex && e->key == key && e->checked == frame_no) {
			if (e->pack1) pack_poll(e);
			e->last_use = frame_no;
			tex_scale = e->scale;
			tex_mips = e->mips;
			return e->tex;
		}
	}
	// Texture data is re-hashed at most once per frame.
	TexEntry *slot = NULL, *lru = &texcache[0];
	for (int i = 0; i < MAX_TEX; i++) {
		TexEntry *e = &texcache[i];
		if (e->tex && e->key == key) {
			if (e->checked == frame_no) {
				if (e->pack1) pack_poll(e);
				e->last_use = frame_no;
				last_slot = i;
				tex_scale = e->scale;
				tex_mips = e->mips;
				return e->tex;
			}
			slot = e;
			break;
		}
		if (e->last_use < lru->last_use) lru = e;
	}
	uint64_t hash = hash_bytes(src, bytes);
	if (slot && slot->hash == hash) {
		if (slot->pack1) pack_poll(slot);
		slot->checked = slot->last_use = frame_no;
		last_slot = (int)(slot - texcache);
		tex_scale = slot->scale;
		tex_mips = slot->mips;
		return slot->tex;
	}
	// The same texture with new contents: how many frames in a row that happened (movie frames).
	if (slot) slot->changes = frame_no - slot->changed_frame <= 2 ? (slot->changes < 255 ? slot->changes + 1 : 255) : 0;
	else slot = lru, slot->changes = 0;
	slot->changed_frame = frame_no;
	slot->pack1 = 0;
	free(slot->ui_orig);
	slot->ui_orig = NULL;
	last_slot = (int)(slot - texcache);
	batch_flush();  // queued draws may use the GL texture about to be (re)filled

	// A string from the game's font engine: drawn again at the render scale (text.h, if the build
	// has it and it is on; else the HD font of the texture pack, if there is one).
	const uint32_t *hi = fmt == 3 && !swz && rs > 1 ? text_hires(addr, src, bytes, w, h, stride, rs) : NULL;
	if (!hi && fmt == 3 && !swz && rs > 1) hi = fonthd_hires(addr, src, bytes, w, h, stride, rs);
	static int font_log = -1;
	if (font_log < 0) font_log = getenv("PANGYA_FONTHD_LOG") != NULL;
	if (font_log && fmt == 3) rt_log("fonthd: %s RGBA texture at %08x %dx%d (stride %d)%s", hi ? "drew" : "left", addr, w, h, stride, swz ? " swizzled" : "");
	if (hi) {
		if (!slot->tex) glGenTextures(1, &slot->tex);
		glBindTexture(GL_TEXTURE_2D, slot->tex);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * rs, h * rs, 0, GL_RGBA, GL_UNSIGNED_BYTE, hi);
		gl_invalidate();
		slot->key = key;
		slot->hash = hash;
		slot->checked = slot->last_use = frame_no;
		slot->w = w;
		slot->h = h;
		slot->scale = tex_scale = (float)rs;
		slot->mips = tex_mips = false;
		return slot->tex;
	}

	// Linear copy (unswizzled if needed: blocks of 16 bytes x 8 rows), padded so reads past the
	// last row's pitch stay in bounds.
	size_t pad = (size_t)w * 4 + 16;
	uint8_t *unswz = calloc(1, row_bytes * h + pad);
	const uint8_t *lin = unswz;
	if (fmt < 8) {
		if (swz) {
			size_t bw = row_bytes / 16;
			for (int y = 0; y < h; y++) {
				for (size_t xb = 0; xb < row_bytes; xb++) {
					size_t blk = (size_t)(y / 8) * bw + xb / 16;
					size_t so = blk * 128 + (size_t)(y % 8) * 16 + xb % 16;
					if (so < bytes) unswz[(size_t)y * row_bytes + xb] = src[so];
				}
			}
		}
		else {
			memcpy(unswz, src, bytes);
		}
	}

	uint32_t *out = malloc((size_t)w * h * 4);
	for (int y = 0; y < h && fmt < 8; y++) {
		const uint8_t *row = lin + (size_t)y * row_bytes;
		for (int x = 0; x < w; x++) {
			uint32_t px;
			switch (fmt) {
			case 0: case 1: case 2: { uint16_t v; memcpy(&v, row + x * 2, 2); px = conv16(v, fmt); break; }
			case 3: memcpy(&px, row + x * 4, 4); break;
			case 4: px = clut_lookup((row[x / 2] >> ((x & 1) * 4)) & 0xF); break;
			case 5: px = clut_lookup(row[x]); break;
			case 6: { uint16_t v; memcpy(&v, row + x * 2, 2); px = clut_lookup(v); break; }
			default: { uint32_t v; memcpy(&v, row + x * 4, 4); px = clut_lookup(v); break; }
			}
			out[y * w + x] = px;
		}
	}
	if (fmt >= 8) {  // PSP DXT blocks: indices first, then the two 565 colors (alpha after for DXT3/5)
		int bs = fmt == 8 ? 8 : 16;
		for (int by = 0; by < (h + 3) / 4; by++) {
			for (int bx = 0; bx < (w + 3) / 4; bx++) {
				const uint8_t *b = src + ((size_t)by * ((w + 3) / 4) + bx) * bs;
				const uint8_t *cb = b;
				uint32_t lines;
				uint16_t c1, c2;
				memcpy(&lines, cb, 4);
				memcpy(&c1, cb + 4, 2);
				memcpy(&c2, cb + 6, 2);
				uint32_t pal[4];
				pal[0] = expand565(c1);
				pal[1] = expand565(c2);
				for (int k = 0; k < 3; k++) {
					uint32_t a = (pal[0] >> (k * 8)) & 0xFF, bb = (pal[1] >> (k * 8)) & 0xFF;
					uint32_t m2, m3;
					if (c1 > c2 || fmt != 8) {
						m2 = (2 * a + bb) / 3;
						m3 = (a + 2 * bb) / 3;
					}
					else {
						m2 = (a + bb) / 2;
						m3 = 0;
					}
					pal[2] = (k ? pal[2] : 0xFF000000u) | (m2 << (k * 8));
					pal[3] = (k ? pal[3] : ((c1 > c2 || fmt != 8) ? 0xFF000000u : 0)) | (m3 << (k * 8));
				}
				for (int py = 0; py < 4; py++)
					for (int px2 = 0; px2 < 4; px2++) {
						int x = bx * 4 + px2, y = by * 4 + py;
						if (x >= w || y >= h) continue;
						uint32_t c = pal[(lines >> ((py * 4 + px2) * 2)) & 3];
						if (fmt == 9) {
							uint16_t al;
							memcpy(&al, b + 8 + py * 2, 2);
							c = (c & 0xFFFFFFu) | (((al >> (px2 * 4)) & 0xF) * 17u) << 24;
						}
						else if (fmt == 10) {
							uint8_t a0 = b[14], a1 = b[15];
							uint64_t bits = 0;
							memcpy(&bits, b + 8, 6);
							int idx = (int)((bits >> ((py * 4 + px2) * 3)) & 7);
							uint32_t a;
							if (idx == 0) a = a0;
							else if (idx == 1) a = a1;
							else if (a0 > a1) a = ((8 - idx) * a0 + (idx - 1) * a1) / 7;
							else a = idx == 6 ? 0 : idx == 7 ? 255 : ((6 - idx) * a0 + (idx - 1) * a1) / 5;
							c = (c & 0xFFFFFFu) | a << 24;
						}
						out[y * w + x] = c;
					}
			}
		}
	}
	free(unswz);

	// A replacement from the texture pack (the HD textures) takes the place of everything below.
	static int pack_log = -1;
	if (pack_log < 0) pack_log = getenv("PANGYA_TEXPACK_LOG") != NULL;
	if (pack_log && slot->changes < 4)
		rt_log("texpack: upload %016llx %dx%d fmt %d at %08x", (unsigned long long)texpack_key(out, w, w, h), w, h, fmt, addr);
	// A UI atlas with words that text.h draws again: with the pack's picture too, the picture with
	// those words over it (uitext_over), else the redrawn atlas below.
	bool claimed = rs > 1 && fmt >= 4 && fmt <= 7 && slot->changes < 4 && uitext_claims(out, w, h);
	int pk = slot->changes < 4 ? texpack_find(out, w, h) : -1;
	if (pk >= 0 && claimed) {
		int pw, ph, iw, ih;
		bool failed;
		uint32_t *px = texpack_load(pk, &pw, &ph, &iw, &ih, &failed);
		const uint32_t *over = px ? uitext_over(out, w, h, rs, px, pw, ph, iw, ih) : NULL;
		free(px);
		if (over) {
			if (!slot->tex) glGenTextures(1, &slot->tex);
			glBindTexture(GL_TEXTURE_2D, slot->tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * rs, h * rs, 0, GL_RGBA, GL_UNSIGNED_BYTE, over);
			gl_invalidate();
			free(out);
			slot->key = key, slot->hash = hash, slot->w = w, slot->h = h;
			slot->checked = slot->last_use = frame_no;
			slot->scale = tex_scale = (float)rs;
			slot->mips = tex_mips = false;
			return slot->tex;
		}
		if (!px && !failed) {  // on its way (web): the redrawn atlas until then
			slot->pack1 = pk + 1;
			slot->ui_orig = malloc(sizeof *out * (size_t)w * h);
			if (slot->ui_orig) memcpy(slot->ui_orig, out, sizeof *out * (size_t)w * h);
			else slot->pack1 = 0;
		}
		pk = -1;
	}
	if (pk >= 0) {
		int pw, ph, iw, ih;
		bool failed;
		uint32_t *px = texpack_load(pk, &pw, &ph, &iw, &ih, &failed);
		slot->w = w, slot->h = h;
		bool shown = px && pack_upload(slot, px, pw, ph, iw, ih, w, h);
		free(px);
		if (shown) {
			if (pack_log) rt_log("texpack: replaced %016llx (%dx%d of %dx%d) at %dx", (unsigned long long)texpack_key(out, w, iw, ih), iw, ih, w, h, (int)slot->scale);
			free(out);
			slot->key = key;
			slot->hash = hash;
			slot->checked = slot->last_use = frame_no;
			tex_scale = slot->scale;
			tex_mips = slot->mips;
			return slot->tex;
		}
		if (!px && !failed) slot->pack1 = pk + 1;  // on its way (web): the original until then
	}
	static const char *texdump = (const char *)1;
	if (texdump == (const char *)1) texdump = getenv("PANGYA_TEXDUMP");
	if (texdump) {
		static int n;
		char path[512];
		snprintf(path, sizeof path, "%s/tex%04d_%08x_f%d_%dx%d_s%d%s.ppm", texdump, n++, addr, fmt, w, h, stride, swz ? "_swz" : "");
		FILE *df = fopen(path, "wb");
		if (df) {
			fprintf(df, "P6\n%d %d\n255\n", w, h);
			for (int i = 0; i < w * h; i++) {
				uint8_t rgb[3] = {(uint8_t)out[i], (uint8_t)(out[i] >> 8), (uint8_t)(out[i] >> 16)};
				fwrite(rgb, 1, 3, df);
			}
			fclose(df);
		}
		snprintf(path, sizeof path, "%s/tex%04d_alpha.pgm", texdump, n - 1);
		df = fopen(path, "wb");
		if (df) {
			fprintf(df, "P5\n%d %d\n255\n", w, h);
			for (int i = 0; i < w * h; i++) fputc((int)(out[i] >> 24), df);
			fclose(df);
		}
		snprintf(path, sizeof path, "%s/tex%04d.raw", texdump, n - 1);
		df = fopen(path, "wb");
		if (df) {
			fwrite(src, 1, bytes, df);
			fclose(df);
		}
		snprintf(path, sizeof path, "%s/tex%04d.clut", texdump, n - 1);
		df = fopen(path, "wb");
		if (df) {
			fwrite(clut, 1, sizeof clut, df);
			fclose(df);
		}
	}
	// A UI atlas with text baked in: drawn again at the render scale (text.h), if it is.
	// (atlases are paletted; the GPU draws it, or else the CPU)
	if (!slot->tex) glGenTextures(1, &slot->tex);
	bool ui_gl = rs > 1 && fmt >= 4 && fmt <= 7 && uitext_hires_gl(out, w, h, rs, slot->tex);
	const uint32_t *ui = !ui_gl && rs > 1 && fmt >= 4 && fmt <= 7 ? uitext_hires(out, w, h, rs) : NULL;
	glBindTexture(GL_TEXTURE_2D, slot->tex);
	if (ui) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w * rs, h * rs, 0, GL_RGBA, GL_UNSIGNED_BYTE, ui);
	else if (!ui_gl) glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, out);
	gl_invalidate();
	free(out);
	slot->key = key;
	slot->hash = hash;
	slot->checked = frame_no;
	slot->w = w;
	slot->h = h;
	slot->scale = tex_scale = ui != NULL || ui_gl ? (float)rs : 1.f;
	slot->mips = tex_mips = false;
	slot->last_use = frame_no;
	return slot->tex;
}

// ---------------------------------------------------------------------------------------------
// Vertex decoding and transform
// ---------------------------------------------------------------------------------------------
typedef struct {
	float pos[3], nrm[3], uv[2], w[8];
	uint8_t col[4];
	bool has_col;
} RawVert;

typedef struct {
	int tc, col, nrm, pos, wt, idx, nw, morphs;
	bool through;
	int off_w, off_tc, off_col, off_nrm, off_pos, size;
} VFmt;

static int align_to(int v, int a) { return (v + a - 1) & ~(a - 1); }

static VFmt vfmt_decode(uint32_t vt) {
	VFmt f = {0};
	f.tc = vt & 3;
	f.col = (vt >> 2) & 7;
	f.nrm = (vt >> 5) & 3;
	f.pos = (vt >> 7) & 3;
	f.wt = (vt >> 9) & 3;
	f.idx = (vt >> 11) & 3;
	f.nw = (int)((vt >> 14) & 7) + 1;
	f.morphs = (int)((vt >> 18) & 7) + 1;
	f.through = (vt >> 23) & 1;
	static const int csz[4] = {0, 1, 2, 4};
	int o = 0, maxa = 1;
	if (f.wt) {
		int s = csz[f.wt];
		o = align_to(o, s);
		f.off_w = o;
		o += s * f.nw;
		maxa = s > maxa ? s : maxa;
	}
	if (f.tc) {
		int s = csz[f.tc];
		o = align_to(o, s);
		f.off_tc = o;
		o += s * 2;
		maxa = s > maxa ? s : maxa;
	}
	if (f.col) {
		int s = f.col == 7 ? 4 : 2;
		o = align_to(o, s);
		f.off_col = o;
		o += s;
		maxa = s > maxa ? s : maxa;
	}
	if (f.nrm) {
		int s = csz[f.nrm];
		o = align_to(o, s);
		f.off_nrm = o;
		o += s * 3;
		maxa = s > maxa ? s : maxa;
	}
	{
		int s = csz[f.pos ? f.pos : 3];
		o = align_to(o, s);
		f.off_pos = o;
		o += s * 3;
		maxa = s > maxa ? s : maxa;
	}
	f.size = align_to(o, maxa);
	return f;
}

static float rd_comp(const uint8_t *p, int kind, int i, bool is_signed, bool normalize) {
	switch (kind) {
	case 1: {
		float v = is_signed ? (float)((const int8_t *)p)[i] : (float)p[i];
		return normalize ? v / 128.f : v;
	}
	case 2: {
		int16_t s;
		uint16_t u;
		memcpy(&s, p + i * 2, 2);
		memcpy(&u, p + i * 2, 2);
		float v = is_signed ? (float)s : (float)u;
		return normalize ? v / 32768.f : v;
	}
	default: {
		float f;
		memcpy(&f, p + i * 4, 4);
		return f;
	}
	}
}

static void read_vertex(const VFmt *f, const uint8_t *p, RawVert *v) {
	bool norm = !f->through;
	for (int i = 0; i < f->nw && f->wt; i++) v->w[i] = rd_comp(p + f->off_w, f->wt, i, false, true);
	if (f->tc) {
		v->uv[0] = rd_comp(p + f->off_tc, f->tc, 0, false, norm);
		v->uv[1] = rd_comp(p + f->off_tc, f->tc, 1, false, norm);
	}
	v->has_col = f->col != 0;
	if (f->col == 7) {
		memcpy(v->col, p + f->off_col, 4);
	}
	else if (f->col >= 4) {
		uint16_t c;
		memcpy(&c, p + f->off_col, 2);
		uint32_t rgba = conv16(c, f->col - 4);
		memcpy(v->col, &rgba, 4);
	}
	if (f->nrm) {
		for (int i = 0; i < 3; i++) v->nrm[i] = rd_comp(p + f->off_nrm, f->nrm, i, true, norm);
	}
	int pk = f->pos ? f->pos : 3;
	for (int i = 0; i < 3; i++) {
		bool sgn = !(f->through && i == 2);  // through-mode z is unsigned
		v->pos[i] = rd_comp(p + f->off_pos, pk, i, sgn, norm && pk != 3);
	}
}

static void mul43(const float *m, const float *v, float w, float *out) {
	out[0] = v[0] * m[0] + v[1] * m[3] + v[2] * m[6] + w * m[9];
	out[1] = v[0] * m[1] + v[1] * m[4] + v[2] * m[7] + w * m[10];
	out[2] = v[0] * m[2] + v[1] * m[5] + v[2] * m[8] + w * m[11];
}

static void unpack_rgba(uint32_t rgb, uint32_t a, float *o) {
	o[0] = (rgb & 0xFF) / 255.f;
	o[1] = ((rgb >> 8) & 0xFF) / 255.f;
	o[2] = ((rgb >> 16) & 0xFF) / 255.f;
	o[3] = (a & 0xFF) / 255.f;
}

static uint8_t to8(float v) { return (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.f + 0.5f)); }

static void light_vertex(const float *wpos, const float *wn, const float *vcol, float *c0, float *c1) {
	uint32_t matupd = reg[0x53];
	float mat_amb[4], mat_dif[4], mat_spe[4], emis[4], amb[4];
	unpack_rgba(reg[0x55], reg[0x58], mat_amb);
	unpack_rgba(reg[0x56], 0xFF, mat_dif);
	unpack_rgba(reg[0x57], 0xFF, mat_spe);
	unpack_rgba(reg[0x54], 0xFF, emis);
	unpack_rgba(reg[0x5C], reg[0x5D], amb);
	if (matupd & 1) memcpy(mat_amb, vcol, sizeof mat_amb);
	if (matupd & 2) memcpy(mat_dif, vcol, sizeof mat_dif);
	if (matupd & 4) memcpy(mat_spe, vcol, sizeof mat_spe);
	float spec_coef = f24(reg[0x5B]);
	float out[4] = {emis[0] + amb[0] * mat_amb[0], emis[1] + amb[1] * mat_amb[1], emis[2] + amb[2] * mat_amb[2],
	                amb[3] * mat_amb[3]};
	float spec[3] = {0, 0, 0};
	float n[3] = {wn[0], wn[1], wn[2]};
	if (reg[0x51] & 1) n[0] = -n[0], n[1] = -n[1], n[2] = -n[2];
	float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	if (nl > 0) n[0] /= nl, n[1] /= nl, n[2] /= nl;
	for (int i = 0; i < 4; i++) {
		if (!(reg[0x18 + i] & 1)) continue;
		uint32_t lt = reg[0x5F + i];
		int comp = lt & 3, kind = (lt >> 8) & 3;
		float lp[3] = {f24(reg[0x63 + i * 3]), f24(reg[0x64 + i * 3]), f24(reg[0x65 + i * 3])};
		float L[3], att = 1.f;
		if (kind == 0) {
			memcpy(L, lp, sizeof L);
		}
		else {
			L[0] = lp[0] - wpos[0], L[1] = lp[1] - wpos[1], L[2] = lp[2] - wpos[2];
			float d = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
			float ka = f24(reg[0x7B + i * 3]), kb = f24(reg[0x7C + i * 3]), kc = f24(reg[0x7D + i * 3]);
			float den = ka + kb * d + kc * d * d;
			att = den > 0 ? 1.f / den : 1.f;
			if (att > 1) att = 1;
		}
		float ll = sqrtf(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
		if (ll > 0) L[0] /= ll, L[1] /= ll, L[2] /= ll;
		if (kind == 2) {  // spot
			float dir[3] = {f24(reg[0x6F + i * 3]), f24(reg[0x70 + i * 3]), f24(reg[0x71 + i * 3])};
			float dl = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			if (dl > 0) dir[0] /= dl, dir[1] /= dl, dir[2] /= dl;
			float sd = -(L[0] * dir[0] + L[1] * dir[1] + L[2] * dir[2]);
			float cutoff = f24(reg[0x8B + i]);
			att = sd >= cutoff ? att * powf(sd > 0 ? sd : 0, f24(reg[0x87 + i])) : 0;
		}
		float lac[4], ldc[4], lsc[4];
		unpack_rgba(reg[0x8F + i * 3], 0, lac);
		unpack_rgba(reg[0x90 + i * 3], 0, ldc);
		unpack_rgba(reg[0x91 + i * 3], 0, lsc);
		float d = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
		if (d < 0) d = 0;
		if (comp == 2) d = powf(d, spec_coef);
		for (int k = 0; k < 3; k++) out[k] += att * (lac[k] * mat_amb[k] + d * ldc[k] * mat_dif[k]);
		if (comp == 1 && d > 0) {
			float H[3] = {L[0], L[1], L[2] + 1.f};
			float hl = sqrtf(H[0] * H[0] + H[1] * H[1] + H[2] * H[2]);
			float s = hl > 0 ? (n[0] * H[0] + n[1] * H[1] + n[2] * H[2]) / hl : 0;
			s = s > 0 ? powf(s, spec_coef) : 0;
			for (int k = 0; k < 3; k++) spec[k] += att * s * lsc[k] * mat_spe[k];
		}
	}
	bool separate = reg[0x5E] & 1;
	for (int k = 0; k < 3; k++) {
		c0[k] = out[k] + (separate ? 0 : spec[k]);
		c1[k] = separate ? spec[k] : 0;
	}
	c0[3] = out[3];  // alpha: global ambient alpha * material ambient alpha
	c1[3] = 0;
}

// Transforms one decoded vertex into a GL vertex.
static void transform(const VFmt *f, const RawVert *rv, GVert *o, float fbw, float fbh, int tw, int th) {
	o->box[0] = o->box[1] = -1e9f, o->box[2] = o->box[3] = 1e9f;  // no sprite rectangle
	float c0[4], c1[4] = {0, 0, 0, 0};
	if (rv->has_col) {
		for (int k = 0; k < 4; k++) c0[k] = rv->col[k] / 255.f;
	}
	else {
		unpack_rgba(reg[0x55], reg[0x58], c0);
	}
	if (f->through) {
		float ox = 0, oy = 0;
		o->x = (rv->pos[0] - ox) / fbw * 2.f - 1.f;
		o->y = (rv->pos[1] - oy) / fbh * 2.f - 1.f;
		o->z = rv->pos[2] / 65535.f * 2.f - 1.f;
		o->w = 1.f;
		o->u = tw ? rv->uv[0] / (float)tw : 0;
		o->v = th ? rv->uv[1] / (float)th : 0;
		o->fog = 1.f;
	}
	else {
		float p[3] = {rv->pos[0], rv->pos[1], rv->pos[2]}, n[3] = {rv->nrm[0], rv->nrm[1], rv->nrm[2]};
		if (f->wt) {  // skinning
			float sp[3] = {0, 0, 0}, sn[3] = {0, 0, 0};
			for (int b = 0; b < f->nw; b++) {
				float tp[3], tn[3];
				mul43(bone + b * 12, p, 1.f, tp);
				mul43(bone + b * 12, n, 0.f, tn);
				for (int k = 0; k < 3; k++) sp[k] += tp[k] * rv->w[b], sn[k] += tn[k] * rv->w[b];
			}
			memcpy(p, sp, sizeof p);
			memcpy(n, sn, sizeof n);
		}
		float wp[3], wn[3], vp[3];
		mul43(world, p, 1.f, wp);
		mul43(world, n, 0.f, wn);
		mul43(view, wp, 1.f, vp);
		float cx = vp[0] * proj[0] + vp[1] * proj[4] + vp[2] * proj[8] + proj[12];
		float cy = vp[0] * proj[1] + vp[1] * proj[5] + vp[2] * proj[9] + proj[13];
		float cz = vp[0] * proj[2] + vp[1] * proj[6] + vp[2] * proj[10] + proj[14];
		float cw = vp[0] * proj[3] + vp[1] * proj[7] + vp[2] * proj[11] + proj[15];
		float vsx = f24(reg[0x42]), vsy = f24(reg[0x43]), vsz = f24(reg[0x44]);
		float vcx = f24(reg[0x45]), vcy = f24(reg[0x46]), vcz = f24(reg[0x47]);
		float offx = (float)(reg[0x4C] & 0xFFFF) / 16.f, offy = (float)(reg[0x4D] & 0xFFFF) / 16.f;
		o->x = cx * (2.f * vsx / fbw) + cw * (2.f * (vcx - offx) / fbw - 1.f);
		o->y = cy * (2.f * vsy / fbh) + cw * (2.f * (vcy - offy) / fbh - 1.f);
		o->z = cz * (2.f * vsz / 65535.f) + cw * (2.f * vcz / 65535.f - 1.f);
		o->w = cw;
		// Texture coordinates
		int mapmode = reg[0xC0] & 3;
		float su = f24(reg[0x48]), sv = f24(reg[0x49]), ou = f24(reg[0x4A]), ov = f24(reg[0x4B]);
		if (mapmode == 0) {
			o->u = rv->uv[0] * su + ou;
			o->v = rv->uv[1] * sv + ov;
		}
		else if (mapmode == 1) {
			int src = (reg[0xC0] >> 8) & 3;
			float in[3];
			if (src == 0) memcpy(in, rv->pos, sizeof in);
			else if (src == 1) in[0] = rv->uv[0], in[1] = rv->uv[1], in[2] = 0;
			else memcpy(in, rv->nrm, sizeof in);
			float t[3];
			mul43(tgen, in, 1.f, t);
			o->u = t[2] != 0 ? t[0] / t[2] : t[0];
			o->v = t[2] != 0 ? t[1] / t[2] : t[1];
		}
		else {  // environment map from the normal
			float nn = sqrtf(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]);
			o->u = nn > 0 ? (wn[0] / nn + 1.f) * 0.5f : 0.5f;
			o->v = nn > 0 ? (wn[1] / nn + 1.f) * 0.5f : 0.5f;
		}
		if (reg[0x17] & 1) {
			light_vertex(wp, wn, c0, c0, c1);
		}
		o->fog = (reg[0x1F] & 1) ? (vp[2] + f24(reg[0xCD])) * f24(reg[0xCE]) : 1.f;
	}
	for (int k = 0; k < 4; k++) {
		o->c0[k] = to8(c0[k]);
		o->c1[k] = to8(c1[k]);
	}
}

// ---------------------------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------------------------
static GLenum gl_blend_factor(int f, bool src, uint32_t fix) {
	switch (f) {
	case 0: return src ? GL_DST_COLOR : GL_SRC_COLOR;
	case 1: return src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
	case 2: case 6: return GL_SRC_ALPHA;
	case 3: case 7: return GL_ONE_MINUS_SRC_ALPHA;
	case 4: case 8: return GL_DST_ALPHA;
	case 5: case 9: return GL_ONE_MINUS_DST_ALPHA;
	default:
		if ((fix & 0xFFFFFF) == 0xFFFFFF) return GL_ONE;
		if ((fix & 0xFFFFFF) == 0) return GL_ZERO;
		return GL_CONSTANT_COLOR;
	}
}

static const GLenum gl_func[8] = {GL_NEVER, GL_ALWAYS, GL_EQUAL, GL_NOTEQUAL, GL_LESS, GL_LEQUAL, GL_GREATER, GL_GEQUAL};

// Everything the GL needs for a draw. Draws with an identical state are merged into one batch
// (the game sends the terrain as thousands of single triangles), and only the parts that differ
// from what the GL already has are re-applied.
typedef struct {
	Fbo *fbo;
	GLenum mode;
	GLuint tex;
	int sampler;      // index into samplers[]: min linear | mag linear << 1 | clamp s << 2 | clamp t << 3 | mipmaps << 4
	int scissor[4];   // x, y, w, h in PSP pixels
	uint8_t cmask[4];
	uint8_t blend, depth_test, depth_mask, cull;
	GLenum bsrc, bdst, beq, depth_func, cull_face;
	float bcolor[3];
	int32_t clear_mode, tex_enable, tfunc, tfunc_rgba, color_double, alpha_test, at_func, fog_enable, blend_double;
	float at_ref, uvs[2], env[3], fog[3];
	int32_t ct_func, ct_ref[3], ct_mask[3];  // color test (ct_func 1 = always / off)
	int32_t xbr;  // 2D upscaling filter: 0 off, else 4 * (1 xBR | 2 bicubic) + clamp s + 2 * clamp t
} RenderState;

static RenderState glc;  // what the GL currently has
static bool glc_valid;

// Something else (presentation, transfers) touched the GL state: re-apply everything next time.
static void gl_invalidate(void) { glc_valid = false; }

static void gl_apply(const RenderState *s) {
#define CH(f) (!glc_valid || memcmp(&glc.f, &s->f, sizeof s->f))
	if (!glc_valid) {
		glUseProgram(prog);
		glBindVertexArray(vao);
		glBindBuffer(GL_ARRAY_BUFFER, vbo);
		glActiveTexture(GL_TEXTURE0);
		glEnable(GL_SCISSOR_TEST);
	}
	if (CH(fbo)) {
		glBindFramebuffer(GL_FRAMEBUFFER, s->fbo->fbo);
		glViewport(0, 0, s->fbo->width * rs, s->fbo->height * rs);
	}
	if (CH(scissor)) glScissor(s->scissor[0] * rs, s->scissor[1] * rs, s->scissor[2] * rs, s->scissor[3] * rs);
	if (CH(cmask)) glColorMask(s->cmask[0], s->cmask[1], s->cmask[2], s->cmask[3]);
	if (CH(blend)) (s->blend ? glEnable : glDisable)(GL_BLEND);
	if (s->blend && (CH(bsrc) || CH(bdst))) glBlendFuncSeparate(s->bsrc, s->bdst, GL_ONE, GL_ZERO);
	if (s->blend && CH(beq)) glBlendEquation(s->beq);
	if (s->blend && CH(bcolor)) glBlendColor(s->bcolor[0], s->bcolor[1], s->bcolor[2], 1.f);
	if (CH(depth_test)) (s->depth_test ? glEnable : glDisable)(GL_DEPTH_TEST);
	if (s->depth_test && CH(depth_func)) glDepthFunc(s->depth_func);
	if (CH(depth_mask)) glDepthMask(s->depth_mask);
	if (CH(cull)) (s->cull ? glEnable : glDisable)(GL_CULL_FACE);
	if (s->cull && CH(cull_face)) glCullFace(s->cull_face);
	if (CH(clear_mode)) glUniform1i(u_clearMode, s->clear_mode);
	if (CH(tex_enable)) glUniform1i(u_texEnable, s->tex_enable);
	if (CH(tfunc)) glUniform1i(u_tfunc, s->tfunc);
	if (CH(tfunc_rgba)) glUniform1i(u_tfuncRGBA, s->tfunc_rgba);
	if (CH(color_double)) glUniform1i(u_colorDouble, s->color_double);
	if (CH(alpha_test)) glUniform1i(u_alphaTest, s->alpha_test);
	if (CH(at_func)) glUniform1i(u_atFunc, s->at_func);
	if (CH(at_ref)) glUniform1f(u_atRef, s->at_ref);
	if (CH(fog_enable)) glUniform1i(u_fogEnable, s->fog_enable);
	if (CH(fog)) glUniform3f(u_fogColor, s->fog[0], s->fog[1], s->fog[2]);
	if (CH(blend_double)) glUniform1i(u_blendDouble, s->blend_double);
	if (CH(ct_func)) glUniform1i(u_ctFunc, s->ct_func);
	if (CH(xbr)) glUniform1i(u_xbr, s->xbr);
	if (CH(ct_ref)) glUniform3i(u_ctRef, s->ct_ref[0], s->ct_ref[1], s->ct_ref[2]);
	if (CH(ct_mask)) glUniform3i(u_ctMask, s->ct_mask[0], s->ct_mask[1], s->ct_mask[2]);
	if (CH(uvs)) glUniform2f(u_uvScale, s->uvs[0], s->uvs[1]);
	if (CH(env)) glUniform4f(u_envColor, s->env[0], s->env[1], s->env[2], 1.f);
	if (s->tex_enable && CH(tex)) glBindTexture(GL_TEXTURE_2D, s->tex);
	if (s->tex_enable && CH(sampler)) glBindSampler(0, samplers[s->sampler]);
#undef CH
	glc = *s;
	if (!s->tex_enable) glc.tex = 0, glc.sampler = -1;  // not bound: compare as different next time
	glc_valid = true;
}

static RenderState batch_state;
static GVert *batch_v;
static size_t batch_n, batch_cap;
static uint64_t stat_draws, stat_batches;

static void batch_reserve(size_t n) {
	if (batch_n + n <= batch_cap) return;
	batch_cap = (batch_n + n) * 2 + 4096;
	batch_v = realloc(batch_v, batch_cap * sizeof(GVert));
}

// Draws everything queued. Called whenever the GL is about to read or change something a queued
// draw depends on (textures, framebuffers, presentation).
static void batch_flush(void) {
	if (!batch_n) return;
	gl_apply(&batch_state);
	glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(batch_n * sizeof(GVert)), batch_v, GL_STREAM_DRAW);
	glDrawArrays(batch_state.mode, 0, (GLsizei)batch_n);
	stat_batches++;
	batch_n = 0;
}

// Starts or continues the batch for a draw with state s; the caller then appends its vertices.
static void batch_begin(const RenderState *s, size_t max_verts) {
	if (batch_n && (s->mode != GL_TRIANGLES || batch_state.mode != GL_TRIANGLES || memcmp(s, &batch_state, sizeof *s))) {
		batch_flush();
	}
	if (!batch_n) batch_state = *s;
	batch_reserve(max_verts);
	stat_draws++;
}

// The PSP state of the current draw.
static void build_state(RenderState *s, bool through, bool clear) {
	memset(s, 0, sizeof *s);
	s->fbo = cur_fbo;
	s->mode = GL_TRIANGLES;
	s->sampler = 0;
	int x1 = (int)(reg[0xD4] & 0x3FF), y1 = (int)((reg[0xD4] >> 10) & 0x3FF);
	int x2 = (int)(reg[0xD5] & 0x3FF), y2 = (int)((reg[0xD5] >> 10) & 0x3FF);
	s->scissor[0] = x1;
	s->scissor[1] = y1;
	s->scissor[2] = x2 - x1 + 1 > 0 ? x2 - x1 + 1 : 0;
	s->scissor[3] = y2 - y1 + 1 > 0 ? y2 - y1 + 1 : 0;
	s->uvs[0] = s->uvs[1] = 1.f;
	if (clear) {
		uint32_t cm = reg[0xD3];
		bool color = cm & 0x100, alpha = cm & 0x200, depth = cm & 0x400;
		s->clear_mode = 1;
		s->cmask[0] = s->cmask[1] = s->cmask[2] = color;
		s->cmask[3] = alpha;
		s->depth_test = 1;
		s->depth_func = GL_ALWAYS;
		s->depth_mask = depth;
		return;
	}
	// The framebuffer alpha channel is the stencil buffer: it is only written when the stencil
	// test is on and some stencil op modifies it (otherwise blending reads a stencil-like alpha).
	uint32_t mrgb = reg[0xE8], ma = reg[0xE9], sop = reg[0xDD];
	bool stencil_writes = (reg[0x24] & 1) && (ma & 0xFF) != 0xFF &&
	                      ((sop & 0xFF) != 2 || ((sop >> 8) & 0xFF) != 2 || ((sop >> 16) & 0xFF) != 2);
	s->cmask[0] = (mrgb & 0xFF) != 0xFF;
	s->cmask[1] = (mrgb & 0xFF00) != 0xFF00;
	s->cmask[2] = (mrgb & 0xFF0000) != 0xFF0000;
	s->cmask[3] = stencil_writes;
	if (reg[0x21] & 1) {
		uint32_t bm = reg[0xDF];
		int sf = bm & 0xF, df = (bm >> 4) & 0xF, eq = (bm >> 8) & 7;
		s->blend = 1;
		s->bsrc = gl_blend_factor(sf, true, reg[0xE0]);
		s->bdst = gl_blend_factor(df, false, reg[0xE1]);
		s->blend_double = sf == 6;  // DOUBLESRCALPHA: src * 2a == (2 * src) * a
		if (s->bsrc == GL_CONSTANT_COLOR || s->bdst == GL_CONSTANT_COLOR) {
			uint32_t fix = s->bsrc == GL_CONSTANT_COLOR ? reg[0xE0] : reg[0xE1];
			s->bcolor[0] = (fix & 0xFF) / 255.f;
			s->bcolor[1] = ((fix >> 8) & 0xFF) / 255.f;
			s->bcolor[2] = ((fix >> 16) & 0xFF) / 255.f;
		}
		static const GLenum eqs[8] = {GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX,
		                              GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_ADD};
		s->beq = eqs[eq];
	}
	if (reg[0x23] & 1) {
		s->depth_test = 1;
		s->depth_func = gl_func[reg[0xDE] & 7];
	}
	s->depth_mask = !(reg[0xE7] & 1) && (reg[0x23] & 1);
	if (!through && (reg[0x1D] & 1)) {
		s->cull = 1;
		s->cull_face = (reg[0x9B] & 1) ? GL_FRONT : GL_BACK;
	}
	s->alpha_test = reg[0x22] & 1;
	s->ct_func = (reg[0x27] & 1) ? (int)(reg[0xD8] & 3) : 1;
	for (int k = 0; k < 3; k++) {
		s->ct_ref[k] = (int)((reg[0xD9] >> (8 * k)) & 0xFF);
		s->ct_mask[k] = (int)((reg[0xDA] >> (8 * k)) & 0xFF);
	}
	s->at_func = (int)(reg[0xDB] & 7);
	s->at_ref = (float)((reg[0xDB] >> 8) & 0xFF);
	s->fog_enable = !through && (reg[0x1F] & 1);
	uint32_t fc = reg[0xCF];
	s->fog[0] = (fc & 0xFF) / 255.f;
	s->fog[1] = ((fc >> 8) & 0xFF) / 255.f;
	s->fog[2] = ((fc >> 16) & 0xFF) / 255.f;
	uint32_t tf = reg[0xC9];
	s->tfunc = (int)(tf & 7);
	s->tfunc_rgba = (tf >> 8) & 1;
	s->color_double = (tf >> 16) & 1;
	uint32_t ec = reg[0xCA];
	s->env[0] = (ec & 0xFF) / 255.f;
	s->env[1] = ((ec >> 8) & 0xFF) / 255.f;
	s->env[2] = ((ec >> 16) & 0xFF) / 255.f;
}

static uint32_t prim_cmd_addr;  // list address of the PRIM being drawn (debug dumps)
static GVert *vbuf;
static size_t vbuf_cap;
static int dbg_prims;

static void draw_prim(uint32_t data) {
	int count = (int)(data & 0xFFFF), type = (int)((data >> 16) & 7);
	if (count <= 0) return;
	VFmt f = vfmt_decode(reg[0x12]);
	uint32_t vaddr = vertex_addr;
	uint32_t iaddr = index_addr;
	if (!rt_valid(vaddr, 1)) {
		if (gedebug) rt_log("GE prim skipped: bad vertex address %08x", vaddr);
		return;
	}

	bind_target();
	bool clear = reg[0xD3] & 1;
	bool tex_on = (reg[0x1E] & 1) && !clear;
	int tw = 0, th = 0;
	float uvsx = 1, uvsy = 1;
	GLuint tex = 0;
	if (tex_on) {
		tex = texture_get(&tw, &th, &uvsx, &uvsy);
		if (!tex) tex_on = false;
	}
	if (tex_on && tex == cur_fbo->tex) {
		// Sampling the framebuffer being drawn into is undefined in GL and an error in WebGL2:
		// sample a snapshot instead.
		batch_flush();
		static GLuint snap;
		static int snap_w, snap_h;
		int w = cur_fbo->width * rs, h = cur_fbo->height * rs;
		if (!snap) glGenTextures(1, &snap);
		glBindTexture(GL_TEXTURE_2D, snap);
		if (snap_w != w || snap_h != h) {
			snap_w = w;
			snap_h = h;
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, cur_fbo->fbo);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
		gl_invalidate();
		tex = snap;
	}
	RenderState st;
	build_state(&st, f.through, clear);
	st.tex_enable = tex_on;
	if (tex_on) {
		uint32_t flt = reg[0xC6], wrap = reg[0xC7];
		st.tex = tex;
		st.uvs[0] = uvsx;
		st.uvs[1] = uvsy;
		st.sampler = (int)((flt & 1) | ((flt >> 8) & 1) << 1 | (wrap & 1) << 2 | ((wrap >> 8) & 1) << 3);
		// The 2D interface (through mode, ordinary textures) goes through the xBR filter above 1x.
		static int mips_on = -1;
		if (mips_on < 0) mips_on = !getenv("PANGYA_TEXPACK_MIPS") || atoi(getenv("PANGYA_TEXPACK_MIPS")) != 0;
		if (tex_scale > 1.f) st.sampler |= 3 | (tex_mips && mips_on ? 16 : 0);  // finer than the GE's texture: smooth sampling
		else if (tex_filter && rs > 1 && !tex_from_fbo) {
			st.xbr = 4 * (tex_filter == 1 ? 1 : 3) + (int)(wrap & 1) + 2 * (int)((wrap >> 8) & 1);
			if (tex_filter == 2) st.sampler |= 3;  // the sharp pixels' edges come from bilinear
		} else if (ui_filter && rs > 1 && f.through && !tex_from_fbo)
			st.xbr = 4 * ui_filter + (int)(wrap & 1) + 2 * (int)((wrap >> 8) & 1);
	}

	// Decode + transform every referenced vertex.
	if ((size_t)count > vbuf_cap) {
		vbuf_cap = (size_t)count * 2;
		vbuf = realloc(vbuf, vbuf_cap * sizeof(GVert));
	}
	float fbw = (float)cur_fbo->width, fbh = (float)cur_fbo->height;
	GVert *tv = vbuf;
	int isz = f.idx == 1 ? 1 : f.idx == 2 ? 2 : 0;
	float mw[8];
	for (int m = 0; m < 8; m++) mw[m] = f24(reg[0x2C + m]);
	bool dumping = ge_dump_state == 1;
	uint64_t h_raw = 1469598103934665603ull;
	uint32_t dump_idx[6];
	float dump_pos[6][3];
	for (int i = 0; i < count; i++) {
		uint32_t vi = (uint32_t)i;
		if (isz == 1) vi = M8(iaddr + (uint32_t)i);
		else if (isz == 2) vi = M16(iaddr + (uint32_t)i * 2);
		RawVert rv = {0};
		if (f.morphs == 1) {
			read_vertex(&f, MP(vaddr + vi * (uint32_t)f.size * 1), &rv);
		}
		else {  // morph targets are stored back to back
			for (int m = 0; m < f.morphs; m++) {
				RawVert t = {0};
				read_vertex(&f, MP(vaddr + (vi * (uint32_t)f.morphs + (uint32_t)m) * (uint32_t)f.size), &t);
				for (int k = 0; k < 3; k++) rv.pos[k] += t.pos[k] * mw[m], rv.nrm[k] += t.nrm[k] * mw[m];
				for (int k = 0; k < 2; k++) rv.uv[k] += t.uv[k] * mw[m];
				for (int k = 0; k < 8; k++) rv.w[k] += t.w[k] * mw[m];
				if (t.has_col) {
					for (int k = 0; k < 4; k++) rv.col[k] = (uint8_t)(rv.col[k] + t.col[k] * mw[m]);
					rv.has_col = true;
				}
			}
		}
		if (dumping) {
			h_raw = hash_bytes((const uint8_t *)&rv, sizeof rv) ^ (h_raw * 31);
			if (i < 6) dump_idx[i] = vi, memcpy(dump_pos[i], rv.pos, sizeof dump_pos[i]);
		}
		transform(&f, &rv, &tv[i], fbw, fbh, tw, th);
	}
	if (dumping) {
		FILE *df = ge_dump_file;
		int draw_i = ge_dump_draws++;
		uint64_t h_m = hash_bytes((const uint8_t *)world, sizeof world) ^ hash_bytes((const uint8_t *)view, sizeof view) * 3 ^
		               hash_bytes((const uint8_t *)proj, sizeof proj) * 7;
		if (f.wt) h_m ^= hash_bytes((const uint8_t *)bone, sizeof(float) * 12 * (size_t)f.nw) * 13;
		float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f, wmin = 1e30f;
		for (int i = 0; i < count; i++) {
			float w = tv[i].w != 0 ? tv[i].w : 1e-9f;
			float x = tv[i].x / w, y = tv[i].y / w;
			bx0 = fminf(bx0, x), by0 = fminf(by0, y), bx1 = fmaxf(bx1, x), by1 = fmaxf(by1, y), wmin = fminf(wmin, tv[i].w);
		}
		fprintf(df, "D2 %d t=%d n=%d vt=%06x raw=%016llx mtx=%016llx bb=%.3f,%.3f,%.3f,%.3f wmin=%.3f v=%08x i=%08x at=%08x\n",
		        draw_i, type, count, reg[0x12], (unsigned long long)h_raw, (unsigned long long)h_m, bx0, by0, bx1, by1, wmin, vaddr,
		        iaddr, prim_cmd_addr);
		// Vertices of PANGYA_GEDUMP_VERTS=vtype:mincount draws, and of small draws spanning more
		// than the screen (the shape of a broken triangle).
		static int want_n = -2;
		static unsigned want_vt;
		if (want_n == -2) {
			want_n = -1;
			if (getenv("PANGYA_GEDUMP_VERTS")) sscanf(getenv("PANGYA_GEDUMP_VERTS"), "%x:%d", &want_vt, &want_n);
		}
		bool big = count <= 6 && (bx1 - bx0 > 2.5f || by1 - by0 > 2.5f);
		if (big || (want_n >= 0 && reg[0x12] == want_vt && count >= want_n)) {
			float vsx = f24(reg[0x42]), vsy = f24(reg[0x43]), vsz = f24(reg[0x44]);
			float vcx = f24(reg[0x45]), vcy = f24(reg[0x46]), vcz = f24(reg[0x47]);
			fprintf(df, "  viewport s=(%g %g %g) c=(%g %g %g) off=(%g %g) clip=%x minz=%x maxz=%x blend=%d zt=%d/%d zw=%d tex=%d fog=%d cull=%d\n",
			        vsx, vsy, vsz, vcx, vcy, vcz, (reg[0x4C] & 0xFFFF) / 16.f, (reg[0x4D] & 0xFFFF) / 16.f, reg[0x1C], reg[0xD6],
			        reg[0xD7], reg[0x21] & 1, reg[0x23] & 1, reg[0xDE] & 7, !(reg[0xE7] & 1), tex_on, reg[0x1F] & 1, reg[0x1D] & 1);
			if (ctx_dumped < 4 && rt_valid(prim_cmd_addr - 192, 196)) {
				ctx_dumped++;
				fprintf(df, "  list before PRIM:");
				for (uint32_t a = prim_cmd_addr - 192; a <= prim_cmd_addr; a += 4) fprintf(df, " %08x", M32(a));
				fprintf(df, "\n");
			}
			if (f.wt) {
				fprintf(df, "  bone0=");
				for (int k = 0; k < 12; k++) fprintf(df, "%g%c", bone[k], k == 11 ? '\n' : ' ');
			}
			fprintf(df, "  world=");
			for (int k = 0; k < 12; k++) fprintf(df, "%g%c", world[k], k == 11 ? '\n' : ' ');
			for (int i = 0; i < count; i++) {
				const GVert *g = &tv[i];
				fprintf(df, "  v%d", i);
				if (i < 6) fprintf(df, " idx=%u pos=(%g %g %g)", dump_idx[i], dump_pos[i][0], dump_pos[i][1], dump_pos[i][2]);
				fprintf(df, " clip=(%.2f %.2f %.2f %.2f) ndc=(%.3f %.3f %.3f) c=%02x%02x%02x%02x uv=(%.3f %.3f)\n", g->x, g->y, g->z,
				        g->w, g->x / g->w, g->y / g->w, g->z / g->w, g->c0[0], g->c0[1], g->c0[2], g->c0[3], g->u, g->v);
			}
		}
	}

	// Assemble GL triangles (sprites -> two triangles, strips/fans -> lists) straight into the batch.
	static int nodraw = -1;
	if (nodraw < 0) nodraw = getenv("PANGYA_NODRAW") != NULL;
	st.mode = type == 0 ? GL_POINTS : type == 1 ? GL_LINES : type == 2 ? GL_LINE_STRIP : GL_TRIANGLES;
	if (!nodraw && type <= 6) {
		batch_begin(&st, (size_t)count * 3);
		GVert *o = batch_v + batch_n;
		size_t n = 0;
		switch (type) {
		case 0: case 1: case 2: memcpy(o, tv, sizeof(GVert) * (size_t)count); n = (size_t)count; break;
		case 3: n = (size_t)count - (size_t)count % 3; memcpy(o, tv, sizeof(GVert) * n); break;
		case 4:
			for (int i = 2; i < count; i++) {
				if (i & 1) { o[n++] = tv[i - 1]; o[n++] = tv[i - 2]; o[n++] = tv[i]; }
				else { o[n++] = tv[i - 2]; o[n++] = tv[i - 1]; o[n++] = tv[i]; }
			}
			break;
		case 5:
			for (int i = 2; i < count; i++) { o[n++] = tv[0]; o[n++] = tv[i - 1]; o[n++] = tv[i]; }
			break;
		case 6:
			for (int i = 0; i + 1 < count; i += 2) {
				GVert tl = tv[i], br = tv[i + 1];  // the second vertex carries color/z
				if (tex_on && !tex_from_fbo && tw > 0 && th > 0) {
					// half a texel of the texture as sampled (an atlas redrawn at the render scale is finer)
					float hu = 0.5f / ((float)tw * tex_scale), hv = 0.5f / ((float)th * tex_scale);
					float u0 = fminf(tl.u, br.u), u1 = fmaxf(tl.u, br.u), v0 = fminf(tl.v, br.v), v1 = fmaxf(tl.v, br.v);
					float bx[4] = {u0 + hu, v0 + hv, u1 - hu, v1 - hv};
					if (bx[0] > bx[2]) bx[0] = bx[2] = (u0 + u1) / 2.f;
					if (bx[1] > bx[3]) bx[1] = bx[3] = (v0 + v1) / 2.f;
					memcpy(br.box, bx, sizeof bx);
				}
				GVert v00 = br, v10 = br, v01 = br, v11 = br;
				v00.x = tl.x, v00.y = tl.y, v00.u = tl.u, v00.v = tl.v;
				v10.x = br.x, v10.y = tl.y, v10.u = br.u, v10.v = tl.v;
				v01.x = tl.x, v01.y = br.y, v01.u = tl.u, v01.v = br.v;
				v11.x = br.x, v11.y = br.y, v11.u = br.u, v11.v = br.v;
				o[n++] = v00; o[n++] = v10; o[n++] = v01;
				o[n++] = v10; o[n++] = v11; o[n++] = v01;
			}
			break;
		default: break;
		}
		batch_n += n;
		if (st.mode != GL_TRIANGLES) batch_flush();
	}
	if (gedebug && dbg_prims++ < 400) {
		rt_log("GE prim tex@%08x%s type=%d n=%d fb=%05x(%dx%d) through=%d clear=%d(%x) tex=%d fmt=%d %dx%d vt=%06x "
		       "v0=(%.2f %.2f %.2f %.2f) c=%02x%02x%02x%02x uv=(%.3f %.3f) scissor=%x/%x zt=%d/%d blend=%d at=%d/%x",
		       ((reg[0xA8] & 0x0F0000u) << 8) | (reg[0xA0] & 0xFFFFFFu), uvsx != 1.f ? "(FBO)" : "",
		       type, count, cur_fbo->addr, cur_fbo->width, cur_fbo->height, f.through, clear, reg[0xD3], tex_on,
		       (int)(reg[0xC3] & 0xF), tw, th, reg[0x12], tv[0].x, tv[0].y, tv[0].z, tv[0].w, tv[0].c0[0], tv[0].c0[1],
		       tv[0].c0[2], tv[0].c0[3], tv[0].u, tv[0].v, reg[0xD4], reg[0xD5], reg[0x23] & 1, reg[0xDE], reg[0x21] & 1,
		       reg[0x22] & 1, reg[0xDB]);
	}
	if (dumping && getenv("PANGYA_GEDUMP_STATE")) {
		fprintf(ge_dump_file, "DRAW type=%d n=%d vt=%06x tex=%d@%08x f%d %dx%d clut=%x | blend=%d df=%03x fa=%06x fb=%06x tfunc=%05x "
		       "at=%d/%06x zt=%d/%d zmask=%d mask=%06x/%02x light=%d fog=%d clear=%x cull=%d/%d | c0=%02x%02x%02x%02x "
		       "uv=(%.2f,%.2f) w=%.2f ct=%d/%d/%06x/%06x st=%d/%06x/%06x\n",
		       type, count, reg[0x12], tex_on, ((reg[0xA8] & 0x0F0000u) << 8) | (reg[0xA0] & 0xFFFFFFu), (int)(reg[0xC3] & 0xF),
		       tw, th, reg[0xC5], reg[0x21] & 1, reg[0xDF], reg[0xE0], reg[0xE1], reg[0xC9], reg[0x22] & 1, reg[0xDB],
		       reg[0x23] & 1, reg[0xDE] & 7, reg[0xE7] & 1, reg[0xE8], reg[0xE9], reg[0x17] & 1, reg[0x1F] & 1, reg[0xD3],
		       reg[0x1D] & 1, reg[0x9B] & 1, tv[0].c0[0], tv[0].c0[1], tv[0].c0[2], tv[0].c0[3], tv[0].u, tv[0].v, tv[0].w,
		       reg[0x27] & 1, reg[0xD8] & 3, reg[0xD9] & 0xFFFFFF, reg[0xDA] & 0xFFFFFF, reg[0x24] & 1, reg[0xDC] & 0xFFFFFF,
		       reg[0xDD] & 0xFFFFFF);
	}
	// The GE advances the vertex/index pointer past what it consumed.
	if (isz) index_addr += (uint32_t)(count * isz);
	else vertex_addr += (uint32_t)(count * f.size);
}

// ---------------------------------------------------------------------------------------------
// Block transfer
// ---------------------------------------------------------------------------------------------
// Framebuffer that contains a VRAM address (32-bit framebuffers only), with the pixel position.
static Fbo *fbo_containing(uint32_t addr, int *px, int *py) {
	if ((addr & 0x0FE00000u) != PSP_VRAM_BASE) return NULL;
	uint32_t off = addr & 0x001FFFFFu;
	for (int i = 0; i < MAX_FBOS; i++) {
		Fbo *f = &fbos[i];
		uint32_t size = (uint32_t)(f->width * f->height * 4);
		if (f->used && f->last_draw && off >= f->addr && off < f->addr + size) {
			uint32_t pix = (off - f->addr) / 4;
			*px = (int)(pix % (uint32_t)f->width);
			*py = (int)(pix / (uint32_t)f->width);
			return f;
		}
	}
	return NULL;
}

// A framebuffer region copied to VRAM at dst (its pixels already there): kept at full resolution
// in a framebuffer of its own at that address, used while VRAM still holds those pixels.
static void copy_target(Fbo *sf, int x0, int y0, uint32_t dst, int dstride, int w, int h) {
	Fbo *t = fbo_get(dst & 0x001FFFFFu, dstride, h);
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, sf->fbo);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t->fbo);
	glBlitFramebuffer(x0 * rs, y0 * rs, (x0 + w) * rs, (y0 + h) * rs, 0, 0, w * rs, h * rs, GL_COLOR_BUFFER_BIT, GL_NEAREST);
	t->copy = true;
	t->copy_hash = copy_hash(t);
	t->copy_checked = frame_no;
	t->last_draw = ++draw_counter;
	gl_invalidate();
}

static void block_transfer(uint32_t data) {
	int bpp = (data & 1) ? 4 : 2;
	uint32_t src = ((reg[0xB3] & 0xFF0000u) << 8) | (reg[0xB2] & 0xFFFFFFu);
	uint32_t dst = ((reg[0xB5] & 0xFF0000u) << 8) | (reg[0xB4] & 0xFFFFFFu);
	int sstride = (int)(reg[0xB3] & 0x7FF), dstride = (int)(reg[0xB5] & 0x7FF);
	int sx = (int)(reg[0xEB] & 0x3FF), sy = (int)((reg[0xEB] >> 10) & 0x3FF);
	int dx = (int)(reg[0xEC] & 0x3FF), dy = (int)((reg[0xEC] >> 10) & 0x3FF);
	int w = (int)(reg[0xEE] & 0x3FF) + 1, h = (int)((reg[0xEE] >> 10) & 0x3FF) + 1;
	if (gedebug) rt_log("GE transfer %08x(+%d,%d stride %d) -> %08x(+%d,%d stride %d) %dx%d bpp=%d", src, sx, sy, sstride, dst, dx, dy, dstride, w, h, bpp);

	// The live image of a framebuffer is on the GPU, not in VRAM.
	int spx = 0, spy = 0, dpx = 0, dpy = 0;
	Fbo *sf = bpp == 4 ? fbo_containing(src, &spx, &spy) : NULL;
	Fbo *df = bpp == 4 ? fbo_containing(dst, &dpx, &dpy) : NULL;
	if (!df && bpp == 4) {  // the destination may be a framebuffer that was never drawn yet
		Fbo *f = fbo_find(dst & 0x001FFFFFu);
		if (f) df = f, dpx = dpy = 0;
	}
	if (gedebug && (sf || df))
		rt_log("GE transfer: src fbo %08x %dx%d (+%d,%d), dst fbo %08x %dx%d (+%d,%d)", sf ? sf->addr : 0, sf ? sf->width : 0,
		       sf ? sf->height : 0, spx, spy, df ? df->addr : 0, df ? df->width : 0, df ? df->height : 0, dpx, dpy);
	if (sf || df) batch_flush();  // queued draws must land before the GPU image is read or changed
	if (sf && sstride == sf->width) {
		int x0 = spx + sx, y0 = spy + sy;
		if (df && dstride == df->width) {  // GPU -> GPU (coordinates in scaled pixels)
			int ox = dpx + dx, oy = dpy + dy;
			glDisable(GL_SCISSOR_TEST);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, sf->fbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, df->fbo);
			if (sf == df) {  // overlapping copy inside one framebuffer: go through a temp texture
				GLuint tmp;
				glGenTextures(1, &tmp);
				glBindTexture(GL_TEXTURE_2D, tmp);
				glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, x0 * rs, y0 * rs, w * rs, h * rs, 0);
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tmp, 0);
				glBlitFramebuffer(0, 0, w * rs, h * rs, ox * rs, oy * rs, (ox + w) * rs, (oy + h) * rs, GL_COLOR_BUFFER_BIT,
				                  GL_NEAREST);
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sf->tex, 0);
				glDeleteTextures(1, &tmp);
			}
			else {
				glBlitFramebuffer(x0 * rs, y0 * rs, (x0 + w) * rs, (y0 + h) * rs, ox * rs, oy * rs, (ox + w) * rs, (oy + h) * rs,
				                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
			}
			df->last_draw = ++draw_counter;
			// Keep VRAM in step (the picture may later be read as a texture from memory, e.g.
			// after the target is evicted) and, when the copy lands inside a larger framebuffer
			// rather than at its start, keep it at full resolution at its own address too.
			uint32_t d0 = dst + (uint32_t)((dy * dstride + dx) * 4);
			if (rt_valid(d0, (uint32_t)(((h - 1) * dstride + w) * 4))) fb_read(df->fbo, ox, oy, w, h, MP(d0), dstride);
			if (df->copy) df->copy_hash = copy_hash(df);
			if ((dst & 0x001FFFFFu) != df->addr && dx == 0 && dy == 0 && (dst & 0x0FE00000u) == PSP_VRAM_BASE &&
			    dstride >= w && dstride <= 1024 && h <= 512)
				copy_target(sf, x0, y0, dst, dstride, w, h);
			gl_invalidate();
			cur_fbo = NULL;
			return;
		}
		// GPU -> memory (e.g. part of a frame kept to be drawn again as a texture): read back the
		// pixels for the CPU, and keep the picture at full resolution in a copy target there.
		uint32_t d0 = dst + (uint32_t)((dy * dstride + dx) * 4);
		if (rt_valid(d0, (uint32_t)(((h - 1) * dstride + w) * 4))) fb_read(sf->fbo, x0, y0, w, h, MP(d0), dstride);
		if ((dst & 0x0FE00000u) == PSP_VRAM_BASE && dx == 0 && dy == 0 && dstride >= w && dstride <= 1024 && h <= 512)
			copy_target(sf, x0, y0, dst, dstride, w, h);
		cur_fbo = NULL;
		return;
	}
	for (int y = 0; y < h; y++) {
		uint32_t s = src + (uint32_t)(((sy + y) * sstride + sx) * bpp);
		uint32_t d = dst + (uint32_t)(((dy + y) * dstride + dx) * bpp);
		if (rt_valid(s, (uint32_t)(w * bpp)) && rt_valid(d, (uint32_t)(w * bpp))) memmove(MP(d), MP(s), (size_t)(w * bpp));
	}
	// Memory -> framebuffer: upload the copied pixels into the GPU image too.
	if (df && dstride == df->width) {
		fb_upload(df, dpx + dx, dpy + dy, w, h, MP(dst + (uint32_t)((dy * dstride + dx) * 4)), dstride);
		df->last_draw = ++draw_counter;
		if (df->copy) df->copy_hash = copy_hash(df);
	}
}

// ---------------------------------------------------------------------------------------------
// Command entry point (called by the list walker in display.c)
// ---------------------------------------------------------------------------------------------
void ge_command(uint32_t cmd, uint32_t data, uint32_t addr) {
	(void)addr;
	switch (cmd) {
	case 0x04: prim_cmd_addr = addr; draw_prim(data); return;
	case 0x01: vertex_addr = rel_addr(data); break;
	case 0x02: index_addr = rel_addr(data); break;
	case 0x3A: world_i = (int)(data & 0xF); break;
	case 0x3B:
		if (ge_dump_state == 1 && ge_dump_mtx_lines < 400) {
			ge_dump_mtx_lines++;
			fprintf(ge_dump_file, "W %08x [%d] %06x\n", addr, world_i, data);
		}
		if (world_i < 12) world[world_i++] = f24(data);
		break;
	case 0x3C: view_i = (int)(data & 0xF); break;
	case 0x3D: if (view_i < 12) view[view_i++] = f24(data); break;
	case 0x3E: proj_i = (int)(data & 0xF); break;
	case 0x3F: if (proj_i < 16) proj[proj_i++] = f24(data); break;
	case 0x40: tgen_i = (int)(data & 0xF); break;
	case 0x41: if (tgen_i < 12) tgen[tgen_i++] = f24(data); break;
	case 0x2A: bone_i = (int)(data & 0x7F); break;
	case 0x2B:
		if (bone_i < 96) bone[bone_i++] = f24(data);
		break;
	case 0xC4: {  // load CLUT
		uint32_t a = ((reg[0xB1] & 0xF0000u) << 8) | (reg[0xB0] & 0xFFFFFFu);
		uint32_t bytes = (data & 0x3F) * 32;
		if (bytes > sizeof clut) bytes = sizeof clut;
		if (rt_valid(a, bytes)) memcpy(clut, MP(a), bytes);
		clut_dirty = true;
		break;
	}
	case 0xEA: block_transfer(data); break;
	default: break;
	}
	reg[cmd] = data;
}

// ---------------------------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------------------------
// Every texture is looked up and drawn again (the texture pack or the text drawing changed).
void ge_forget_textures(void) {
	for (int i = 0; i < MAX_TEX; i++) {
		texcache[i].hash = 0, texcache[i].checked = 0, texcache[i].pack1 = 0;
		free(texcache[i].ui_orig);
		texcache[i].ui_orig = NULL;
	}
}

// Returns the texture to show for the displayed framebuffer address (an FBO if the GE rendered
// into it, otherwise the raw VRAM contents uploaded to a scratch texture).
GLuint ge_display_texture(uint32_t fb_addr, int stride, int fmt, int *src_w, int *src_h) {
	frame_no++;
	batch_flush();
	if (texpack_changed()) ge_forget_textures();  // the texture pack changed
	if (fonthd_changed()) ge_forget_textures();   // an HD font arrived: the lines are drawn again
	Fbo *f = fbo_find(fb_addr & 0x001FFFFFu);
	static unsigned long dbg_every = 0;
	if (!dbg_every) dbg_every = getenv("PANGYA_GEDEBUG_EVERY") ? strtoul(getenv("PANGYA_GEDEBUG_EVERY"), NULL, 10) : 120;
	if (gedebug > 0 && frame_no % dbg_every == 0) {
		rt_log("GE display %08x fbo=%s last_draw=%llu; fbos:", fb_addr, f ? "yes" : "no", f ? (unsigned long long)f->last_draw : 0ull);
		for (int i = 0; i < MAX_FBOS; i++)
			if (fbos[i].used) rt_log("   fbo %05x %dx%d last_draw=%llu", fbos[i].addr, fbos[i].width, fbos[i].height, (unsigned long long)fbos[i].last_draw);
		dbg_prims = 0;
	}
	if (f && f->last_draw) {  // the visible 480x272 corner of the (scaled) framebuffer
		*src_w = 480 * rs;
		*src_h = 272 * rs;
		return f->tex;
	}
	static GLuint scratch;
	static uint32_t px[512 * 272];
	if (!scratch) glGenTextures(1, &scratch);
	int bpp = fmt == 3 ? 4 : 2;
	if (stride <= 0) stride = 512;
	bool valid = fb_addr && rt_valid(fb_addr, (uint32_t)(stride * 272 * bpp));
	for (int y = 0; y < 272; y++) {
		for (int x = 0; x < 480; x++) {
			uint32_t a = fb_addr + (uint32_t)((y * stride + x) * bpp);
			if (!valid) px[y * 512 + x] = 0xFF000000u;
			else if (fmt == 3) px[y * 512 + x] = M32(a) | 0xFF000000u;
			else px[y * 512 + x] = conv16(M16(a), fmt) | 0xFF000000u;
		}
	}
	glBindTexture(GL_TEXTURE_2D, scratch);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 512, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	gl_invalidate();
	*src_w = 480;
	*src_h = 272;
	return scratch;
}

// The presentation code changed GL bindings and state.
void ge_unbind_target(void) {
	cur_fbo = NULL;
	gl_invalidate();
}

// Draw calls the game issued and GL draws they were merged into, since the last call.
void ge_draw_stats(uint64_t *draws, uint64_t *batches) {
	*draws = stat_draws;
	*batches = stat_batches;
	stat_draws = stat_batches = 0;
}

int ge_render_scale(void) { return rs; }
