#include "drv.h"
#include <stdlib.h>
WORD host_alloc(DWORD size, BYTE **out);
WORD EXPORT DeviceBitmapBits(LPGBITMAP bm, WORD fGet, WORD start, WORD count, LPBYTE bits, LPBYTE bmi, LPDRAWMODE dm, LPWORD xlat);
WORD EXPORT BitBlt(LPVOID lpDst, SHORT dx, SHORT dy, LPVOID lpSrc, SHORT sx, SHORT sy, SHORT w, SHORT h, DWORD rop, LPPBRUSH br, LPDRAWMODE dm);
DWORD EXPORT ExtTextOut(LPVOID lpDev, SHORT x, SHORT y, LPGRECT clip, LPSTR str, SHORT count, LPFONTINFO font, LPDRAWMODE dm, LPTEXTXFORM xf, LPWORD dx, LPGRECT opq, WORD opts);
SHORT EXPORT RealizeObject(LPVOID lpDev, SHORT style, LPVOID lpIn, LPVOID lpOut, LPTEXTXFORM lpXform);
WORD EXPORT Output(LPVOID lpDev, WORD style, WORD count, LPGPOINT pts, LPPPEN pen, LPPBRUSH br, LPDRAWMODE dm, LPGRECT clip);

static BYTE *fb;
static PDEVICE pdev;

static void set_mode(WORD w, WORD h, BYTE bpp)
{
    memset(&g, 0, sizeof(g));
    g.mi.xres = w; g.mi.yres = h; g.mi.bpp = bpp; g.mi.Bpp = (bpp + 7) / 8;
    g.mi.pf = bpp == 8 ? PF_8 : bpp == 15 ? PF_555 : bpp == 16 ? PF_565 : bpp == 24 ? PF_24 : PF_32;
    if (bpp == 15) { g.mi.rsize = 5; g.mi.rpos = 10; g.mi.gsize = 5; g.mi.gpos = 5; g.mi.bsize = 5; }
    if (bpp == 16) { g.mi.rsize = 5; g.mi.rpos = 11; g.mi.gsize = 6; g.mi.gpos = 5; g.mi.bsize = 5; }
    if (bpp >= 24) { g.mi.rsize = 8; g.mi.rpos = 16; g.mi.gsize = 8; g.mi.gpos = 8; g.mi.bsize = 8; }
    g.xres = w; g.yres = h; g.bpp = bpp; g.Bpp = g.mi.Bpp; g.pf = g.mi.pf;
    g.pitch = w * g.Bpp; g.palettized = g.Bpp == 1;
    g.fbsize = (DWORD)g.pitch * h;
    g.access = ACC_LFB; g.lfbsel = host_alloc(g.fbsize, &fb); g.lfbinc = 8; g.ahincr = 8;
    g.enabled = 1;
    palette_init();
    memset(&pdev, 0, sizeof pdev);
    pdev.bm.bmType = PDEV_TYPE; pdev.bm.bmWidth = w; pdev.bm.bmHeight = h;
    pdev.bm.bmBitsPixel = g.Bpp == 2 ? 16 : g.Bpp * 8; pdev.bm.bmPlanes = 1;
}

static void save_ppm(const char *fn)
{
    FILE *f = fopen(fn, "wb");
    WORD x, y;
    fprintf(f, "P6 %u %u 255\n", g.xres, g.yres);
    for (y = 0; y < g.yres; y++) for (x = 0; x < g.xres; x++) {
        DWORD v = pix_get(fb + (DWORD)y * g.pitch + x * g.Bpp, g.Bpp);
        DWORD rgb = phys_to_rgb(v);
        fputc(rgb & 255, f); fputc((rgb >> 8) & 255, f); fputc((rgb >> 16) & 255, f);
    }
    fclose(f);
}

