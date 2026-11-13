// VFPU (Allegrex vector unit) executor for the ~40 instruction forms the game uses.
// Register storage is column-major per matrix: v[mtx*16 + col*4 + row]. Instruction register
// fields use the hardware numbering mtx*4 + col + row*32 (+ transpose bit for vectors/matrices).
#include <math.h>
#include <string.h>

#include "rt.h"

enum { PFX_S = 0, PFX_T = 1, PFX_D = 2, CC = 3 };

static inline int vsize(uint32_t op) { return (int)(((op >> 7) & 1) | ((op >> 14) & 2)) + 1; }
static inline int store_index(int mtx, int col, int row) { return mtx * 16 + (col & 3) * 4 + (row & 3); }

// Storage indices of an n-element vector register.
static void vregs(int reg, int n, int out[4]) {
	int mtx = (reg >> 2) & 7, col = reg & 3, row, transpose = (reg >> 5) & 1;
	switch (n) {
	case 1: transpose = 0; row = (reg >> 5) & 3; break;
	case 2: row = (reg >> 5) & 2; break;
	case 3: row = (reg >> 6) & 1; break;
	default: row = (reg >> 5) & 2; break;
	}
	for (int i = 0; i < n; i++) {
		out[i] = transpose ? store_index(mtx, row + i, col) : store_index(mtx, col, row + i);
	}
}

static void read_vec(CPU *c, int reg, int n, float *v) {
	int r[4];
	vregs(reg, n, r);
	for (int i = 0; i < n; i++) v[i] = c->v[r[i]];
}

// Matrix as m[j*4 + i] (column j, row i), side n.
static void mat_index(int reg, int n, int idx[16]) {
	int row, transpose = (reg >> 5) & 1;
	switch (n) {
	case 1: transpose = 0; row = (reg >> 5) & 3; break;
	case 2: row = (reg >> 5) & 2; break;
	case 3: row = (reg >> 6) & 1; break;
	default: row = (reg >> 5) & 2; break;
	}
	int mtx = (reg >> 2) & 7, col = reg & 3;
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			idx[j * 4 + i] = transpose ? store_index(mtx, row + i, col + j) : store_index(mtx, col + j, row + i);
		}
	}
}
static void read_mat(CPU *c, int reg, int n, float *m) {
	int idx[16];
	mat_index(reg, n, idx);
	memset(m, 0, 16 * sizeof(float));
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++) m[j * 4 + i] = c->v[idx[j * 4 + i]];
}
static void write_mat(CPU *c, int reg, int n, const float *m) {
	int idx[16];
	mat_index(reg, n, idx);
	for (int j = 0; j < n; j++)
		for (int i = 0; i < n; i++) c->v[idx[j * 4 + i]] = m[j * 4 + i];
}

static void apply_st(float *r, uint32_t pfx, int n) {
	if (pfx == 0xE4) return;
	static const float consts[8] = {0.f, 1.f, 2.f, 0.5f, 3.f, 1.f / 3.f, 0.25f, 1.f / 6.f};
	float orig[4] = {0, 0, 0, 0};
	memcpy(orig, r, (size_t)n * sizeof(float));
	for (int i = 0; i < n; i++) {
		int sel = (pfx >> (i * 2)) & 3, ab = (pfx >> (8 + i)) & 1, cst = (pfx >> (12 + i)) & 1, neg = (pfx >> (16 + i)) & 1;
		r[i] = cst ? consts[sel + (ab << 2)] : (ab ? fabsf(orig[sel]) : orig[sel]);
		if (neg) r[i] = -r[i];
	}
}

// Writes with D-prefix saturation and write mask, then resets all prefixes.
static void write_vec(CPU *c, int reg, int n, float *v) {
	uint32_t d = c->vfpu_ctrl[PFX_D];
	int r[4];
	vregs(reg, n, r);
	for (int i = 0; i < n; i++) {
		int sat = (d >> (i * 2)) & 3, mask = (d >> (8 + i)) & 1;
		float x = v[i];
		if (sat == 1) x = x < 0 ? 0 : (x > 1 ? 1 : x);
		else if (sat == 3) x = x < -1 ? -1 : (x > 1 ? 1 : x);
		if (!mask) c->v[r[i]] = x;
	}
}

static void eat_prefixes(CPU *c) {
	c->vfpu_ctrl[PFX_S] = 0xE4;
	c->vfpu_ctrl[PFX_T] = 0xE4;
	c->vfpu_ctrl[PFX_D] = 0;
}

static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static float half_to_float(uint16_t h) {
	uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF;
	if (e == 0) return m ? (s ? -1.f : 1.f) * ldexpf((float)m, -24) : u2f(s);
	if (e == 31) return u2f(s | 0x7F800000u | (m << 13));
	return u2f(s | ((e + 112) << 23) | (m << 13));
}

