#!/usr/bin/env python3
"""Side-by-side review of vector UI text: original sprite (scaled, nearest) | runtime rendering.

    python3 -I tools/uitext/review.py ISO OUT.png atlas[:name-substring] ... [--k 3] [--max 60]
Needs build/uitext_preview (gcc -O2 -Iruntime -Igenerated -o build/uitext_preview tools/uitext/preview.c -lm).
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build as B  # noqa: E402
import gim  # noqa: E402

PORT = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('iso')
    ap.add_argument('out')
    ap.add_argument('sel', nargs='+')
    ap.add_argument('--k', type=int, default=3)
    ap.add_argument('--max', type=int, default=60)
    ap.add_argument('--all', action='store_true', help='also sprites that are not in the spec')
    ap.add_argument('--spec', default=str(PORT / 'generated' / 'ui_text.spec'))
    a = ap.parse_args()
    pak_off, _ = B.iso_file(a.iso, 'PSP_GAME/USRDIR/pspdata.pak')
    index = B.pak_index(a.iso)
    f = open(a.iso, 'rb')

    def read(n):
        o, s = index[n]
        f.seek(pak_off + o)
        return f.read(s)

    spec = {}
    atlas = None
    for line in open(a.spec):
        t = line.split()
        if not t or t[0] in ('#', 'R'):  # R: the atlas's sprite rectangles
            continue
        if t[0] == 'A':
            atlas = t[1]
            continue
        spec.setdefault(atlas, []).append(tuple(map(int, t[1:5])))
    labels = B.load_labels()
    rows = []
    for sel in a.sel:
        name, _, sub = sel.partition(':')
        img = gim.decode(read(name + '.gim'))
        w, h = img.size
        W, H = 1, 1
        while W < w:
            W *= 2
        while H < h:
            H *= 2
        canvas = Image.new('RGBA', (W, H), (0, 0, 0, 0))
        canvas.paste(img, (0, 0))
        k = a.k
        new = None
        with tempfile.TemporaryDirectory() as td:
            src, dst = os.path.join(td, 'in.rgba'), os.path.join(td, 'out.rgba')
            open(src, 'wb').write(canvas.tobytes())
            env = dict(os.environ, PANGYA_ASSETS=str(PORT / 'assets'), PANGYA_UI_TEXT=a.spec)
            r = subprocess.run([str(PORT / 'build' / 'uitext_preview'), src, str(W), str(H), str(k), dst], env=env,
                               capture_output=True, text=True)
            if r.returncode == 0:
                new = Image.frombytes('RGBA', (W * k, H * k), open(dst, 'rb').read())
        try:
            sprites = gim.sprites(read(name + '.txt'))
        except KeyError:  # no list of its own (shared, or in sprites.tsv)
            sprites = []
        sprites += B.extra_sprites(name)
        names = {(x, y, w_, h_): n for n, x, y, w_, h_ in sprites}
        lab = labels.get(name, {})
        rects = list(lab) if not a.all else list(names)
        for rect in rects:
            n = lab.get(rect, (names.get(rect, '?'), ''))[0]
            if sub and sub not in n:
                continue
            x, y, w_, h_ = rect
            covered = any(not (sx >= x + w_ or sx + sw <= x or sy >= y + h_ or sy + sh <= y) for sx, sy, sw, sh in spec.get(name, ()))
            orig = img.crop((x, y, x + w_, y + h_)).resize((w_ * k, h_ * k), Image.NEAREST)
            vec = new.crop((x * k, y * k, (x + w_) * k, (y + h_) * k)) if new and covered else None
            rows.append((f'{name}:{n}', orig, vec))
    rows = rows[:a.max]
    if not rows:
        print('nothing selected')
        return
    cw = max(o.width for _, o, _ in rows)
    sheet = Image.new('RGBA', (cw * 2 + 30, sum(o.height + 8 for _, o, _ in rows) + 8), (90, 90, 140, 255))
    y = 8
    for label, o, v in rows:
        sheet.alpha_composite(o, (6, y))
        if v:
            sheet.alpha_composite(v, (cw + 24, y))
        else:  # not vectorized: a red mark
            for i in range(10):
                for j in range(10):
                    sheet.putpixel((cw + 24 + i, y + j), (255, 0, 0, 255))
        y += o.height + 8
    sheet.convert('RGB').save(a.out)
    print(len(rows), 'rows ->', a.out)


if __name__ == '__main__':
    main()
