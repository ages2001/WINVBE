/*
 * 915PATCH - Intel video BIOS mode table patcher, hardware independent part.
 * Works on a video BIOS image in memory; see i915core.h.
 */
#include <string.h>
#include "i915core.h"

#define MODE_SZ   5         /* mode, bpp, resolution offset (word), unknown */
#define RES1_SZ   8         /* clock(w) x1 xtot x2 y1 ytot y2 */
#define ML2_SZ    28        /* clock(dw) + 12 words */
#define ML3_SZ    38        /* type 2 modeline + timing_h, timing_v, 6 bytes */
#define RES23_HDR 6
#define DTD_SZ    18        /* EDID detailed timing descriptor */

static u16 rd16(u8 FARP *p) { return (u16)(p[0] | (p[1] << 8)); }
static void wr16(u8 FARP *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 FARP *p, u32 v) { wr16(p, (u16)v); wr16(p + 2, (u16)(v >> 16)); }

const char *vb_type_name(int t)
{
    switch (t) {
    case BT_1: return "type 1 (830..865G, GM965)";
    case BT_2: return "type 2 (915G/915GM/945G/945GM)";
    case BT_3: return "type 3 (946GZ/965/G3x/G4x)";
    case BT_4: return "type 4 (GM45/GL40/GS45)";
    }
    return "unknown";
}

/* the mode table: four consecutive entries with Intel mode numbers 3xh */
static int find_from(VBMAP *m, u32 start)
{
    u32 p;
    m->table = 0;
    m->count = 0;
    if (m->size < 0x200) return 0;
    for (p = start; p + 4 * MODE_SZ < m->size; p++) {
        u8 FARP *e = m->bios + p;
        if ((e[0] & 0xF0) == 0x30 && (e[5] & 0xF0) == 0x30 &&
            (e[10] & 0xF0) == 0x30 && (e[15] & 0xF0) == 0x30) {
            u32 q = p;
            u16 n = 0;
            while (q + MODE_SZ <= m->size && m->bios[q] != 0xFF && n < 255) {
                u16 r = rd16(m->bios + q + 2);
                if (r < 0x40 || r >= m->size) break;     /* not a resolution pointer */
                q += MODE_SZ;
                n++;
            }
            if (n >= 4 && q < m->size && m->bios[q] == 0xFF) {
                m->table = (u16)p;
                m->count = n;
                return 1;
            }
        }
    }
    return 0;
}

int vb_find_table(VBMAP *m)
{
    return find_from(m, 0x40);
}

static u32 rd32(u8 FARP *p) { return rd16(p) | ((u32)rd16(p + 2) << 16); }

/* a resolution a video BIOS mode can have */
static int plausible(u16 x, u16 y)
{
    return x >= 320 && x <= 4096 && y >= 200 && y <= 4096 && x >= y && x <= 3 * y;
}

static u16 res_of(VBMAP *m, u16 i)
{
    return rd16(m->bios + m->table + i * MODE_SZ + 2);
}

static int res_ok(VBMAP *m, u16 r, u16 need)
{
    return (u32)r + need <= m->size;
}

/* resolution of entry i decoded as layout t */
static int decode(VBMAP *m, u16 i, int t, u16 *x, u16 *y)
{
    u16 r = res_of(m, i);
    u8 FARP *b = m->bios + r;
    if (t == BT_1 || t == BT_4) {
        if (!res_ok(m, r, t == BT_1 ? RES1_SZ : DTD_SZ)) return 0;
        *x = b[2] | ((b[4] & 0xF0) << 4);
        *y = b[5] | ((b[7] & 0xF0) << 4);
    } else {
        if (!res_ok(m, r, RES23_HDR + ML2_SZ)) return 0;
        *x = rd16(b + RES23_HDR + 4) + 1;
        *y = rd16(b + RES23_HDR + 16) + 1;
    }
    return 1;
}

/* an 18 byte EDID detailed timing descriptor that makes sense: the GM45
   class BIOSes keep their mode timings in this form (type 1 is its first
   8 bytes) */
