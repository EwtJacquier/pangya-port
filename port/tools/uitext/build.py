#!/usr/bin/env python3
"""Text sprites of the game's UI atlases -> generated/ui_text.spec (drawn as vector text at run time).

    python3 -I tools/uitext/build.py "../Pangya - Fantasy Golf (USA).iso" [--only atlas,...] [--out FILE]

Every .gim in pspdata.pak with a sprite list (.txt) is decoded. Text comes from glyph sets (four
or more single-character sprites: font2_7, profile_A...) and from the hand transcription in
labels.tsv. For each sprite fit.py renders candidate styles (font, weight, outlines, glow,
gradient) and keeps the one closest to the original pixels; text over a button or panel gets a
mask of the pixels to rebuild before drawing. Sprites drawn alike (a menu's items, a glyph set)
share one style and their colors. runtime/text.c recognizes the atlas by the CRC of its pixels
and draws those sprites again at the render scale.
"""
import argparse
import os
import re
import statistics
import struct
import sys
import zlib
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fit as F  # noqa: E402
import gim  # noqa: E402

PORT = Path(__file__).resolve().parents[2]


# ---- ISO / pak --------------------------------------------------------------------------------
def iso_file(iso, path):
    with open(iso, 'rb') as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        lba, size = struct.unpack_from('<I', pvd, 156 + 2)[0], struct.unpack_from('<I', pvd, 156 + 10)[0]
        for part in path.split('/'):
            f.seek(lba * 2048)
            data = f.read(size)
            p, found = 0, None
            while p < len(data):
                n = data[p]
                if n == 0:
                    p = (p // 2048 + 1) * 2048
                    continue
                name = data[p + 33:p + 33 + data[p + 32]].decode('latin1').split(';')[0]
                if name.upper() == part.upper():
                    found = struct.unpack_from('<I', data, p + 2)[0], struct.unpack_from('<I', data, p + 10)[0]
                    break
                p += n
            if not found:
                raise SystemExit(f'{path}: not in the ISO')
            lba, size = found
    return lba * 2048, size


def pak_index(iso):
    """psppackfilelist.pak: u16 3, u16 id, u32 offset, u32 size, u32 size, name, zero padding."""
    off, size = iso_file(iso, 'PSP_GAME/USRDIR/psppackfilelist.pak')
    with open(iso, 'rb') as f:
        f.seek(off)
        d = f.read(size)
    out, p = {}, 0
    while p + 16 <= len(d):
        kind, _, foff, fsz, _ = struct.unpack_from('<HHIII', d, p)
        if kind != 3:
            p += 4
            continue
        e = d.index(b'\0', p + 16)
        out.setdefault(d[p + 16:e].decode('latin1').lower(), (foff, fsz))
        p = (e + 4) & ~3
        while p + 2 <= len(d) and d[p:p + 2] != b'\x03\x00':
            p += 4
    return out


# ---- sprites ----------------------------------------------------------------------------------
def opaque_background(img):
    """Text on a panel (e.g. a button): the border of the sprite is mostly opaque."""
    w, h = img.size
    px = img.load()
    border = [px[x, y][3] for x in range(w) for y in (0, h - 1)] + [px[x, y][3] for y in range(h) for x in (0, w - 1)]
    return sum(a >= 100 for a in border) > 0.6 * len(border)


# Words in sprite names that describe the sprite, not its text (style groups ignore them).
NAME_NOISE = {'on', 'off', 'no', 'text', 'txt', 'btn', 'button', 'icon', 'back', 'bg', 'cursor', 'balloon',
              'line', 'bar', 'box', 'frame', 'img', 'image', 'big', 'small', 'mini', 'popup', 'arrow',
              'slot', 'gauge', 'board', 'blank', 'mark', 'point', 'glow', 'light', 'shadow', 'base', 'sel',
              'select', 'cur', 'num', 'number', 'font', 'msg', 'ui', 'brown', 'blue', 'red', 'green', 'gray',
              'charaline', 'scroll', 'tab', 'left', 'right', 'top', 'bottom', 'up', 'down'}


def alpha_silhouette(crop):
    w, h = crop.size
    px = crop.load()
    return [[px[x, y][3] >= 100 for x in range(w)] for y in range(h)]


def median_window(h):
    return 7 if h <= 20 else 9 if h <= 32 else 11 if h <= 48 else 13


def text_mask(crop):
    """Pixels that stand out from the local background (letters and their outlines): the
    background is what a median filter wider than the strokes leaves."""
    from PIL import ImageFilter
    w, h = crop.size
    bg = crop.filter(ImageFilter.MedianFilter(median_window(h))).load()
    px = crop.load()
    m = [[max(abs(px[x, y][i] - bg[x, y][i]) for i in range(4)) > 48 for x in range(w)] for y in range(h)]
    return drop_specks(m, 3)


def drop_specks(m, n):
    """m without its groups of fewer than n connected pixels (in place)."""
    h, w = len(m), len(m[0])
    seen = [[False] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if m[y][x] and not seen[y][x]:
                comp, q = [], [(x, y)]
                seen[y][x] = True
                while q:
                    cx, cy = q.pop()
                    comp.append((cx, cy))
                    for dx in (-1, 0, 1):
                        for dy in (-1, 0, 1):
                            nx, ny = cx + dx, cy + dy
                            if 0 <= nx < w and 0 <= ny < h and m[ny][nx] and not seen[ny][nx]:
                                seen[ny][nx] = True
                                q.append((nx, ny))
                if len(comp) < n:
                    for cx, cy in comp:
                        m[cy][cx] = False
    return m


def flat_mask(crop):
    """Letters on a flat panel (a tag, a badge): the pixels far from the panel's color (the
    median of the box), when a local median can't tell them apart (letters as dense as the
    panel). None when that leaves no panel to speak of."""
    w, h = crop.size
    px = crop.load()
    med = [sorted(px[x, y][i] for y in range(h) for x in range(w))[w * h // 2] for i in range(4)]
    m = drop_specks([[max(abs(px[x, y][i] - med[i]) for i in range(4)) > 64 for x in range(w)] for y in range(h)], 3)
    cov = sum(map(sum, m)) / (w * h)
    return m if 0.1 <= cov <= 0.7 else None


def light_outline_mask(crop):
    """Letters with a white outline over a picture (a course name on its photo): the white
    pixels that form strokes, and all they enclose (the letters' fill). None when nothing
    like that is there."""
    w, h = crop.size
    px = crop.load()
    white = [[px[x, y][3] >= 200 and min(px[x, y][:3]) >= 236 and max(px[x, y][:3]) - min(px[x, y][:3]) <= 18
              for x in range(w)] for y in range(h)]
    white = drop_specks(white, 12)
    if sum(map(sum, white)) < 0.08 * w * h:
        return None
    wall = [[any(0 <= x + dx < w and 0 <= y + dy < h and white[y + dy][x + dx] for dx in (-1, 0, 1) for dy in (-1, 0, 1))
             for x in range(w)] for y in range(h)]  # closes 1-pixel gaps in the outline
    outside = [[False] * w for _ in range(h)]
    q = [(x, y) for y in range(h) for x in range(w) if (x in (0, w - 1) or y in (0, h - 1)) and not wall[y][x]]
    for x, y in q:
        outside[y][x] = True
    while q:
        cx, cy = q.pop()
        for nx, ny in ((cx + 1, cy), (cx - 1, cy), (cx, cy + 1), (cx, cy - 1)):
            if 0 <= nx < w and 0 <= ny < h and not wall[ny][nx] and not outside[ny][nx]:
                outside[ny][nx] = True
                q.append((nx, ny))
    m = [[not outside[y][x] for x in range(w)] for y in range(h)]  # the outline, a pixel around it, and inside
    cov = sum(map(sum, m)) / (w * h)
    return m if 0.15 <= cov <= 0.85 else None


def inner_mask(m):
    """The mask without the parts that touch the sprite's edge (a button's frame), or None when
    nothing changes or nothing is left."""
    h, w = len(m), len(m[0])
    out = [row[:] for row in m]
    q = [(x, y) for y in range(h) for x in range(w) if m[y][x] and (x in (0, w - 1) or y in (0, h - 1))]
    if not q:
        return None
    for x, y in q:
        out[y][x] = False
    while q:
        cx, cy = q.pop()
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                nx, ny = cx + dx, cy + dy
                if 0 <= nx < w and 0 <= ny < h and out[ny][nx]:
                    out[ny][nx] = False
                    q.append((nx, ny))
    return out if sum(map(sum, out)) >= 12 else None


def segments(m, gap):
    """Runs of columns with mask pixels, split where at least `gap` empty columns separate them."""
    w = len(m[0])
    cols = [any(row[x] for row in m) for x in range(w)]
    segs, start, empty = [], None, 0
    for x, c in enumerate(cols + [False] * gap):
        if c:
            if start is None:
                start = x
            empty, last = 0, x
        elif start is not None:
            empty += 1
            if empty >= gap:
                segs.append((start, last + 1))
                start = None
    return segs


def mask_hex(sub, x0, y0, x1, y1):
    """The pixels to rebuild under the new text (the letters grown by one pixel), as hex bits."""
    h, w = len(sub), len(sub[0])
    bits = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            bits.append(any(0 <= x + dx < w and 0 <= y + dy < h and sub[y + dy][x + dx]
                            for dx in (-1, 0, 1) for dy in (-1, 0, 1)))
    out = []
    for i in range(0, len(bits), 4):
        v = 0
        for j, b in enumerate(bits[i:i + 4]):
            v |= b << (3 - j)
        out.append('%x' % v)
    return ''.join(out)


LABELS = Path(__file__).resolve().parent / 'labels.tsv'


def load_labels():
    """{atlas: {rect: (sprite name, text, boxed)}}. "@x,y,w,h text" puts the text in that box of the
    sprite (art around it stays), keyed by the box; one sprite may hold several. "@!x,y,w,h": a
    box with nothing but the text (boxed is 'clear': redrawn whole, no old pixels left)."""
    out = {}
    if LABELS.exists():
        for line in LABELS.read_text(encoding='utf-8').splitlines():
            if not line or line.startswith('#'):
                continue
            atlas, name, rect, text = line.split('\t', 3)
            x, y, w, h = (int(v) for v in rect.split(','))
            sb = re.fullmatch(r'@(!?)(\d+),(\d+),(\d+),(\d+)\s+(.*)', text)
            if sb:
                bx, by, bw, bh = (int(v) for v in sb.groups()[1:5])
                # "@!": nothing but the text in the box (redrawn whole); else art may share it
                out.setdefault(atlas, {})[(x + bx, y + by, bw, bh)] = (name, sb.group(6), 'clear' if sb.group(1) else True)
            else:
                out.setdefault(atlas, {})[(x, y, w, h)] = (name, text, False)
    return out


GLYPH_WORDS = {'percent': '%', 'plus': '+', 'minus': '-', 'slash': '/', 'colon': ':', 'dot': '.', 'comma': ','}


def glyph_of(name):
    """(set, character) for a sprite named like one glyph of a set: font2_7, 7, num_07, middle7,
    15pt_no-, num_percent; else None. (Whether a set is text is decided by fitting it.)"""
    m = re.fullmatch(r'(.+)_(.)', name)
    if m:
        return m.group(1), m.group(2)
    if len(name) == 1 and name.isprintable() and name != ' ':
        return '', name
    m = re.fullmatch(r'(.*[A-Za-z_])0?([0-9])', name)
    if m:
        return m.group(1).rstrip('_'), m.group(2)
    m = re.fullmatch(r'(.*?)_?(' + '|'.join(GLYPH_WORDS) + ')', name)
    if m and m.group(1):
        return m.group(1).rstrip('_'), GLYPH_WORDS[m.group(2)]
    m = re.fullmatch(r'(.*[A-Za-z_])([-+%/:.,])', name)
    if m:
        return m.group(1).rstrip('_'), m.group(2)
    return None


# Atlases whose sprite list has another name (all course titles share one), and sprite lists of
# atlases that come without one (tools/uitext/sprites.tsv: atlas, name, x,y,w,h).
SHARED_LISTS = [(r'(0[2-9]|10)_mapname_.*', 'mapname')]
# Glyph sets drawn in another set's look instead of their own (at their own size): the yards in
# the 3D view ([font_wind, tinted green by the game) like the overhead view's (gui_font font3),
# which read better (the user's choice).
# Also the course intros' Hole / Par digits (0X_mapname_*: their own look fitted as white blobs).
STYLE_LIKE = [(r'\[font_wind', 'fw', 'gui_font', 'font3'), (r'(0[2-9]|10)_mapname_.*', '', 'gui_font', 'font3')]
EXTRA_SPRITES = Path(__file__).with_name('sprites.tsv')
# Glyph sets drawn as outlines only, the letters themselves clear (the scoreboard's shot counts,
# score_text 15pt_no, tinted by the game): (atlas, set, outline colour). Fitted in that look.
HOLLOW = [(r'score_text', '15pt_no', (255, 255, 255, 255))]


def sprite_lists(index):
    """{atlas: list name or None (only extra sprites)}"""
    out = {n[:-4]: n[:-4] for n in index if n.endswith('.txt') and n[:-4] + '.gim' in index}
    for n in index:
        if n.endswith('.gim'):
            for pat, lst in SHARED_LISTS:
                if re.fullmatch(pat, n[:-4]) and lst + '.txt' in index:
                    out[n[:-4]] = lst
    if EXTRA_SPRITES.exists():
        for line in EXTRA_SPRITES.read_text(encoding='utf-8').splitlines():
            if line and not line.startswith('#') and line.split('\t')[0] + '.gim' in index:
                out.setdefault(line.split('\t')[0], None)
    return out


def extra_sprites(atlas):
    out = []
    if EXTRA_SPRITES.exists():
        for line in EXTRA_SPRITES.read_text(encoding='utf-8').splitlines():
            if not line or line.startswith('#'):
                continue
            a, name, rect = line.split('\t')[:3]
            if a == atlas:
                out.append((name, *(int(v) for v in rect.split(','))))
    return out


def style_key(name):
    """Sprites drawn the same way share a name pattern: text_license_off_C -> text_*_off_C,
    score_bogey_C -> score_*_C (the first word stays: title_match_C is another kind)."""
    toks = name.split('_')
    keep = NAME_NOISE - {'image', 'img'}  # words of a look (on, off, big...), not of the content
    return '_'.join(t if ((i == 0 and len(toks) > 1) or len(t) <= 1 or t.lower() in keep) and not t.isdigit() else '*'
                    for i, t in enumerate(toks))


def line_bands(m, n):
    """Row ranges of n text lines in a mask: runs of rows with mask pixels, the closest ones merged
    until n remain; bands split halfway between them. None if there are fewer runs than lines."""
    h = len(m)
    rows = [any(r) for r in m]
    runs, y = [], 0
    while y < h:
        if rows[y]:
            y0 = y
            while y < h and rows[y]:
                y += 1
            runs.append([y0, y])
        else:
            y += 1
    if len(runs) < n:
        # Lines that touch (their outlines meet): cut at the thinnest row near each boundary.
        counts = [sum(r) for r in m]
        cuts = [0]
        for i in range(1, n):
            c = h * i // n
            lo, hi = max(cuts[-1] + 3, c - h // (2 * n)), min(h - 3, c + h // (2 * n))
            if lo >= hi:
                return None
            cuts.append(min(range(lo, hi), key=lambda y: (counts[y], abs(y - c))))
        cuts.append(h)
        return [(cuts[i], cuts[i + 1]) for i in range(n)]
    while len(runs) > n:
        gaps = [runs[i + 1][0] - runs[i][1] for i in range(len(runs) - 1)]
        i = gaps.index(min(gaps))
        runs[i:i + 2] = [[runs[i][0], runs[i + 1][1]]]
    cuts = [0] + [(runs[i][1] + runs[i + 1][0]) // 2 for i in range(n - 1)] + [h]
    return [(cuts[i], cuts[i + 1]) for i in range(n)]


def inpaint_py(crop, m):
    """The background under masked pixels, interpolated from the nearest unmasked ones in the row
    and the column (a stand-in for the runtime's inpainting, used to judge fits)."""
    w, h = crop.size
    px = crop.load()
    out = crop.copy()
    op = out.load()
    for y in range(h):
        for x in range(w):
            if not m[y][x]:
                continue
            acc, ws = [0.0] * 4, 0.0
            for dx, dy in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                xx, yy, d = x, y, 0
                while 0 <= xx < w and 0 <= yy < h and m[yy][xx]:
                    xx += dx
                    yy += dy
                    d += 1
                if 0 <= xx < w and 0 <= yy < h:
                    for i in range(4):
                        acc[i] += px[xx, yy][i] / d
                    ws += 1 / d
            if ws:
                op[x, y] = tuple(int(a / ws + 0.5) for a in acc)
    return out


def grow_mask(m, g):
    h, w = len(m), len(m[0])
    return [[any(0 <= x + dx < w and 0 <= y + dy < h and m[y + dy][x + dx]
                 for dx in range(-g, g + 1) for dy in range(-g, g + 1)) for x in range(w)] for y in range(h)]


def col_ranges(segs, w):
    """Column ranges to try: all the letters, and without up to 3 runs at either end (icons)."""
    n = len(segs)
    out = [(0, w)]
    if n > 1:
        for i in range(min(3, n)):
            for j in range(max(i, n - 4), n):
                out.append((max(0, segs[i][0] - 1), min(w, segs[j][1] + 1)))
    return list(dict.fromkeys(out))


def trimmed_box(mask_img):
    """Bounding box of a line's letters, ignoring the edge of a neighbour line's outline that a
    band cut leaves at the top or bottom (rows far sparser than the letters)."""
    w, h = mask_img.size
    px = mask_img.load()
    rows = [sum(1 for x in range(w) if px[x, y]) for y in range(h)]
    if not any(rows):
        return None
    mx = max(rows)
    ys = [y for y in range(h) if rows[y] >= 0.12 * mx]
    box = mask_img.getbbox()
    return (box[0], ys[0], box[2], ys[-1] + 1)


# A group's look goes to a member unless it fits that one this much worse than its own (ratio):
# members of one menu or message look alike even where one of them alone would fit a bit better.
GROUP_SLACK = float(os.environ.get('UITEXT_GROUP_SLACK', '0.12'))
MAX_MISS = 0.12  # text on a transparent background explains (nearly) all the opaque pixels
UNIFY = 45  # group members whose colors are this close share them (measurement noise, not design)


def solid(img, cut=200):
    """A panel: (nearly) every pixel inside its silhouette's box is opaque (outlined text leaves
    gaps between and around the letters)."""
    a = img.getchannel('A')
    bb = a.point(lambda v: 255 if v >= 100 else 0).getbbox()
    if not bb:
        return False
    sub = a.crop(bb)
    return sub.point(lambda v, c=cut: 255 if v >= c else 0).histogram()[255] >= 0.95 * sub.width * sub.height


def ratio(err, empty):
    """A fit's error relative to drawing nothing (no text to speak of: no match)."""
    return err / empty if empty >= 1 else float('inf')


def known_line(text, crop, fixed=None, multiline=False, boxed=False, fonts=(0, 1), sx_max=1.35):
    """Best fit for one line of known text in crop: (kind, fit, (x0, x1), mask) or None. boxed:
    the crop is the part of a sprite where labels.tsv says the text is (art around it)."""
    w, h = crop.size
    if h < 5 or w < 4:
        return None
    # boxed == 'clear' ("@!" in labels.tsv): nothing but text on a transparent background. A long
    # line of thin letters strays from the original's by a pixel here and there (its advances
    # aren't the font's): more of the original's pixels may go unexplained; no panel to rebuild.
    plain = boxed == 'clear'
    cands = []
    # Text on a transparent background (an icon beside it is left out by trying column ranges;
    # a close match over the whole sprite settles it). Also on a sprite whose border is opaque:
    # an outline can fill a small one to its edges.
    settled = False
    am = alpha_silhouette(crop)
    for X0, X1 in (col_ranges(segments(am, 2), w) if not opaque_background(crop) else [(0, w)]):
        sub = crop.crop((X0, 0, X1, h))
        am_img = sub.getchannel('A').point(lambda v: 255 if v >= 100 else 0)
        box = trimmed_box(am_img) if multiline else am_img.getbbox()
        if not box or (box[3] - box[1] < 5 and not (fixed and fixed.get('em'))) or solid(sub, 128 if boxed else 200):
            continue
        r = F.fit(text, sub, box, fixed=fixed, fonts=fonts, sx_range=(0.62, sx_max))
        # (a look imposed on purpose, STYLE_LIKE, may leave much of the original's ink unexplained)
        if r and (r.get('miss', 0) <= (0.3 if plain else MAX_MISS) or (fixed and fixed.get('force'))):
            r['ratio'] = ratio(r['err'], F.empty_error(sub))
            cands.append(('S', r, (X0, X1), None))
            if (X0, X1) == (0, w) and r['ratio'] <= 0.35:
                settled = True
                break
    # Text over a background (button, panel): letters are what stands out from it.
    if plain:
        settled = True
    m = text_mask(crop) if not settled else [[False] * w for _ in range(h)]
    masks = [m] + ([mi] if not settled and (mi := inner_mask(m)) else [])
    if boxed and not settled:  # text inside art: also the masks for a flat panel and for a photo
        masks += [mm for mm in (flat_mask(crop), light_outline_mask(crop)) if mm]
    ranges = [(mm, X0, X1) for mm in masks for X0, X1 in col_ranges(segments(mm, 2), w)[:8]]
    for m, X0, X1 in ranges:
        sub = [[m[y][x] if X0 <= x < X1 else False for x in range(w)] for y in range(h)]
        ys = [y for y in range(h) if any(sub[y])]
        if len(ys) < 5:
            continue
        xs = [x for x in range(w) if any(sub[y][x] for y in range(h))]
        box = (xs[0], ys[0], xs[-1] + 1, ys[-1] + 1)
        grown = grow_mask(sub, 2)
        if sum(map(sum, grown)) > 0.97 * w * h:  # no background left to rebuild it from
            continue
        bg = inpaint_py(crop, grown)
        # the letters' box, and the same without sparse rows (bits of a frame above or below)
        counts = [sum(r) for r in sub]
        dense = [y for y in range(h) if counts[y] >= 0.12 * max(counts)]
        boxes = [box]
        if dense and (dense[0], dense[-1] + 1) != (box[1], box[3]):
            dx = [x for x in range(w) if any(sub[y][x] for y in range(dense[0], dense[-1] + 1))]
            boxes.append((dx[0], dense[0], dx[-1] + 1, dense[-1] + 1))
        for bx in boxes:
            r = F.fit(text, crop, bx, bg=bg, fixed=fixed, fonts=fonts, sx_range=(0.62, sx_max))
            if r:
                r['ratio'] = ratio(r['err'], F.empty_error(crop, bg))
                cands.append(('P', r, (0, w), grown))
    # a band of touching lines also holds a strip of the neighbour's outline: allow for it
    limit = (0.72 if multiline else 0.55) + (0.15 if boxed else 0)  # a marked box holds text
    if fixed:  # the group's style, on a sprite whose text is known: a looser match will do
        limit = 10.0 if fixed.get('force') else limit + 0.15
    good = [c for c in cands if c[1]['ratio'] <= limit]
    if not good:
        return None
    # a column range that leaves letters out must be clearly better than all of them (an icon)
    return min(good, key=lambda c: c[1]['ratio'] + (0.04 if c[0] == 'P' else 0) + (0.08 if c[2] != (0, w) else 0))


# Words with a glossy look the fitter blurs into one flat colour (the ground under the ball:
# Fairway, Rough, Green...; the hole results' letters: BIRDIE, PAR, BOGEY...): a fill gradient, a
# white outline, a dark one outside it. Their colours are measured on the sprites themselves, over
# all the sprites of a group at once (the letters of one word look the same); the fitted size,
# place and outline widths stay.
GLOSSY = r'\[ground|result_font_0[12]'
SOLO = r'result_font_01'  # atlases whose labels each have colours of their own (not one look per menu)


def glossy_colors(crops):
    """(fill top, fill bottom, outline) measured on crops, or None."""
    edge, fill = [], []
    for crop in crops:
        px = crop.convert('RGBA').load()
        w, h = crop.size
        clear = [[px[x, y][3] < 128 for x in range(w)] for y in range(h)]

        def near_clear(x, y, d):
            return any(clear[j][i] for j in range(max(0, y - d), min(h, y + d + 1))
                       for i in range(max(0, x - d), min(w, x + d + 1)))
        for y in range(h):
            for x in range(w):
                c = px[x, y]
                if c[3] < 250:
                    continue
                if near_clear(x, y, 1):
                    edge.append(c)
                elif not near_clear(x, y, 4) and min(c[:3]) < 200 and max(c[:3]) - min(c[:3]) > 40:
                    fill.append((y, c))
    if len(edge) < 20 or len(fill) < 20:
        return None
    ys = sorted(y for y, _ in fill)
    lo, hi = ys[len(ys) // 10], ys[len(ys) * 9 // 10]
    third = max(1, (hi - lo) // 3)
    top = [c for y, c in fill if y <= lo + third] or [c for _, c in fill]
    bot = [c for y, c in fill if y >= hi - third] or [c for _, c in fill]
    return F.med(top), F.med(bot), F.med(edge)


def glossy_look(r, colors):
    ft, fb, dark = colors
    rings = list(r['rings'])  # outside-in
    rings = [(wd, dark) for wd, _ in rings[:-1]] + [(max(rings[-1][0], 1.25), (255, 255, 255, 255))] if rings else rings
    r = dict(r, ft=ft, fb=fb, rings=rings)
    if r.get('glow'):
        r['glow'] = (r['glow'][0], dark[:3] + (r['glow'][1][3],))
    return r


def spec_fields(r):
    """ox oy em sx bold font ft fb n [w color]... [e:ext] (outlines inside-out, a glow last as -w;
    ext: how far the outlines reach deeper below the letters)."""
    rings = list(reversed(r['rings']))  # inside-out
    if r.get('glow'):
        rings.append((-r['glow'][0], r['glow'][1]))
    ring_desc = ' '.join(f'{wd:.2f} {rgba(c)}' for wd, c in rings)
    ext = f" e:{r['ext']:.2f}" if r.get('ext') else ''
    return (f"{r['ox']:.3f} {r['oy']:.3f} {r['em']:.3f} {r['sx']:.3f} {r['bold']:.3f} {r['font']} "
            f"{rgba(r['ft'])} {rgba(r['fb'])} {len(rings)} {ring_desc}{ext}").replace('  ', ' ')


def known_label(rect, crop, text, fixed=None, boxed=False, fonts=(0, 1), sx_max=1.35):
    """[entry] for a sprite with known text ("|" separates lines, fitted together)."""
    x, y, w, h = rect
    lines = [t.strip() for t in text.split('|')]
    if len(lines) == 1:
        r = known_line(lines[0], crop, fixed, boxed=boxed, fonts=fonts, sx_max=sx_max)
        if not r:
            return []
        kind, fit, (c0, c1), grown = r
        return [entry(kind, (x + c0, y, c1 - c0, h), fit, grown, lines[0], w)]
    # Several lines: one fit over the whole sprite, each line in the box its band gives it.
    cands = []
    for kind in ('S', 'P', 'Pi'):
        if kind == 'S' and solid(crop, 128 if boxed else 200):
            continue
        m = alpha_silhouette(crop) if kind == 'S' else text_mask(crop)
        if kind == 'Pi':  # without a button's frame
            m = inner_mask(m)
            if not m:
                continue
        bands = line_bands(m, len(lines))
        if not bands:
            continue
        mimg = Image.new('L', (w, h))
        mimg.putdata([255 if v else 0 for row in m for v in row])
        boxes = []
        for b0, b1 in bands:
            bb = trimmed_box(mimg.crop((0, b0, w, b1)))
            if not bb:
                break
            boxes.append((bb[0], bb[1] + b0, bb[2], bb[3] + b0))
        if len(boxes) != len(lines):
            continue
        bg, grown = None, None
        if kind != 'S':
            grown = grow_mask(m, 2)
            if sum(map(sum, grown)) > 0.97 * w * h:  # no background left to rebuild it from
                continue
            bg = inpaint_py(crop, grown)
        r = F.fit(lines, crop, boxes, bg=bg, fixed=fixed, fonts=fonts)
        if r and (kind != 'S' or r.get('miss', 0) <= MAX_MISS):
            r['ratio'] = ratio(r['err'], F.empty_error(crop, bg))
            cands.append((kind[0], r, grown))
    good = [c for c in cands if c[1]['ratio'] <= (0.75 if fixed else 0.6) + (0.15 if boxed else 0)]
    if not good:
        return []
    # a rebuilt panel must be clearly better than text alone (as for one line): over a transparent
    # background it only keeps the old pixels outside its mask
    kind, fit, grown = min(good, key=lambda c: c[1]['ratio'] + (0.04 if c[0] != 'S' else 0))
    return [entry(kind, rect, fit, grown, '|'.join(lines), w)]


def entry(kind, rect, fit, grown, text, w):
    mstr = None
    if kind == 'P':
        ys = [yy for yy in range(len(grown)) if any(grown[yy])]
        xs = [xx for xx in range(w) if any(grown[yy][xx] for yy in range(len(grown)))]
        mx0, mx1, my0, my1 = xs[0], xs[-1] + 1, ys[0], ys[-1] + 1
        mstr = f'm:{mx0},{my0},{mx1 - mx0},{my1 - my0},{mask_hex(grown, mx0, my0, mx1, my1)}'
    return dict(kind=kind, rect=rect, fit=fit, mask=mstr, text=text, ratio=fit['ratio'])


# Results of earlier builds, by what they depend on (the sprite, its text, the style imposed on
# it, and the fitting code): a build after a change refits only what the change touches.
CACHE = PORT / 'build' / 'uitext_cache'
_code_key = None


def _cached(kind, args, work):
    global _code_key
    import hashlib
    import inspect
    import pickle
    if _code_key is None:
        h = hashlib.sha1()
        for f in ('fit.py', 'edt.c', 'gim.py'):
            h.update((Path(__file__).with_name(f)).read_bytes())
        for fn in (known_line, known_label, line_bands, trimmed_box, text_mask, inner_mask, inpaint_py, grow_mask,
                   drop_specks, flat_mask, light_outline_mask,
                   col_ranges, segments, alpha_silhouette, opaque_background, median_window, entry, mask_hex, solid,
                   _label, _glyph):
            h.update(inspect.getsource(fn).encode())
        for f in F.FONT_FILES:
            h.update(Path(f).read_bytes())
        _code_key = h.digest()
    h = hashlib.sha1(_code_key)
    h.update(kind.encode())
    h.update(repr([a for i, a in enumerate(args) if i != 3]).encode())
    h.update(args[3])
    path = CACHE / (h.hexdigest() + '.pkl')
    try:
        return pickle.loads(path.read_bytes())
    except Exception:
        pass
    out = work(args)
    try:
        CACHE.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(f'.{os.getpid()}.tmp')
        tmp.write_bytes(pickle.dumps(out))
        os.replace(tmp, path)
    except OSError:
        pass
    return out


# Atlases of running text (explanations, tips, missions): the game's thin letters, which the bold
# font can fit by numbers but not by looks.
REGULAR_ONLY = re.compile(r'tutorial_text_chapter_|tutorial_tool_tip_|mission_text')


def _label(args):
    atlas, rect, size, data, text, fixed = args[:6]
    boxed = len(args) > 6 and args[6]
    crop = Image.frombytes('RGBA', size, data)
    try:
        # a lone big letter (the hole results' and missions' words, spelled one sprite a letter)
        # may be much wider than the font's
        return atlas, rect, known_label(rect, crop, text, fixed, boxed, (1,) if REGULAR_ONLY.match(atlas) else (0, 1),
                                        1.7 if len(text) == 1 else 1.35)
    except Exception:  # one odd sprite must not stop the build
        return atlas, rect, []


def _job(args):
    return _cached('label', args, _label)


def rgba(c):
    return '%02x%02x%02x%02x' % tuple(c)


def _glyph(args):
    """A glyph of a set: its character, or (letters whose two cases differ only in size) the
    case that fits better."""
    atlas, rect, size, data, text, fixed = args[:6]
    ambiguous = args[6] if len(args) > 6 else False
    crop = Image.frombytes('RGBA', size, data)
    best = None
    cands = [text] + ([text.swapcase()] if ambiguous and not fixed else [])
    for t in cands:
        try:
            r = known_label(rect, crop, t, fixed)
        except Exception:
            r = []
        if r and (not best or r[0]['ratio'] < best[0]['ratio']):
            best = r
    return atlas, rect, best or []


def _glyph_job(args):
    return _cached('glyph', args, _glyph)


# ---- main -------------------------------------------------------------------------------------
def config_of(fit):
    return (fit['font'], round(fit['bold'], 2), tuple(round(w, 2) for w, _ in fit['rings']),
            round(fit['glow'][0], 2) if fit.get('glow') else None, round(fit.get('ext', 0), 2))


def main():
    import multiprocessing
    from collections import Counter
    ap = argparse.ArgumentParser()
    ap.add_argument('iso')
    ap.add_argument('--out', default=str(PORT / 'generated' / 'ui_text.spec'))
    ap.add_argument('--only', help='comma-separated atlases (testing; writes nothing unless --out is given)')
    ap.add_argument('--jobs', type=int, default=max(1, multiprocessing.cpu_count() - 1))
    args = ap.parse_args()
    pak_off, _ = iso_file(args.iso, 'PSP_GAME/USRDIR/pspdata.pak')
    index = pak_index(args.iso)
    iso = open(args.iso, 'rb')

    def read(name):
        off, size = index[name]
        iso.seek(pak_off + off)
        return iso.read(size)

    labels = load_labels()
    only = set(args.only.split(',')) if args.only else None
    atlases, jobs, glyph_jobs, group_of, boxed = {}, [], [], {}, set()
    sprite_rects = {}  # every sprite of each atlas (the runtime upscales each within its own)
    for atlas, lst in sorted(sprite_lists(index).items()):
        if only and atlas not in only:
            continue
        img = gim.decode(read(atlas + '.gim'))
        if img is None:
            continue
        atlases[atlas] = img
        sp = (gim.sprites(read(lst + '.txt')) if lst else []) + extra_sprites(atlas)
        sprite_rects[atlas] = sorted({(x, y, w, h) for _, x, y, w, h in sp})
        labeled = {r for r in labels.get(atlas, {})}  # a transcription wins over a name's guess
        # Glyph sets: six or more single-character sprites of one set (font2_0, num_07, middle-...).
        glyph = {name: glyph_of(name) for name, *_ in sp}
        sets = Counter(g[0] for g in glyph.values() if g)
        chars, heights = {}, {}
        for name, x, y, w, h in sp:
            g = glyph[name]
            if g:
                chars.setdefault(g[0], set()).add(g[1])
                heights.setdefault(g[0], []).append(h)
        seen = set()
        for name, x, y, w, h in sp:
            g = glyph[name]
            rect = (x, y, w, h)
            # a set has 6+ glyphs (BtnArrow_1..4 are arrows, not digits)
            if not g or sets[g[0]] < 6 or rect in seen or rect in labeled or x + w > img.width or y + h > img.height or h < 5:
                continue
            if h > 1.6 * statistics.median(heights[g[0]]):  # GaugeText_A: a block of words, not a glyph
                continue
            if not g[1].isprintable() or g[1] == ' ':
                continue
            seen.add(rect)
            crop = img.crop((x, y, x + w, y + h))
            c = g[1]
            # font3_m draws "M": a letter may be in the other case when the set lacks that case (not
            # in a set of capitals: ob_O of OB / GIVE UP is a capital O)
            ambiguous = (c.lower() in 'cmosuvwxz' and c.swapcase() not in chars[g[0]]
                         and not any(o.isupper() for o in chars[g[0]] if o != c))
            glyph_jobs.append((atlas, rect, crop.size, crop.tobytes(), c, None, ambiguous))
            group_of[(atlas, rect)] = 'set:' + g[0]
        for rect, (name, text, sb) in labels.get(atlas, {}).items():
            x, y, w, h = rect
            if sb is True:  # the text sits in that part of a sprite with art around it
                boxed.add((atlas, rect))
            if rect in seen or x + w > img.width or y + h > img.height:
                continue
            seen.add(rect)
            crop = img.crop((x, y, x + w, y + h))
            jobs.append((atlas, rect, crop.size, crop.tobytes(), text, None) + ((sb,) if sb else ()))
            group_of[(atlas, rect)] = 'lab:' + (name if re.fullmatch(SOLO, atlas) else style_key(name)) + ('|' if '|' in text else '')
    print(f'{len(atlases)} atlases, {len(glyph_jobs)} glyphs, {len(jobs)} labels; fitting with {args.jobs} processes',
          flush=True)
    results = {}
    with multiprocessing.Pool(args.jobs) as pool:
        for atlas, rect, out in pool.imap_unordered(_glyph_job, glyph_jobs, chunksize=4):
            results[(atlas, rect)] = out
        for atlas, rect, out in pool.imap_unordered(_job, jobs, chunksize=2):
            results[(atlas, rect)] = out
        # Glyph sets: a set is text if most of its glyphs fit their characters (else: icons).
        alljobs = {(j[0], j[1]): j for j in glyph_jobs + jobs}
        members, fitted = {}, {}
        for key in results:
            members.setdefault((key[0], group_of[key]), []).append(key)
        for gk, keys in members.items():
            if not gk[1].startswith('set:'):
                continue
            ok = [results[k][0]['ratio'] for k in keys if len(results[k]) == 1]
            # A font draws every glyph in the same colors; icons named like glyphs (gift_1..9:
            # clothes, icon_1..9: portraits) each have their own. How much of each glyph's colors
            # (4 levels a channel) the set's average palette holds:
            hists = []
            for k in keys:
                jb = alljobs[k]
                h = Counter((p[0] >> 6, p[1] >> 6, p[2] >> 6) for p in Image.frombytes('RGBA', jb[2], jb[3]).getdata()
                            if p[3] >= 100)
                n = sum(h.values())
                if n:
                    hists.append({c: v / n for c, v in h.items()})
            avg = Counter()
            for h in hists:
                for c, v in h.items():
                    avg[c] += v / len(hists)
            alike = statistics.median(sum(min(v, avg[c]) for c, v in h.items()) for h in hists) if hists else 1.0
            if os.environ.get('UITEXT_DEBUG'):
                print('set', gk, len(keys), 'fitted', len(ok), 'alike', round(alike, 2), 'median', ok and round(statistics.median(ok), 3))
            if len(ok) < 0.5 * len(keys) or statistics.median(ok) > 0.45 or alike < 0.75:
                for k in keys:
                    results[k] = []
        # Groups drawn alike share one configuration, fitted again on the members that chose
        # another and on the members that found no fit alone.
        by_group = {}
        for key, out in results.items():
            if len(out) == 1:
                by_group.setdefault((key[0], group_of[key]), []).append(key)
        glyph_keys = {(j[0], j[1]) for j in glyph_jobs}
        # Per group, the candidate looks: the most common configuration, or (several equally
        # common, or none repeated) up to three of them, each tried on the whole group.
        plans = []  # (group, its members, [(config, refit jobs)])
        forced = set()  # members of spelled words: they take their word's look
        for gk, keys in sorted(by_group.items()):
            if len(keys) < 2:
                continue
            failed = [k for k in members.get(gk, []) if not results[k]]
            by_cfg = {}
            for k in sorted(keys):
                by_cfg.setdefault(config_of(results[k][0]['fit']), []).append(results[k][0]['fit'])
            most = max(len(v) for v in by_cfg.values())
            tops = sorted((c for c in by_cfg if len(by_cfg[c]) == most), key=lambda c: min(f['ratio'] for f in by_cfg[c]))[:3]
            cands = []
            for cfg in tops:
                ref = min(by_cfg[cfg], key=lambda f: f['ratio'])
                fixed = dict(font=ref['font'], bold=ref['bold'], rings=ref['rings'], ext=ref.get('ext', 0.0),
                             glow=(ref['glow'][0], None) if ref.get('glow') else None)
                todo = []
                # a word spelled one sprite a letter (BOGEY, SUCCESS) is a set too: one look for all
                spelled = all(len(results[k][0]['text']) == 1 for k in keys)
                if gk[1].startswith('set:') or spelled:
                    if spelled:
                        forced.update(keys + failed)
                    # A glyph set is one look: the same outlines and colors on every glyph, all of
                    # them drawn (their characters are known); only the placement is per glyph.
                    same = by_cfg[cfg]
                    rc = [F.med([f['rings'][i][1] for f in same]) for i in range(len(ref['rings']))]
                    fixed['colors'] = (rc, (F.med([f['ft'] for f in same]), F.med([f['fb'] for f in same])))
                    if fixed['glow']:
                        fixed['glow'] = (fixed['glow'][0], F.med([f['glow'][1] for f in same]))
                    fixed['force'] = True
                    # the set's size: what its capitals and digits ask for; their usual stretch
                    caps = [f for k in keys for f in [results[k][0]['fit']] if re.fullmatch(r'[A-Z0-9]', results[k][0]['text'])]
                    base = caps or [results[k][0]['fit'] for k in keys]
                    fixed['em'] = statistics.median(f['em'] for f in base)
                    fixed['sx'] = min(1.25, max(0.8, statistics.median(f['sx'] for f in base)))
                    for k in sorted(members.get(gk, [])):
                        jb = alljobs[k]
                        text = results[k][0]['text'] if results[k] else jb[4]
                        todo.append((jb[0], jb[1], jb[2], jb[3], text, fixed))
                else:
                    for k in sorted(keys + failed):
                        if not results[k] or config_of(results[k][0]['fit']) != cfg:
                            jb = alljobs[k]
                            todo.append((jb[0], jb[1], jb[2], jb[3], jb[4], fixed) + tuple(jb[6:7]))
                cands.append((cfg, todo))
            plans.append((gk, keys + failed, cands))
        refit = [jb for _, _, cands in plans for _, todo in cands for jb in todo]
        print(f'refitting {len(refit)} sprites to their group style', flush=True)
        done = {}
        refit_g = [jb for jb in refit if (jb[0], jb[1]) in glyph_keys]
        refit_l = [jb for jb in refit if (jb[0], jb[1]) not in glyph_keys]
        for jb, (atlas, rect, out) in list(zip(refit_g, pool.imap(_glyph_job, refit_g, chunksize=4))) + \
                list(zip(refit_l, pool.imap(_job, refit_l, chunksize=2))):
            done[(atlas, rect, id(jb[5]))] = out
        for gk, keys, cands in plans:
            def outcome(cfg, todo):
                """Each member's result under this look (its own when it already has it)."""
                got = {(jb[0], jb[1]): done[(jb[0], jb[1], id(jb[5]))] for jb in todo}
                return {k: got.get(k, results[k]) for k in keys}
            # the look that fits the group best (least error over its members), when several could be it
            scored = []
            for cfg, todo in cands:
                o = outcome(cfg, todo)
                scored.append((statistics.mean(v[0]['ratio'] if v else 2.0 for v in o.values()), cfg, o))
            if os.environ.get('UITEXT_DEBUG') and len(scored) > 1:
                print('tie', gk, [(c, round(sc, 3), [round(v[0]['ratio'], 3) if v else None for _, v in sorted(o.items())])
                                  for sc, c, o in scored], 'chose', min(scored, key=lambda t: t[0])[1])
            _, cfg, o = min(scored, key=lambda t: t[0])
            for k, out in o.items():
                # the group's look, unless it fits this member clearly worse than its own (text on a
                # transparent background and over a rebuilt panel: their ratios have different bases,
                # their errors from the original don't)
                own = results.get(k)
                if own and out and out[0]['kind'] != own[0]['kind']:
                    closer = out[0]['fit']['err'] <= own[0]['fit']['err'] * 1.08
                else:
                    closer = bool(out) and (not own or out[0]['ratio'] <= own[0]['ratio'] + GROUP_SLACK)
                if out and (k in glyph_keys or k in forced or closer):
                    results[k] = out
        # Sets in another set's look (STYLE_LIKE): that set's final look, every glyph refitted.
        borrowed = [(ta, tset, sa, sset) for tpat, tset, sa, sset in STYLE_LIKE
                    for ta in sorted({a for a, g in members if re.fullmatch(tpat, a) and g == 'set:' + tset})]
        for ta, tset, sa, sset in borrowed:
            src = [results[k][0]['fit'] for k in sorted(members.get((sa, 'set:' + sset), [])) if len(results[k]) == 1]
            tgt = sorted(members.get((ta, 'set:' + tset), []))
            own = [results[k][0] for k in tgt if len(results[k]) == 1]
            if not src or not own:
                continue
            ref = min(src, key=lambda f: f['ratio'])
            base = [e['fit'] for e in own if re.fullmatch(r'[A-Z0-9]', e['text'])] or [e['fit'] for e in own]
            fixed = dict(font=ref['font'], bold=ref['bold'], rings=ref['rings'], ext=ref.get('ext', 0.0),
                         glow=ref.get('glow'), colors=([c for _, c in ref['rings']], (ref['ft'], ref['fb'])),
                         force=True, em=statistics.median(f['em'] for f in base),
                         sx=min(1.25, max(0.8, statistics.median(f['sx'] for f in base))))
            todo = [(alljobs[k][0], alljobs[k][1], alljobs[k][2], alljobs[k][3],
                     results[k][0]['text'] if results[k] else alljobs[k][4], fixed) for k in tgt]
            for atlas, rect, out in pool.imap(_glyph_job, todo, chunksize=4):
                if out:
                    results[(atlas, rect)] = out
        # Hollow sets (HOLLOW): the outline's width and the letters' weight that fit the set best,
        # then every glyph at the set's size.
        for hpat, hset, hcol in HOLLOW:
            for ta in sorted({a for a, g in members if re.fullmatch(hpat, a) and g == 'set:' + hset}):
                tgt = sorted(members.get((ta, 'set:' + hset), []))
                clear = hcol[:3] + (0,)
                looks = [dict(font=fo, bold=b, rings=[(wd, hcol)], ext=0.0, glow=None, colors=([hcol], (clear, clear)),
                              force=True) for fo in (0, 1) for b in (0.0, 0.5, 1.0) for wd in (1.0, 1.5, 2.0)]
                todo = [(alljobs[k][0], alljobs[k][1], alljobs[k][2], alljobs[k][3], alljobs[k][4], fx)
                        for fx in looks for k in tgt]
                got = {}
                for jb, (atlas, rect, out) in zip(todo, pool.imap(_glyph_job, todo, chunksize=4)):
                    got.setdefault(id(jb[5]), {})[(atlas, rect)] = out
                scored = [(statistics.mean(o[0]['fit']['err'] if o else 1e9 for o in got[id(fx)].values()), i)
                          for i, fx in enumerate(looks) if id(fx) in got]
                if not scored:
                    continue
                fx = dict(looks[min(scored)[1]])
                outs = [o[0]['fit'] for o in got[id(looks[min(scored)[1]])].values() if o]
                if not outs:
                    continue
                fx['em'] = statistics.median(f['em'] for f in outs)
                fx['sx'] = min(1.25, max(0.8, statistics.median(f['sx'] for f in outs)))
                todo = [(alljobs[k][0], alljobs[k][1], alljobs[k][2], alljobs[k][3], alljobs[k][4], fx) for k in tgt]
                for atlas, rect, out in pool.imap(_glyph_job, todo, chunksize=4):
                    if out:
                        results[(atlas, rect)] = out
                print(f'hollow set {ta} {hset}: font {fx["font"]}, weight {fx["bold"]}, outline {fx["rings"][0][0]}', flush=True)
    # Colors close to their group's are the group's (labels of one menu look identical).
    by_group = {}
    for key, out in results.items():
        if len(out) == 1:
            by_group.setdefault((key[0], group_of[key]), []).append(out[0]['fit'])
    for gk, fits in by_group.items():
        if os.environ.get('UITEXT_DEBUG'):
            print('group', gk, len(fits), sorted(rgba(f['ft']) for f in fits))
        if len(fits) < 3:
            continue
        # Members whose color is near the group's take it (one menu's items look identical); a
        # member drawn in another color (a title among buttons) keeps its own.
        for field in ('ft', 'fb'):
            m = F.med([f[field] for f in fits])
            for f in fits:
                if F.cdist(f[field], m) < UNIFY:
                    f[field] = m
        glows = [f['glow'][1] for f in fits if f.get('glow')]
        if len(glows) >= 3:
            m = F.med(glows)
            for f in fits:
                if f.get('glow') and F.cdist(f['glow'][1], m) < UNIFY:
                    f['glow'] = (f['glow'][0], m)
        cnt = Counter(len(f['rings']) for f in fits)
        nr = max(sorted(cnt), key=lambda n: cnt[n])  # the most common count (the fewest outlines on a tie)
        for i in range(nr):
            m = F.med([f['rings'][i][1] for f in fits if len(f['rings']) == nr])
            for f in fits:
                if len(f['rings']) == nr and F.cdist(f['rings'][i][1], m) < UNIFY:
                    f['rings'][i] = (f['rings'][i][0], m)
    lines, total = [], 0
    glossy = {}  # (atlas, group) -> measured colours (GLOSSY)
    for (a, rect), out in sorted(results.items()):
        if re.fullmatch(GLOSSY, a) and a in atlases:
            for e in out:
                if e['kind'] == 'S':
                    x, y, w, h = e['rect']
                    glossy.setdefault((a, group_of.get((a, rect))), []).append(atlases[a].crop((x, y, x + w, y + h)))
    glossy = {k: glossy_colors(v) for k, v in glossy.items()}
    for atlas, img in atlases.items():
        kept = []
        for (a, rect), out in sorted(results.items()):
            if a != atlas:
                continue
            for e in out:
                x, y, w, h = e['rect']
                if e['kind'] == 'S' and glossy.get((atlas, group_of.get((a, rect)))):
                    e = dict(e, fit=glossy_look(e['fit'], glossy[(atlas, group_of.get((a, rect)))]))
                extra = ''
                tl = e['fit'].get('lines') or []
                if len(tl) > 1:  # where the other lines go: l:ox,oy,sx;ox,oy,sx
                    extra = ' l:' + ';'.join(f'{lx:.3f},{ly:.3f},{ls:.3f}' for lx, ly, ls in tl[1:])
                keep = ' k:1' if e['kind'] == 'S' and (a, rect) in boxed else ''  # art shares the box
                kept.append(f"{e['kind']} {x} {y} {w} {h} {spec_fields(e['fit'])}{keep}" + (f" {e['mask']}" if e['mask'] else '')
                            + extra + f" {e['text']}")
        if kept:
            crc = zlib.crc32(img.tobytes())
            # 64 pixels along the diagonal: the runtime's first test of an uploaded texture
            pxs = img.load()
            smp = zlib.crc32(b''.join(bytes(pxs[i * img.width // 64, i * img.height // 64]) for i in range(64)))
            lines.append(f'A {atlas} {img.width} {img.height} {crc:08x} {len(kept)} {smp:08x}')
            # the atlas's sprites: upscaling one mustn't blend in its neighbours' edges
            lines.append('R ' + ' '.join(f'{x},{y},{w},{h}' for x, y, w, h in sprite_rects.get(atlas, [])))
            lines += kept
            total += len(kept)
    if only and args.out == str(PORT / 'generated' / 'ui_text.spec'):
        args.out = '/dev/null'
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text('# generated by tools/uitext/build.py\n' + '\n'.join(lines) + '\n')
    print(f'{total} text sprites in {sum(1 for l in lines if l[0] == "A")} atlases -> {args.out}')


if __name__ == '__main__':
    main()
