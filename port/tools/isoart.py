"""Pictures for the web page, read from the player's own ISO: the title illustration and the
Pangya Island picture behind the sign-in screen (and the title logo's sprite sheet).

When the texture pack (build/textures) has an upscaled version of a picture, that one is used.
Python's standard library only (serve_web.py imports this).

    python3 -I tools/isoart.py ISO NAME OUT.png [PACK]     # NAME: title, title_logo
"""
import struct
import sys
import zlib
from pathlib import Path

ART = {  # name -> GIM file in pspdata.pak (and its sprite list, if any)
    'title': ('title_img_3.gim', None),  # the page's illustration
    'island': ('6_015.gim', None),       # the page's background: Pangya Island (the gallery's)
    'title_logo': ('title_logo.gim', 'title_logo.txt'),
}
SHOWN = ('title', 'island')  # what the page uses (made when the server starts)


# ---- ISO 9660 and the game's pak ------------------------------------------------------------------
def iso_file(f, path):
    """(byte offset, size) of a file on the ISO."""
    f.seek(16 * 2048)
    pvd = f.read(2048)
    lba, size = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
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
            raise FileNotFoundError(path)
        lba, size = found
    return lba * 2048, size


def pak_files(f):
    """{name: (offset in the ISO, size)} of pspdata.pak's files (psppackfilelist.pak entries:
    u16 3, u16 id, u32 offset, u32 size, u32 size, name, zero padding)."""
    off, size = iso_file(f, 'PSP_GAME/USRDIR/psppackfilelist.pak')
    f.seek(off)
    d = f.read(size)
    base, _ = iso_file(f, 'PSP_GAME/USRDIR/pspdata.pak')
    out, p = {}, 0
    while p + 16 <= len(d):
        kind, _, foff, fsz, _ = struct.unpack_from('<HHIII', d, p)
        if kind != 3:
            p += 4
            continue
        e = d.index(b'\0', p + 16)
        out.setdefault(d[p + 16:e].decode('latin1').lower(), (base + foff, fsz))
        p = (e + 4) & ~3
        while p + 2 <= len(d) and d[p:p + 2] != b'\x03\x00':
            p += 4
    return out


# ---- GIM pictures ------------------------------------------------------------------------------------
def _rgba16(v, fmt):
    if fmt == 0:  # 5650
        r, g, b = v & 31, (v >> 5) & 63, (v >> 11) & 31
        return bytes((r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255))
    if fmt == 1:  # 5551
        r, g, b = v & 31, (v >> 5) & 31, (v >> 10) & 31
        return bytes((r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255 if v >> 15 else 0))
    return bytes(((v & 15) * 17, ((v >> 4) & 15) * 17, ((v >> 8) & 15) * 17, (v >> 12) * 17))  # 4444


def _blocks(d, off, end, out, depth=0):
    while off + 16 <= end and depth < 4:
        bid, _, size, nxt, doff = struct.unpack_from('<HHIII', d, off)
        out.append((bid, off + doff))
        if bid in (2, 3):
            _blocks(d, off + doff, min(end, off + size), out, depth + 1)
        if not nxt or not size:
            break
        off += nxt


def _info(d, p):
    _, _, fmt, order, w, h, bpp, align, _ = struct.unpack_from('<HHHHHHHHH', d, p)
    _, start, end = struct.unpack_from('<III', d, p + 0x18)
    return dict(fmt=fmt, order=order, w=w, h=h, bpp=bpp, align=align or 1, data=p + start, end=p + end)


