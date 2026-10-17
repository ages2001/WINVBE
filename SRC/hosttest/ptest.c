/* host test for patch/i915core.c with synthetic video BIOS images */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../patch/i915core.h"

static u8 img[65536];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void w16(u32 o, u16 v) { img[o] = v; img[o + 1] = v >> 8; }

static void gtf_ref(int x, int y, int freq, unsigned long *clock, int *hss, int *hse, int *hbl, int *vss, int *vse, int *vbl)
{
    int hb, vb, vfreq;
    vb = y + (y + 1) / (20000.0 / (11 * freq) - 1) + 1.5;
    vfreq = vb * freq;
    hb = 16 * (int)(x * (30.0 - 300000.0 / vfreq) / (70.0 + 300000.0 / vfreq) / 16.0 + 0.5);
    *vss = y; *vse = y + 3; *vbl = vb - 1;
    *hss = x + hb / 2 - (x + hb + 50) / 100 * 8 - 1;
    *hse = x + hb / 2 - 1; *hbl = x + hb - 1;
    *clock = (x + hb) * vfreq / 1000;
}

static void build(int type)
{
    static const int res[8][2] = { {640,480},{800,600},{1024,768},{1280,1024},{1600,1200},{1920,1440},{2048,1536},{1280,800} };
    u32 tab = 0x2000, rb = 0x3000, i, j, step;
    memset(img, 0, sizeof(img));
    img[0] = 0x55; img[1] = 0xAA; img[2] = 128;
    memcpy(img + 0x100, "Intel Corp VBIOS", 16);
    step = type == BT_1 ? 8 : type == BT_2 ? 6 + 3 * 28 : 6 + 3 * 38;
    for (i = 0; i < 8; i++) {
        u32 r = rb + i * step;
        img[tab + i * 5] = 0x30 + i * 2;  img[tab + i * 5 + 1] = 8;
        w16(tab + i * 5 + 2, r);
        if (type == BT_1) {
            img[r + 2] = res[i][0] & 0xFF; img[r + 4] = (res[i][0] >> 4) & 0xF0;
            img[r + 5] = res[i][1] & 0xFF; img[r + 7] = (res[i][1] >> 4) & 0xF0;
        } else {
            u32 sz = type == BT_2 ? 28 : 38;
            for (j = 0; j < 3; j++) {
                w16(r + 6 + j * sz + 4, res[i][0] - 1);
                w16(r + 6 + j * sz + 16, res[i][1] - 1);
            }
        }
    }
    img[tab + 8 * 5] = 0xFF;
}