static BYTE *readfile(const char *fn, long *len)
{
    FILE *f = fopen(fn, "rb"); BYTE *p; long n;
    if (!f) { perror(fn); exit(1); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc(n); fread(p, 1, n, f); fclose(f);
    if (len) *len = n;
    return p;
}

/* make a memory bitmap */
static void mkbm(GBITMAP *bm, WORD w, WORD h, BYTE bits)
{
    BYTE *p;
    memset(bm, 0, sizeof *bm);
    bm->bmWidth = w; bm->bmHeight = h; bm->bmPlanes = 1; bm->bmBitsPixel = bits;
    bm->bmWidthBytes = ((w * bits + 15) / 16) * 2;
    host_alloc((DWORD)bm->bmWidthBytes * h, &p);
    bm->bmBits = p;
}

static void dib_test(const char *fn, SHORT x, SHORT y)
{
    long n; BYTE *d = readfile(fn, &n);
    if (!d) return;
    if (d[0] == 'B' && d[1] == 'M') { d += 14; n -= 14; }          /* .bmp file */
    else if (!d[0] && !d[1] && d[2] == 1 && !d[3]) {               /* .ico: first image */
        DWORD o = d[18] | (d[19] << 8) | ((DWORD)d[20] << 16); d += o; n -= o;
    }
    DWORD hs = d[0] | (d[1] << 8); WORD w, h, bits, nc; BYTE *seg; BYTE *bits_p;
    GBITMAP bm;
    w = *(WORD *)(d + 4); h = *(WORD *)(d + 8); bits = *(WORD *)(d + 14);
    if (hs == 12) { w = *(WORD *)(d + 4); h = *(WORD *)(d + 6); bits = *(WORD *)(d + 10); }
    nc = bits <= 8 ? 1 << bits : 0;
    host_alloc(65536, &seg);
    memcpy(seg, d, n);
    bits_p = seg + hs + nc * (hs == 12 ? 3 : 4);
    mkbm(&bm, w, h, bits == 1 ? 1 : (g.Bpp == 2 ? 16 : g.Bpp * 8));
    DeviceBitmapBits(&bm, 0, 0, h, bits_p, seg, NULL, NULL);
    {
        DRAWMODE dm; memset(&dm, 0, sizeof dm);
        dm.bkColor = rgb_to_phys(0xFFFFFF); dm.TextColor = 0;
        BitBlt(&pdev, x, y, &bm, 0, 0, w, h, 0xCC0020, NULL, &dm);
    }
}

/* GetDIBits / SetDIBits in bands (start > 0): the buffer holds only the
   band - ASan catches a write or read past its end */
static void band_test(void)
{
    GBITMAP bm;
    BYTE hdr[40 + 8], *buf;
    WORD k, sel;
    LPBYTE p;
    mkbm(&bm, 128, 1088, 1);
    for (k = 0; k < bm.bmWidthBytes * 1088u; k++) bm.bmBits[k] = (BYTE)(k / 16);
    memset(hdr, 0, sizeof hdr);
    *(DWORD *)hdr = 40; *(LONG *)(hdr + 4) = 128; *(LONG *)(hdr + 8) = 1088;
    *(WORD *)(hdr + 12) = 1; *(WORD *)(hdr + 14) = 1;
    /* exactly 256 scans at the end of a 64 KB tile: going past it means
       the next (unallocated) selector - "bad selector" */
    host_alloc(65536, &buf);
    buf += 65536 - 16 * 256;
    DeviceBitmapBits(&bm, 1, 256, 256, buf, hdr, NULL, NULL);
    /* scan 256 (bottom-up) = bitmap row 1088-1-256 */
    if (buf[0] != (BYTE)((1088 - 1 - 256) * 16 / 16)) { fprintf(stderr, "band get: %x\n", buf[0]); exit(1); }
    DeviceBitmapBits(&bm, 0, 512, 256, buf, hdr, NULL, NULL);
    (void)sel; (void)p;
    fprintf(stderr, "band test ok\n");
}

int main(int argc, char **argv)
{
    int bpp = argc > 1 ? atoi(argv[1]) : 8;
    char fn[256];
    int i, x = 10;
    static const int ids[] = {32754, 32753, 32752, 32747, 32748, 32749, 32760, 32759, 32767, 32766};
    set_mode(640, 480, bpp);
    band_test();
    /* background fill */
    {
        LOGBRUSH lb = { BS_SOLID, 0xC0C0C0, 0, 0 }; PBRUSH pb; DRAWMODE dm;
        memset(&dm, 0, sizeof dm); dm.Rop2 = 13;
        RealizeObject(&pdev, OBJ_BRUSH, &lb, &pb, NULL);
        BitBlt(&pdev, 0, 0, NULL, 0, 0, 640, 480, 0xF00021, &pb, &dm);
        lb.lbStyle = BS_HATCHED; lb.lbColor = 0xFF0000; lb.lbHatch = 5; lb.lbBkColor = 0xFFFFFF;
        RealizeObject(&pdev, OBJ_BRUSH, &lb, &pb, NULL);
        BitBlt(&pdev, 400, 300, NULL, 0, 0, 100, 60, 0xF00021, &pb, &dm);
    }
    for (i = 0; i < 10; i++) {
        sprintf(fn, "../res/b%d.bmp", ids[i]);
        dib_test(fn, x, 10);
        x += 60;
    }
    dib_test("../res/i32512.ico", 10, 100);
    save_ppm("out.ppm");
    return 0;
}
