#!/usr/bin/env python3
"""Static recompiler: Allegrex (PSP MIPS) machine code -> C.

Inputs:
  generated/image.bin, generated/meta.json   (prx_load.py)
  re/export/program.json                      (Ghidra: functions, jump tables)
Outputs (in generated/):
  funcs.h          prototypes for every recompiled function
  code_NNN.c       function bodies
  func_table.c     sorted address -> function table (indirect calls)
  hle.h / hle_stubs.c  wrappers for the 202 imported PSP OS functions
  recomp_report.txt    anything the translator could not handle

  python3 recomp.py PORT_DIR PROGRAM_JSON
"""
import bisect
import json
import struct
import sys
from collections import defaultdict
from pathlib import Path

FUNCS_PER_FILE = 250
HOOKS = set()  # function entries replaced by hand-written code (port/hooks.txt)
PATCHES = {}  # instruction address -> C run right after it (port/patches.txt)
PATCH_DECLS = []  # "decl ..." lines of port/patches.txt: what that C calls, declared in funcs.h


class Ctx:
    def __init__(self, img, base):
        self.img, self.base = img, base

    def word(self, a):
        return struct.unpack_from("<I", self.img, a - self.base)[0]


def simm(op):
    v = op & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def R(n):
    return "0u" if n == 0 else f"c->r[{n}]"


def W(n, expr):
    return "" if n == 0 else f"c->r[{n}] = {expr};"


def addr_expr(op):
    rs, imm = (op >> 21) & 31, simm(op)
    if rs == 0:
        return f"0x{imm & 0xFFFFFFFF:x}u"
    return f"({R(rs)} + {imm}u)" if imm >= 0 else f"({R(rs)} - {-imm}u)"


