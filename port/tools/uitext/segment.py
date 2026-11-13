#!/usr/bin/env python3
"""Text boxes for sprites whose lines mix text with button icons and colors (the tutorial's
explanations): labels.tsv lines with one @box per run of same-colored text.

    python3 -I tools/uitext/segment.py ISO tools/uitext/segments.tsv  (into labels.tsv)

SOURCE.tsv: atlas, sprite name, x,y,w,h, then the text with "|" between lines, "[]" for an
icon and "{...}" around highlighted (red or yellow) text, e.g.  First, press [] to activate|the Power Gauge.
The sprite's letters are found as connected pixels (icons: black with a white symbol), grouped
into lines and into runs of one color between icons; the runs must match the transcription's,
in reading order, or the sprite is reported and skipped.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as B  # noqa: E402
import gim  # noqa: E402


def components(img):
    w, h = img.size
    px = img.load()
    seen = [[False] * w for _ in range(h)]
    out = []
    for y in range(h):
        for x in range(w):
            if seen[y][x] or px[x, y][3] < 100:
                continue
            q, comp = [(x, y)], []
            seen[y][x] = True
            while q:
                cx, cy = q.pop()
                comp.append((cx, cy))
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        nx, ny = cx + dx, cy + dy
                        if 0 <= nx < w and 0 <= ny < h and not seen[ny][nx] and px[nx, ny][3] >= 100:
                            seen[ny][nx] = True
                            q.append((nx, ny))
            xs, ys = [p[0] for p in comp], [p[1] for p in comp]
            cols = [px[p] for p in comp]
            light = sum(1 for c in cols if min(c[:3]) >= 190)
            dark = sum(1 for c in cols if max(c[:3]) < 60 and c[3] >= 200)
            # the highlight: red (the tutorial) or yellow (the in-game tips)
            hl = sum(1 for c in cols if c[0] - max(c[1], c[2]) >= 60 or (c[0] >= 180 and c[1] >= 150 and c[2] <= 80))
            small = len(comp) <= 12  # a dot or a comma: what color it has at all
            out.append(dict(x0=min(xs), y0=min(ys), x1=max(xs) + 1, y1=max(ys) + 1, n=len(comp),
                            light=light, dark=dark, red=hl > (0 if small else 0.4 * len(comp))))
    # Button icons: what has white among dark letters, or black among white ones.
    lum = sorted(sum(px[x, y][:3]) / 3 for y in range(h) for x in range(w) if px[x, y][3] >= 200)
    white_text = bool(lum) and lum[len(lum) // 2] >= 150
    for c in out:
        c['icon'] = c['dark'] >= 3 and c['light'] >= 3 if white_text else c['light'] >= 3
    return out


def merge_icons(comps):
    """An icon's parts (a d-pad's arrows, a button's letter) are one icon."""
    icons = [c for c in comps if c['icon']]
    # icons that overlap become one box (two that only touch are two: a d-pad on each of two lines)
    boxes = []
    for i in sorted(icons, key=lambda c: c['x0']):
        for b in boxes:
            if i['x0'] < b['x1'] and i['x1'] > b['x0'] and i['y0'] < b['y1'] and i['y1'] > b['y0']:
                b.update(x0=min(b['x0'], i['x0']), y0=min(b['y0'], i['y0']), x1=max(b['x1'], i['x1']), y1=max(b['y1'], i['y1']))
                break
        else:
            boxes.append(dict(i))
    # letters inside an icon's box belong to it (the "L" on a shoulder button)
    letters = [c for c in comps if not c['icon'] and not any(
        c['x0'] >= b['x0'] - 1 and c['x1'] <= b['x1'] + 1 and c['y0'] >= b['y0'] - 1 and c['y1'] <= b['y1'] + 1 for b in boxes)]
    return boxes, letters


def lines_of(letters, n_lines):
    """Letters grouped into n_lines rows (by the big ones' vertical centers; dots and
    punctuation join the row they overlap most)."""
    big = sorted((c for c in letters if c['y1'] - c['y0'] >= 5), key=lambda c: (c['y0'] + c['y1']) / 2)
    if not big:
        return None
    centers = [(c['y0'] + c['y1']) / 2 for c in big]
    gaps = sorted(range(1, len(centers)), key=lambda i: centers[i] - centers[i - 1], reverse=True)[:n_lines - 1]
    cuts = sorted(gaps)
    bounds = [centers[0] - 100] + [(centers[i - 1] + centers[i]) / 2 for i in cuts] + [centers[-1] + 100]
    rows = [[] for _ in range(n_lines)]
    for c in letters:
        cy = (c['y0'] + c['y1']) / 2
        for r in range(n_lines):
            if bounds[r] <= cy < bounds[r + 1]:
                rows[r].append(c)
                break
    return rows