static int dtd_ok(u8 FARP *p)
{
    u16 clk = rd16(p), ha, hb, va, vb, hso, hsw, vso, vsw;
    u32 tot, hz;
    ha = p[2] | ((p[4] & 0xF0) << 4); hb = p[3] | ((p[4] & 0x0F) << 8);
    va = p[5] | ((p[7] & 0xF0) << 4); vb = p[6] | ((p[7] & 0x0F) << 8);
    hso = p[8] | ((p[11] & 0xC0) << 2); hsw = p[9] | ((p[11] & 0x30) << 4);
    vso = (p[10] >> 4) | ((p[11] & 0x0C) << 2); vsw = (p[10] & 0x0F) | ((p[11] & 0x03) << 4);
    if (clk < 1000 || clk > 60000 || !plausible(ha, va)) return 0;     /* 10 kHz units */
    if (!hb || !vb || hb > ha || vb > va || !hsw || !vsw) return 0;
    if ((u32)hso + hsw > hb || (u32)vso + vsw > vb) return 0;
    tot = (u32)(ha + hb) * (va + vb);
    hz = (u32)clk * 10000UL / tot;
    return hz >= 40 && hz <= 200;
}

/* a mode whose block is (still) empty: GM45 BIOSes have slots for modes
   that are filled in at run time (60h-71h: 18 zero bytes, only a refresh
   rate byte pair 07h 3Ch at offset 6) */
static int entry_empty(VBMAP *m, u16 i)
{
    u16 r = res_of(m, i), k;
    if (!res_ok(m, r, DTD_SZ)) return 0;
    for (k = 0; k < DTD_SZ; k++)
        if (m->bios[r + k] && k != 6 && k != 7) return 0;
    return 1;
}

/* number of entries that give a sensible resolution as layout t, empty
   slots left out; 0 if the table does not look like a mode table at all:
   the mode numbers and bits per pixel must be real ones, and a standard
   VGA/SVGA resolution (640x480, 800x600 or 1024x768) and at least two
   different sizes must be there (byte sequences elsewhere in the BIOS can
   look like a table).  *used = entries that are not empty */
static u16 score_res(VBMAP *m, int t, u16 *used)
{
    u16 i, n = 0, x, y, sane = 0, std = 0, x0 = 0, y0 = 0, diff = 0, cnt = 0;
    for (i = 0; i < m->count; i++) {
        u8 FARP *e = m->bios + m->table + i * MODE_SZ;
        u8 b = e[1];
        if (entry_empty(m, i)) continue;
        cnt++;
        if (e[0] >= 0x30 && e[0] < 0x80 && (b == 8 || b == 15 || b == 16 || b == 24 || b == 32)) sane++;
        if (!decode(m, i, t, &x, &y) || !plausible(x, y)) continue;
        if (t == BT_4 && !dtd_ok(m->bios + res_of(m, i))) continue;
        n++;
        if ((x == 640 && y == 480) || (x == 800 && y == 600) || (x == 1024 && y == 768)) std++;
        if (n == 1) { x0 = x; y0 = y; }
        else if (x != x0 || y != y0) diff = 1;
    }
    if (used) *used = cnt;
    if (cnt < 4 || (u32)sane * 4 < (u32)cnt * 3 || !std || !diff) return 0;
    return n;
}

/* layout t fits: three quarters of the (not empty) entries make sense */
static int score_ok(VBMAP *m, int t)
{
    u16 used, n = score_res(m, t, &used);
    return n && (u32)n * 4 >= (u32)used * 3;
}

/* a type 2/3 modeline: clock, then x1 htotal x2 hblank hss hse, same for y */
static int modeline_ok(u8 FARP *ml)
{
    u32 clk = rd32(ml);
    u16 x1 = rd16(ml + 4), ht = rd16(ml + 6), hss = rd16(ml + 12), hse = rd16(ml + 14);
    u16 y1 = rd16(ml + 16), vt = rd16(ml + 18), vss = rd16(ml + 24), vse = rd16(ml + 26);
    if (!plausible((u16)(x1 + 1), (u16)(y1 + 1))) return 0;
    if (clk < 2000UL || clk > 600000UL) return 0;           /* kHz or 10 kHz units */
    if (ht <= x1 || ht > 2 * x1 + 64 || vt <= y1 || vt > 2 * y1 + 64) return 0;
    if (hss < x1 || hse <= hss || hse > ht || vss < y1 || vse <= vss || vse > vt) return 0;
    return 1;
}

