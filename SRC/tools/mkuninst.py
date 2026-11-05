#!/usr/bin/env python3
"""mkuninst.py <in.ico> <out.ico> : the icon with a red X in its lower right
quarter (white edge), for "Uninstall WINVBE". All images of the icon file
(1, 4 and 8 bits per pixel) are changed; 1 bpp images get a black X."""
import struct, sys

def nearest(pal, rgb):
    best, bi = None, 0
    for i, (b, g, r) in enumerate(pal):
        d = (r - rgb[0]) ** 2 + (g - rgb[1]) ** 2 + (b - rgb[2]) ** 2
        if best is None or d < best:
            best, bi = d, i
    return bi

def edit(img):
    hs, w, h2, planes, bpp = struct.unpack('<IiiHH', img[:16])
    h = h2 // 2
    ncol = struct.unpack('<I', img[32:36])[0] or (1 << bpp)
    pal = [tuple(img[hs + 4 * i:hs + 4 * i + 3]) for i in range(ncol)]
    xo = hs + 4 * ncol
    xs = ((w * bpp + 31) // 32) * 4
    ao = xo + xs * h
    ast = ((w + 31) // 32) * 4
    b = bytearray(img)
    red = nearest(pal, (255, 0, 0)) if bpp > 1 else 0
    white = nearest(pal, (255, 255, 255)) if bpp > 1 else 1

    def put(x, y, c):
        if not (0 <= x < w and 0 <= y < h):
            return
        row = h - 1 - y                         # bottom-up
        if bpp == 8:
            b[xo + row * xs + x] = c
        elif bpp == 4:
            o = xo + row * xs + x // 2
            b[o] = (b[o] & 0x0F) | (c << 4) if x % 2 == 0 else (b[o] & 0xF0) | c
        else:
            o = xo + row * xs + x // 8
            m = 0x80 >> (x % 8)
            b[o] = (b[o] | m) if c else (b[o] & ~m)
        o = ao + row * ast + x // 8              # opaque
        b[o] &= ~(0x80 >> (x % 8)) & 0xFF

    n = w // 2                                  # the X: lower right quarter
    x0, y0 = w - n, h - n
    t = 3 if w >= 32 else 2
    for pass_, (col, extra) in enumerate(((white, 1), (red, 0))):
        for i in range(n):
            for k in range(-(t // 2) - extra, t - t // 2 + extra):
                for (x, y) in ((x0 + i + k, y0 + i), (x0 + n - 1 - i + k, y0 + i)):
                    if x0 - extra <= x < w and y0 - extra <= y < h:
                        put(x, y, col)
    return bytes(b)

d = open(sys.argv[1], 'rb').read()
n = struct.unpack('<HHH', d[:6])[2]
out = bytearray(d)
for i in range(n):
    sz, off = struct.unpack('<II', d[6 + 16 * i + 8:6 + 16 * i + 16])
    out[off:off + sz] = edit(d[off:off + sz])
open(sys.argv[2], 'wb').write(out)