def emit_simple(op, pc, unknown):
    """C for one non-control-flow instruction (no delay-slot semantics)."""
    o = op >> 26
    rs, rt, rd, sa, fn = (op >> 21) & 31, (op >> 16) & 31, (op >> 11) & 31, (op >> 6) & 31, op & 63
    if op == 0:
        return ""
    if o == 0:
        if fn == 0x00: return W(rd, f"{R(rt)} << {sa}")
        if fn == 0x02:
            if rs == 1: return W(rd, f"({R(rt)} >> {sa}) | ({R(rt)} << {(32 - sa) & 31})" if sa else R(rt))
            return W(rd, f"{R(rt)} >> {sa}")
        if fn == 0x03: return W(rd, f"(uint32_t)((int32_t){R(rt)} >> {sa})")
        if fn == 0x04: return W(rd, f"{R(rt)} << ({R(rs)} & 31)")
        if fn == 0x06:
            if sa == 1: return W(rd, f"({R(rt)} >> ({R(rs)} & 31)) | ({R(rt)} << ((32 - ({R(rs)} & 31)) & 31))")
            return W(rd, f"{R(rt)} >> ({R(rs)} & 31)")
        if fn == 0x07: return W(rd, f"(uint32_t)((int32_t){R(rt)} >> ({R(rs)} & 31))")
        if fn == 0x0A: return f"if ({R(rt)} == 0) {{ {W(rd, R(rs))} }}" if rd else ""
        if fn == 0x0B: return f"if ({R(rt)} != 0) {{ {W(rd, R(rs))} }}" if rd else ""
        if fn == 0x0D: return f"rt_break(c, 0x{pc:08x}u, 0x{(op >> 6) & 0xFFFFF:x}u);"
        if fn == 0x0F: return ""  # sync
        if fn == 0x10: return W(rd, "c->hi")
        if fn == 0x11: return f"c->hi = {R(rs)};"
        if fn == 0x12: return W(rd, "c->lo")
        if fn == 0x13: return f"c->lo = {R(rs)};"
        if fn == 0x16: return W(rd, f"({R(rs)} ? (uint32_t)__builtin_clz({R(rs)}) : 32u)")
        if fn == 0x17: return W(rd, f"(~{R(rs)} ? (uint32_t)__builtin_clz(~{R(rs)}) : 32u)")
        if fn in (0x18, 0x19, 0x1C, 0x1D, 0x2E, 0x2F):
            signed = fn in (0x18, 0x1C, 0x2E)
            prod = (f"(int64_t)(int32_t){R(rs)} * (int64_t)(int32_t){R(rt)}" if signed
                    else f"(uint64_t){R(rs)} * (uint64_t){R(rt)}")
            if fn in (0x18, 0x19):
                return f"{{ uint64_t p = (uint64_t)({prod}); c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}"
            sign = "+" if fn in (0x1C, 0x1D) else "-"
            return (f"{{ uint64_t acc = ((uint64_t)c->hi << 32) | c->lo; acc = acc {sign} (uint64_t)({prod}); "
                    f"c->lo = (uint32_t)acc; c->hi = (uint32_t)(acc >> 32); }}")
        if fn == 0x1A: return f"rt_div(c, {R(rs)}, {R(rt)});"
        if fn == 0x1B: return f"rt_divu(c, {R(rs)}, {R(rt)});"
        if fn in (0x20, 0x21): return W(rd, f"{R(rs)} + {R(rt)}")
        if fn in (0x22, 0x23): return W(rd, f"{R(rs)} - {R(rt)}")
        if fn == 0x24: return W(rd, f"{R(rs)} & {R(rt)}")
        if fn == 0x25: return W(rd, f"{R(rs)} | {R(rt)}")
        if fn == 0x26: return W(rd, f"{R(rs)} ^ {R(rt)}")
        if fn == 0x27: return W(rd, f"~({R(rs)} | {R(rt)})")
        if fn == 0x2A: return W(rd, f"(uint32_t)((int32_t){R(rs)} < (int32_t){R(rt)})")
        if fn == 0x2B: return W(rd, f"(uint32_t)({R(rs)} < {R(rt)})")
        if fn == 0x2C: return W(rd, f"((int32_t){R(rs)} > (int32_t){R(rt)} ? {R(rs)} : {R(rt)})")
        if fn == 0x2D: return W(rd, f"((int32_t){R(rs)} < (int32_t){R(rt)} ? {R(rs)} : {R(rt)})")
    if o in (0x08, 0x09): return W(rt, f"{R(rs)} + {simm(op) & 0xFFFFFFFF:#x}u")
    if o == 0x0A: return W(rt, f"(uint32_t)((int32_t){R(rs)} < {simm(op)})")
    if o == 0x0B: return W(rt, f"(uint32_t)({R(rs)} < {simm(op) & 0xFFFFFFFF:#x}u)")
    if o == 0x0C: return W(rt, f"{R(rs)} & {op & 0xFFFF:#x}u")
    if o == 0x0D: return W(rt, f"{R(rs)} | {op & 0xFFFF:#x}u")
    if o == 0x0E: return W(rt, f"{R(rs)} ^ {op & 0xFFFF:#x}u")
    if o == 0x0F: return W(rt, f"{(op & 0xFFFF) << 16:#x}u")
    a = addr_expr(op)
    if o == 0x20: return W(rt, f"S8({a})")
    if o == 0x21: return W(rt, f"S16({a})")
    if o in (0x23, 0x30): return W(rt, f"M32({a})")
    if o == 0x24: return W(rt, f"M8({a})")
    if o == 0x25: return W(rt, f"M16({a})")
    if o == 0x22: return f"rt_lwl(c, {rt}, {a});" if rt else ""
    if o == 0x26: return f"rt_lwr(c, {rt}, {a});" if rt else ""
    if o == 0x28: return f"M8({a}) = (uint8_t){R(rt)};"
    if o == 0x29: return f"M16({a}) = (uint16_t){R(rt)};"
    if o == 0x2B: return f"M32({a}) = {R(rt)};"
    if o == 0x38: return f"M32({a}) = {R(rt)}; {W(rt, '1u')}"
    if o == 0x2A: return f"rt_swl(c, {rt}, {a});"
    if o == 0x2E: return f"rt_swr(c, {rt}, {a});"
    if o == 0x2F: return ""  # cache
    if o == 0x31: return f"c->fpr.u[{rt}] = M32({a});"
    if o == 0x39: return f"M32({a}) = c->fpr.u[{rt}];"
    if o == 0x1F:
        if fn == 0x00:  # ext
            size = rd + 1
            mask = (1 << size) - 1 if size < 32 else 0xFFFFFFFF
            return W(rt, f"({R(rs)} >> {sa}) & {mask:#x}u")
        if fn == 0x04:  # ins
            size = rd - sa + 1
            mask = ((1 << size) - 1) << sa & 0xFFFFFFFF
            return W(rt, f"({R(rt)} & {~mask & 0xFFFFFFFF:#x}u) | (({R(rs)} << {sa}) & {mask:#x}u)")
        if fn == 0x20:
            if sa == 0x02: return W(rd, f"((({R(rt)} & 0xff00ff00u) >> 8) | (({R(rt)} & 0x00ff00ffu) << 8))")
            if sa == 0x03: return W(rd, f"__builtin_bswap32({R(rt)})")
            if sa == 0x10: return W(rd, f"(uint32_t)(int32_t)(int8_t){R(rt)}")
            if sa == 0x14: return W(rd, f"rt_bitrev({R(rt)})")
            if sa == 0x18: return W(rd, f"(uint32_t)(int32_t)(int16_t){R(rt)}")
    if o == 0x11:
        fs, ft, fd = rd, rt, sa
        if rs == 0x00: return W(rt, f"c->fpr.u[{fs}]")
        if rs == 0x04: return f"c->fpr.u[{fs}] = {R(rt)};"
        if rs == 0x02: return W(rt, "c->fcr31") if fs == 31 else W(rt, "0x3351u")
        if rs == 0x06: return f"c->fcr31 = {R(rt)};" if fs == 31 else ""
        if rs == 0x10:
            F = lambda n: f"c->fpr.f[{n}]"
            if fn == 0x00: return f"{F(fd)} = {F(fs)} + {F(ft)};"
            if fn == 0x01: return f"{F(fd)} = {F(fs)} - {F(ft)};"
            if fn == 0x02: return f"{F(fd)} = {F(fs)} * {F(ft)};"
            if fn == 0x03: return f"{F(fd)} = {F(fs)} / {F(ft)};"
            if fn == 0x04: return f"{F(fd)} = sqrtf({F(fs)});"
            if fn == 0x05: return f"c->fpr.u[{fd}] = c->fpr.u[{fs}] & 0x7fffffffu;"
            if fn == 0x06: return f"c->fpr.u[{fd}] = c->fpr.u[{fs}];"
            if fn == 0x07: return f"c->fpr.u[{fd}] = c->fpr.u[{fs}] ^ 0x80000000u;"
            if fn in (0x0C, 0x0D, 0x0E, 0x0F):
                return f"c->fpr.u[{fd}] = rt_f2i({F(fs)}, {fn - 0x0C});"
            if fn == 0x24:  # cvt.w.s uses the current rounding mode (same numbering as rt_f2i)
                return f"c->fpr.u[{fd}] = rt_f2i({F(fs)}, (int)(c->fcr31 & 3));"
            if 0x30 <= fn <= 0x3F:
                cond = fn & 7
                parts = []
                if cond & 1: parts.append(f"(isnan({F(fs)}) || isnan({F(ft)}))")
                if cond & 2: parts.append(f"({F(fs)} == {F(ft)})")
                if cond & 4: parts.append(f"({F(fs)} < {F(ft)})")
                expr = " || ".join(parts) if parts else "0"
                return f"c->fcr31 = ({expr}) ? (c->fcr31 | 0x800000u) : (c->fcr31 & ~0x800000u);"
        if rs == 0x14 and fn == 0x20:
            return f"c->fpr.f[{fd}] = (float)(int32_t)c->fpr.u[{fs}];"
    # The VFPU forms run most often, as C (the same as runtime/vfpu.c does them): loads and stores
    # (lv.s/sv.s/lv.q/sv.q, which prefixes don't touch), the prefixes themselves and vnop/vflush/
    # vsync. Every other VFPU encoding (COP2 moves, arithmetic) goes to the runtime.
    if o in (0x32, 0x3A, 0x36, 0x3E):
        quad = o in (0x36, 0x3E)
        reg = ((op >> 16) & 0x1F) | (((op & 1) if quad else (op & 3)) << 5)
        idx = vfpu_regs(reg, 4 if quad else 1)
        imm = op & 0xFFFC
        imm = imm - 0x10000 if imm & 0x8000 else imm
        base = f"c->r[{(op >> 21) & 31}] + {imm & 0xFFFFFFFF:#x}u"
        load = o in (0x32, 0x36)
        parts = [f"VLD({k}, a_ + {4 * i}u);" if load else f"VST({k}, a_ + {4 * i}u);" for i, k in enumerate(idx)]
        return "{ uint32_t a_ = " + base + "; " + " ".join(parts) + " }"
    if o == 0x3F:
        return "/* vnop / vflush / vsync */"
    if o == 0x37 and ((op >> 23) & 7) < 6:
        sub = (op >> 23) & 7
        which, val = (0, op & 0xFFFFF) if sub < 2 else (1, op & 0xFFFFF) if sub < 4 else (2, op & 0xFFF)
        return f"c->vfpu_ctrl[{which}] = {val:#x}u;"
    if o == 0x12 and ((op >> 21) & 31) in (3, 7) and (op & 0xFF) < 128:  # mfv / mtv (prefixes play no part)
        rt, k = (op >> 16) & 31, vfpu_regs(op & 0xFF, 1)[0]
        if (op >> 21) & 31 == 3:
            return f"{{ uint32_t t_; memcpy(&t_, &c->v[{k}], 4); c->r[{rt}] = t_; }}" if rt else "/* mfv to $zero */"
        return f"memcpy(&c->v[{k}], &c->r[{rt}], 4);"
    pfx = vfpu_prefixes_before(pc)
    fast = vfpu_fast(op, pfx)
    if fast and pfx == VPFX_NONE:  # arithmetic as C while no prefix is set (else the runtime applies them)
        return f"VFAST(0x{pc:08x}u, 0x{op:08x}u, {{ {fast} }});"
    if fast:  # with the prefixes the vpfx instructions right before set, when they are those as it runs
        return f"VFASTP(0x{pc:08x}u, 0x{op:08x}u, {pfx[0]:#x}u, {pfx[1]:#x}u, {pfx[2]:#x}u, {{ {fast} }});"
    if o in (0x12, 0x18, 0x19, 0x1B, 0x32, 0x34, 0x35, 0x36, 0x37, 0x3A, 0x3C, 0x3D, 0x3E, 0x3F):
        return f"rt_vfpu(c, 0x{pc:08x}u, 0x{op:08x}u);"
    unknown.append(f"{pc:08x} {op:08x} unhandled instruction")
    return f"rt_break(c, 0x{pc:08x}u, 0xdead);"


