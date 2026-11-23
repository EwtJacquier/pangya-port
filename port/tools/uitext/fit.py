"""Fits a vector text style to an original sprite by rendering candidates and comparing pixels.

Model (the same one runtime/text.c draws): the string in a font (regular or bold), grown by
`bold`, then outlines stacked outward (each a width and a color; they may reach deeper below the
letters, `ext`), optionally a soft glow outside them, and the fill on top (a color, or a vertical
gradient). Every candidate is rendered at 1x from a 4x supersampled coverage (outlines from the
distance to the letters, as the runtime) and compared with the original over black and over
white. The fill's color is what its purest pixels show; the outlines' colors are solved by robust
least squares (edt.c). The best few are refined: size, and each line's place and stretch. All
widths are in 1x pixels.
"""
import ctypes
import itertools
import os
import subprocess
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont, ImageStat

K = 4
FONT_DIR = Path(__file__).resolve().parents[2] / 'assets' / 'fonts'
FONT_FILES = [FONT_DIR / 'texgyreadventor-bold.otf', FONT_DIR / 'texgyreadventor-regular.otf']
EM = 200
_fonts = {}


def font(idx, size):
    size = max(1.0, round(size * 4) / 4)
    key = (idx, size)
    if key not in _fonts:
        _fonts[key] = ImageFont.truetype(str(FONT_FILES[idx]), size)
    return _fonts[key]


_ink = {}


def ink(idx, text):
    if (idx, text) not in _ink:
        _ink[(idx, text)] = font(idx, EM).getbbox(text, anchor='ls')
    return _ink[(idx, text)]


def coverage(idx, text, em_px, sx, ox, oy, w, h):
    """4x coverage of the string (L, w*K x h*K), baseline origin (ox, oy) in 1x pixels."""
    big = Image.new('L', (max(1, int(w * K / sx) + 8 * K), h * K), 0)
    ImageDraw.Draw(big).text((ox * K / sx, oy * K), text, fill=255, font=font(idx, em_px * K), anchor='ls')
    return big.resize((max(1, int(round(big.width * sx))), h * K), Image.BILINEAR).crop((0, 0, w * K, h * K))


