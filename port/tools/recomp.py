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
    # Every VFPU encoding (including COP2 moves and VFPU loads/stores) goes to the runtime.
    if o in (0x12, 0x18, 0x19, 0x1B, 0x32, 0x34, 0x35, 0x36, 0x37, 0x3A, 0x3C, 0x3D, 0x3E, 0x3F):
        return f"rt_vfpu(c, 0x{pc:08x}u, 0x{op:08x}u);"
    unknown.append(f"{pc:08x} {op:08x} unhandled instruction")
    return f"rt_break(c, 0x{pc:08x}u, 0xdead);"


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
            s = emit_simple(op, pc, unknown)
            if s:
                out.append("\t" + s)
            continue

        kind, target, extra, likely, link = bi
        dpc = pc + 4
        dop = ctx.word(dpc)
        if branch_info(dop, dpc):
            report.append(f"{entry:08x}: branch in delay slot at {dpc:08x}")
        delay = emit_simple(dop, dpc, unknown)
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
