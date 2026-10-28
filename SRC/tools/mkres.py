#!/usr/bin/env python3
"""
WINVBE resource generator: draws all OEM bitmaps, cursors and icons used by
USER from scratch (original artwork) and writes .bmp/.cur/.ico files plus the
.rc script.  Sizes follow the Windows 3.x display driver resource layout.
The release drivers use the standard Windows ones instead (stockres.py); this
artwork is the fallback when msfiles/stock is missing.  The OEM tables
(oem*.bin, nametab.bin, nt*.bin) are always used.
"""
import os, struct, sys, math

OUT = sys.argv[1] if len(sys.argv) > 1 else 'res'
os.makedirs(OUT, exist_ok=True)

# standard 16 color DIB palette (R,G,B)
PAL16 = [(0, 0, 0), (128, 0, 0), (0, 128, 0), (128, 128, 0), (0, 0, 128), (128, 0, 128), (0, 128, 128),
         (128, 128, 128), (192, 192, 192), (255, 0, 0), (0, 255, 0), (255, 255, 0), (0, 0, 255), (255, 0, 255),
         (0, 255, 255), (255, 255, 255)]
K, DR, DG, DY, DB, DM, DC, DGRAY, LGRAY, R, G, Y, B, M, C, W = range(16)


class Canvas:
    def __init__(self, w, h, bg=0):
        self.w, self.h = w, h
        self.p = [[bg] * w for _ in range(h)]

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.p[y][x] = c

    def get(self, x, y):
        return self.p[y][x]

    def rect(self, x0, y0, x1, y1, c):           # filled, inclusive
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.set(x, y, c)

    def frame(self, x0, y0, x1, y1, c, t=1):
        for i in range(t):
            self.hline(x0 + i, x1 - i, y0 + i, c)
            self.hline(x0 + i, x1 - i, y1 - i, c)
            self.vline(x0 + i, y0 + i, y1 - i, c)
            self.vline(x1 - i, y0 + i, y1 - i, c)

    def hline(self, x0, x1, y, c):
        for x in range(x0, x1 + 1):
            self.set(x, y, c)

    def vline(self, x, y0, y1, c):
        for y in range(y0, y1 + 1):
            self.set(x, y, c)

    def line(self, x0, y0, x1, y1, c):
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        err = dx + dy
        while True:
            self.set(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy; x0 += sx
            if e2 <= dx:
                err += dx; y0 += sy

    def poly(self, pts, c):                       # filled polygon (even-odd, pixel centres)
        ys = [p[1] for p in pts]
        for y in range(int(min(ys)), int(max(ys)) + 1):
            yc = y + 0.5
            xs = []
            n = len(pts)
            for i in range(n):
                (xa, ya), (xb, yb) = pts[i], pts[(i + 1) % n]
                if (ya <= yc < yb) or (yb <= yc < ya):
                    xs.append(xa + (yc - ya) * (xb - xa) / (yb - ya))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(int(math.ceil(xs[i] - 0.5)), int(math.floor(xs[i + 1] - 0.5)) + 1):
                    self.set(x, y, c)

    def disc(self, cx, cy, r, c):
        for y in range(self.h):
            for x in range(self.w):
                if (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2 <= r * r:
                    self.set(x, y, c)

    def ring(self, cx, cy, r0, r1, c):
        for y in range(self.h):
            for x in range(self.w):
                d = (x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2
                if r0 * r0 <= d <= r1 * r1:
                    self.set(x, y, c)

    def blit(self, other, ox, oy, transparent=None):
        for y in range(other.h):
            for x in range(other.w):
                c = other.p[y][x]
                if c != transparent:
                    self.set(ox + x, oy + y, c)

    def text(self, art, ox, oy, cmap):
        for y, row in enumerate(art):
            for x, ch in enumerate(row):
                if ch in cmap:
                    self.set(ox + x, oy + y, cmap[ch])


# ---------------------------------------------------------------------------
# file writers

def dib_rows(cv, bits, palmap=None):
    rows = []
    for y in range(cv.h - 1, -1, -1):
        if bits == 4:
            row = bytearray()
            for x in range(0, cv.w, 2):
                a = cv.p[y][x]
                b = cv.p[y][x + 1] if x + 1 < cv.w else 0
                row.append((a << 4) | b)
        else:  # 1 bpp: palette index nonzero -> bit 1
            row = bytearray((cv.w + 7) // 8)
            for x in range(cv.w):
                if cv.p[y][x]:
                    row[x >> 3] |= 0x80 >> (x & 7)
        while len(row) % 4:
            row.append(0)
        rows.append(bytes(row))
    return b''.join(rows)


def write_bmp(name, cv, bits):
    if bits == 4:
        pal = b''.join(bytes((b, g, r, 0)) for r, g, b in PAL16)
    else:
        pal = bytes((0, 0, 0, 0, 255, 255, 255, 0))
    data = dib_rows(cv, bits)
    hdr = struct.pack('<IiiHHIIiiII', 40, cv.w, cv.h, 1, bits, 0, len(data), 0, 0, 16 if bits == 4 else 2, 0)
    off = 14 + len(hdr) + len(pal)
    fh = b'BM' + struct.pack('<IHHI', off + len(data), 0, 0, off)
    open(os.path.join(OUT, name), 'wb').write(fh + hdr + pal + data)


def icon_image(xorcv, andcv, bits):
    """DIB for icon/cursor: header (height doubled) + palette + XOR + AND"""
    if bits == 4:
        pal = b''.join(bytes((b, g, r, 0)) for r, g, b in PAL16)
    else:
        pal = bytes((0, 0, 0, 0, 255, 255, 255, 0))
    xd = dib_rows(xorcv, bits)
    ad = dib_rows(andcv, 1)
    hdr = struct.pack('<IiiHHIIiiII', 40, xorcv.w, xorcv.h * 2, 1, bits, 0, len(xd) + len(ad), 0, 0, 0, 0)
    return hdr + pal + xd + ad


def write_ico(name, images):
    """images: list of (w, h, ncolors, bits, data)"""
    hdr = struct.pack('<HHH', 0, 1, len(images))
    off = 6 + 16 * len(images)
    dirs, blob = b'', b''
    for w, h, nc, bits, data in images:
        dirs += struct.pack('<BBBBHHII', w, h, nc, 0, 1, bits, len(data), off + len(blob))
        blob += data
    open(os.path.join(OUT, name), 'wb').write(hdr + dirs + blob)


def write_cur(name, hx, hy, data):
    hdr = struct.pack('<HHH', 0, 2, 1)
    d = struct.pack('<BBBBHHII', 32, 32, 0, 0, hx, hy, len(data), 22)
    open(os.path.join(OUT, name), 'wb').write(hdr + d + data)


# ---------------------------------------------------------------------------
# button helpers (Windows 3.x look, own drawing)

def button(w, h, pressed=False):
    cv = Canvas(w, h, LGRAY)
    cv.frame(0, 0, w - 1, h - 1, K)
    if pressed:
        cv.hline(1, w - 2, 1, DGRAY)
        cv.vline(1, 1, h - 2, DGRAY)
    else:
        cv.hline(1, w - 3, 1, W)
        cv.vline(1, 1, h - 3, W)
        cv.hline(1, w - 2, h - 2, DGRAY)
        cv.vline(w - 2, 1, h - 2, DGRAY)
        cv.hline(2, w - 2, h - 3, DGRAY)
        cv.vline(w - 3, 2, h - 2, DGRAY)
    return cv


def tri(cv, cx, cy, size, direction, c=K):
    """solid triangle, size = half base"""
    for i in range(size):
        if direction == 'up':
            cv.hline(cx - i, cx + i, cy - size // 2 + i, c)
        elif direction == 'down':
            cv.hline(cx - i, cx + i, cy + size // 2 - i, c)
        elif direction == 'left':
            cv.vline(cx - size // 2 + i, cy - i, cy + i, c)
        elif direction == 'right':
            cv.vline(cx + size // 2 - i, cy - i, cy + i, c)


def arrow_glyph(cv, cx, cy, direction, c=K, head=4, stem=3, sw=1):
    """arrow: triangle head (head rows) plus a stem of half-width sw"""
    total = head + stem
    top = cy - total // 2
    for k in range(total):
        if direction in ('up', 'left'):
            r = k
        else:
            r = total - 1 - k
        if r < head:
            half = r
        else:
            half = sw
        if direction in ('up', 'down'):
            cv.hline(cx - half, cx + half, top + k, c)
        else:
            cv.vline(top + k, cy - half, cy + half, c)

bitmaps = {}

# scroll arrows 17x17 (normal and pressed)
for name, rid_n, rid_d, d in (('up', 32753, 32743, 'up'), ('dn', 32752, 32742, 'down'),
                              ('rg', 32751, 32741, 'right'), ('lf', 32750, 32740, 'left')):
    for pressed, rid in ((False, rid_n), (True, rid_d)):
        cv = button(17, 17, pressed)
        o = 1 if pressed else 0
        arrow_glyph(cv, 8 + o, 8 + o, d)
        bitmaps[rid] = (cv, 4)

# inactive (grayed) scroll arrows 17x17 - Windows NT 3.1 also loads these
for rid, d in ((32737, 'up'), (32736, 'down'), (32735, 'right'), (32734, 'left')):
    cv = button(17, 17, False)
    arrow_glyph(cv, 9, 9, d, W)
    arrow_glyph(cv, 8, 8, d, DGRAY)
    bitmaps[rid] = (cv, 4)

# minimize / maximize / restore 19x18
for rid, rid_d, kind in ((32749, 32746, 'reduce'), (32748, 32745, 'zoom'), (32747, 32744, 'restore')):
    for pressed, r in ((False, rid), (True, rid_d)):
        cv = button(19, 18, pressed)
        o = 1 if pressed else 0
        if kind == 'reduce':
            tri(cv, 9 + o, 9 + o, 5, 'down')
        elif kind == 'zoom':
            tri(cv, 9 + o, 8 + o, 5, 'up')
        else:
            tri(cv, 9 + o, 5 + o, 4, 'up')
            tri(cv, 9 + o, 12 + o, 4, 'down')
        bitmaps[r] = (cv, 4)

# system menu boxes 36x18 (window and MDI child)
cv = Canvas(36, 18, LGRAY)
for ox, bw in ((2, 13), (24, 7)):
    cv.rect(ox, 7, ox + bw - 1, 9, W)
    cv.frame(ox, 7, ox + bw - 1, 9, K)
    cv.hline(ox + 1, ox + bw, 10, DGRAY)
    cv.vline(ox + bw, 8, 10, DGRAY)
bitmaps[32754] = (cv, 4)

# menu arrow 7x11 (mono, 1 = white)
cv = Canvas(7, 11, 1)
for i in range(5):
    cv.vline(1 + i, 1 + i, 9 - i, 0)
bitmaps[32739] = (cv, 1)

# combo box arrow 7x9 (mono)
cv = Canvas(7, 9, 1)
cv.rect(2, 1, 4, 3, 0)
for i in range(4):
    cv.hline(i, 6 - i, 4 + i, 0)
cv.hline(0, 6, 8, 0)
bitmaps[32738] = (cv, 1)

# old style (2.x) bitmaps, mono
def old_arrow(d):
    cv = Canvas(15, 15, 1)
    arrow_glyph(cv, 7, 7, d, 0)
    tri(cv, 7, 7, 0, d, 0)
    return cv
for rid, d in ((32765, 'up'), (32764, 'down'), (32763, 'right'), (32762, 'left')):
    cv = Canvas(15, 15, 1)
    arrow_glyph(cv, 7, 7, d, 0, head=6, stem=5, sw=2)
    bitmaps[rid] = (cv, 1)

for rid, kind in ((32757, 'reduce'), (32756, 'zoom'), (32755, 'restore')):
    cv = Canvas(25, 19, 1)
    cv.frame(0, 0, 24, 18, 0)
    if kind == 'reduce':
        tri(cv, 12, 10, 7, 'down', 0)
    elif kind == 'zoom':
        tri(cv, 12, 8, 7, 'up', 0)
    else:
        tri(cv, 12, 5, 5, 'up', 0)
        tri(cv, 12, 13, 5, 'down', 0)
    bitmaps[rid] = (cv, 1)

cv = Canvas(50, 19, 1)
cv.frame(2, 7, 21, 10, 0)
cv.hline(3, 22, 11, 0); cv.vline(22, 8, 11, 0)
cv.frame(33, 7, 42, 10, 0)
cv.hline(34, 43, 11, 0); cv.vline(43, 8, 11, 0)
bitmaps[32767] = (cv, 1)

# size box 13x14 (mono)
cv = Canvas(13, 14, 1)
cv.rect(0, 6, 7, 7, 0)
cv.rect(5, 6, 7, 13, 0)
bitmaps[32766] = (cv, 1)

# size box for scroll bar corner 15x15
cv = Canvas(15, 15, 1)
cv.frame(0, 0, 14, 14, 0)
cv.frame(0, 0, 8, 8, 0)
bitmaps[32761] = (cv, 1)

# check mark 14x14
cv = Canvas(14, 14, 1)
for i, (x, y) in enumerate([(2, 7), (3, 8), (4, 9), (5, 10), (6, 9), (7, 8), (8, 7), (9, 6), (10, 5), (11, 4), (12, 3)]):
    cv.vline(x, y - 1, y, 0)
bitmaps[32760] = (cv, 1)

# check boxes 56x39: 4 columns x 3 rows of 14x13
cv = Canvas(56, 39, 1)
for col in range(4):
    ox = col * 14
    thick = col >= 2
    checked = col & 1
    # row 0: check boxes
    cv.frame(ox, 0, ox + 12, 12, 0, 2 if thick else 1)
    if checked:
        cv.line(ox + 2 + thick, 2 + thick, ox + 10 - thick, 10 - thick, 0)
        cv.line(ox + 10 - thick, 2 + thick, ox + 2 + thick, 10 - thick, 0)
    # row 1: radio buttons
    rc = Canvas(13, 13, 1)
    rc.ring(6.5, 6.5, 4.6 if not thick else 3.9, 6.3, 0)
    if checked:
        rc.disc(6.5, 6.5, 2.6, 0)
    cv.blit(rc, ox, 13)
    # row 2: three-state boxes
    if checked:
        cv.frame(ox, 26, ox + 12, 38, 0, 2 if thick else 1)
        for y in range(27 + thick, 38 - thick):
            for x in range(ox + 1 + thick, ox + 12 - thick):
                if (x + y) & 1:
                    cv.set(x, y, 0)
bitmaps[32759] = (cv, 1)

# button corners 30x10 (three 10x10 cells)
cv = Canvas(30, 10, 1)
c1 = Canvas(10, 10, 1); c1.ring(5, 5, 2.6, 3.6, 0)
c2 = Canvas(10, 10, 1); c2.ring(5, 5, 2.6, 4.4, 0)
c3 = Canvas(10, 10, 1); c3.rect(3, 3, 6, 6, 0)
cv.blit(c1, 0, 0); cv.blit(c2, 10, 0); cv.blit(c3, 20, 0)
bitmaps[32758] = (cv, 1)

for rid, (cv, bits) in bitmaps.items():
    write_bmp(f'b{rid}.bmp', cv, bits)

# ---------------------------------------------------------------------------
# cursors (32x32 mono): chars: '#' black, '.' white, ' ' transparent, '~' invert

def cursor(art, hx, hy):
    xorc = Canvas(32, 32, 0)
    andc = Canvas(32, 32, 1)
    for y, row in enumerate(art):
        for x, ch in enumerate(row):
            if ch == '#':
                andc.set(x, y, 0); xorc.set(x, y, 0)
            elif ch == '.':
                andc.set(x, y, 0); xorc.set(x, y, 1)
            elif ch == '~':
                andc.set(x, y, 1); xorc.set(x, y, 1)
    return icon_image(xorc, andc, 1), hx, hy


def gen_arrow():
    rows = []
    for y in range(19):
        rows.append([' '] * 32)
    # body: left edge x=0, diagonal edge, notch
    for y in range(16):
        for x in range(0, y + 1):
            if x <= 10:
                rows[y][x] = '.'
    for y in range(12, 16):
        for x in range(y - 4, y + 1):
            if x < 32:
                rows[y][x] = ' '
    # tail
    for y in range(11, 19):
        for x in range(5 + (y - 11) // 2, 8 + (y - 11) // 2):
            rows[y][x] = '.'
    # outline
    art = [''.join(r) for r in rows]
    return outline(art)


def outline(art):
    h = len(art)
    w = max(len(r) for r in art)
    g = [list(r.ljust(w)) for r in art]
    out = [r[:] for r in g]
    for y in range(h):
        for x in range(w):
            if g[y][x] == '.':
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h and g[ny][nx] == ' ':
                        out[ny][nx] = '#'
                    elif not (0 <= nx < w and 0 <= ny < h):
                        pass
    # pixels on the border of the bitmap get black outline inside
    return [''.join(r) for r in out]


def shape_from_fn(fn, size=32):
    art = []
    for y in range(size):
        art.append(''.join('.' if fn(x, y) else ' ' for x in range(size)))
    return outline(art)


# arrow (pointer) - drawn from geometry
def arrow_fn(x, y):
    if y < 1 or y > 17:
        return False
    if y <= 12:
        return 1 <= x <= y
    # lower part: left side and tail
    if y <= 14 and 1 <= x <= 13 - (y - 12) * 0 and x <= 12 - (y - 13) * 2 and x < 6 + 0:
        return True
    return False


def make_arrow():
    art = [[' '] * 32 for _ in range(32)]
    for y in range(1, 13):
        for x in range(1, y + 1):
            art[y][x] = '.'
    for y in range(13, 15):
        for x in range(1, 13 - (y - 12) * 3):
            if x <= 4 or x >= 6 + (y - 13):
                art[y][x] = '.'
    # tail going down-right
    for y in range(12, 20):
        x0 = 5 + (y - 12) // 2
        for x in range(x0, x0 + 3):
            art[y][x] = '.'
    art = [''.join(r) for r in art]
    return outline(art)


cursors = {}
def classic_arrow():
    """standard pointer: 45 degree head, notch and a two pixel wide tail"""
    return ['#',
            '##',
            '#.#',
            '#..#',
            '#...#',
            '#....#',
            '#.....#',
            '#......#',
            '#.......#',
            '#........#',
            '#.....#####',
            '#..#..#',
            '#.# #..#',
            '##  #..#',
            '#    #..#',
            '     #..#',
            '      ##']


cursors = {}
cursors[32512] = cursor(classic_arrow(), 0, 0)

# I-beam: inverting, so it stays visible on any background
ib = [[' '] * 32 for _ in range(32)]
for x in (12, 13, 14, 16, 17, 18):
    ib[6][x] = '~'; ib[25][x] = '~'
for y in range(7, 25):
    ib[y][15] = '~'
cursors[32513] = cursor([''.join(r) for r in ib], 15, 15)

# hourglass
hg = [[' '] * 32 for _ in range(32)]
for y in range(4, 28):
    t = y - 4
    if t < 12:
        half = 9 - (t * 8) // 12
    else:
        half = 1 + ((t - 12) * 8) // 12
    for x in range(16 - half, 16 + half + 1):
        hg[y][x] = '.'
hg = outline([''.join(r) for r in hg])
hg = [list(r) for r in hg]
for x in range(5, 28):
    hg[2][x] = '#'; hg[3][x] = '#'; hg[28][x] = '#'; hg[29][x] = '#'
# sand
for y in range(19, 27):
    t = y - 4
    half = 1 + ((t - 12) * 8) // 12 - 2
    for x in range(16 - half, 16 + half + 1):
        if half > 0:
            hg[y][x] = '#'
for y in range(7, 11):
    half = 9 - ((y - 4) * 8) // 12 - 2
    for x in range(16 - half, 16 + half + 1):
        if half > 0:
            hg[y][x] = '#'
hg[15][16] = '#'; hg[16][16] = '#'
cursors[32514] = cursor([''.join(r) for r in hg], 16, 16)

# crosshair
cr = [[' '] * 32 for _ in range(32)]
for i in range(4, 29):
    if not 14 <= i <= 18:
        cr[16][i] = '#'
        cr[i][16] = '#'
cursors[32515] = cursor([''.join(r) for r in cr], 16, 16)

# up arrow
ua = [[' '] * 32 for _ in range(32)]
for y in range(2, 12):
    for x in range(16 - (y - 2), 16 + (y - 2) + 1):
        ua[y][x] = '.'
for y in range(12, 28):
    for x in range(13, 20):
        ua[y][x] = '.'
cursors[32516] = cursor(outline([''.join(r) for r in ua]), 16, 2)


def arrow_poly(ux, uy, L=11.0, hl=5.0, hw=4.6, sw=1.0):
    """double headed arrow polygon around (16,16) along unit vector (ux,uy)"""
    n = math.hypot(ux, uy); ux, uy = ux / n, uy / n
    vx, vy = -uy, ux
    pts_uv = [(L, 0), (L - hl, hw), (L - hl, sw), (-(L - hl), sw), (-(L - hl), hw), (-L, 0),
              (-(L - hl), -hw), (-(L - hl), -sw), (L - hl, -sw), (L - hl, -hw)]
    return [(16 + a * ux + b * vx, 16 + a * uy + b * vy) for a, b in pts_uv]


def double_arrow(dx, dy, four=False):
    cv = Canvas(32, 32, 0)
    if four:
        cv.poly(arrow_poly(1, 0), 1)
        cv.poly(arrow_poly(0, 1), 1)
    else:
        cv.poly(arrow_poly(dx, dy, L=11.5 if dx and dy else 11.0), 1)
    art = [''.join('.' if cv.p[y][x] else ' ' for x in range(32)) for y in range(32)]
    return cursor(outline(art), 16, 16)


cursors[32640] = double_arrow(1, 0, four=True)       # SIZE
cursors[32642] = double_arrow(1, 1)                  # SIZENWSE
cursors[32643] = double_arrow(1, -1)                 # SIZENESW
cursors[32644] = double_arrow(1, 0)                  # SIZEWE
cursors[32645] = double_arrow(0, 1)                  # SIZENS
cursors[32646] = double_arrow(1, 0, four=True)       # SIZEALL (NT)
# NO: circle with a slash (NT)
cv = Canvas(32, 32, 0)
for y in range(32):
    for x in range(32):
        r = math.hypot(x - 15.5, y - 15.5)
        on_ring = 8.0 <= r <= 11.5
        d = abs((x - 15.5) - (y - 15.5)) / math.sqrt(2)
        on_bar = d <= 1.8 and r <= 10.0
        if on_ring or on_bar:
            cv.set(x, y, 1)
art = [''.join('.' if cv.p[y][x] else ' ' for x in range(32)) for y in range(32)]
cursors[32648] = cursor(outline(art), 16, 16)
# icon cursor: empty framed square
ic = [[' '] * 32 for _ in range(32)]
for y in range(4, 28):
    for x in range(4, 28):
        ic[y][x] = '.' if 6 <= x <= 25 and 6 <= y <= 25 else '#'
cursors[32641] = cursor([''.join(r) for r in ic], 16, 16)

for rid, (data, hx, hy) in cursors.items():
    write_cur(f'c{rid}.cur', hx, hy, data)

# ---------------------------------------------------------------------------
# icons 32x32 16 colors + mono variant

def icon(cv, mask):
    """cv: Canvas with colors; mask: Canvas (1 = transparent)"""
    col = icon_image(cv, mask, 4)
    mono = Canvas(32, 32, 0)
    for y in range(32):
        for x in range(32):
            r, g, b = PAL16[cv.p[y][x]]
            mono.set(x, y, 1 if r + g + b >= 384 else 0)
    mon = icon_image(mono, mask, 1)
    return [(32, 32, 16, 4, col), (32, 32, 2, 1, mon)]


def circle_icon(fill, glyph_art, glyph_color, edge=K):
    cv = Canvas(32, 32, 0)
    mask = Canvas(32, 32, 1)
    for y in range(32):
        for x in range(32):
            d = (x + 0.5 - 16) ** 2 + (y + 0.5 - 16) ** 2
            if d <= 14.5 ** 2:
                mask.set(x, y, 0)
                cv.set(x, y, edge if d > 13.4 ** 2 else fill)
    cv.text(glyph_art, 16 - len(glyph_art[0]) // 2, 16 - len(glyph_art) // 2, {'#': glyph_color})
    return cv, mask


icons = {}
# application (sample): little window
cv = Canvas(32, 32, 0); mask = Canvas(32, 32, 1)
mask.rect(2, 3, 29, 28, 0)
cv.rect(2, 3, 29, 28, W)
cv.frame(2, 3, 29, 28, K)
cv.rect(3, 4, 28, 8, DB)
cv.hline(3, 28, 9, K)
cv.rect(4, 5, 7, 7, LGRAY)
cv.rect(6, 12, 25, 13, DGRAY); cv.rect(6, 16, 21, 17, DGRAY); cv.rect(6, 20, 23, 21, DGRAY)
icons[32512] = icon(cv, mask)

X_ART = ['##     ##', '###   ###', ' ### ### ', '  #####  ', '   ###   ', '  #####  ', ' ### ### ',
         '###   ###', '##     ##']
Q_ART = [' ##### ', '##   ##', '##   ##', '    ## ', '   ##  ', '   ##  ', '   ##  ', '       ', '   ##  ',
         '   ##  ']
I_ART = ['  ##  ', '  ##  ', '      ', ' ###  ', '  ##  ', '  ##  ', '  ##  ', '  ##  ', ' #### ']
icons[32513] = icon(*circle_icon(R, X_ART, W))          # hand / stop
icons[32514] = icon(*circle_icon(C, Q_ART, DB))         # question
icons[32516] = icon(*circle_icon(B, I_ART, W))          # note / information
# exclamation: yellow triangle
cv = Canvas(32, 32, 0); mask = Canvas(32, 32, 1)
pts = [(16, 1.5), (30.5, 29.5), (1.5, 29.5)]
mask.poly(pts, 0)
cv.poly(pts, K)
cv.poly([(16, 4.5), (28, 27.8), (4, 27.8)], Y)
cv.rect(15, 10, 17, 21, K)
cv.rect(15, 23, 17, 25, K)
icons[32515] = icon(cv, mask)

for rid, imgs in icons.items():
    write_ico(f'i{rid}.ico', imgs)

# ---------------------------------------------------------------------------
# OEM binary resources

def rgb(r, g, b):
    return bytes((r, g, b, 0))

# config: metrics + default system colors (Windows 3.0 layout: 9 words + 22 colors)
config = struct.pack('<9H', 17, 17, 2, 2, 1, 1, 0, 1, 1)
# Windows 3.0 scheme of the 3.0 256-color drivers (sky blue captions, using
# the static palette colours 166,202,240 and 160,160,164). On Windows 3.1 the
# driver replaces it at load time with the 3.1 VGA scheme (see enable.c).
colors = [rgb(192, 192, 192),   # scrollbar
          rgb(160, 160, 164),   # desktop
          rgb(166, 202, 240),   # active caption
          rgb(255, 255, 255),   # inactive caption
          rgb(255, 255, 255),   # menu
          rgb(255, 255, 255),   # window
          rgb(0, 0, 0),         # window frame
          rgb(0, 0, 0),         # menu text
          rgb(0, 0, 0),         # window text
          rgb(0, 0, 0),         # caption text
          rgb(160, 160, 164),   # active border
          rgb(255, 255, 255),   # inactive border
          rgb(255, 255, 255),   # app workspace
          rgb(166, 202, 240),   # highlight
          rgb(0, 0, 0),         # highlight text
          rgb(192, 192, 192),   # button face
          rgb(128, 128, 128),   # button shadow
          rgb(192, 192, 192),   # gray text
          rgb(0, 0, 0)]         # button text
config += b''.join(colors)
config = config.ljust(0x70, b'\0')
open(os.path.join(OUT, 'oem1.bin'), 'wb').write(config)

# control panel color table (48 colors)
ctab = [(255, 128, 128), (255, 255, 232), (128, 255, 128), (0, 255, 128), (128, 255, 255), (0, 128, 255),
        (255, 128, 192), (255, 128, 255), (255, 0, 0), (255, 255, 128), (128, 255, 0), (0, 255, 64),
        (0, 255, 255), (0, 128, 192), (128, 128, 192), (255, 0, 255), (128, 64, 64), (255, 255, 0),
        (0, 255, 0), (0, 128, 128), (0, 64, 128), (128, 128, 255), (128, 0, 64), (255, 0, 128),
        (128, 0, 0), (255, 128, 0), (0, 128, 0), (0, 128, 64), (0, 0, 255), (0, 0, 160), (128, 0, 128),
        (128, 0, 255), (64, 0, 0), (128, 64, 0), (0, 64, 0), (0, 64, 64), (0, 0, 128), (0, 0, 64),
        (64, 0, 64), (64, 0, 128), (0, 0, 0), (128, 128, 0), (128, 128, 64), (128, 128, 128),
        (64, 128, 128), (192, 192, 192), (130, 130, 130), (255, 255, 255)]
open(os.path.join(OUT, 'oem2.bin'), 'wb').write(struct.pack('<H', len(ctab)) + b''.join(rgb(*c) for c in ctab))


# Windows NT 3.x display driver RCDATA 1 / 2: same tables, WORD per colour
# component. NT 3.1 USER takes the default system colours (Windows 3.1 VGA
# scheme) and the colour dialog table from here; NT 3.5 - XP only read the
# first 9 words.
nt_colors = [(192, 192, 192), (192, 192, 192), (0, 0, 128), (255, 255, 255), (255, 255, 255),
             (255, 255, 255), (0, 0, 0), (0, 0, 0), (0, 0, 0), (255, 255, 255), (192, 192, 192),
             (192, 192, 192), (255, 255, 255), (0, 0, 128), (255, 255, 255), (192, 192, 192),
             (128, 128, 128), (192, 192, 192), (0, 0, 0)]
nt1 = struct.pack('<9H', 17, 17, 2, 2, 1, 1, 0, 1, 1)
nt1 += b''.join(struct.pack('<3H', *c) for c in nt_colors)
open(os.path.join(OUT, 'nt1.bin'), 'wb').write(nt1.ljust(136, b'\0'))
nt2 = struct.pack('<H', len(ctab)) + b''.join(struct.pack('<3H', *c) for c in ctab)
open(os.path.join(OUT, 'nt2.bin'), 'wb').write(nt2.ljust(296, b'\0'))


def logfont(h, w, charset, pitch, face):
    return struct.pack('<5h8B', h, w, 0, 0, 0, 0, 0, 0, charset, 0, 2, 2, pitch) + face.encode() + b'\0'

fonts = logfont(12, 8, 255, 1, 'Terminal') + logfont(12, 9, 0, 1, 'Courier') + logfont(12, 9, 0, 2, 'Helv')
open(os.path.join(OUT, 'oem3.bin'), 'wb').write(fonts.ljust(0x50, b'\0'))

# name table (Windows 3.0 RT_NAMETABLE): maps "OEMBIN" type names to ordinals
def ntentry(typ, rid, tname, rname):
    body = struct.pack('<HH', typ, rid) + tname.encode() + b'\0' + rname.encode() + b'\0'
    return struct.pack('<H', len(body) + 2) + body

nt = ntentry(0xF006, 0x0001, 'OEMBIN', '') + ntentry(0xF006, 0x8003, 'OEMBIN', 'FONTS') + \
     ntentry(0xF006, 0x0002, 'OEMBIN', '')
open(os.path.join(OUT, 'nametab.bin'), 'wb').write(nt.ljust(0x40, b'\0'))

# ---------------------------------------------------------------------------
rc = []
for rid in sorted(bitmaps):
    rc.append(f'{rid} BITMAP b{rid}.bmp')
for rid in sorted(cursors):
    rc.append(f'{rid} CURSOR c{rid}.cur')
for rid in sorted(icons):
    rc.append(f'{rid} ICON i{rid}.ico')
rc.append('1 28678 PRELOAD FIXED oem1.bin')
rc.append('2 28678 oem2.bin')
rc.append('3 28678 PRELOAD oem3.bin')
rc.append('1 15 nametab.bin')
rc.append('1 VERSIONINFO_PLACEHOLDER')
open(os.path.join(OUT, 'winvbe.rc'), 'w').write('\n'.join(r for r in rc if 'PLACEHOLDER' not in r) + '\n')
# Windows NT 3.x display DLL (WINVBE.DLL of I386\NT3X): bitmaps, cursors,
# icons and the NT RCDATA tables
nt = [f'{rid} BITMAP "b{rid}.bmp"' for rid in sorted(bitmaps)]
nt += [f'{rid} CURSOR "c{rid}.cur"' for rid in sorted(cursors)]
nt += [f'{rid} ICON "i{rid}.ico"' for rid in sorted(icons)]
nt += ['1 RCDATA "nt1.bin"', '2 RCDATA "nt2.bin"']
open(os.path.join(OUT, 'nt3.rc'), 'w').write('\n'.join(nt) + '\n')
print('generated', len(bitmaps), 'bitmaps', len(cursors), 'cursors', len(icons), 'icons')

# ---------------------------------------------------------------------------
# application icon for WINVBECF.EXE: a monitor in the Windows 95 style with
# a sky gradient, a window and colour swatches on the screen, in 32x32 and
# 16x16, 16 and 256 colours (+ monochrome)

def sky(t, vga):
    """screen background at height t (0 top .. 1 bottom); vga: VGA colours only"""
    if vga:
        return (0, 0, 128) if t < 0.4 else (0, 0, 255) if t < 0.8 else (0, 128, 128)
    return (int(0 + 40 * t), int(40 + 150 * t), int(150 + 105 * t))


def app_icon(n, vga=False):
    """rows of RGB tuples (None = transparent) for an n x n icon (n = 32 or 16)"""
    T = None
    px = [[T] * n for _ in range(n)]

    def put(x, y, c):
        if 0 <= x < n and 0 <= y < n:
            px[y][x] = c

    def rect(x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                put(x, y, c)

    def bevel(x0, y0, x1, y1, face, hi, sh, out):
        rect(x0, y0, x1, y1, face)
        for x in range(x0, x1 + 1):
            put(x, y0, hi); put(x, y1, sh)
        for y in range(y0, y1 + 1):
            put(x0, y, hi); put(x1, y, sh)
        for x in range(x0 - 1, x1 + 2):
            put(x, y0 - 1, out); put(x, y1 + 1, out)
        for y in range(y0 - 1, y1 + 2):
            put(x0 - 1, y, out); put(x1 + 1, y, out)

    BODY, HI, SH, OUT = (200, 200, 196), (255, 255, 255), (120, 120, 120), (0, 0, 0)
    if n == 32:
        bevel(2, 2, 29, 22, BODY, HI, SH, OUT)
        put(1, 1, T); put(30, 1, T); put(1, 23, T); put(30, 23, T)        # rounded corners
        sx0, sy0, sx1, sy1 = 5, 5, 26, 18
        rect(sx0 - 1, sy0 - 1, sx1 + 1, sy1 + 1, (128, 128, 128) if vga else (64, 64, 64))
        for y in range(sy0, sy1 + 1):                                       # sky gradient
            c = sky((y - sy0) / (sy1 - sy0), vga)
            for x in range(sx0, sx1 + 1):
                put(x, y, c)
        # a window on the desktop
        rect(8, 7, 19, 15, (255, 255, 255))
        rect(8, 7, 19, 8, (0, 0, 160))
        put(18, 7, (192, 192, 192)); put(18, 8, (192, 192, 192))
        for x in range(8, 20):
            put(x, 16, (0, 0, 0))
        for y in range(7, 17):
            put(20, y, (0, 0, 0))
        for i, c in enumerate([(255, 0, 0), (255, 255, 0) if vga else (255, 200, 0), (0, 255, 0) if vga else (0, 200, 0), (0, 255, 255) if vga else (0, 160, 255)]):
            rect(10 + i * 2, 10, 11 + i * 2, 13, c)
        # colour swatches in the corner (resolution / colour settings)
        for i, c in enumerate([(255, 0, 0), (255, 255, 0), (0, 255, 0), (0, 255, 255), (255, 0, 255)]):
            rect(22, 7 + i * 2, 24, 8 + i * 2, c)
        put(26, 20, (0, 220, 0)); put(25, 20, (0, 140, 0))                 # power LED
        for x in range(5, 12):
            put(x, 20, (150, 150, 146))
        # stand
        bevel(13, 24, 18, 25, BODY, HI, SH, OUT)
        bevel(8, 27, 23, 28, BODY, HI, SH, OUT)
        put(7, 26, T); put(24, 26, T); put(7, 29, T); put(24, 29, T)
        for y in range(23, 26):
            put(12, y, OUT); put(19, y, OUT)
    else:
        bevel(1, 1, 14, 11, BODY, HI, SH, OUT)
        put(0, 0, T); put(15, 0, T); put(0, 12, T); put(15, 12, T)
        sx0, sy0, sx1, sy1 = 3, 3, 12, 9
        for y in range(sy0, sy1 + 1):
            c = sky((y - sy0) / (sy1 - sy0), vga)
            for x in range(sx0, sx1 + 1):
                put(x, y, c)
        rect(4, 4, 9, 8, (255, 255, 255))
        rect(4, 4, 9, 4, (0, 0, 160))
        put(5, 6, (255, 0, 0)); put(6, 6, (255, 255, 0) if vga else (255, 200, 0)); put(7, 6, (0, 255, 0) if vga else (0, 200, 0)); put(8, 6, (0, 255, 255) if vga else (0, 160, 255))
        put(11, 4, (255, 0, 0)); put(11, 5, (255, 255, 0)); put(11, 6, (0, 255, 0)); put(11, 7, (0, 255, 255))
        put(13, 10, (0, 220, 0))
        bevel(6, 13, 9, 13, BODY, HI, SH, OUT)
        bevel(4, 14, 11, 14, BODY, HI, SH, OUT)
        put(3, 13, T); put(12, 13, T); put(3, 15, T); put(12, 15, T)
    return px


BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


def _near16(r, g, b):
    best, bd = 0, None
    for i, (pr, pg, pb) in enumerate(PAL16):
        d = (r - pr) ** 2 * 3 + (g - pg) ** 2 * 4 + (b - pb) ** 2 * 2
        if bd is None or d < bd:
            best, bd = i, d
    return best, bd


def nearest16(c, x, y):
    # colours close to a VGA colour are taken as they are; the rest (the
    # sky gradient) gets an ordered dither between the two nearest
    best, bd = _near16(*c)
    if bd <= 9 * 40 * 40:
        return best
    t = (BAYER[y & 3][x & 3] - 7.5) * 8
    return _near16(*(min(255, max(0, v + t)) for v in c))[0]


def ico_dib(px, bits):
    n = len(px)
    androws = []
    for y in range(n - 1, -1, -1):
        row = bytearray((n + 31) // 32 * 4)
        for x in range(n):
            if px[y][x] is None:
                row[x >> 3] |= 0x80 >> (x & 7)
        androws.append(bytes(row))
    rows = []
    if bits == 8:
        cols = []
        for r in px:
            for c in r:
                if c is not None and c not in cols:
                    cols.append(c)
        cols = [(0, 0, 0)] + [c for c in cols if c != (0, 0, 0)]
        assert len(cols) <= 256
        pal = b''.join(bytes((b, g, r, 0)) for r, g, b in cols) + bytes(4 * (256 - len(cols)))
        for y in range(n - 1, -1, -1):
            row = bytearray(cols.index(px[y][x]) if px[y][x] is not None else 0 for x in range(n))
            while len(row) % 4:
                row.append(0)
            rows.append(bytes(row))
        ncol = 0
    elif bits == 4:
        pal = b''.join(bytes((b, g, r, 0)) for r, g, b in PAL16)
        for y in range(n - 1, -1, -1):
            row = bytearray()
            for x in range(0, n, 2):
                a = nearest16(px[y][x], x, y) if px[y][x] is not None else 0
                b = nearest16(px[y][x + 1], x + 1, y) if px[y][x + 1] is not None else 0
                row.append((a << 4) | b)
            while len(row) % 4:
                row.append(0)
            rows.append(bytes(row))
        ncol = 16
    else:
        pal = bytes((0, 0, 0, 0, 255, 255, 255, 0))
        for y in range(n - 1, -1, -1):
            row = bytearray((n + 31) // 32 * 4)
            for x in range(n):
                c = px[y][x]
                if c is not None and sum(c) >= 384:
                    row[x >> 3] |= 0x80 >> (x & 7)
            rows.append(bytes(row))
        ncol = 2
    xd, ad = b''.join(rows), b''.join(androws)
    hdr = struct.pack('<IiiHHIIiiII', 40, n, n * 2, 1, bits, 0, len(xd) + len(ad), 0, 0, 0, 0)
    return (n, n, ncol, bits, hdr + pal + xd + ad)


i32, i16, v32, v16 = app_icon(32), app_icon(16), app_icon(32, True), app_icon(16, True)
# 16 colours first: Windows 3.0 takes the first image that fits the display
write_ico('winvbecf.ico', [ico_dib(v32, 4), ico_dib(v16, 4), ico_dib(i32, 8), ico_dib(i16, 8), ico_dib(v32, 1)])