/* layout from the content of the resolution blocks: for BIOSes whose blocks
   are not evenly spaced (GM965, GM45 and later), where 915resolution falls
   back to type 1 */
static int detect_by_content(VBMAP *m)
{
    u16 i, n = 0, ok23 = 0, s28 = 0, s38 = 0, hdr2 = 0, x, y;
    if (score_ok(m, BT_4)) { m->type = BT_4; return 1; }
    for (i = 0; i < m->count; i++) {
        u16 r = res_of(m, i);
        u8 FARP *ml;
        if (entry_empty(m, i)) continue;
        n++;
        if (!res_ok(m, r, RES23_HDR + 2 * ML3_SZ)) continue;
        ml = m->bios + r + RES23_HDR;
        if (!modeline_ok(ml)) continue;
        ok23++;
        x = rd16(ml + 4); y = rd16(ml + 16);
        if (rd16(ml + ML2_SZ + 4) == x && rd16(ml + ML2_SZ + 16) == y) s28++;
        if (rd16(ml + ML3_SZ + 4) == x && rd16(ml + ML3_SZ + 16) == y) s38++;
        if (m->bios[r] == (u8)((x + 1) / 8) && m->bios[r + 1] == (u8)((y + 1) / 16 - 1)) hdr2++;
    }
    if (n && (u32)ok23 * 4 >= (u32)n * 3 && score_res(m, BT_3, NULL)) {
        if (s38 != s28) m->type = s38 > s28 ? BT_3 : BT_2;
        else m->type = hdr2 * 2 > ok23 ? BT_2 : BT_3;
        return 1;
    }
    if (score_ok(m, BT_1)) { m->type = BT_1; return 1; }
    return 0;
}

int vb_locate(VBMAP *m)
{
    u32 p = 0x40;
    while (find_from(m, p)) {
        if (vb_detect_type(m)) return 1;
        p = (u32)m->table + 1;
    }
    m->type = BT_UNKNOWN;
    return 0;
}

/* distance between the two lowest resolution blocks tells the layout of
   evenly spaced blocks (830 ... 965); otherwise the content decides */
int vb_detect_type(VBMAP *m)
{
    u16 i, r1 = 0xFFFF, r2 = 0xFFFF, d;
    int t = BT_UNKNOWN;
    for (i = 0; i < m->count; i++) {
        u16 r = res_of(m, i);
        if (entry_empty(m, i)) continue;
        if (r < r1) { r2 = r1; r1 = r; }
        else if (r > r1 && r < r2) r2 = r;
    }
    m->type = BT_UNKNOWN;
    if (r2 != 0xFFFF && r2 > r1) {
        d = r2 - r1;
        if (d % DTD_SZ == 0 && score_ok(m, BT_4)) { m->type = BT_4; return 1; }
        if (d > RES23_HDR && (d - RES23_HDR) % ML3_SZ == 0) t = BT_3;
        else if (d > RES23_HDR && (d - RES23_HDR) % ML2_SZ == 0) t = BT_2;
        else if (d % RES1_SZ == 0) t = BT_1;
    }
    if (t != BT_UNKNOWN && score_ok(m, t)) {
        m->type = t;
        return 1;
    }
    return detect_by_content(m);
}

/* /TYPE:n - the first table that gives sensible resolutions in layout t */
int vb_force_type(VBMAP *m, int t)
{
    u32 p = 0x40;
    if (t < BT_1 || t > BT_4) return 0;
    while (find_from(m, p)) {
        if (score_ok(m, t)) { m->type = t; return 1; }
        p = (u32)m->table + 1;
    }
    m->type = BT_UNKNOWN;
    return 0;
}

int vb_find_next(VBMAP *m, u32 start)
{
    return find_from(m, start);
}

int vb_get_mode(VBMAP *m, u16 i, VBMODE *o)
{
    u8 FARP *e;
    if (i >= m->count) return 0;
    e = m->bios + m->table + i * MODE_SZ;
    o->mode = e[0];
    o->bpp = e[1];
    o->res = rd16(e + 2);
    if (m->type == BT_UNKNOWN || entry_empty(m, i) || !decode(m, i, m->type, &o->x, &o->y))
        o->x = o->y = 0;
    return 1;
}