def vfpu_regs(reg, n):
    """Storage indices (v[mtx*16 + col*4 + row]) of an n-element VFPU vector register: runtime/vfpu.c vregs()."""
    mtx, col, transpose = (reg >> 2) & 7, reg & 3, (reg >> 5) & 1
    if n == 1:
        transpose, row = 0, (reg >> 5) & 3
    elif n == 3:
        row = (reg >> 6) & 1
    else:
        row = (reg >> 5) & 2
    at = lambda m, cc, r: m * 16 + (cc & 3) * 4 + (r & 3)
    return [at(mtx, row + i, col) if transpose else at(mtx, col, row + i) for i in range(n)]


def vfpu_mat(reg, n):
    """Storage indices of an n x n VFPU matrix register, as m[j*4 + i] (column j, row i): runtime/vfpu.c mat_index()."""
    transpose = (reg >> 5) & 1
    if n == 1:
        transpose, row = 0, (reg >> 5) & 3
    elif n == 3:
        row = (reg >> 6) & 1
    else:
        row = (reg >> 5) & 2
    mtx, col = (reg >> 2) & 7, reg & 3
    at = lambda m, cc, r: m * 16 + (cc & 3) * 4 + (r & 3)
    idx = [None] * 16
    for j in range(n):
        for i in range(n):
            idx[j * 4 + i] = at(mtx, row + i, col + j) if transpose else at(mtx, col + j, row + i)
    return idx


def VMMUL(MS, MT, MD, n, V, store):
    """vmmul: each element a float sum of products (as runtime/vfpu.c does it here)."""
    dst, exprs = [], []
    for a in range(n):
        for b in range(n):
            dst.append(MD[a * 4 + b])
            e = "0.f"
            for k in range(n):
                e = f"({e} + {V(MS[b * 4 + k])} * {V(MT[a * 4 + k])})"
            exprs.append(e)
    return store(dst, exprs)


CTX = None  # the program being translated (translate sets it): vfpu_prefixes_before reads back from pc


def vfpu_half_bits(h):
    """The float bits of a half float, as runtime/vfpu.c half_to_float makes them."""
    sgn, e, m = (h >> 15) << 31, (h >> 10) & 31, h & 0x3FF
    if e == 0:
        if not m:
            return sgn
        return struct.unpack("<I", struct.pack("<f", (-1.0 if sgn else 1.0) * m * 2.0 ** -24))[0]
    if e == 31:
        return sgn | 0x7F800000 | (m << 13)
    return sgn | ((e + 112) << 23) | (m << 13)


VPFX_NONE = (0xE4, 0xE4, 0)  # the prefixes when none is set: S and T the identity swizzle, D nothing


def vfpu_prefixes_before(pc):
    """The prefixes that the vpfxs / vpfxt / vpfxd right before pc set (the nearest of each kind, up to
    three back), the others as when none is set. Only a guess: the code checks it as it runs
    (VFASTP), something may jump in between."""
    ps, pt, pd = VPFX_NONE
    seen = set()
    for back in (4, 8, 12):
        try:
            op = CTX.word(pc - back)
        except Exception:
            break
        if op >> 26 != 0x37 or ((op >> 23) & 7) >= 6:
            break
        kind = ((op >> 23) & 7) >> 1  # 0 S, 1 T, 2 D
        if kind in seen:
            continue
        seen.add(kind)
        if kind == 0:
            ps = op & 0xFFFFF
        elif kind == 1:
            pt = op & 0xFFFFF
        else:
            pd = op & 0xFFF
    return ps, pt, pd


VCST = ["0.f", "3.40282347e38f", "1.41421356f", "0.70710678f", "1.12837917f", "0.63661977f", "0.31830989f",
        "0.78539816f", "1.57079633f", "3.14159265f", "2.71828183f", "1.44269504f", "0.43429448f", "0.69314718f",
        "2.30258509f", "6.28318531f", "0.52359878f", "0.30103000f", "3.32192809f", "0.86602540f"]
PFX_CONSTS = ["0.f", "1.f", "2.f", "0.5f", "3.f", "(1.f / 3.f)", "0.25f", "(1.f / 6.f)"]