def segments_of(row, icons):
    """Runs of one color between this row's icons, left to right: [(red, x0, y0, x1, y1)]."""
    ry0, ry1 = min(c['y0'] for c in row), max(c['y1'] for c in row)
    # this line's icons: across it, in a gap between its letters (a taller one from the line above
    # passes behind letters)
    mine = [i for i in icons if min(i['y1'], ry1) - max(i['y0'], ry0) >= 0.3 * (ry1 - ry0)
            and not any(c['x0'] < i['x1'] - 1 and c['x1'] > i['x0'] + 1 for c in row)]
    items = sorted([('L', c) for c in row] + [('I', i) for i in mine], key=lambda t: t[1]['x0'])
    segs, cur = [], None
    for kind, c in items:
        if kind == 'I':
            cur = None
            continue
        red = c['red']
        if cur and cur['red'] == red:
            cur.update(x0=min(cur['x0'], c['x0']), y0=min(cur['y0'], c['y0']), x1=max(cur['x1'], c['x1']),
                       y1=max(cur['y1'], c['y1']))
        else:
            cur = dict(red=red, x0=c['x0'], y0=c['y0'], x1=c['x1'], y1=c['y1'])
            segs.append(cur)
    return segs


def parse(text):
    """Lines of [(red, text)] runs."""
    out = []
    for line in text.split('|'):
        runs = []
        for part in re.split(r'(\[\]|\{[^}]*\})', line):
            if part == '[]':
                runs.append(None)
            elif part.startswith('{'):
                runs.append((True, part[1:-1].strip()))
            elif part.strip():
                runs.append((False, part.strip()))
        out.append([r for r in runs if r is not None and r[1]])
    return out


def main():
    iso, src = sys.argv[1], sys.argv[2]
    pak_off, _ = B.iso_file(iso, 'PSP_GAME/USRDIR/pspdata.pak')
    index = B.pak_index(iso)
    f = open(iso, 'rb')
    cache = {}
    for line in Path(src).read_text(encoding='utf-8').splitlines():
        if not line.strip() or line.startswith('#'):
            continue
        atlas, name, rect, text = line.split('\t', 3)
        x, y, w, h = map(int, rect.split(','))
        if atlas not in cache:
            off, size = index[atlas + '.gim']
            f.seek(pak_off + off)
            cache[atlas] = gim.decode(f.read(size))
        crop = cache[atlas].crop((x, y, x + w, y + h))
        comps = components(crop)
        if '[]' not in text:  # no icons in this one: outlined letters may look like them
            for c in comps:
                c['icon'] = False
        icons, letters = merge_icons(comps)
        want = parse(text)
        rows = lines_of(letters, len(want))
        if not rows or not all(rows):
            print(f'# {atlas} {name}: no letters', file=sys.stderr)
            continue
        out, ok = [], True
        # rows' vertical extents, split halfway where they would overlap (each sprite's box is
        # drawn whole: a neighbour's margin must not reach into its letters)
        ext = [[max(0, min(c['y0'] for c in row) - 1), min(h, max(c['y1'] for c in row) + 1)] for row in rows]
        for a_, b_ in zip(ext, ext[1:]):
            if a_[1] > b_[0]:
                m = (max(c['y1'] for c in rows[ext.index(a_)]) + min(c['y0'] for c in rows[ext.index(b_)]) + 1) // 2
                a_[1], b_[0] = m, m
        for r, (row, runs) in enumerate(zip(rows, want)):
            segs = segments_of(row, icons)
            if [s['red'] for s in segs] != [red for red, _ in runs]:
                print(f'# {atlas} {name} line {r + 1}: found {["red" if s["red"] else "text" for s in segs]}, '
                      f'transcribed {["red" if red else "text" for red, _ in runs]}', file=sys.stderr)
                ok = False
                break
            ry0, ry1 = min(c['y0'] for c in row), max(c['y1'] for c in row)
            line = []
            for s, (red, t) in zip(segs, runs):
                # a margin (antialiasing; the font's letters a little wider), none into an icon
                bx0, bx1 = max(0, s['x0'] - 4), min(w, s['x1'] + 4)
                for i in icons:
                    if i['y0'] < ry1 and i['y1'] > ry0:
                        if i['x1'] <= s['x0']:
                            bx0 = max(bx0, i['x1'])
                        if i['x0'] >= s['x1']:
                            bx1 = min(bx1, i['x0'])
                line.append([bx0, bx1, s, t])
            for a_, b_ in zip(line, line[1:]):  # neighbours on the line: split halfway
                if a_[1] > b_[0]:
                    a_[1] = b_[0] = (a_[2]['x1'] + b_[2]['x0'] + 1) // 2
            by0, by1 = ext[r]
            for bx0, bx1, _, t in line:
                # "@!" (redrawn whole) unless an icon reaches into the box
                touch = any(i['x0'] < bx1 and i['x1'] > bx0 and i['y0'] < by1 and i['y1'] > by0 for i in icons)
                out.append(f'{atlas}\t{name}\t{rect}\t@{"" if touch else "!"}{bx0},{by0},{bx1 - bx0},{by1 - by0} {t}')
        if ok:
            print('\n'.join(out))


if __name__ == '__main__':
    main()