/* GTF timings in the form 915resolution writes them (sync/blank values
   are "end - 1" positions); integer arithmetic only */
void vb_gtf(u16 x, u16 y, u16 freq, u32 *clock, u16 *hss, u16 *hse, u16 *hbl,
            u16 *vss, u16 *vse, u16 *vbl)
{
    u32 d1000, vb, vfreq, a, b, r, hb;
    /* vbl = y + (y + 1) / (20000 / (11 * freq) - 1) + 1.5 */
    d1000 = 20000000UL / (11UL * freq) - 1000UL;
    vb = ((u32)y * 1000UL + ((u32)y + 1) * 1000000UL / d1000 + 1500UL) / 1000UL;
    vfreq = vb * freq;
    /* hbl = 16 * (int)(x * (30 - 300000/vfreq) / (70 + 300000/vfreq) / 16 + 0.5) */
    a = (30UL * vfreq - 300000UL) / 100UL;
    b = (70UL * vfreq + 300000UL) / 100UL;
    r = a * 16384UL / b;                        /* ratio in 1/16384 */
    hb = 16UL * (((u32)x * r / 16384UL + 8UL) / 16UL);
    *vss = y;
    *vse = y + 3;
    *vbl = (u16)(vb - 1);
    *hss = (u16)(x + hb / 2 - (x + hb + 50) / 100 * 8 - 1);
    *hse = (u16)(x + hb / 2 - 1);
    *hbl = (u16)(x + hb - 1);
    *clock = ((u32)x + hb) * vfreq / 1000UL;
}