def _unswizzle(raw, pitch, rows):
    """GE swizzle: blocks of 16 bytes x 8 rows, left to right, then the next band of 8 rows."""
    out = bytearray(pitch * rows)
    bw = pitch // 16
    for by in range((rows + 7) // 8):
        band = raw[by * bw * 128:(by + 1) * bw * 128]
        for r in range(min(8, rows - by * 8)):
            y = by * 8 + r
            for k in range(16):
                out[y * pitch + k:(y + 1) * pitch:16] = band[r * 16 + k::128][:bw]
    return bytes(out)


def gim(d):
    """The first picture of a GIM file: (width, height, RGBA bytes)."""
    if d[:11] != b'MIG.00.1PSP':
        raise ValueError('not a GIM picture')
    bl = []
    _blocks(d, 0x10, len(d), bl)
    img = next(_info(d, p) for b, p in bl if b == 4)
    pal = next((_info(d, p) for b, p in bl if b == 5), None)
    w, h, fmt, bpp = img['w'], img['h'], img['fmt'], img['bpp']
    pitch = ((w * bpp + 7) // 8 + img['align'] - 1) // img['align'] * img['align']
    raw = d[img['data']:img['end']]
    rows = len(raw) // pitch
    if img['order'] == 1:
        full = (rows + 7) // 8 * 8
        raw = _unswizzle(raw[:pitch * full].ljust(pitch * full, b'\0'), pitch, rows)
    out = bytearray()
    if fmt == 3:
        for y in range(h):
            out += raw[y * pitch:y * pitch + w * 4]
    elif fmt in (0, 1, 2):
        for y in range(h):
            row = raw[y * pitch:y * pitch + w * 2]
            out += b''.join(_rgba16(v, fmt) for v, in struct.iter_unpack('<H', row))
    elif fmt in (4, 5):
        pr = d[pal['data']:pal['end']]
        if pal['fmt'] == 3:
            colors = [pr[i:i + 4] for i in range(0, len(pr) - 3, 4)]
        else:
            colors = [_rgba16(v, pal['fmt']) for v, in struct.iter_unpack('<H', pr[:len(pr) & ~1])]
        colors += [b'\0\0\0\0'] * (256 - len(colors))
        for y in range(h):
            row = raw[y * pitch:(y + 1) * pitch]
            if fmt == 5:
                out += b''.join(colors[i] for i in row[:w])
            else:
                idx = [n for b in row for n in (b & 15, b >> 4)][:w]
                out += b''.join(colors[i] for i in idx)
    else:
        raise ValueError(f'GIM format {fmt}')
    return w, h, bytes(out)


def key(w, h, rgba):
    """The texture pack's key of a picture: CRC-32 and Adler-32 of its RGBA texels."""
    return f'{zlib.crc32(rgba) & 0xffffffff:08x}{zlib.adler32(rgba) & 0xffffffff:08x}'


# ---- QOI (the texture pack's pictures) and PNG ------------------------------------------------------
def qoi(d):
    """(width, height, RGBA bytes) of a QOI picture."""
    if d[:4] != b'qoif':
        raise ValueError('not a QOI picture')
    w, h = struct.unpack_from('>II', d, 4)
    n = w * h
    out = bytearray(n * 4)
    index = [b'\0\0\0\0'] * 64
    r, g, b, a = 0, 0, 0, 255
    p, i = 14, 0
    while i < n:
        c = d[p]
        p += 1
        if c == 0xfe:
            r, g, b = d[p], d[p + 1], d[p + 2]
            p += 3
        elif c == 0xff:
            r, g, b, a = d[p], d[p + 1], d[p + 2], d[p + 3]
            p += 4
        elif c >> 6 == 0:
            r, g, b, a = index[c]
        elif c >> 6 == 1:
            r, g, b = (r + (c >> 4 & 3) - 2) & 255, (g + (c >> 2 & 3) - 2) & 255, (b + (c & 3) - 2) & 255
        elif c >> 6 == 2:
            dg = (c & 63) - 32
            c2 = d[p]
            p += 1
            r, g, b = (r + dg - 8 + (c2 >> 4)) & 255, (g + dg) & 255, (b + dg - 8 + (c2 & 15)) & 255
        else:
            run = (c & 63) + 1
            px = bytes((r, g, b, a))
            out[i * 4:(i + run) * 4] = px * run
            i += run
            continue
        px = bytes((r, g, b, a))
        index[(r * 3 + g * 5 + b * 7 + a * 11) & 63] = px
        out[i * 4:i * 4 + 4] = px
        i += 1
    return w, h, bytes(out)


def png(w, h, rgba):
    """PNG bytes: RGB when every texel is opaque, each row filtered against the one above (PNG's
    "Up" filter: much smaller for pictures, still the standard library only)."""
    opaque = rgba[3::4] == b'\xff' * (w * h)
    px, bpp = (bytes(b for i, b in enumerate(rgba) if i & 3 != 3), 3) if opaque else (rgba, 4)
    stride = w * bpp
    out, prev = [], bytes(stride)
    for y in range(h):
        row = px[y * stride:(y + 1) * stride]
        out.append(b'\2' + bytes((a - b) & 255 for a, b in zip(row, prev)) if y else b'\0' + row)
        prev = row
    chunk = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2 if opaque else 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(b''.join(out), 6)) + chunk(b'IEND', b''))


# ---- what the page asks for --------------------------------------------------------------------------
def original(iso, name):
    """(width, height, RGBA bytes, sprites) of one of ART, as the ISO has it (sprites: name ->
    [x, y, w, h] when the picture has a sprite list)."""
    gim_name, txt_name = ART[name]
    with open(iso, 'rb') as f:
        files = pak_files(f)
        off, size = files[gim_name]
        f.seek(off)
        w, h, rgba = gim(f.read(size))
        sprites = {}
        if txt_name and txt_name in files:
            off, size = files[txt_name]
            f.seek(off)
            for line in f.read(size).decode('latin1').splitlines():
                parts = line.split()
                if len(parts) == 5 and all(v.isdigit() for v in parts[1:]):
                    sprites[parts[0].removesuffix('_I')] = [int(v) for v in parts[1:]]
    return w, h, rgba, sprites


def pack_file(pack, w, h, rgba):
    """The texture pack's file for this picture (its upscaled version), or None."""
    k = key(w, h, rgba)
    try:
        for line in (Path(pack) / 'index.txt').read_text().splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[0] == k and parts[1] == str(w) and parts[2] == str(h):
                return Path(pack) / parts[3]
    except OSError:
        pass
    return None


def picture(w, h, rgba, upscaled=None):
    """PNG bytes of the picture, or of its upscaled version (a pack file) when there is one, and
    the scale used."""
    if upscaled:
        try:
            pw, ph, big = qoi(Path(upscaled).read_bytes())
            if pw % w == 0 and pw // w == ph // h:
                return png(pw, ph, big), pw // w
        except (OSError, ValueError):
            pass
    return png(w, h, rgba), 1


if __name__ == '__main__':
    w, h, rgba, sprites = original(sys.argv[1], sys.argv[2])
    data, scale = picture(w, h, rgba, pack_file(sys.argv[4], w, h, rgba) if len(sys.argv) > 4 else None)
    Path(sys.argv[3]).write_bytes(data)
    print(dict(w=w, h=h, scale=scale, sprites=sprites))
