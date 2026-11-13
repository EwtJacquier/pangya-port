"""PSP GIM images and the game's sprite lists (.txt: name, x, y, w, h per line).

decode() returns the image exactly as the PSP GE samples it (palette entries copied as stored,
16-bit colors expanded like runtime/ge.c conv16), so its CRC matches what the runtime decodes.
"""
import struct

from PIL import Image


def _conv16(v, fmt):
    """Same expansion as runtime/ge.c (expand565 / expand5551 / expand4444)."""
    if fmt == 0:  # 5650
        r, g, b = v & 31, (v >> 5) & 63, (v >> 11) & 31
        return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255)
    if fmt == 1:  # 5551
        r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
        return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255 if v >> 15 else 0)
    return ((v & 15) * 17, ((v >> 4) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 12) & 15) * 17)  # 4444


def _blocks(d, off, end, out):
    while off + 16 <= end:
        bid, _, size, nxt, doff = struct.unpack_from('<HHIII', d, off)
        out.append((bid, off + doff))
        if bid in (2, 3):
            _blocks(d, off + doff, off + size, out)
        if not nxt or not size:
            break
        off += nxt


def _image_info(d, p):
    _, _, fmt, order, w, h, bpp, pitch_align, _ = struct.unpack_from('<HHHHHHHHH', d, p)
    _, pix_start, pix_end = struct.unpack_from('<III', d, p + 0x18)
    return dict(fmt=fmt, order=order, w=w, h=h, bpp=bpp, pitch_align=pitch_align or 1, data=p + pix_start, end=p + pix_end)


def decode(d):
    """GIM bytes -> PIL RGBA image of the first picture, or None if unsupported."""
    if d[:11] != b'MIG.00.1PSP':
        return None
    bl = []
    _blocks(d, 0x10, len(d), bl)
    img = next((_image_info(d, p) for b, p in bl if b == 4), None)
    pal = next((_image_info(d, p) for b, p in bl if b == 5), None)
    if not img:
        return None
    w, h, fmt, bpp = img['w'], img['h'], img['fmt'], img['bpp']
    pitch = ((w * bpp + 7) // 8 + img['pitch_align'] - 1) // img['pitch_align'] * img['pitch_align']
    raw = d[img['data']:img['end']]
    rows = len(raw) // pitch if pitch else 0
    if rows < h:
        return None
    if img['order'] == 1:  # swizzled: blocks of 16 bytes x 8 rows
        lin = bytearray(len(raw))
        bw = pitch // 16
        for by in range((rows + 7) // 8):
            for bx in range(bw):
                for r in range(8):
                    y = by * 8 + r
                    if y >= rows:
                        break
                    so = (by * bw + bx) * 128 + r * 16
                    lin[y * pitch + bx * 16:y * pitch + bx * 16 + 16] = raw[so:so + 16]
        raw = bytes(lin)
    palette = None
    if pal:
        pr = d[pal['data']:pal['end']]
        if pal['fmt'] == 3:
            palette = pr
        elif pal['fmt'] in (0, 1, 2):
            palette = b''.join(bytes(_conv16(struct.unpack_from('<H', pr, i)[0], pal['fmt'])) for i in range(0, len(pr) - 1, 2))
    out = bytearray()
    if fmt == 3:
        for y in range(h):
            out += raw[y * pitch:y * pitch + w * 4]
    elif fmt in (0, 1, 2):
        for y in range(h):
            for x in range(w):
                out += bytes(_conv16(struct.unpack_from('<H', raw, y * pitch + x * 2)[0], fmt))
    elif fmt in (4, 5) and palette:
        n = len(palette) // 4
        lut = [palette[i * 4:i * 4 + 4] for i in range(n)] + [b'\0\0\0\0'] * (256 - n)
        for y in range(h):
            row = raw[y * pitch:(y + 1) * pitch]
            if fmt == 5:
                out += b''.join(lut[row[x]] for x in range(w))
            else:
                out += b''.join(lut[(row[x >> 1] >> ((x & 1) * 4)) & 15] for x in range(w))
    else:
        return None
    return Image.frombytes('RGBA', (w, h), bytes(out))


def sprites(text):
    """Sprite list: [(name, x, y, w, h)]."""
    out = []
    for line in text.decode('latin1').splitlines():
        f = [x for x in line.replace('\r', '').split('\t') if x.strip()]
        if len(f) >= 5:
            try:
                out.append((f[0].strip(), int(f[1]), int(f[2]), int(f[3]), int(f[4])))
            except ValueError:
                pass
    return out