/* blocks that are not evenly spaced (GM965/GM45 style), real timings */
static void build_uneven(int type)
{
    static const int res[8][2] = { {640,480},{800,600},{1024,768},{1280,1024},{1600,1200},{1280,800},{1440,900},{1024,600} };
    static const int gap[8] = { 3, 0, 5, 2, 0, 7, 1, 4 };
    u32 tab = 0x2400, r = 0x5000, i, j;
    memset(img, 0, sizeof(img));
    img[0] = 0x55; img[1] = 0xAA; img[2] = 128;
    memcpy(img + 0x100, "Intel(R) GM45 VBIOS", 19);
    for (i = 0; i < 8; i++) {
        int x = res[i][0], y = res[i][1], nml = 1 + i % 3;
        u32 sz = type == BT_2 ? 28 : 38;
        img[tab + i * 5] = 0x30 + i * 2;  img[tab + i * 5 + 1] = 8;
        w16(tab + i * 5 + 2, r);
        img[r] = 0xA5; img[r + 1] = 0x5A;
        if (type == BT_1) {
            int hb = x / 4, vb = y / 20;
            img[r + 2] = x & 0xFF; img[r + 3] = hb & 0xFF; img[r + 4] = ((x >> 4) & 0xF0) | ((hb >> 8) & 0x0F);
            img[r + 5] = y & 0xFF; img[r + 6] = vb & 0xFF; img[r + 7] = ((y >> 4) & 0xF0) | ((vb >> 8) & 0x0F);
            r += 8 + gap[i];
            continue;
        }
        if (type == BT_2) { img[r] = x / 8; img[r + 1] = y / 16 - 1; }
        for (j = 0; j < (u32)nml; j++) {
            u32 ml = r + 6 + j * sz;
            unsigned long c; u16 a[6];
            u32 c2;
            vb_gtf(x, y, 60, &c2, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
            c = c2;
            w16(ml, c & 0xFFFF); w16(ml + 2, c >> 16);
            w16(ml + 4, x - 1); w16(ml + 6, a[2]); w16(ml + 8, x - 1); w16(ml + 10, a[2]);
            w16(ml + 12, a[0]); w16(ml + 14, a[1]);
            w16(ml + 16, y - 1); w16(ml + 18, a[5]); w16(ml + 20, y - 1); w16(ml + 22, a[5]);
            w16(ml + 24, a[3]); w16(ml + 26, a[4]);
        }
        r += 6 + nml * sz + gap[i];
    }
    img[tab + 8 * 5] = 0xFF;
}

int main(void)
{
    int t, f, x, y;
    /* GTF integer version against the floating point formula */
    for (f = 60; f <= 85; f += 5)
        for (x = 640; x <= 2560; x += 160)
            for (y = 400; y <= 1600; y += 120) {
                unsigned long c1; u32 c2; int a[6]; u16 b[6];
                gtf_ref(x, y, f, &c1, &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
                vb_gtf(x, y, f, &c2, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
                CHECK(abs(a[2] - b[2]) <= 16 && a[5] == b[5] && labs((long)c1 - (long)c2) <= (long)c1 / 50,
                      "gtf %dx%d@%d: ref hbl %d vbl %d clk %lu, got %u %u %lu", x, y, f, a[2], a[5], c1, b[2], b[5], (unsigned long)c2);
            }
    for (t = BT_1; t <= BT_3; t++) {
        VBMAP m; VBMODE md; u16 i;
        build(t);
        m.bios = img; m.size = 65536;
        CHECK(vb_find_table(&m) && m.table == 0x2000 && m.count == 8, "type %d: table %x count %u", t, m.table, m.count);
        CHECK(vb_locate(&m) && m.type == t, "type %d detected as %d", t, m.type);
        vb_get_mode(&m, 2, &md);
        CHECK(md.mode == 0x34 && md.x == 1024 && md.y == 768, "type %d mode 2: %x %ux%u", t, md.mode, md.x, md.y);
        CHECK(vb_set_mode(&m, 0x3C, 1400, 1050, 32, 0, 0) == 1, "type %d set", t);
        vb_get_mode(&m, 6, &md);
        CHECK(md.x == 1400 && md.y == 1050 && md.bpp == 32, "type %d after set: %ux%u bpp %u", t, md.x, md.y, md.bpp);
        for (i = 0; i < 8; i++) { if (i == 6) continue; vb_get_mode(&m, i, &md); CHECK(md.x != 1400, "type %d other mode changed", t); }
        CHECK(vb_set_mode(&m, 0x7F, 800, 600, 0, 0, 0) == 0, "type %d unknown mode accepted", t);
        printf("type %d (%s): ok\n", t, vb_type_name(m.type));
    }
    for (t = BT_1; t <= BT_3; t++) {
        VBMAP m; VBMODE md;
        build_uneven(t);
        m.bios = img; m.size = 65536;
        CHECK(vb_find_table(&m) && m.count == 8, "uneven type %d: table", t);
        CHECK(vb_locate(&m) && m.type == t, "uneven type %d detected as %d", t, m.type);
        vb_get_mode(&m, 5, &md);
        CHECK(md.x == 1280 && md.y == 800, "uneven type %d mode 5: %ux%u", t, md.x, md.y);
        CHECK(vb_set_mode(&m, 0x3A, 1366, 768, 0, 0, 0) == 1, "uneven type %d set", t);
        vb_get_mode(&m, 5, &md);
        CHECK(md.x == 1366 && md.y == 768, "uneven type %d after set: %ux%u", t, md.x, md.y);
        vb_get_mode(&m, 6, &md);
        CHECK(md.x == 1440 && md.y == 900, "uneven type %d next mode: %ux%u", t, md.x, md.y);
        if (t == BT_1) {
            u16 r = img[0x2400 + 5 * 5 + 2] | img[0x2400 + 5 * 5 + 3] << 8;
            CHECK(img[r] == 0xA5 && img[r + 1] == 0x5A && img[r + 3] == (320 & 0xFF) && (img[r + 4] & 0x0F) == 1,
                  "type 1 bytes kept: %02x %02x %02x %02x", img[r], img[r + 1], img[r + 3], img[r + 4]);
        }
        printf("uneven type %d: ok\n", t);
    }
    {   /* GM45 seen in the field: a table-like byte sequence before the real
           table (modes 3F 36.. bits 54/255, all 3790x1951) must be skipped */
        static const u8 gm[6][2] = { {0x3F,54},{0x36,54},{0x36,54},{0x36,255},{0x36,54},{0x18,24} };
        VBMAP m; VBMODE md; u32 i;
        build_uneven(BT_1);
        for (i = 0; i < 6; i++) { img[0x1000 + i * 5] = gm[i][0]; img[0x1000 + i * 5 + 1] = gm[i][1]; w16(0x1000 + i * 5 + 2, 0x1100); }
        img[0x1000 + 30] = 0xFF;
        img[0x1102] = 3790 & 0xFF; img[0x1104] = (3790 >> 4) & 0xF0; img[0x1105] = 1951 & 0xFF; img[0x1107] = (1951 >> 4) & 0xF0;
        m.bios = img; m.size = 65536;
        CHECK(vb_find_table(&m) && m.table == 0x1000, "garbage candidate first");
        CHECK(vb_locate(&m) && m.table == 0x2400 && m.type == BT_1, "real table after garbage: %x type %d", m.table, m.type);
        vb_get_mode(&m, 0, &md);
        CHECK(md.x == 640 && md.y == 480, "real table mode 0 %ux%u", md.x, md.y);
        CHECK(vb_force_type(&m, BT_1) && m.table == 0x2400, "forced type skips garbage");
        img[0x2400 + 8 * 5 - 5] = 0;   /* break nothing else */
        printf("garbage table skipped: ok\n");
    }
    {   /* only a garbage table: refused */
        static const u8 gm[6][2] = { {0x3F,54},{0x36,54},{0x36,54},{0x36,255},{0x36,54},{0x18,24} };
        VBMAP m; u32 i;
        memset(img, 0, sizeof(img));
        img[0] = 0x55; img[1] = 0xAA; img[2] = 128;
        for (i = 0; i < 6; i++) { img[0x1000 + i * 5] = gm[i][0]; img[0x1000 + i * 5 + 1] = gm[i][1]; w16(0x1000 + i * 5 + 2, 0x1100); }
        img[0x1000 + 30] = 0xFF;
        img[0x1102] = 3790 & 0xFF; img[0x1104] = (3790 >> 4) & 0xF0; img[0x1105] = 1951 & 0xFF; img[0x1107] = (1951 >> 4) & 0xF0;
        m.bios = img; m.size = 65536;
        CHECK(vb_find_table(&m) && !vb_locate(&m), "GM45 garbage accepted");
        CHECK(!vb_force_type(&m, BT_1), "GM45 garbage accepted with /TYPE:1");
        printf("garbage-only BIOS refused: ok\n");
    }
    {   /* GM45 (Cantiga) layout: blocks of EDID detailed timings (60/75/85 Hz),
           three colour depths sharing a block, empty run-time slots 60h-71h
           and the look-alike bytes found at C000:B808 in a real BIOS */
        static const int res[6][2] = { {640,480},{800,600},{1024,768},{1280,1024},{1600,1200},{1920,1440} };
        static const int tm[6][3][7] = {   /* clk(10kHz) hblank vblank hso hsw vso vsw */
            { {2518,144,29,8,96,2,2}, {3150,200,20,16,64,1,3}, {3600,192,29,56,56,1,3} },
            { {4000,256,28,40,128,1,4}, {4950,256,25,16,80,1,3}, {5625,248,31,32,64,1,3} },
            { {6500,320,38,24,136,3,6}, {7875,288,32,16,96,1,3}, {9450,352,40,48,96,1,3} },
            { {10800,408,42,48,112,1,3}, {13500,408,42,16,144,1,3}, {15750,448,48,64,160,1,3} },
            { {16200,560,50,64,192,1,3}, {20250,560,50,64,192,1,3}, {22950,560,50,64,192,1,3} },
            { {23400,680,60,128,208,1,3}, {29700,720,60,144,224,1,3}, {0,0,0,0,0,0,0} } };
        static const u8 gm[6][2] = { {0x3F,0x36},{0x36,0x36},{0x36,0x36},{0x36,0xFF},{0x36,0x36},{0x18,0x18} };
        VBMAP m; VBMODE md; u32 i, j, k, e = 0x268;
        memset(img, 0, sizeof(img));
        img[0] = 0x55; img[1] = 0xAA; img[2] = 0x7F;
        for (i = 0; i < 6; i++)
            for (j = 0; j < 3; j++) {
                u32 d = 0x136 + i * 54 + j * 18;
                const int *t = tm[i][j];
                int x = res[i][0], y = res[i][1];
                if (!t[0]) continue;        /* the mode table follows (as in the real BIOS) */
                w16(d, t[0]); img[d + 2] = x; img[d + 3] = t[1]; img[d + 4] = ((x >> 4) & 0xF0) | (t[1] >> 8);
                img[d + 5] = y; img[d + 6] = t[2]; img[d + 7] = ((y >> 4) & 0xF0) | (t[2] >> 8);
                img[d + 8] = t[3]; img[d + 9] = t[4]; img[d + 10] = (t[5] << 4) | t[6]; img[d + 17] = 0x1E;
            }
        for (k = 0; k < 3; k++)                         /* 8, 16, 32 bit modes */
            for (i = 0; i < 6; i++, e += 5) {
                static const u8 mn[3][6] = { {0x30,0x32,0x34,0x38,0x3A,0x3C}, {0x41,0x43,0x45,0x49,0x4B,0x4D},
                                             {0x50,0x52,0x54,0x58,0x5A,0x5C} };
                img[e] = mn[k][i]; img[e + 1] = k == 0 ? 8 : k == 1 ? 16 : 32;
                w16(e + 2, 0x136 + i * 54); img[e + 4] = (u8)(3 + i * 3 + k);
            }
        for (i = 0; i < 18; i++, e += 5) {              /* empty slots 60h-71h */
            img[e] = (u8)(0x60 + i); img[e + 1] = (i % 3 == 0) ? 8 : (i % 3 == 1) ? 16 : 32;
            w16(e + 2, 0x130C + (i / 3) * 26);
        }
        img[e] = 0xFF;
        for (i = 0; i < 6; i++) { img[0x130C + i * 26 + 6] = 0x07; img[0x130C + i * 26 + 7] = 0x3C; }
        for (i = 0; i < 6; i++) { img[0xB808 + i * 5] = gm[i][0]; memset(img + 0xB809 + i * 5, gm[i][1], 4); }
        img[0xB808 + 30] = 0xFF;
        m.bios = img; m.size = 0xFE00;
        CHECK(vb_locate(&m) && m.table == 0x268 && m.count == 36 && m.type == BT_4,
              "GM45: table %x count %u type %d", m.table, m.count, m.type);
        vb_get_mode(&m, 16, &md);
        CHECK(md.mode == 0x5A && md.x == 1600 && md.y == 1200 && md.bpp == 32, "GM45 mode 17: %x %ux%u", md.mode, md.x, md.y);
        vb_get_mode(&m, 20, &md);
        CHECK(md.mode == 0x62 && md.x == 0 && md.y == 0, "GM45 empty slot: %x %ux%u", md.mode, md.x, md.y);
        CHECK(vb_set_mode(&m, 0x62, 1280, 800, 0, 0, 0) == 0, "GM45 empty slot patched");
        CHECK(vb_set_mode(&m, 0x5C, 1280, 800, 0, 0, 0) == 1, "GM45 set");
        for (k = 0; k < 3; k++) {
            vb_get_mode(&m, k * 6 + 5, &md);
            CHECK(md.x == 1280 && md.y == 800, "GM45 shared block bpp %u: %ux%u", md.bpp, md.x, md.y);
        }
        for (j = 0; j < 2; j++) {                       /* GTF 60 / 75 Hz timings */
            u8 *d = img + 0x244 + j * 18;
            int hb = d[3] | ((d[4] & 15) << 8), vb = d[6] | ((d[7] & 15) << 8);
            int hso = d[8] | ((d[11] & 0xC0) << 2), hsw = d[9] | ((d[11] & 0x30) << 4);
            unsigned long c = d[0] | (d[1] << 8), hz = c * 10000UL / ((1280UL + hb) * (800UL + vb));
            CHECK((d[2] | ((d[4] & 0xF0) << 4)) == 1280 && (d[5] | ((d[7] & 0xF0) << 4)) == 800, "GM45 timing %u size", (unsigned)j);
            CHECK(hz == (j ? 75UL : 60UL) || hz + 1 == (j ? 75UL : 60UL), "GM45 timing %u: %lu Hz", (unsigned)j, hz);
            CHECK(hso + hsw <= hb && d[17] == 0x1E && c == (j ? 10721UL : 8346UL), "GM45 timing %u: clock %lu sync %d+%d/%d", (unsigned)j, c, hso, hsw, hb);
        }
        CHECK(img[0x268] == 0x30 && img[0x269] == 8, "GM45: the mode table after the two timings changed");
        vb_get_mode(&m, 4, &md);
        CHECK(md.x == 1600 && md.y == 1200, "GM45 other mode changed");
        m.table = 0;
        CHECK(vb_locate(&m) && m.type == BT_4, "GM45 after patch: type %d", m.type);
        CHECK(vb_force_type(&m, BT_4) && m.table == 0x268, "GM45 /TYPE:4");
        printf("GM45 EDID timing layout: ok\n");
    }
    {   /* not a mode table layout: must be refused */
        VBMAP m; u32 i;
        build_uneven(BT_1);
        for (i = 0x5000; i < 0x5200; i++) img[i] = (u8)(i * 37 + 11);
        m.bios = img; m.size = 65536;
        CHECK(vb_find_table(&m) && !vb_locate(&m), "garbage accepted as type %d", m.type);
        CHECK(!vb_force_type(&m, BT_1), "garbage accepted with forced type");
        printf("garbage refused: ok\n");
    }
    printf(fails ? "%d FAILURES\n" : "all tests passed\n", fails);
    return fails != 0;
}