static _Noreturn void unknown(uint32_t pc, uint32_t op) { rt_fatal("VFPU instruction %08x at %08x not implemented", op, pc); }

void rt_vfpu(CPU *c, uint32_t pc, uint32_t op) {
	uint32_t o = op >> 26;
	int vd = op & 0x7F, vs = (op >> 8) & 0x7F, vt = (op >> 16) & 0x7F, n = vsize(op);
	float s[4] = {0}, t[4] = {0}, d[4] = {0};
	int rs = (op >> 21) & 31;

	switch (o) {
	case 0x32: case 0x3A: {  // lv.s / sv.s
		int reg = (int)(((op >> 16) & 0x1F) | ((op & 3) << 5)), r[4];
		vregs(reg, 1, r);
		uint32_t addr = c->r[rs] + (uint32_t)(int32_t)(int16_t)(op & 0xFFFC);
		if (o == 0x32) c->v[r[0]] = u2f(M32(addr));
		else M32(addr) = f2u(c->v[r[0]]);
		return;
	}
	case 0x36: case 0x3E: {  // lv.q / sv.q
		int reg = (int)(((op >> 16) & 0x1F) | ((op & 1) << 5)), r[4];
		vregs(reg, 4, r);
		uint32_t addr = c->r[rs] + (uint32_t)(int32_t)(int16_t)(op & 0xFFFC);
		for (int i = 0; i < 4; i++) {
			if (o == 0x36) c->v[r[i]] = u2f(M32(addr + 4u * i));
			else M32(addr + 4u * i) = f2u(c->v[r[i]]);
		}
		return;
	}
	case 0x12: {  // mfv/mfvc (rs=3), mtv/mtvc (rs=7)
		int rt = (op >> 16) & 31, imm = op & 0xFF;
		if (rs == 3) {
			uint32_t v;
			if (imm < 128) {
				int r[4];
				vregs(imm, 1, r);
				v = f2u(c->v[r[0]]);
			}
			else {
				v = c->vfpu_ctrl[(imm - 128) & 15];
			}
			if (rt) c->r[rt] = v;
		}
		else if (rs == 7) {
			if (imm < 128) {
				int r[4];
				vregs(imm, 1, r);
				c->v[r[0]] = u2f(c->r[rt]);
			}
			else {
				c->vfpu_ctrl[(imm - 128) & 15] = c->r[rt];
			}
		}
		else unknown(pc, op);
		return;
	}
	case 0x37: {  // prefixes, viim, vfim
		int sub = (op >> 23) & 7;
		if (sub < 2) c->vfpu_ctrl[PFX_S] = op & 0xFFFFF;
		else if (sub < 4) c->vfpu_ctrl[PFX_T] = op & 0xFFFFF;
		else if (sub < 6) c->vfpu_ctrl[PFX_D] = op & 0xFFF;
		else {
			int reg = (op >> 16) & 0x7F;
			d[0] = sub == 6 ? (float)(int16_t)(op & 0xFFFF) : half_to_float((uint16_t)op);
			write_vec(c, reg, 1, d);
			eat_prefixes(c);
		}
		return;
	}
	case 0x3F:  // vflush / vnop / vsync
		return;
	case 0x18: case 0x19: case 0x1B: {  // binary vector ops
		int sub = (op >> 23) & 7;
		read_vec(c, vs, n, s);
		read_vec(c, vt, n, t);
		apply_st(s, c->vfpu_ctrl[PFX_S], n);
		apply_st(t, c->vfpu_ctrl[PFX_T], n);
		int outn = n;
		if (o == 0x18) {
			for (int i = 0; i < n; i++) {
				switch (sub) {
				case 0: d[i] = s[i] + t[i]; break;
				case 1: d[i] = s[i] - t[i]; break;
				case 7: d[i] = s[i] / t[i]; break;
				default: unknown(pc, op);
				}
			}
		}
		else if (o == 0x19) {
			switch (sub) {
			case 0: for (int i = 0; i < n; i++) d[i] = s[i] * t[i]; break;  // vmul
			case 1: d[0] = 0; for (int i = 0; i < n; i++) d[0] += s[i] * t[i]; outn = 1; break;  // vdot
			case 2: {  // vscl: t is a single register
				float tv[4];
				read_vec(c, vt, 1, tv);
				for (int i = 0; i < n; i++) d[i] = s[i] * tv[0];
				break;
			}
			case 4: d[0] = t[n - 1]; for (int i = 0; i < n - 1; i++) d[0] += s[i] * t[i]; outn = 1; break;  // vhdp
			case 6: d[0] = s[0] * t[1] - s[1] * t[0]; outn = 1; break;  // vdet
			default: unknown(pc, op);
			}
		}
		else {  // 0x1B
			if (sub == 0) {  // vcmp
				int cond = op & 0xF, bits = 0, any = 0, all = 1;
				for (int i = 0; i < n; i++) {
					bool r;
					switch (cond) {
					case 0: r = false; break;
					case 1: r = s[i] == t[i]; break;
					case 2: r = s[i] < t[i]; break;
					case 3: r = s[i] <= t[i]; break;
					case 4: r = true; break;
					case 5: r = s[i] != t[i]; break;
					case 6: r = s[i] >= t[i]; break;
					case 7: r = s[i] > t[i]; break;
					case 8: r = s[i] == 0; break;
					case 9: r = isnan(s[i]); break;
					case 10: r = isinf(s[i]); break;
					case 11: r = isnan(s[i]) || isinf(s[i]); break;
					case 12: r = s[i] != 0; break;
					case 13: r = !isnan(s[i]); break;
					case 14: r = !isinf(s[i]); break;
					default: r = !(isnan(s[i]) || isinf(s[i])); break;
					}
					bits |= (int)r << i;
					any |= r;
					all &= r;
				}
				c->vfpu_ctrl[CC] = (c->vfpu_ctrl[CC] & ~0x3Fu) | (uint32_t)bits | ((uint32_t)any << 4) | ((uint32_t)all << 5);
				eat_prefixes(c);
				return;
			}
			for (int i = 0; i < n; i++) {
				switch (sub) {
				case 2: d[i] = fminf(s[i], t[i]); break;
				case 3: d[i] = fmaxf(s[i], t[i]); break;
				case 6: d[i] = s[i] >= t[i] ? 1.f : 0.f; break;
				case 7: d[i] = s[i] < t[i] ? 1.f : 0.f; break;
				default: unknown(pc, op);
				}
			}
		}
		write_vec(c, vd, outn, d);
		eat_prefixes(c);
		return;
	}
	case 0x34: {
		int sub = (op >> 21) & 31, fn = (op >> 16) & 31;
		if (sub == 3) {  // vcst
			static const float k[20] = {0.f, 3.40282347e38f, 1.41421356f, 0.70710678f, 1.12837917f, 0.63661977f,
			                            0.31830989f, 0.78539816f, 1.57079633f, 3.14159265f, 2.71828183f, 1.44269504f,
			                            0.43429448f, 0.69314718f, 2.30258509f, 6.28318531f, 0.52359878f, 0.30103000f,
			                            3.32192809f, 0.86602540f};
			for (int i = 0; i < n; i++) d[i] = fn < 20 ? k[fn] : 0.f;
			write_vec(c, vd, n, d);
			eat_prefixes(c);
			return;
		}
		if (sub >= 16 && sub <= 20) {  // vf2in/iz/iu/id, vi2f (scale 2^imm)
			read_vec(c, vs, n, s);
			apply_st(s, c->vfpu_ctrl[PFX_S], n);
			int imm = fn;
			uint32_t u[4];
			for (int i = 0; i < n; i++) {
				if (sub == 20) {
					uint32_t bits = f2u(s[i]);
					d[i] = ldexpf((float)(int32_t)bits, -imm);
					continue;
				}
				double x = ldexp((double)s[i], imm), r;
				switch (sub) {
				case 16: r = nearbyint(x); break;
				case 17: r = trunc(x); break;
				case 18: r = ceil(x); break;
				default: r = floor(x); break;
				}
				int32_t iv = isnan(x) ? 0x7FFFFFFF : r >= 2147483647.0 ? 0x7FFFFFFF : r <= -2147483648.0 ? INT32_MIN : (int32_t)r;
				u[i] = (uint32_t)iv;
				d[i] = u2f(u[i]);
			}
			write_vec(c, vd, n, d);
			eat_prefixes(c);
			return;
		}
		if (sub != 0 && sub != 2) unknown(pc, op);
		read_vec(c, vs, n, s);
		apply_st(s, c->vfpu_ctrl[PFX_S], n);
		int outn = n;
		if (sub == 0) {
			for (int i = 0; i < n; i++) {
				switch (fn) {
				case 0x00: d[i] = s[i]; break;
				case 0x01: d[i] = fabsf(s[i]); break;
				case 0x02: d[i] = -s[i]; break;
				case 0x06: d[i] = 0.f; break;
				case 0x07: d[i] = 1.f; break;
				case 0x10: d[i] = 1.f / s[i]; break;
				case 0x11: d[i] = 1.f / sqrtf(s[i]); break;
				case 0x12: d[i] = sinf(s[i] * (float)M_PI_2); break;
				case 0x13: d[i] = cosf(s[i] * (float)M_PI_2); break;
				case 0x14: d[i] = exp2f(s[i]); break;
				case 0x15: d[i] = log2f(s[i]); break;
				case 0x16: d[i] = sqrtf(s[i]); break;
				case 0x17: d[i] = asinf(s[i]) / (float)M_PI_2; break;
				case 0x18: d[i] = -1.f / s[i]; break;
				default: unknown(pc, op);
				}
			}
		}
		else {  // sub == 2: VFPU9 group
			switch (fn) {
			case 0x04: for (int i = 0; i < n; i++) d[i] = 1.f - s[i]; break;  // vocp
			case 0x06: d[0] = 0; for (int i = 0; i < n; i++) d[0] += s[i]; outn = 1; break;  // vfad
			case 0x07: d[0] = 0; for (int i = 0; i < n; i++) d[0] += s[i]; d[0] /= (float)n; outn = 1; break;  // vavg
			case 0x0A: for (int i = 0; i < n; i++) d[i] = s[i] > 0 ? 1.f : s[i] < 0 ? -1.f : 0.f; break;  // vsgn
			default: unknown(pc, op);
			}
		}
		write_vec(c, vd, outn, d);
		eat_prefixes(c);
		return;
	}
	case 0x3C: {  // matrix ops
		int idx = (op >> 21) & 31, group = idx >> 2;
		float ms[16], mt[16], md[16];
		if (idx == 28) {  // vmmov / vmidt / vmzero / vmone
			int fn = (op >> 16) & 15;
			if (fn == 0) read_mat(c, vs, n, md);
			for (int j = 0; j < n; j++)
				for (int i = 0; i < n; i++) {
					if (fn == 3) md[j * 4 + i] = i == j ? 1.f : 0.f;
					else if (fn == 6) md[j * 4 + i] = 0.f;
					else if (fn == 7) md[j * 4 + i] = 1.f;
					else if (fn != 0) unknown(pc, op);
				}
			write_mat(c, vd, n, md);
			eat_prefixes(c);
			return;
		}
		switch (group) {
		case 0:  // vmmul
			read_mat(c, vs, n, ms);
			read_mat(c, vt, n, mt);
			for (int a = 0; a < n; a++)
				for (int b = 0; b < n; b++) {
					float sum = 0;
					for (int k = 0; k < n; k++) sum += ms[b * 4 + k] * mt[a * 4 + k];
					md[a * 4 + b] = sum;
				}
			write_mat(c, vd, n, md);
			break;
		case 1: case 2: case 3: {  // vtfmN / vhtfmN
			int ins = group;  // rows - 1
			read_mat(c, vs, ins + 1, ms);
			float tv[4] = {0, 0, 0, 0};
			int tn = n < ins + 1 ? n : ins + 1;
			read_vec(c, vt, tn, tv);
			if (n <= ins) tv[ins] = 1.f;  // homogeneous form
			for (int i = 0; i <= ins; i++) {
				d[i] = 0;
				for (int k = 0; k <= ins; k++) d[i] += ms[i * 4 + k] * tv[k];
			}
			write_vec(c, vd, ins + 1, d);
			break;
		}
		case 4: {  // vmscl
			read_mat(c, vs, n, ms);
			float sc[4];
			read_vec(c, vt, 1, sc);
			for (int j = 0; j < n; j++)
				for (int i = 0; i < n; i++) md[j * 4 + i] = ms[j * 4 + i] * sc[0];
			write_mat(c, vd, n, md);
			break;
		}
		case 5:  // vcrsp.t / vqmul.q
			read_vec(c, vs, n, s);
			read_vec(c, vt, n, t);
			if (n == 3) {
				d[0] = s[1] * t[2] - s[2] * t[1];
				d[1] = s[2] * t[0] - s[0] * t[2];
				d[2] = s[0] * t[1] - s[1] * t[0];
			}
			else if (n == 4) {
				d[0] = s[0] * t[3] + s[1] * t[2] - s[2] * t[1] + s[3] * t[0];
				d[1] = -s[0] * t[2] + s[1] * t[3] + s[2] * t[0] + s[3] * t[1];
				d[2] = s[0] * t[1] - s[1] * t[0] + s[2] * t[3] + s[3] * t[2];
				d[3] = -s[0] * t[0] - s[1] * t[1] - s[2] * t[2] + s[3] * t[3];
			}
			else unknown(pc, op);
			write_vec(c, vd, n, d);
			break;
		default: unknown(pc, op);
		}
		eat_prefixes(c);
		return;
	}
	default: unknown(pc, op);
	}
}