def vfpu_fast(op, pfx=VPFX_NONE):
    """C for the common VFPU forms, computed exactly as runtime/vfpu.c does (same operations in the
    same order, results stored after all reads), with the prefixes pfx (S, T, D) applied as it
    applies them (apply_st, write_vec); None for the forms left to the runtime."""
    ps, pt, pd = pfx
    plain = pfx == VPFX_NONE
    o = op >> 26
    vd, vs, vt = op & 0x7F, (op >> 8) & 0x7F, (op >> 16) & 0x7F
    n = (((op >> 7) & 1) | ((op >> 14) & 2)) + 1
    V = lambda k: f"c->v[{k}]"

    def lane(regs, p, i, m=None):  # lane i of a source with prefix p (apply_st: over its m lanes)
        m = n if m is None else m
        if p == 0xE4:
            return V(regs[i])
        sel, ab, cst, neg = (p >> (2 * i)) & 3, (p >> (8 + i)) & 1, (p >> (12 + i)) & 1, (p >> (16 + i)) & 1
        if cst:
            e = PFX_CONSTS[sel + (ab << 2)]
        else:
            e = V(regs[sel]) if sel < m else "0.f"
            if ab:
                e = f"fabsf({e})"
        return f"(-{e})" if neg else e

    def store(dst, exprs):  # the D prefix's saturation and write mask (write_vec)
        tmps = " ".join(f"float d{i}_ = {e};" for i, e in enumerate(exprs))
        writes = []
        for i, k in enumerate(dst):
            sat, mask = (pd >> (2 * i)) & 3, (pd >> (8 + i)) & 1
            if mask:
                continue
            x = f"d{i}_"
            if sat == 1:
                x = f"({x} < 0 ? 0.f : ({x} > 1 ? 1.f : {x}))"
            elif sat == 3:
                x = f"({x} < -1 ? -1.f : ({x} > 1 ? 1.f : {x}))"
            writes.append(f"{V(k)} = {x};")
        return tmps + " " + " ".join(writes)

    def store_bits(dst, exprs):  # 32-bit patterns into registers (no prefix: see the callers)
        tmps = " ".join(f"uint32_t d{i}_ = {e};" for i, e in enumerate(exprs))
        return tmps + " " + " ".join(f"memcpy(&{V(k)}, &d{i}_, 4);" for i, k in enumerate(dst))

    def elementwise(fmt):
        S, T, D = vfpu_regs(vs, n), vfpu_regs(vt, n), vfpu_regs(vd, n)
        return store(D, [fmt.format(s=lane(S, ps, i), t=lane(T, pt, i)) for i in range(n)])

    def chain(start, terms):  # ((start + a) + b) ... as float, like d += x
        e = start
        for t in terms:
            e = f"({e} + {t})"
        return e

    B = lambda k: f"rt_vbits(c, {k})"  # a register's bits (cpu.h)
    sub = (op >> 23) & 7
    if o == 0x37 and sub in (6, 7):  # viim / vfim: a constant (an integer, a half float)
        k = vfpu_regs((op >> 16) & 0x7F, 1)[0]
        if sub == 6:
            val = float(op & 0xFFFF if not op & 0x8000 else (op & 0xFFFF) - 0x10000)
            bits = struct.unpack("<I", struct.pack("<f", val))[0]
        else:
            bits = vfpu_half_bits(op & 0xFFFF)
        if pd == 0:
            return store_bits([k], [f"{bits:#010x}u"])
        f = struct.unpack("<f", struct.pack("<I", bits))[0]
        if f != f or f in (float("inf"), float("-inf")):
            return None
        return store([k], [f"{float.hex(f)}f" if f else ("-0.f" if bits >> 31 else "0.f")])
    if o == 0x1B and sub == 0:  # vcmp: each lane's condition into CC bits 0-3, any (bit 4), all (bit 5)
        cond = op & 0xF
        S, T = vfpu_regs(vs, n), vfpu_regs(vt, n)
        forms = {0: "0", 1: "{s} == {t}", 2: "{s} < {t}", 3: "{s} <= {t}", 4: "1", 5: "{s} != {t}", 6: "{s} >= {t}",
                 7: "{s} > {t}", 8: "{s} == 0.f", 9: "isnan({s})", 10: "isinf({s})", 11: "(isnan({s}) || isinf({s}))",
                 12: "{s} != 0.f", 13: "!isnan({s})", 14: "!isinf({s})", 15: "!(isnan({s}) || isinf({s}))"}
        lanes = " ".join(f"uint32_t b{i}_ = ({forms[cond].format(s=lane(S, ps, i), t=lane(T, pt, i))}) ? 1u : 0u;"
                         for i in range(n))
        bits = " | ".join(f"b{i}_ << {i}" for i in range(n))
        return (f"{lanes} c->vfpu_ctrl[VFPU_CTRL_CC] = (c->vfpu_ctrl[VFPU_CTRL_CC] & ~0x3Fu) | {bits} | "
                f"({' | '.join(f'b{i}_' for i in range(n))}) << 4 | ({' & '.join(f'b{i}_' for i in range(n))}) << 5;")
    if o == 0x34 and ((op >> 21) & 31) == 1 and ((op >> 16) & 31) >= 24:  # packing and unpacking integers
        if not plain:
            return None
        fn = (op >> 16) & 31
        S = vfpu_regs(vs, n)
        u = [B(S[i]) if i < n else "0u" for i in range(4)]
        if fn in (24, 25):  # vuc2i / vc2i: each byte of the first to the top of a word
            w = [f"(({u[0]} >> {8 * i}) & 0xFFu) * 0x01010101u >> 1" if fn == 24 else f"(({u[0]} >> {8 * i}) & 0xFFu) << 24"
                 for i in range(4)]
        elif fn in (26, 27):  # vus2i / vs2i: each halfword to the top of a word
            w = []
            for i in range(2 if n >= 2 else 1):
                w += [f"({u[i]} & 0xFFFFu) << 15", f"({u[i]} & 0xFFFF0000u) >> 1"] if fn == 26 else \
                     [f"({u[i]} & 0xFFFFu) << 16", f"{u[i]} & 0xFFFF0000u"]
        elif fn in (28, 29):  # vi2uc / vi2c: four words to their top bytes
            parts = [f"((uint32_t)(((int32_t){u[i]} < 0 ? 0 : (int32_t){u[i]}) >> 23) & 0xFFu) << {8 * i}" if fn == 28
                     else f"({u[i]} >> 24) << {8 * i}" for i in range(4)]
            w = [" | ".join(parts)]
        else:  # vi2us / vi2s: pairs of words to halfwords
            w = []
            for i in range((n + 1) // 2):
                lo, hi = u[2 * i], u[2 * i + 1]
                w.append(f"((uint32_t)(((int32_t){lo} < 0 ? 0 : (int32_t){lo}) >> 15) & 0xFFFFu) | "
                         f"((uint32_t)(((int32_t){hi} < 0 ? 0 : (int32_t){hi}) >> 15) & 0xFFFFu) << 16" if fn == 30
                         else f"({lo} >> 16) | ({hi} & 0xFFFF0000u)")
        return store_bits(vfpu_regs(vd, len(w)), w)
    if o == 0x34 and 16 <= ((op >> 21) & 31) <= 20:  # vf2in / vf2iz / vf2iu / vf2id, vi2f: scaled by 2^imm
        if not plain:
            return None
        kind, imm = (op >> 21) & 31, (op >> 16) & 31
        S, D = vfpu_regs(vs, n), vfpu_regs(vd, n)
        if kind == 20:  # exact: an int's float times a power of two stays normal (ldexpf's result)
            return store(D, [f"(float)(int32_t){B(S[i])} * 0x1p-{imm}f" for i in range(n)])
        return store_bits(D, [f"rt_vf2i({V(S[i])}, {imm}, {kind - 16})" for i in range(n)])
    if o == 0x34 and ((op >> 21) & 31) == 3:  # vcst: a constant in every lane
        fn = (op >> 16) & 31
        return store(vfpu_regs(vd, n), [VCST[fn] if fn < 20 else "0.f" for _ in range(n)])
    if o == 0x34 and ((op >> 21) & 31) == 21:  # vcmov: s (S prefix) where the condition holds, else d (T prefix)
        tf, imm3 = (op >> 19) & 1, ((op >> 16) & 31) & 7
        S, D = vfpu_regs(vs, n), vfpu_regs(vd, n)
        cc = "c->vfpu_ctrl[VFPU_CTRL_CC]"
        exprs = []
        for i in range(n):
            s_, d_ = lane(S, ps, i), lane(D, pt, i)
            if imm3 < 6:
                exprs.append(f"(({cc} >> {imm3}) & 1u) == {int(not tf)}u ? {s_} : {d_}")
            elif imm3 == 6:
                exprs.append(f"(({cc} >> {i}) & 1u) == {int(not tf)}u ? {s_} : {d_}")
            else:
                exprs.append(d_)
        return store(D, exprs)
    if o == 0x3C and ((op >> 21) & 31) == 29:  # vrot: a quarter-turn angle's sine and cosine into chosen lanes
        imm = (op >> 16) & 31
        sl, cl = (imm >> 2) & 3, imm & 3
        a = lane(vfpu_regs(vs, 1), ps, 0, 1)
        sn = f"{'-' if imm & 0x10 else ''}sinf({a} * (float)M_PI_2)"
        cs = f"cosf({a} * (float)M_PI_2)"
        return store(vfpu_regs(vd, n), [cs if i == cl else sn if i == sl or sl == cl else "0.f" for i in range(n)])
    if o == 0x18 and sub in (0, 1, 7):
        return elementwise({0: "{s} + {t}", 1: "{s} - {t}", 7: "{s} / {t}"}[sub])
    if o == 0x19:
        if sub == 0:
            return elementwise("{s} * {t}")
        S, T = vfpu_regs(vs, n), vfpu_regs(vt, n)
        if sub == 1:  # vdot
            return store(vfpu_regs(vd, 1), [chain("0.f", [f"{lane(S, ps, i)} * {lane(T, pt, i)}" for i in range(n)])])
        if sub == 2:  # vscl (the runtime reads t's first lane as it is, no T prefix)
            t0 = vfpu_regs(vt, 1)[0]
            return store(vfpu_regs(vd, n), [f"{lane(S, ps, i)} * {V(t0)}" for i in range(n)])
        if sub == 4:  # vhdp
            return store(vfpu_regs(vd, 1), [chain(lane(T, pt, n - 1),
                                                  [f"{lane(S, ps, i)} * {lane(T, pt, i)}" for i in range(n - 1)])])
        return None
    if o == 0x1B and sub in (2, 3, 6, 7):
        return elementwise({2: "fminf({s}, {t})", 3: "fmaxf({s}, {t})", 6: "({s} >= {t} ? 1.f : 0.f)",
                            7: "({s} < {t} ? 1.f : 0.f)"}[sub])
    if o == 0x34 and ((op >> 21) & 31) == 0:
        fn = (op >> 16) & 31
        S, D = vfpu_regs(vs, n), vfpu_regs(vd, n)
        forms = {0x00: "{s}", 0x01: "fabsf({s})", 0x02: "-{s}", 0x06: "0.f", 0x07: "1.f", 0x10: "1.f / {s}",
                 0x16: "sqrtf({s})", 0x04: "({s} <= 0.f ? 0.f : {s} > 1.f ? 1.f : {s})",
                 0x05: "({s} <= -1.f ? -1.f : {s} > 1.f ? 1.f : {s})", 0x11: "1.f / sqrtf({s})",
                 0x12: "sinf({s} * (float)M_PI_2)", 0x13: "cosf({s} * (float)M_PI_2)", 0x14: "exp2f({s})",
                 0x15: "log2f({s})", 0x17: "asinf({s}) / (float)M_PI_2", 0x18: "-1.f / {s}"}
        if fn == 0x03:  # vidt
            one = vd & (3 if n >= 3 else 1)
            return store(D, ["1.f" if i == one else "0.f" for i in range(n)])
        if fn in forms:
            return store(D, [forms[fn].format(s=lane(S, ps, i)) for i in range(n)])
        return None
    if o == 0x3C:
        if not plain:
            return None
        idx = (op >> 21) & 31
        group = idx >> 2
        if idx == 28:  # vmmov / vmidt / vmzero / vmone
            fn = (op >> 16) & 15
            MD = vfpu_mat(vd, n)
            cells = [(j, i) for j in range(n) for i in range(n)]
            if fn == 0:
                MS = vfpu_mat(vs, n)
                return store([MD[j * 4 + i] for j, i in cells], [V(MS[j * 4 + i]) for j, i in cells])
            const = {3: None, 6: "0.f", 7: "1.f"}
            if fn not in const:
                return None
            return store([MD[j * 4 + i] for j, i in cells],
                         [("1.f" if i == j else "0.f") if fn == 3 else const[fn] for j, i in cells])
        if group == 0:  # vmmul
            return VMMUL(vfpu_mat(vs, n), vfpu_mat(vt, n), vfpu_mat(vd, n), n, V, store)
        if group in (1, 2, 3):  # vtfmN / vhtfmN
            ins = group
            MS = vfpu_mat(vs, ins + 1)
            tn = min(n, ins + 1)
            T = vfpu_regs(vt, tn)
            tv = [V(T[k]) if k < tn else "0.f" for k in range(ins + 1)]
            if n <= ins:
                tv[ins] = "1.f"
            exprs = [chain("0.f", [f"{V(MS[i * 4 + k])} * {tv[k]}" for k in range(ins + 1)]) for i in range(ins + 1)]
            return store(vfpu_regs(vd, ins + 1), exprs)
    return None


def emit_patched(op, pc, unknown):
    """emit_simple, followed by the C that port/patches.txt adds after this instruction."""
    s = emit_simple(op, pc, unknown)
    return f"{s} {PATCHES[pc]}".strip() if pc in PATCHES else s


def branch_info(op, pc):
    """Return (kind, target, cond_expr, likely, link) for control-flow ops, else None."""
    o = op >> 26
    rs, rt = (op >> 21) & 31, (op >> 16) & 31
    rel = (pc + 4 + (simm(op) << 2)) & 0xFFFFFFFF
    if o == 0x02: return ("j", (pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2), None, False, False)
    if o == 0x03: return ("jal", (pc & 0xF0000000) | ((op & 0x03FFFFFF) << 2), None, False, True)
    if o == 0 and (op & 63) == 0x08: return ("jr", rs, None, False, False)
    if o == 0 and (op & 63) == 0x09: return ("jalr", rs, (op >> 11) & 31, False, True)
    conds = {
        0x04: f"{R(rs)} == {R(rt)}", 0x05: f"{R(rs)} != {R(rt)}",
        0x06: f"(int32_t){R(rs)} <= 0", 0x07: f"(int32_t){R(rs)} > 0",
        0x14: f"{R(rs)} == {R(rt)}", 0x15: f"{R(rs)} != {R(rt)}",
        0x16: f"(int32_t){R(rs)} <= 0", 0x17: f"(int32_t){R(rs)} > 0",
    }
    if o in conds:
        return ("b", rel, conds[o], o >= 0x14, False)
    if o == 0x01 and rt in (0, 1, 2, 3):
        cond = f"(int32_t){R(rs)} < 0" if rt in (0, 2) else f"(int32_t){R(rs)} >= 0"
        return ("b", rel, cond, rt >= 2, False)
    if o == 0x11 and rs == 0x08:
        cond = "(c->fcr31 & 0x800000u)" if rt & 1 else "!(c->fcr31 & 0x800000u)"
        return ("b", rel, cond, bool(rt & 2), False)
    if o == 0x12 and rs == 0x08:  # bvf/bvt/bvfl/bvtl, cc bit in (op>>18)&7
        bit = (op >> 18) & 7
        cond = f"((c->vfpu_ctrl[VFPU_CTRL_CC] >> {bit}) & 1)"
        if not (op >> 16) & 1:
            cond = "!" + cond
        return ("b", rel, cond, bool((op >> 17) & 1), False)
    return None


def flow(ctx, starts, owned, is_claimed, text_lo, text_hi):
    """Addresses reachable from starts (recursive descent), stopping at code already owned by
    this function or claimed by another one. Returns (new addresses, jal targets seen)."""
    seen, calls, work = set(), set(), list(starts)
    while work:
        pc = work.pop()
        while text_lo <= pc < text_hi and pc not in seen and pc not in owned and not is_claimed(pc):
            seen.add(pc)
            op = ctx.word(pc)
            bi = branch_info(op, pc)
            if not bi:
                pc += 4
                continue
            kind, target = bi[0], bi[1]
            seen.add(pc + 4)  # delay slot
            o, rs, rt = op >> 26, (op >> 21) & 31, (op >> 16) & 31
            always = kind == "j" or (o == 0x04 and rs == rt) or (o == 0x01 and rs == 0 and rt in (1, 3))
            if kind in ("b", "j"):
                work.append(target)
            if kind in ("jal", "jalr"):
                if kind == "jal":
                    calls.add(target)
                pc += 8
                continue
            if kind == "jr" or always:
                break
            pc += 8
    return seen, calls


def to_ranges(addrs):
    out = []
    for a in sorted(addrs):
        if out and out[-1][1] + 1 == a:
            out[-1][1] = a + 3
        else:
            out.append([a, a + 3])
    return [tuple(r) for r in out]


def discover_code(ctx, funcs, imports, text_lo, text_hi):
    """Completes Ghidra's function list: code a function branches or falls into that nobody owns
    joins that function, and unclaimed targets of jal become new functions."""
    def claimed_index():
        rs = sorted((lo, hi) for f in funcs for lo, hi in f["ranges"])
        return rs, [lo for lo, _ in rs]

    def make_is_claimed(rs, starts):
        def is_claimed(a):
            i = bisect.bisect_right(starts, a) - 1
            return i >= 0 and rs[i][0] <= a <= rs[i][1]
        return is_claimed

    entries = {f["entry"] for f in funcs} | set(imports)
    added, extended = [], []
    for _ in range(8):  # new code may call or reach more new code
        rs, starts = claimed_index()
        is_claimed = make_is_claimed(rs, starts)
        pending_calls = set()
        changed = False
        for f in funcs:
            owned = {a for lo, hi in f["ranges"] for a in range(lo, hi + 1, 4)}
            seeds = []
            for pc in sorted(owned):
                bi = branch_info(ctx.word(pc), pc)
                if bi and bi[0] in ("b", "j") and bi[1] not in owned:
                    seeds.append(bi[1])
                if bi and bi[0] == "jal":
                    pending_calls.add(bi[1])
            last = max(owned)
            prev = ctx.word(last - 4)
            pb = branch_info(prev, last - 4)
            lb = branch_info(ctx.word(last), last)
            ends = (pb and (pb[0] in ("jr", "j") or (prev >> 26 == 0x04 and ((prev >> 21) & 31) == ((prev >> 16) & 31)))) \
                or (lb is not None)
            if not ends:
                seeds.append(last + 4)
            new, calls = flow(ctx, seeds, owned, is_claimed, text_lo, text_hi)
            pending_calls |= calls
            if new:
                f["ranges"] = to_ranges(owned | new)
                extended.append((f["entry"], 4 * len(new)))
                rs, starts = claimed_index()
                is_claimed = make_is_claimed(rs, starts)
                changed = True
        for t in sorted(pending_calls):
            if t in entries or is_claimed(t) or not text_lo <= t < text_hi:
                continue
            new, _ = flow(ctx, [t], set(), is_claimed, text_lo, text_hi)
            if new:
                funcs.append({"entry": t, "ranges": to_ranges(new)})
                entries.add(t)
                added.append(t)
                rs, starts = claimed_index()
                is_claimed = make_is_claimed(rs, starts)
                changed = True
        if not changed:
            break
    return added, extended


def translate(ctx, func, funcs_by_entry, jumptables, report):
    global CTX
    CTX = ctx
    entry = func["entry"]
    addrs = sorted({a for lo, hi in func["ranges"] for a in range(lo, hi + 1, 4)})
    inset = set(addrs)
    unknown = []

    # Pass 1: which addresses need labels.
    labels = {entry}
    needs_all_labels = False
    for pc in addrs:
        bi = branch_info(ctx.word(pc), pc)
        if not bi:
            continue
        kind, target = bi[0], bi[1]
        if kind in ("b", "j") and target in inset:
            labels.add(target)
        if kind == "jr" and target != 31:
            tgts = jumptables.get(pc)
            if tgts:
                labels.update(t for t in tgts if t in inset)
            else:
                needs_all_labels = True
    if needs_all_labels:
        labels.update(addrs)

    def jump_to(target, from_pc):
        if target in inset:
            return f"goto L_{target:08x};"
        if target in funcs_by_entry:
            return f"{{ f_{target:08x}(c); return; }}"
        report.append(f"{entry:08x}: branch at {from_pc:08x} to {target:08x} outside function and not an entry")
        return f"rt_bad_jump(c, 0x{from_pc:08x}u, 0x{target:08x}u);"

    suffix = "_orig" if entry in HOOKS else ""
    out = [f"void f_{entry:08x}{suffix}(CPU *c) {{"]
    if addrs[0] != entry:
        out.append(f"\tgoto L_{entry:08x};")
    delay_slots = set()
    emitted_labels = set()

    def label(a):
        if a not in emitted_labels:
            emitted_labels.add(a)
            out.append(f"L_{a:08x}:;")

    prev = None
    for pc in addrs:
        if prev is not None and pc != prev + 4:
            out.append(f"\trt_fallthrough(c, 0x{prev + 4:08x}u);")  # gap between body ranges
        prev = pc
        if pc in delay_slots:
            continue  # emitted together with its branch
        if pc in labels:
            label(pc)
        op = ctx.word(pc)
        bi = branch_info(op, pc)
        if not bi:
            s = emit_patched(op, pc, unknown)
            if s:
                out.append("\t" + s)
            continue

        kind, target, extra, likely, link = bi
        dpc = pc + 4
        dop = ctx.word(dpc)
        if branch_info(dop, dpc):
            report.append(f"{entry:08x}: branch in delay slot at {dpc:08x}")
        delay = emit_patched(dop, dpc, unknown)
        delay_slots.add(dpc)

        if kind == "b":
            if likely:
                out.append(f"\tif ({extra}) {{ {delay} {jump_to(target, pc)} }}")
            else:
                out.append(f"\t{{ int cond = ({extra}); {delay} if (cond) {jump_to(target, pc)} }}")
        elif kind == "j":
            out.append(f"\t{{ {delay} {jump_to(target, pc)} }}")
        elif kind == "jal":
            if target in funcs_by_entry:
                call = f"f_{target:08x}(c);"
            else:
                report.append(f"{entry:08x}: jal at {pc:08x} to non-entry {target:08x}")
                call = f"rt_call(c, 0x{target:08x}u);"
            out.append(f"\t{{ c->r[31] = 0x{pc + 8:08x}u; {delay} {call} }}")
        elif kind == "jalr":
            out.append(f"\t{{ uint32_t t = {R(target)}; {W(extra, f'0x{pc + 8:08x}u')} {delay} rt_call(c, t); }}")
        elif kind == "jr":
            if target == 31:
                out.append(f"\t{{ {delay} return; }}")
            else:
                cases = sorted(t for t in (jumptables.get(pc) or addrs) if t in inset)
                sw = " ".join(f"case 0x{t:08x}u: goto L_{t:08x};" for t in cases)
                out.append(f"\t{{ uint32_t t = {R(target)}; {delay} switch (t) {{ {sw} "
                           f"default: rt_tail_call(c, 0x{pc:08x}u, t); return; }} }}")

        if dpc in labels:
            # Something jumps straight into this delay slot: give it its own copy, then continue at pc+8.
            nxt = dpc + 4
            out.append(f"\tgoto L_{nxt:08x};" if nxt in inset else f"\trt_fallthrough(c, 0x{nxt:08x}u);")
            label(dpc)
            if delay:
                out.append("\t" + delay)
            if nxt in inset:
                labels.add(nxt)

    out.append(f"\trt_fallthrough(c, 0x{addrs[-1] + 4:08x}u);")
    out.append("}")
    for u in unknown:
        report.append(f"{entry:08x}: {u}")
    return out


def main():
    port, program_json = Path(sys.argv[1]), Path(sys.argv[2])
    gen = port / "generated"
    meta = json.loads((gen / "meta.json").read_text())
    base = int(meta["base"], 16)
    ctx = Ctx((gen / "image.bin").read_bytes(), base)
    prog = json.loads(program_json.read_text())
    for hooks_file in [port / "hooks.txt"] + sorted(port.glob("hooks.*.txt")):  # hooks.private.txt etc., when present
        for line in hooks_file.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if line:
                HOOKS.add(int(line, 16))
    for patches_file in [port / "patches.txt"] + sorted(port.glob("patches.*.txt")):
        if patches_file.exists():
            for line in patches_file.read_text().splitlines():
                line = line.split("#", 1)[0].strip()
                if line.startswith("decl "):
                    PATCH_DECLS.append(line[5:].strip())
                elif line:
                    addr, code = line.split(None, 1)
                    PATCHES[int(addr, 16)] = code

    imports = {int(i["addr"], 16): i for i in meta["imports"]}
    funcs = []
    for f in prog["functions"]:
        entry = int(f["entry"], 16)
        if entry in imports:
            continue
        ranges = [(int(a, 16), int(b, 16)) for a, b in f["ranges"]]
        # Skip pseudo-functions Ghidra found inside data (e.g. .data/.rodata).
        text_lo, text_hi = (int(x, 16) for x in meta["text"])
        if not text_lo <= entry < text_hi:
            continue
        funcs.append({"entry": entry, "ranges": ranges})
    funcs.sort(key=lambda f: f["entry"])

    # Ghidra sometimes leaves holes inside a function body (code it did not follow). Execution
    # falling into one would abort at run time (rt_fallthrough; e.g. the results screen after a
    # round, FUN_089385f4), so holes that no other function claims become part of the function.
    claimed = sorted((lo, hi) for f in funcs for lo, hi in f["ranges"])
    starts = [lo for lo, _ in claimed]

    def is_claimed(a):
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and claimed[i][0] <= a <= claimed[i][1]

    filled = []
    for f in funcs:
        r = sorted(f["ranges"])
        holes = []
        for (_, hi), (lo, _) in zip(r, r[1:]):
            g0, g1 = hi + 1, lo - 1
            if g1 >= g0 and not any(is_claimed(a) for a in range(g0, g1 + 1, 4)):
                holes.append((g0, g1))
        if holes:
            f["ranges"] = sorted(r + holes)
            filled += [(f["entry"], g0, g1 - g0 + 1) for g0, g1 in holes]
    text_lo, text_hi = (int(x, 16) for x in meta["text"])
    added, extended = discover_code(ctx, funcs, imports, text_lo, text_hi)
    funcs.sort(key=lambda f: f["entry"])
    funcs_by_entry = {f["entry"]: f for f in funcs}
    for a in imports:
        funcs_by_entry.setdefault(a, None)

    jumptables = {}
    for j in prog["jumptables"]:
        if j["targets"]:
            jumptables[int(j["at"], 16)] = [int(t, 16) for t in j["targets"]]

    report = []
    # Clean old outputs.
    for p in gen.glob("code_*.c"):
        p.unlink()
    header = ['#pragma once', '#include "cpu.h"']
    header += [f"void f_{f['entry']:08x}(CPU *c);" for f in funcs]
    header += [f"void f_{a:08x}(CPU *c);" for a in sorted(imports)]
    header += [f"void f_{a:08x}_orig(CPU *c);  /* original of a hooked function */" for a in sorted(HOOKS)]
    header += PATCH_DECLS
    (gen / "funcs.h").write_text("\n".join(header) + "\n")

    prelude = ['#include <math.h>', '#include "funcs.h"',
               'static inline uint32_t rt_bitrev(uint32_t v) { uint32_t r = 0; for (int i = 0; i < 32; i++) { r = (r << 1) | (v & 1); v >>= 1; } return r; }',
               '#pragma GCC diagnostic ignored "-Wunused-label"', '']
    n_files = 0
    for i in range(0, len(funcs), FUNCS_PER_FILE):
        body = list(prelude)
        for f in funcs[i:i + FUNCS_PER_FILE]:
            body.extend(translate(ctx, f, funcs_by_entry, jumptables, report))
            body.append("")
        (gen / f"code_{n_files:03d}.c").write_text("\n".join(body))
        n_files += 1

    # HLE imports.
    hle_h = ['#pragma once', '#include "cpu.h"']
    stubs = ['#include <stdio.h>', '#include "hle.h"', '#include "funcs.h"', '']
    seen = set()
    for a, imp in sorted(imports.items()):
        name = imp["name"]
        if name not in seen:
            seen.add(name)
            hle_h.append(f"void hle_{name}(CPU *c);")
            stubs.append(f'__attribute__((weak)) void hle_{name}(CPU *c) {{ rt_hle_unimplemented(c, "{name}"); }}')
        stubs.append(f"void f_{a:08x}(CPU *c) {{ rt_hle_poll(); if (rt_trace > 1) rt_hle_enter(c, \"{name}\"); "
                     f"hle_{name}(c); if (rt_trace > 1) rt_hle_leave(c, \"{name}\"); }}")
    hle_h.append("void rt_hle_unimplemented(CPU *c, const char *name);")
    hle_h.append("void rt_hle_poll(void);")
    hle_h.append("extern int rt_trace;")
    hle_h.append("void rt_hle_enter(CPU *c, const char *name);")
    hle_h.append("void rt_hle_leave(CPU *c, const char *name);")
    (gen / "hle.h").write_text("\n".join(hle_h) + "\n")
    (gen / "hle_stubs.c").write_text("\n".join(stubs) + "\n")

    # Indirect call table: every function entry, sorted.
    entries = sorted([f["entry"] for f in funcs] + list(imports))
    table = ['#include "funcs.h"', '#include <stddef.h>',
             'typedef struct { uint32_t addr; RecompFunc fn; } FuncEntry;',
             f'const size_t rt_func_count = {len(entries)};',
             'const FuncEntry rt_func_table[] = {']
    table += [f"\t{{0x{a:08x}u, f_{a:08x}}}," for a in entries]
    table.append("};")
    (gen / "func_table.c").write_text("\n".join(table) + "\n")

    report.append(f"functions found by flow analysis: {', '.join(f'{a:08x}' for a in added) or 'none'}; "
                  f"bodies extended past Ghidra's ranges: {', '.join(f'{e:08x}+{n}' for e, n in extended) or 'none'}")
    report.append(f"holes filled inside function bodies: {len(filled)} ({sum(n for _, _, n in filled)} bytes); larger ones: " +
                  ", ".join(f"{e:08x}@{a:08x}+{n}" for e, a, n in filled if n > 8))
    missing_ptrs = [p for p in meta["code_pointers"] if int(p, 16) not in funcs_by_entry]
    report.append(f"code pointers that are not function entries (jump-table cases etc.): {len(missing_ptrs)}")
    (gen / "recomp_report.txt").write_text("\n".join(report) + "\n")
    print(f"{len(funcs)} functions -> {n_files} files, {len(imports)} HLE stubs, report lines: {len(report)}")


if __name__ == "__main__":
    main()
