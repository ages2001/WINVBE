#!/usr/bin/env python3
"""
WINVBE font generator: builds the large-font (120 dpi) system fonts
   WVBSYS.FON  "System"    proportional, bold, 20 px  (ANSI)
   WVBFIX.FON  "Fixedsys"  fixed 10 x 20               (ANSI)
   WVBOEM.FON  "Terminal"  fixed 10 x 20               (OEM, code page 437)
as Windows 2.x/3.x raster font resources (FNT version 2.00 inside a
resource-only NE module), so large fonts work without the Windows disks.

Glyphs are rendered from the DejaVu fonts (Bitstream Vera / DejaVu licence,
see FONTS.TXT); box drawing and block characters of the OEM font are drawn
procedurally so that they join exactly across character cells.

usage: mkfon.py <outdir>
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

DEJAVU = '/usr/share/fonts/truetype/dejavu/'
H = 20          # cell height
BASE = 16       # baseline row (= ascent)
CELL = 10       # fixed pitch width


# --------------------------------------------------------------------------
# glyph rendering

def render(font, ch, fixed=None):
    """returns (width, rows) where rows is a list of H strings of '#'/'.'"""
    im = Image.new('1', (64, H + 8), 0)
    d = ImageDraw.Draw(im)
    d.fontmode = '1'
    ox = 16
    d.text((ox, BASE), ch, font=font, fill=1, anchor='ls')
    bb = im.getbbox()
    adv = int(round(font.getlength(ch)))
    if fixed:
        w = fixed
        x0 = ox
        if bb:
            # keep the ink inside the cell
            if bb[0] < x0:
                x0 = bb[0]
            if bb[2] > x0 + w:
                x0 = max(bb[2] - w, bb[0] - 0)
    else:
        x0 = ox
        w = max(adv, 1)
        if bb:
            if bb[0] < x0:          # negative left bearing: shift glyph right
                w += x0 - bb[0]
                x0 = bb[0]
            if bb[2] > x0 + w:
                w = bb[2] - x0
    rows = []
    for y in range(H):
        rows.append(''.join('#' if 0 <= x < 64 and y < im.size[1] and im.getpixel((x, y)) else '.'
                            for x in range(x0, x0 + w)))
    return w, rows


def box_glyph(w=CELL):
    return [['.'] * w for _ in range(H)]


def fill(g, x0, x1, y0, y1):
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(len(g[0]), x1 + 1)):
            g[y][x] = '#'


# line positions inside a 10 x 20 cell (2 pixel strokes)
SV = (4, 5)                   # single vertical columns
DV = ((2, 3), (6, 7))         # double vertical: left, right line
SH = (8, 9)                   # single horizontal rows
DH = ((6, 7), (10, 11))       # double horizontal: top, bottom line


def vband(w, side):
    """columns of the vertical line of weight w nearest to side ('l' or 'r')"""
    if w == 1:
        return SV
    return DV[0] if side == 'l' else DV[1]


def hband(w, side):
    """rows of the horizontal line of weight w nearest to side ('t' or 'b')"""
    if w == 1:
        return SH
    return DH[0] if side == 't' else DH[1]


def box_draw(u, d, l, r):
    """box drawing glyph from arm weights (0 none, 1 single, 2 double)"""
    g = box_glyph()
    cx0, cx1 = 4, 5          # centre band used when there is no perpendicular line
    cy0, cy1 = 9, 10
    for vert, horz, top, left in ((u, l, True, True), (u, r, True, False), (d, l, False, True), (d, r, False, False)):
        vs = 'l' if left else 'r'
        hs = 't' if top else 'b'
        if vert and horz:                     # corner of this quadrant
            vx = vband(vert, vs)
            hy = hband(horz, hs)
            if top:
                fill(g, vx[0], vx[1], 0, hy[1])
            else:
                fill(g, vx[0], vx[1], hy[0], H - 1)
            if left:
                fill(g, 0, vx[1], hy[0], hy[1])
            else:
                fill(g, vx[0], CELL - 1, hy[0], hy[1])
        elif vert:                            # vertical half arm continues to the far horizontal line
            vx = vband(vert, vs)
            hw = max(l, r)
            if top:
                yend = hband(hw, 'b')[1] if hw else cy1
                fill(g, vx[0], vx[1], 0, yend)
            else:
                ystart = hband(hw, 't')[0] if hw else cy0
                fill(g, vx[0], vx[1], ystart, H - 1)
        elif horz:
            hy = hband(horz, hs)
            vw = max(u, d)
            if left:
                xend = vband(vw, 'r')[1] if vw else cx1
                fill(g, 0, xend, hy[0], hy[1])
            else:
                xstart = vband(vw, 'l')[0] if vw else cx0
                fill(g, xstart, CELL - 1, hy[0], hy[1])
    # straight lines crossing a line of a different kind run through
    if u and u == d and not (u == 2 and max(l, r) == 2):
        for c in ((SV,) if u == 1 else DV):
            fill(g, c[0], c[1], 0, H - 1)
    if l and l == r and not (l == 2 and max(u, d) == 2):
        for rr in ((SH,) if l == 1 else DH):
            fill(g, 0, CELL - 1, rr[0], rr[1])
    return CELL, [''.join(r) for r in g]


BOX = {  # code page 437 box drawing: (up, down, left, right)
    0xB3: (1, 1, 0, 0), 0xB4: (1, 1, 1, 0), 0xB5: (1, 1, 2, 0), 0xB6: (2, 2, 1, 0),
    0xB7: (0, 2, 1, 0), 0xB8: (0, 1, 2, 0), 0xB9: (2, 2, 2, 0), 0xBA: (2, 2, 0, 0),
    0xBB: (0, 2, 2, 0), 0xBC: (2, 0, 2, 0), 0xBD: (2, 0, 1, 0), 0xBE: (1, 0, 2, 0),
    0xBF: (0, 1, 1, 0), 0xC0: (1, 0, 0, 1), 0xC1: (1, 0, 1, 1), 0xC2: (0, 1, 1, 1),
    0xC3: (1, 1, 0, 1), 0xC4: (0, 0, 1, 1), 0xC5: (1, 1, 1, 1), 0xC6: (1, 1, 0, 2),
    0xC7: (2, 2, 0, 1), 0xC8: (2, 0, 0, 2), 0xC9: (0, 2, 0, 2), 0xCA: (2, 0, 2, 2),
    0xCB: (0, 2, 2, 2), 0xCC: (2, 2, 0, 2), 0xCD: (0, 0, 2, 2), 0xCE: (2, 2, 2, 2),
    0xCF: (1, 0, 2, 2), 0xD0: (2, 0, 1, 1), 0xD1: (0, 1, 2, 2), 0xD2: (0, 2, 1, 1),
    0xD3: (2, 0, 0, 1), 0xD4: (1, 0, 0, 2), 0xD5: (0, 1, 0, 2), 0xD6: (0, 2, 0, 1),
    0xD7: (2, 2, 1, 1), 0xD8: (1, 1, 2, 2), 0xD9: (1, 0, 1, 0), 0xDA: (0, 1, 0, 1),
}


def block_glyph(c):
    g = box_glyph()
    if c == 0xB0:       # light shade (25 %)
        for y in range(H):
            for x in range(CELL):
                if x % 4 == (y % 2) * 2:
                    g[y][x] = '#'
    elif c == 0xB1:     # medium shade (50 %)
        for y in range(H):
            for x in range(CELL):
                if (x + y) % 2 == 0:
                    g[y][x] = '#'
    elif c == 0xB2:     # dark shade (75 %)
        for y in range(H):
            for x in range(CELL):
                if x % 4 != (y % 2) * 2:
                    g[y][x] = '#'
    elif c == 0xDB:
        fill(g, 0, CELL - 1, 0, H - 1)
    elif c == 0xDC:
        fill(g, 0, CELL - 1, H // 2, H - 1)
    elif c == 0xDD:
        fill(g, 0, CELL // 2 - 1, 0, H - 1)
    elif c == 0xDE:
        fill(g, CELL // 2, CELL - 1, 0, H - 1)
    elif c == 0xDF:
        fill(g, 0, CELL - 1, 0, H // 2 - 1)
    elif c == 0xFE:     # small square
        fill(g, 2, 7, 7, 12)
    return CELL, [''.join(r) for r in g]


def empty_box(w):
    """glyph for undefined characters: an outlined rectangle"""
    g = [['.'] * w for _ in range(H)]
    for y in range(5, BASE):
        g[y][1] = g[y][w - 2] = '#'
    for x in range(1, w - 1):
        g[5][x] = g[BASE - 1][x] = '#'
    return w, [''.join(r) for r in g]


# code page 437, 0x01..0x1F and 0x7F
CP437_LOW = ('\u0000☺☻♥♦♣♠•◘○◙♂♀♪♫'
             '☼►◄↕‼¶§▬↨↑↓→←∟↔▲▼')


def oem_char(c):
    if c < 0x20:
        return CP437_LOW[c]
    if c == 0x7F:
        return '⌂'
    return bytes([c]).decode('cp437')


def ansi_char(c):
    """Windows ANSI (1252) as used by Windows 3.x; None = undefined"""
    if c == 0x7F or c == 0x80:
        return None
    if c == 0xA0:
        return ' '
    try:
        return bytes([c]).decode('cp1252')
    except UnicodeDecodeError:
        return None


# --------------------------------------------------------------------------
# FNT 2.00 writer

def build_fnt(glyphs, first, last, face, points, res, ascent, intlead, weight, charset,
              pixwidth, pitchfam, default, brk, copyright):
    """glyphs: dict code -> (width, rows)"""
    n = last - first + 1
    sentinel = (8, ['.' * 8] * H)
    allg = [glyphs[c] for c in range(first, last + 1)] + [sentinel]
    table_off = 0x76
    bits_off = table_off + 4 * len(allg)
    bits = bytearray()
    table = bytearray()
    widthbytes = 0
    for w, rows in allg:
        table += struct.pack('<HH', w, bits_off + len(bits))
        nb = (w + 7) // 8
        widthbytes += nb
        for b in range(nb):
            for y in range(H):
                v = 0
                for x in range(8):
                    xx = b * 8 + x
                    if xx < w and rows[y][xx] == '#':
                        v |= 0x80 >> x
                bits.append(v)
    face_off = bits_off + len(bits)
    total = face_off + len(face) + 1
    widths = [glyphs[c][0] for c in range(first, last + 1)]
    avg = glyphs[ord('x')][0] if ord('x') in glyphs else sum(widths) // len(widths)
    hdr = struct.pack('<HI60sHHHHHHHBBBHBHHBHHBBBBHIIIIB',
                      0x0200, total, copyright.encode('ascii')[:59].ljust(60, b'\0'),
                      0, points, res, res, ascent, intlead, 0,
                      0, 0, 0, weight, charset, pixwidth, H, pitchfam,
                      avg, max(widths), first, last, default - first, brk - first,
                      widthbytes, 0, face_off, 0, bits_off, 0)
    assert len(hdr) == 0x76, len(hdr)
    data = hdr + table + bits + face.encode('ascii') + b'\0'
    assert len(data) == total
    assert total < 0x10000
    return data


# --------------------------------------------------------------------------
# NE (resource-only module) writer

STUB = (b'\x0e\x1f\xba\x0e\x00\xb4\x09\xcd\x21\xb8\x01\x4c\xcd\x21'
        b'WINVBE font file - requires Microsoft Windows.\r\n$')


def build_fon(fnt, modname, description, face):
    # GDI finds the stock fonts by module name: FONTS, FIXFONTS, OEMFONTS
    shift = 4
    align = 1 << shift
    # MZ header + stub
    stub = STUB.ljust(0x40, b'\0')
    mz = bytearray(0x40)
    mz[0:2] = b'MZ'
    struct.pack_into('<HHHHHHHHHHHHH', mz, 2, (0x80) % 512, 1, 0, 4, 0, 0xFFFF, 0, 0xB8, 0, 0, 0, 0x40, 0)
    struct.pack_into('<I', mz, 0x3C, 0x80)
    out = bytearray(mz + stub)
    ne_off = len(out)
    assert ne_off == 0x80
    # resource directory entry for FONTDIR
    fontdir = struct.pack('<HH', 1, 1) + fnt[:113] + b'\0' + face.encode('ascii') + b'\0'
    # tables after the 64-byte NE header
    restab = bytearray()
    restab += struct.pack('<H', shift)
    rt_types = [(0x8007, 0x0C50, None), (0x8008, 0x1C30, 0x8001)]
    names_off = 2 + 20 * len(rt_types) + 2
    res_entries_pos = []
    for t, fl, rid in rt_types:
        restab += struct.pack('<HHI', t, 1, 0)
        res_entries_pos.append(len(restab))
        restab += struct.pack('<HHHHHH', 0, 0, fl, rid if rid else names_off, 0, 0)
    restab += struct.pack('<H', 0)
    restab += b'\x07FONTDIR' + b'\0'
    resname = bytes([len(modname)]) + modname.encode('ascii') + b'\0\0' + b'\0'
    modref = b''
    impname = b'\0'
    entry = b'\0\0'
    rel = 0x40
    restab_off = rel
    resname_off = restab_off + len(restab)
    modref_off = resname_off + len(resname)
    impname_off = modref_off + len(modref)
    entry_off = impname_off
    after = entry_off + max(len(impname), len(entry))
    nonres = bytes([len(description)]) + description.encode('ascii') + b'\0\0' + b'\0'
    nonres_file = ne_off + after
    pos = nonres_file + len(nonres)
    pos = (pos + align - 1) // align * align
    fd_pos = pos
    pos += (len(fontdir) + align - 1) // align * align
    fnt_pos = pos
    pos += (len(fnt) + align - 1) // align * align
    # fill resource entries
    for (t, fl, rid), epos, (p, data) in zip(rt_types, res_entries_pos, ((fd_pos, fontdir), (fnt_pos, fnt))):
        struct.pack_into('<HH', restab, epos, p >> shift, (len(data) + align - 1) >> shift)
    ne = bytearray(0x40)
    ne[0:2] = b'NE'
    ne[2] = 5
    ne[3] = 10
    struct.pack_into('<HH', ne, 0x04, entry_off, len(entry))
    struct.pack_into('<H', ne, 0x0C, 0x8300)           # library, Windows API compatible
    struct.pack_into('<H', ne, 0x1C, 0)                 # segments
    struct.pack_into('<H', ne, 0x1E, 0)                 # module references
    struct.pack_into('<H', ne, 0x20, len(nonres))
    struct.pack_into('<HHHHH', ne, 0x22, rel, restab_off, resname_off, modref_off, impname_off)
    struct.pack_into('<I', ne, 0x2C, nonres_file)
    struct.pack_into('<HH', ne, 0x30, 0, shift)
    struct.pack_into('<H', ne, 0x34, 0)
    ne[0x36] = 2                                        # Windows
    struct.pack_into('<H', ne, 0x3E, 0x0300)            # expected Windows version
    out += ne + restab + resname + modref
    out += impname if len(impname) >= len(entry) else entry
    assert len(out) == ne_off + after
    out += nonres
    out = out.ljust(fd_pos, b'\0') + fontdir
    out = out.ljust(fnt_pos, b'\0') + fnt
    out = out.ljust(pos, b'\0')
    return bytes(out)


# --------------------------------------------------------------------------

COPY = 'WINVBE - glyphs derived from DejaVu fonts (Bitstream Vera licence)'


def make_system():
    f = ImageFont.truetype(DEJAVU + 'DejaVuSans-Bold.ttf', 16)
    g = {}
    for c in range(0x20, 0x100):
        ch = ansi_char(c)
        g[c] = render(f, ch) if ch is not None else empty_box(9)
    g[0x20] = (5, ['.' * 5] * H)
    g[0xA0] = g[0x20]
    fnt = build_fnt(g, 0x20, 0xFF, 'System', 10, 120, BASE, 4, 700, 0, 0, 0x21, 0x80, 0x20, COPY)
    return build_fon(fnt, 'FONTS', 'FONTRES 100,120,120 : WINVBE System 120 dpi', 'System')


def make_fixed():
    f = ImageFont.truetype(DEJAVU + 'DejaVuSansMono-Bold.ttf', 16)
    g = {}
    for c in range(0x20, 0x100):
        ch = ansi_char(c)
        g[c] = render(f, ch, CELL) if ch is not None else empty_box(CELL)
    g[0x20] = g[0xA0] = (CELL, ['.' * CELL] * H)
    fnt = build_fnt(g, 0x20, 0xFF, 'Fixedsys', 12, 120, BASE, 2, 400, 0, CELL, 0x30, 0x80, 0x20, COPY)
    return build_fon(fnt, 'FIXFONTS', 'FONTRES 100,120,120 : WINVBE Fixedsys 120 dpi', 'Fixedsys')


def make_oem():
    f = ImageFont.truetype(DEJAVU + 'DejaVuSansMono-Bold.ttf', 16)
    g = {}
    for c in range(0x01, 0xFF):
        if c in BOX:
            g[c] = box_draw(*BOX[c])
        elif c in (0xB0, 0xB1, 0xB2, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xFE):
            g[c] = block_glyph(c)
        else:
            g[c] = render(f, oem_char(c), CELL)
            if not any('#' in r for r in g[c][1]) and c not in (0x20, 0xFF):
                g[c] = empty_box(CELL)
    g[0x20] = (CELL, ['.' * CELL] * H)
    fnt = build_fnt(g, 0x01, 0xFE, 'Terminal', 12, 120, BASE, 2, 400, 255, CELL, 0x30, ord('?'), 0x20, COPY)
    return build_fon(fnt, 'OEMFONTS', 'FONTRES 100,120,120 : WINVBE Terminal 120 dpi', 'Terminal')


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else '.'
    os.makedirs(out, exist_ok=True)
    for name, fn in (('WVBSYS.FON', make_system), ('WVBFIX.FON', make_fixed), ('WVBOEM.FON', make_oem)):
        data = fn()
        open(os.path.join(out, name), 'wb').write(data)
        print(name, len(data))


if __name__ == '__main__':
    main()