def med(px):
    n = len(px)
    return tuple(sorted(p[i] for p in px)[n // 2] for i in range(4))


HI = lambda v: 255 if v >= 230 else 0  # noqa: E731
LO = lambda v: 255 if v <= 25 else 0  # noqa: E731


def cdist(a, b):
    return sum((a[i] - b[i]) ** 2 for i in range(3)) ** 0.5


def compose(size, bg, layers, fill_cov, ft, fb, frows):
    """1x model: layers [(cov L 1x, rgba)] outermost first, then the fill (gradient over frows)."""
    out = bg.copy() if bg else Image.new('RGBA', size, (0, 0, 0, 0))
    for cov, c in layers:
        a = cov.point(lambda v, ca=c[3]: v * ca // 255)
        lay = Image.new('RGBA', size, c[:3] + (0,))
        lay.putalpha(a)
        out.alpha_composite(lay)
    if ft[3] == 0 and fb[3] == 0:  # hollow letters: they cut through their outlines, which alone show
        out.putalpha(ImageChops.multiply(out.getchannel('A'), ImageChops.invert(fill_cov)))
        return out
    if ft == fb:
        lay = Image.new('RGBA', size, ft[:3] + (0,))
        lay.putalpha(fill_cov.point(lambda v, ca=ft[3]: v * ca // 255))
    else:
        y0, y1 = frows
        col = Image.new('RGBA', (1, size[1]))
        for y in range(size[1]):
            t = min(1.0, max(0.0, (y - y0) / max(1, y1 - y0)))
            col.putpixel((0, y), tuple(int(ft[i] + (fb[i] - ft[i]) * t + 0.5) for i in range(4)))
        lay = col.resize(size, Image.NEAREST)
        a = ImageChops.multiply(lay.getchannel('A'), fill_cov)
        lay.putalpha(a)
    out.alpha_composite(lay)
    return out


def error(model, orig, weight=None):
    e = 0.0
    for bgc in ((0, 0, 0, 255), (255, 255, 255, 255)):
        a = Image.new('RGBA', orig.size, bgc)
        a.alpha_composite(model)
        b = Image.new('RGBA', orig.size, bgc)
        b.alpha_composite(orig)
        d = ImageChops.difference(a.convert('RGB'), b.convert('RGB'))
        if weight:
            d = ImageChops.multiply(d, weight)
        e += sum(ImageStat.Stat(d).mean)
    return e


def smear(cov4, n):
    """The coverage stretched n 4x-pixels downward (an extruded base for the outlines)."""
    if n <= 0:
        return cov4
    out = cov4.copy()
    for j in range(1, n + 1):
        sh = Image.new('L', cov4.size, 0)
        sh.paste(cov4.crop((0, 0, cov4.width, cov4.height - j)), (0, j))
        out = ImageChops.lighter(out, sh)
    return out


_edt = None


def _edt_lib():
    """tools/uitext/edt.c, built next to the other build outputs on first use."""
    global _edt
    if _edt is None:
        src = Path(__file__).with_name('edt.c')
        lib = Path(__file__).resolve().parents[2] / 'build' / 'uitext_edt.so'
        if not lib.exists() or lib.stat().st_mtime < src.stat().st_mtime:
            lib.parent.mkdir(parents=True, exist_ok=True)
            tmp = lib.with_suffix(f'.{os.getpid()}.so')
            subprocess.run(['gcc', '-O2', '-shared', '-fPIC', '-o', str(tmp), str(src), '-lm'], check=True)
            os.replace(tmp, lib)
        _edt = ctypes.CDLL(str(lib))
        _edt.dist8.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_char_p]
        _edt.lsq_colors.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_char_p,
                                    ctypes.c_char_p, ctypes.POINTER(ctypes.c_float),
                                    ctypes.POINTER(ctypes.c_float), ctypes.c_float,
                                    ctypes.POINTER(ctypes.c_float)]
    return _edt


ROBUST = 40.0


def lsq(layers, orig, bg=None, trow=None, known=None, robust=None):
    """Colors [rgba] of layers (1x coverages, bottom first, the fill last) that best explain orig;
    with trow (t per row) the fill is a gradient and gets two colors (top, bottom); known: the
    fill's color(s), given. None if the layers can't tell their colors apart."""
    w, h = orig.size
    m = len(layers) + (1 if trow else 0)
    out = (ctypes.c_float * (4 * m))()
    tr = (ctypes.c_float * h)(*trow) if trow else None
    kn = (ctypes.c_float * (4 * len(known)))(*[v for c in known for v in c]) if known else None
    ok = _edt_lib().lsq_colors(b''.join(c.tobytes() for c in layers), len(layers), w, h, orig.tobytes(),
                               bg.tobytes() if bg else None, tr, kn, ROBUST if robust is None else robust, out)
    if not ok:
        return None
    return [tuple(int(out[i * 4 + k] + 0.5) for k in range(4)) for i in range(m)]


def dist(cov4):
    """Distance from the letters of a 4x coverage, in eighths of a 4x pixel (L image)."""
    w, h = cov4.size
    out = ctypes.create_string_buffer(w * h)
    _edt_lib().dist8(cov4.tobytes(), w, h, out)
    return Image.frombytes('L', (w, h), out.raw)


_luts = {}


def lut_grow(r4):
    """Coverage of the letters grown by r4 4x-pixels, from their distance (as runtime/text.c)."""
    if ('g', r4) not in _luts:
        _luts[('g', r4)] = [int(min(1.0, max(0.0, r4 - q / 8 + 0.5)) * 255 + 0.5) for q in range(256)]
    return _luts[('g', r4)]


def lut_glow(r4, gw4):
    """A glow gw4 wide outside r4: fading linearly toward the outside (as runtime/text.c)."""
    if ('w', r4, gw4) not in _luts:
        _luts[('w', r4, gw4)] = [int(min(1.0, max(0.0, (r4 + gw4 - q / 8) / gw4)) * 255 + 0.5) for q in range(256)]
    return _luts[('w', r4, gw4)]


class Grown:
    """The letters grown by any number of 4x pixels, at 1x."""

    def __init__(self, cov4, size):
        self.cov4, self.size, self.d, self.cache = cov4, size, dist(cov4), {}

    def __getitem__(self, r4):
        if r4 not in self.cache:
            c = self.cov4 if r4 == 0 else ImageChops.lighter(self.cov4, self.d.point(lut_grow(r4)))
            self.cache[r4] = down(c, self.size)
        return self.cache[r4]


def down(img4, size):
    return img4.resize(size, Image.BOX)


def place(idx, text, box, grow, ext=0.0):
    """em, sx, ox, oy so that the string grown by `grow` (1x px; outlines `ext` px deeper below)
    fills box (x0, y0, x1, y1)."""
    x0, y0, x1, y1 = box
    ix0, iy0, ix1, iy1 = ink(idx, text)
    ih, iw = max(1, iy1 - iy0), max(1, ix1 - ix0)
    th, tw = (y1 - y0) - 2 * grow - ext, (x1 - x0) - 2 * grow
    if th < max(3, 0.45 * (y1 - y0)) or tw < 2:  # the letters, not their outlines, fill the box
        return None
    em = EM * th / ih
    s = em / EM
    sx = tw / (iw * s)
    cx, cy = (x0 + x1) / 2, (y0 + y1 - ext) / 2
    return em, sx, cx - (ix0 + ix1) / 2 * s * sx, cy - (iy0 + iy1) / 2 * s


RING_WIDTHS = (2, 4, 6, 8)  # 4x pixels: 0.5 .. 2 px
EXTS = (0.0, 1.0, 2.0)  # outlines extruded downward, 1x pixels
BOLD_BIAS = 1.03  # the game's letters are thin: the bold font must be clearly better to win


def ring_splits(total):
    """Ways to split `total` 4x pixels into 0..3 outlines."""
    if total == 0:
        yield ()
        return
    for n in (1, 2, 3):
        for ws in itertools.product(RING_WIDTHS, repeat=n):
            if sum(ws) == total:
                yield ws


def empty_error(orig, bg=None):
    return error(bg.copy() if bg else Image.new('RGBA', orig.size, (0, 0, 0, 0)), orig)


def layout(idx, texts, em, centers):
    """Baseline origins of lines from their ink centers: [(ox, oy, sx)]."""
    s_ = em / EM
    out = []
    for t, (cx, cy, sxl) in zip(texts, centers):
        ix0, iy0, ix1, iy1 = ink(idx, t)
        out.append((cx - (ix0 + ix1) / 2 * s_ * sxl, cy - (iy0 + iy1) / 2 * s_, sxl))
    return out


def evaluate(idx, g4, cfgs, em, centers, texts, orig, bg, fixed, ext4=0):
    """Best of cfgs [(bold, ring widths)] (all growing the letters by g4 4x-pixels; the outlines
    extruded ext4 4x-pixels downward) with the lines at `centers` [(cx, cy, sx)] and size em;
    colors measured from orig. A dict, or None."""
    w, h = orig.size
    lines = layout(idx, texts, em, centers)
    cov4 = None
    for t, (lx, ly, lsx) in zip(texts, lines):
        c = coverage(idx, t, em, lsx, lx, ly, w, h)
        cov4 = c if cov4 is None else ImageChops.lighter(cov4, c)
    ch1 = Grown(cov4, (w, h))
    ring1 = Grown(smear(cov4, ext4), (w, h)) if ext4 else ch1
    best = None
    given = bool(fixed and fixed.get('colors'))
    for b, ws in cfgs:
        fill_cov = ch1[b]
        # ring masks outermost first (cumulative from the fill outward)
        cum = [b + sum(ws[:i + 1]) for i in range(len(ws))]  # inside-out
        ring_covs = [ring1[c] for c in reversed(cum)]
        fm = fill_cov.point(HI)
        bb = fm.getbbox() or fill_cov.point(lambda v: 255 if v >= 64 else 0).getbbox()
        if not bb:
            continue
        y0, y1 = bb[1], bb[3] - 1
        layers = list(ring_covs) + [fill_cov]
        if given:
            ring_cols, (ft, fb) = fixed['colors']
            ring_cols = list(ring_cols)
            model = compose((w, h), bg, list(zip(ring_covs, ring_cols)), fill_cov, ft, fb, (y0, y1))
            e = error(model, orig) + 1.5 * len(ws)
        else:
            # The fill's color: what its purest pixels show (thin letters blend into the outline
            # next to them; the 40% farthest from that outline's color). The outlines' colors: the
            # ones that explain the original best, coverages mixed as drawn.
            cols = lsq(layers, orig, bg, robust=0)  # for the inner outline's color, roughly
            if not cols:
                continue
            fm = fill_cov.point(HI)
            if not fm.getbbox():  # a tiny glyph (a dot): its fill is all edge
                fm = fill_cov.point(lambda v: 255 if v >= 64 else 0)
            plain = tuple(int(v) for v in ImageStat.Stat(orig, fm).median)
            if ws:
                d = ImageChops.difference(orig.convert('RGB'), Image.new('RGB', (w, h), cols[-2][:3])).convert('L')
                hist = d.histogram(fm)
                total = sum(hist)
                acc, thr = 0, 0
                for v in range(255, -1, -1):
                    acc += hist[v]
                    if acc >= 0.4 * total:
                        thr = v
                        break
                pure = ImageChops.multiply(fm, d.point(lambda v, t=thr: 255 if v >= t else 0))
                if pure.getbbox():
                    fm = pure
            flat = tuple(int(v) for v in ImageStat.Stat(orig, fm).median)
            band = max(1, (y1 - y0 + 1) // 3)
            topm = fm.crop((0, y0, w, y0 + band))
            botm = fm.crop((0, y1 + 1 - band, w, y1 + 1))
            ft = tuple(int(v) for v in ImageStat.Stat(orig.crop((0, y0, w, y0 + band)), topm).median) if topm.getbbox() else flat
            fb = tuple(int(v) for v in ImageStat.Stat(orig.crop((0, y1 + 1 - band, w, y1 + 1)), botm).median) if botm.getbbox() else flat
            ring_cols = (lsq(layers, orig, bg, known=[flat]) or cols)[:-1] if ws else []
            if ext4 and cdist(ring_cols[-1], flat) < 40:
                continue  # an outline in the fill's color is weight: extruded, it fills the counters
            model = compose((w, h), bg, list(zip(ring_covs, ring_cols)), fill_cov, flat, flat, (y0, y1))
            e = error(model, orig) + 1.5 * len(ws)
            if ws and cdist(plain, flat) > 30:
                # The purest pixels can be the outline's (letters thinner than the original's): the
                # plain median wins when it explains the original clearly better.
                rp = (lsq(layers, orig, bg, known=[plain]) or cols)[:-1]
                ep = error(compose((w, h), bg, list(zip(ring_covs, rp)), fill_cov, plain, plain, (y0, y1)), orig) + 1.5 * len(ws)
                if ep < 0.9 * e:
                    e, ring_cols, flat = ep, rp, plain
                    ft = fb = plain
            if cdist(ft, fb) >= 48:  # a gradient only where the original clearly has one
                trow = [min(1.0, max(0.0, (y - y0) / max(1, y1 - y0))) for y in range(h)]
                rg = (lsq(layers, orig, bg, trow, known=[ft, fb]) or [None] * 2)[:-2] if ws else []
                if None not in rg:
                    gm = compose((w, h), bg, list(zip(ring_covs, rg)), fill_cov, ft, fb, (y0, y1))
                    eg = error(gm, orig) + 1.5 * len(ws)
                    if eg < e * 0.93:
                        e, ring_cols = eg, rg
                    else:
                        ft = fb = flat
                else:
                    ft = fb = flat
            else:
                ft = fb = flat
        if idx == 0:
            e *= BOLD_BIAS
        if not best or e < best['err']:
            best = dict(font=idx, em=em, sx=lines[0][2], ox=lines[0][0], oy=lines[0][1], bold=b / K, lines=lines,
                        rings=[(wd / K, c) for wd, c in zip(reversed(ws), ring_cols)], ft=ft, fb=fb, err=e,
                        glow=None, ring_covs=ring_covs, fill_cov=fill_cov, frows=(y0, y1), g4=g4, ring1=ring1, idx=idx,
                        cfg=(b, ws), centers=centers, ext=ext4 / K)
    return best


def refine(r, texts, orig, bg, fixed, sx_range):
    """Size and line places nudged while the error drops (boxes of touching lines are only a
    first guess: their outlines merge)."""
    idx, g4, cfg, ext4 = r['idx'], r['g4'], r['cfg'], int(round(r['ext'] * K))
    em, centers = r['em'], [list(c) for c in r['centers']]
    cur = r
    for step in (1.0, 0.5, 0.25):
        for _ in range(3):
            improved = False
            moves = [('em', None, 1 + 0.06 * step), ('em', None, 1 - 0.06 * step)]
            for i in range(len(centers)):
                for d in (0.5 * step, -0.5 * step):
                    moves += [('cy', i, d), ('cx', i, d)]
                moves += [('sx', i, 1 + 0.04 * step), ('sx', i, 1 - 0.04 * step)]
            for kind, i, v in moves:
                e2, c2 = em, [list(c) for c in centers]
                if kind == 'em':
                    e2 = em * v
                elif kind == 'cx':
                    c2[i][0] += v
                elif kind == 'cy':
                    c2[i][1] += v
                else:
                    c2[i][2] *= v
                    if not sx_range[0] <= c2[i][2] <= sx_range[1]:
                        continue
                t = evaluate(idx, g4, [cfg], e2, [tuple(c) for c in c2], texts, orig, bg, fixed, ext4)
                if t and t['err'] < cur['err'] - 1e-6:
                    cur, em, centers, improved = t, e2, c2, True
            if not improved:
                break
    return cur


def fit(text, orig, sil_box, bg=None, fonts=(0, 1), fixed=None, sx_range=(0.62, 1.35), polish=True):
    """Best style for `text` drawn in orig (RGBA, 1x). sil_box: bbox of the original letters with
    their outlines. bg: the background under the text (None = transparent). fixed: a config to
    reuse (font, bold, rings) — only colors and placement are fitted.
    Returns dict(font, em, sx, ox, oy, bold, rings [(w, rgba)] outermost first, ft, fb, err) or None."""
    w, h = orig.size
    texts = text if isinstance(text, list) else [text]
    boxes = sil_box if isinstance(text, list) else [sil_box]
    exts = (fixed['ext'],) if fixed and 'ext' in fixed else EXTS
    by_place = {}

    def search(candidates):
        """Evaluates (font, bold, ring widths, ext) candidates, grouped by where they put the text."""
        groups = {}
        for idx, b, ws, e in candidates:
            if ws or not e:  # an extrusion shows only in the outlines
                key = (idx, b + sum(ws), int(round(e * K)))
                groups.setdefault(key, []).append((b, ws))
                by_place.setdefault(key, []).append((b, ws))
        found = []
        for (idx, g4, ext4), cfgs in groups.items():
            # Several lines share one size; each keeps its own width and place. The size comes from the
            # boxes' heights; for lines whose outlines touch, also from their widths (bands cut them).
            grow, ext = g4 / K, ext4 / K
            if fixed and fixed.get('em'):
                ems = [fixed['em']]  # a glyph of a set: the set's size, natural width, centered
            else:
                ps = [place(idx, t, bx, grow, ext) for t, bx in zip(texts, boxes)]
                if not all(ps):
                    continue
                ems = [sorted(p[0] for p in ps)[(len(ps) - 1) // 2]]
                if len(texts) > 1:
                    ew = sorted(EM * ((bx[2] - bx[0]) - 2 * grow) / max(1, ink(idx, t)[2] - ink(idx, t)[0])
                                for t, bx in zip(texts, boxes))
                    ems.append(ew[(len(ew) - 1) // 2])
            for em in ems:
                centers = []
                for t, bx in zip(texts, boxes):
                    x0, y0, x1, y1 = bx
                    ix0, iy0, ix1, iy1 = ink(idx, t)
                    if fixed and fixed.get('em'):
                        sxl = fixed.get('sx', 1.0)
                    else:
                        sxl = ((x1 - x0) - 2 * grow) / (max(1, ix1 - ix0) * em / EM)
                    centers.append(((x0 + x1) / 2, (y0 + y1 - ext) / 2, sxl))
                if not all(sx_range[0] <= c[2] <= sx_range[1] for c in centers):
                    continue
                r = evaluate(idx, g4, cfgs, em, centers, texts, orig, bg, fixed, ext4)
                if r:
                    found.append(r)
        return found

    if fixed:
        # (fixed['rings'] as a fit gives them, outside-in; the search takes the widths inside-out:
        # read as they were, a group's members came out with their outlines' widths swapped)
        found = search([(fixed['font'], int(round(fixed['bold'] * K)),
                         tuple(int(round(r * K)) for r, _ in reversed(fixed['rings'])), e) for e in exts])
    else:
        # Up to two outlines first; a third only on the looks that came out best.
        found = search([(idx, b, ws, e) for idx in fonts for b in (0, 1, 2) for total in range(0, 17)
                        for ws in ring_splits(total) if len(ws) <= 2 for e in exts])
        seeds = []
        for r in sorted(found, key=lambda r: r['err']):
            k = (r['idx'], r['cfg'][0], r['ext'])
            if k not in seeds:
                seeds.append(k)
            if len(seeds) == 3:
                break
        found += search([(idx, b, ws, e) for idx, b, e in seeds for total in range(0, 17)
                         for ws in ring_splits(total) if len(ws) == 3])
    if not found:
        return None
    best = min(found, key=lambda r: r['err'])
    if polish and not (fixed and fixed.get('em')):
        # The few best looks, each with its size and places refined; then every outline split of
        # the winner's thickness at the refined place.
        top = sorted(found, key=lambda r: r['err'])[:3 if len(texts) == 1 else 2]
        best = min((refine(r, texts, orig, bg, fixed, sx_range) for r in top), key=lambda r: r['err'])
        ext4 = int(round(best['ext'] * K))
        same = by_place[(best['idx'], best['g4'], ext4)]
        if len(same) > 1:
            r = evaluate(best['idx'], best['g4'], same, best['em'], best['centers'], texts, orig, bg, fixed, ext4)
            if r and r['err'] < best['err']:
                best = r
    if not best:
        return None
    # A soft glow around the outlines: what the hard layers leave out, semi-transparent.
    g4, d = best['g4'], best['ring1'].d
    outer = best['ring_covs'][0] if best['ring_covs'] else best['fill_cov']
    a = orig.getchannel('A')
    hm = ImageChops.multiply(outer.point(lambda v: 255 if v <= 20 else 0), a.point(lambda v: 255 if 12 <= v <= 230 else 0))
    n_halo = ImageStat.Stat(hm).sum[0] / 255
    # A group's look fixes the glow too (fixed['glow']: None, or (width, color or None for this
    # sprite's own)): every member gets it, or none does, whatever it does to this one's error.
    forced = fixed is not None and 'glow' in fixed
    if forced or n_halo >= max(6, 0.05 * w * h):
        given = bool(fixed and fixed.get('colors'))
        if n_halo:
            hc = tuple(int(v) for v in ImageStat.Stat(orig, hm).median)
            hc = hc[:3] + (int(ImageStat.Stat(a, hm).extrema[0][1]),)
        else:
            hc = tuple(best['rings'][0][1]) if best['rings'] else tuple(best['ft'])
        widths = (4, 6, 8, 12)  # 4x pixels
        if forced:
            widths = (int(round(fixed['glow'][0] * K)),) if fixed['glow'] else ()
            if fixed['glow'] and fixed['glow'][1] is not None:
                hc, given = tuple(fixed['glow'][1]), True
        for gw in widths:
            glow = down(d.point(lut_glow(g4, gw)), (w, h))
            layers = [glow] + list(best['ring_covs']) + [best['fill_cov']]
            gcol, rcols, ft, fb = hc, [c for _, c in best['rings']], best['ft'], best['fb']
            if not given:
                y0, y1 = best['frows']
                grad = ft != fb
                trow = [min(1.0, max(0.0, (y - y0) / max(1, y1 - y0))) for y in range(h)] if grad else None
                cols = lsq(layers, orig, bg, trow, known=[ft, fb] if grad else [ft])
                if not cols:
                    continue
                gcol = cols[0]
                rcols = cols[1:len(layers) - 1]
            model = compose((w, h), bg, [(glow, gcol)] + list(zip(best['ring_covs'], rcols)),
                            best['fill_cov'], ft, fb, best['frows'])
            e = (error(model, orig) + 1.0) * (BOLD_BIAS if best['font'] == 0 else 1)
            if e < best['err'] or forced:
                best['err'] = e
                best['glow'] = (gw / K, gcol)
                best['rings'] = [(wd, c) for (wd, _), c in zip(best['rings'], rcols)]
                best['ft'], best['fb'] = ft, fb
    # How much of the original's opaque area the text leaves out (art that isn't letters: a
    # button, an icon; such a sprite isn't text on a transparent background).
    if bg is None:
        layers = list(zip(best['ring_covs'], [c for _, c in best['rings']]))
        if best.get('glow'):
            layers = [(down(d.point(lut_glow(g4, int(round(best['glow'][0] * K)))), (w, h)), best['glow'][1])] + layers
        ma = compose((w, h), None, layers, best['fill_cov'], best['ft'], best['fb'], best['frows']).getchannel('A')
        opaque = orig.getchannel('A').point(lambda v: 255 if v >= 200 else 0)
        miss = ImageChops.multiply(opaque, ma.point(lambda v: 255 if v < 64 else 0))
        best['miss'] = miss.histogram()[255] / max(1, opaque.histogram()[255])
    for k in ('ring_covs', 'fill_cov', 'ring1', 'idx', 'cfg', 'centers'):
        best.pop(k, None)
    return best