int vb_set_mode(VBMAP *m, u8 mode, u16 x, u16 y, u8 bpp, u16 htotal, u16 vtotal)
{
    static const u16 freqs[3] = { 60, 75, 85 };
    u16 i, j, done = 0;
    for (i = 0; i < m->count; i++) {
        u8 FARP *e = m->bios + m->table + i * MODE_SZ;
        u8 FARP *r;
        if (e[0] != mode) continue;
        if (!res_ok(m, rd16(e + 2), m->type == BT_1 ? RES1_SZ : m->type == BT_4 ? DTD_SZ :
                    RES23_HDR + 3 * (m->type == BT_2 ? ML2_SZ : ML3_SZ))) continue;
        if (entry_empty(m, i)) continue;
        if (bpp) e[1] = bpp;
        r = m->bios + rd16(e + 2);
        if (m->type == BT_1) {
            /* as 915resolution: the size, and the blanking only when given
               (the first two bytes are not known) */
            r[2] = (u8)x;
            r[4] = (u8)((htotal ? ((htotal - x) >> 8) & 0x0F : r[4] & 0x0F) | ((x >> 4) & 0xF0));
            r[5] = (u8)y;
            r[7] = (u8)((vtotal ? ((vtotal - y) >> 8) & 0x0F : r[7] & 0x0F) | ((y >> 4) & 0xF0));
            if (htotal) r[3] = (u8)(htotal - x);
            if (vtotal) r[6] = (u8)(vtotal - y);
        } else if (m->type == BT_2 || m->type == BT_3) {
            u16 sz = m->type == BT_2 ? ML2_SZ : ML3_SZ;
            u8 FARP *ml0 = r + RES23_HDR;
            u16 xprev = rd16(ml0 + 4), yprev = rd16(ml0 + 16);
            if (m->type == BT_2) {
                r[0] = (u8)(x / 8);
                r[1] = (u8)(y / 16 - 1);
            }
            for (j = 0; j < 3; j++) {
                u8 FARP *ml = ml0 + j * sz;
                u32 clk;
                u16 hss, hse, hbl, vss, vse, vbl;
                if (rd16(ml + 4) != xprev || rd16(ml + 16) != yprev) continue;
                vb_gtf(x, y, freqs[j], &clk, &hss, &hse, &hbl, &vss, &vse, &vbl);
                wr32(ml, clk);
                wr16(ml + 4, (u16)(x - 1));                     /* x1 */
                wr16(ml + 6, htotal ? htotal : hbl);            /* htotal */
                wr16(ml + 8, (u16)(x - 1));                     /* x2 */
                wr16(ml + 10, hbl);
                wr16(ml + 12, hss);
                wr16(ml + 14, hse);
                wr16(ml + 16, (u16)(y - 1));                    /* y1 */
                wr16(ml + 18, vtotal ? vtotal : vbl);           /* vtotal */
                wr16(ml + 20, (u16)(y - 1));                    /* y2 */
                wr16(ml + 22, vbl);
                wr16(ml + 24, vss);
                wr16(ml + 26, vse);
                if (m->type == BT_3) {
                    wr16(ml + 28, (u16)(y - 1));                /* timing_h */
                    wr16(ml + 30, (u16)(x - 1));                /* timing_v */
                }
            }
        } else if (m->type == BT_4) {
            /* every EDID timing of the block with the old size (GM45: one
               per refresh rate 60/75/85 Hz) gets the new size with GTF
               timings; image size and flags are kept */
            u8 FARP *d0 = r;
            u16 xprev = d0[2] | ((d0[4] & 0xF0) << 4), yprev = d0[5] | ((d0[7] & 0xF0) << 4);
            for (j = 0; j < 3; j++) {
                u8 FARP *d = d0 + j * DTD_SZ;
                u32 clk;
                u16 hss, hse, hbl, vss, vse, vbl, hb, vb, hso, hsw, vso, vsw, f;
                if (j && (!res_ok(m, (u16)(rd16(e + 2) + j * DTD_SZ), DTD_SZ) || !dtd_ok(d) ||
                          (d[2] | ((d[4] & 0xF0) << 4)) != xprev || (d[5] | ((d[7] & 0xF0) << 4)) != yprev))
                    break;
                f = freqs[j];
                if (j == 0 && dtd_ok(d)) {      /* keep the refresh rate of the slot */
                    u32 tot = (u32)((d[2] | ((d[4] & 0xF0) << 4)) + (d[3] | ((d[4] & 0x0F) << 8))) *
                              ((d[5] | ((d[7] & 0xF0) << 4)) + (d[6] | ((d[7] & 0x0F) << 8)));
                    u32 hz = (u32)rd16(d) * 10000UL / tot;
                    f = hz >= 80 ? 85 : hz >= 70 ? 75 : 60;
                } else if (j) {
                    u32 tot = (u32)(xprev + (d[3] | ((d[4] & 0x0F) << 8))) *
                              (yprev + (d[6] | ((d[7] & 0x0F) << 8)));
                    u32 hz = (u32)rd16(d) * 10000UL / tot;
                    f = hz >= 80 ? 85 : hz >= 70 ? 75 : 60;
                }
                vb_gtf(x, y, f, &clk, &hss, &hse, &hbl, &vss, &vse, &vbl);
                hb = htotal > x ? htotal - x : hbl + 1 - x;
                vb = vtotal > y ? vtotal - y : vbl + 1 - y;
                hsw = htotal > x ? (hb / 4 + 7) & ~7 : hse - hss;
                hso = htotal > x ? (hb / 4 + 7) & ~7 : hss + 1 - x;
                if (hso + hsw > hb) { hso = hb / 4; hsw = hb / 4; }
                vso = 1; vsw = 3;
                if (vb < 5) { vso = 1; vsw = vb > 1 ? vb - 1 : 1; }
                if (htotal > x || vtotal > y) clk = (u32)(x + hb) * (y + vb) * f / 1000UL;
                wr16(d, (u16)(clk / 10));                       /* 10 kHz */
                d[2] = (u8)x; d[3] = (u8)hb;
                d[4] = (u8)(((x >> 4) & 0xF0) | ((hb >> 8) & 0x0F));
                d[5] = (u8)y; d[6] = (u8)vb;
                d[7] = (u8)(((y >> 4) & 0xF0) | ((vb >> 8) & 0x0F));
                d[8] = (u8)hso; d[9] = (u8)hsw;
                d[10] = (u8)(((vso & 0x0F) << 4) | (vsw & 0x0F));
                d[11] = (u8)(((hso >> 2) & 0xC0) | ((hsw >> 4) & 0x30) | ((vso >> 2) & 0x0C) | ((vsw >> 4) & 0x03));
            }
        } else
            return 0;
        done++;
    }
    return done;
}
