/* host test of d3d/swr.c: coverage (fill rule), interpolation, texturing,
   z test, blending; writes swr*.ppm */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "../d3d/swr.h"

static unsigned rt32[256 * 256], zb[256 * 256], tex[64 * 64];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void vtx(SWVTX *v, float x, float y, float z, float r, float g, float b, float u, float t)
{
    memset(v, 0, sizeof(*v));
    v->x = x; v->y = y; v->z = z; v->rhw = 1;
    v->c[0] = r; v->c[1] = g; v->c[2] = b; v->c[3] = 1;
    v->s[3] = 1;
    v->t[0][0] = u; v->t[0][1] = t;
}

static void ppm(const char *n)
{
    FILE *f = fopen(n, "wb");
    int i;
    fprintf(f, "P6 256 256 255\n");
    for (i = 0; i < 256 * 256; i++) { fputc(rt32[i] >> 16 & 255, f); fputc(rt32[i] >> 8 & 255, f); fputc(rt32[i] & 255, f); }
    fclose(f);
}

int main(void)
{
    SWCTX c;
    SWVTX a, b, d, e;
    SWTEX t;
    int i, x, y, n;
    sw_init(&c);
    c.rt.bits = (unsigned char *)rt32; c.rt.pitch = 1024; c.rt.w = 256; c.rt.h = 256; c.rt.fmt = SWF_X8R8G8B8;
    c.zb.fmt = SWF_NONE;
    c.rs[22] = 1;                               /* no culling */
    sw_prepare(&c);
    /* 1: coverage - a quad of two triangles with additive blending: every
       pixel exactly once */
    memset(rt32, 0, sizeof(rt32));
    c.rs[27] = 1; c.rs[19] = 2; c.rs[20] = 2;   /* ONE, ONE */
    c.tss[0][1] = 3; c.tss[0][3] = 0;           /* COLOROP SELECTARG2 = diffuse */
    sw_prepare(&c);
    vtx(&a, 10.3f, 10.7f, 0, 0.25f, 0, 0, 0, 0); vtx(&b, 200.1f, 20.2f, 0, 0.25f, 0, 0, 0, 0);
    vtx(&d, 190.6f, 230.4f, 0, 0.25f, 0, 0, 0, 0); vtx(&e, 15.5f, 180.5f, 0, 0.25f, 0, 0, 0, 0);
    sw_triangle(&c, &a, &b, &d);
    sw_triangle(&c, &a, &d, &e);
    n = 0;
    for (i = 0; i < 256 * 256; i++) {
        unsigned r = rt32[i] >> 16 & 255;
        CHECK(r == 0 || r == 64, "pixel %d,%d drawn %u (overlap)", i % 256, i / 256, r);
        if (r) n++;
    }
    printf("quad pixels %d\n", n);
    /* exact grid quad 0..100: 100x100 pixels */
    memset(rt32, 0, sizeof(rt32));
    vtx(&a, 0, 0, 0, 0.25f, 0, 0, 0, 0); vtx(&b, 100, 0, 0, 0.25f, 0, 0, 0, 0);
    vtx(&d, 100, 100, 0, 0.25f, 0, 0, 0, 0); vtx(&e, 0, 100, 0, 0.25f, 0, 0, 0, 0);
    sw_triangle(&c, &a, &b, &d); sw_triangle(&c, &a, &d, &e);
    n = 0;
    for (i = 0; i < 256 * 256; i++) if (rt32[i]) n++;
    CHECK(n == 10000, "grid quad %d pixels, expected 10000", n);
    CHECK(rt32[0] && !rt32[100] && rt32[99 * 256 + 99] && !rt32[100 * 256], "grid quad edges");
    /* 2: gouraud colours */
    c.rs[27] = 0;
    memset(rt32, 0, sizeof(rt32));
    vtx(&a, 0, 0, 0, 1, 0, 0, 0, 0); vtx(&b, 255, 0, 0, 0, 1, 0, 0, 0); vtx(&d, 0, 255, 0, 0, 0, 1, 0, 0);
    sw_triangle(&c, &a, &b, &d);
    ppm("swr1.ppm");
    CHECK((rt32[0] >> 16 & 255) > 250, "red corner %08x", rt32[0]);
    CHECK((rt32[254] >> 8 & 255) > 245, "green corner %08x", rt32[254]);
    /* 3: texture, perspective, bilinear */
    for (y = 0; y < 64; y++) for (x = 0; x < 64; x++) tex[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0xFFFFFFFF : 0xFF0000FF;
    memset(&t, 0, sizeof(t));
    t.lvl[0].bits = (unsigned char *)tex; t.lvl[0].pitch = 256; t.lvl[0].w = 64; t.lvl[0].h = 64; t.lvl[0].fmt = SWF_A8R8G8B8;
    t.nlvl = 1;
    c.tex[0] = &t;
    c.tss[0][1] = 4; c.tss[0][2] = 2; c.tss[0][3] = 0;   /* MODULATE texture, diffuse */
    c.tss[0][16] = c.tss[0][17] = 2;
    sw_prepare(&c);
    memset(rt32, 0, sizeof(rt32));
    vtx(&a, 97.5f, 20, 0.5f, 1, 1, 1, 0, 0); vtx(&b, 158.5f, 20, 0.5f, 1, 1, 1, 1, 0);
    vtx(&d, 250, 240, 0.5f, 1, 1, 1, 1, 1); vtx(&e, 6, 240, 0.5f, 1, 1, 1, 0, 1);
    a.rhw = b.rhw = 0.25f; d.rhw = e.rhw = 1.0f;
    sw_triangle(&c, &a, &b, &d); sw_triangle(&c, &a, &d, &e);
    ppm("swr2.ppm");
    /* the same through the generic pixel path (stencil on without a stencil
       buffer takes it): the fast path must agree */
    {
        static unsigned ref[256 * 256];
        int md = 0, bad = 0;
        memcpy(ref, rt32, sizeof(ref));
        memset(rt32, 0, sizeof(rt32));
        c.rs[52] = 1; sw_prepare(&c);
        sw_triangle(&c, &a, &b, &d); sw_triangle(&c, &a, &d, &e);
        c.rs[52] = 0; sw_prepare(&c);
        for (i = 0; i < 256 * 256; i++) {
            int k;
            for (k = 0; k < 24; k += 8) {
                int df = abs((int)(rt32[i] >> k & 255) - (int)(ref[i] >> k & 255));
                if (df > md) md = df;
                if (df > 24) bad++;
            }
        }
        printf("fast/generic: max difference %d, %d channels off by more than 24\n", md, bad);
        CHECK(bad < 300, "fast path differs from the generic one (%d)", bad);
    }
    /* 4: z test: a far quad then a near triangle; then the far one again must not overwrite */
    c.zb.bits = (unsigned char *)zb; c.zb.pitch = 1024; c.zb.w = c.zb.h = 256; c.zb.fmt = SWF_Z24S8;
    c.rs[7] = 1;
    c.tex[0] = 0;
    c.tss[0][1] = 3;
    sw_prepare(&c);
    sw_clear(&c, 3, 0xFF000000, 1.0f, 0, 0, 0, 256, 256);
    vtx(&a, 50, 50, 0.2f, 0, 1, 0, 0, 0); vtx(&b, 150, 50, 0.2f, 0, 1, 0, 0, 0); vtx(&d, 100, 150, 0.2f, 0, 1, 0, 0, 0);
    sw_triangle(&c, &a, &b, &d);
    vtx(&a, 0, 0, 0.8f, 1, 0, 0, 0, 0); vtx(&b, 255, 0, 0.8f, 1, 0, 0, 0, 0); vtx(&d, 0, 255, 0.8f, 1, 0, 0, 0, 0);
    sw_triangle(&c, &a, &b, &d);
    CHECK((rt32[80 * 256 + 100] & 0xFFFFFF) == 0x00FF00, "z: near green kept %08x", rt32[80 * 256 + 100]);
    CHECK((rt32[10 * 256 + 10] & 0xFFFFFF) == 0xFF0000, "z: far red drawn elsewhere %08x", rt32[10 * 256 + 10]);
    /* 5: alpha blend SRCALPHA/INVSRCALPHA 50 % over red */
    c.rs[7] = 0; c.rs[27] = 1; c.rs[19] = 5; c.rs[20] = 6;
    c.tss[0][4] = 3; c.tss[0][6] = 0;            /* ALPHAOP SELECTARG2 diffuse */
    sw_prepare(&c);
    vtx(&a, 0, 0, 0, 0, 0, 1, 0, 0); vtx(&b, 40, 0, 0, 0, 0, 1, 0, 0); vtx(&d, 0, 40, 0, 0, 0, 1, 0, 0);
    a.c[3] = b.c[3] = d.c[3] = 0.5f;
    sw_triangle(&c, &a, &b, &d);
    CHECK(abs((int)(rt32[5 * 256 + 5] >> 16 & 255) - 128) <= 2 && abs((int)(rt32[5 * 256 + 5] & 255) - 128) <= 2,
          "blend %08x", rt32[5 * 256 + 5]);
    /* 6: 565 target */
    {
        static unsigned short r16[64 * 64];
        c.rt.bits = (unsigned char *)r16; c.rt.pitch = 128; c.rt.w = c.rt.h = 64; c.rt.fmt = SWF_R5G6B5;
        c.zb.fmt = SWF_NONE; c.rs[27] = 0; c.clip[2] = c.clip[3] = 0;
        sw_prepare(&c);
        sw_clear(&c, 1, 0xFF00FF00, 0, 0, 0, 0, 64, 64);
        CHECK(r16[0] == 0x07E0, "565 clear %04x", r16[0]);
        vtx(&a, 0, 0, 0, 1, 1, 1, 0, 0); vtx(&b, 64, 0, 0, 1, 1, 1, 0, 0); vtx(&d, 0, 64, 0, 1, 1, 1, 0, 0);
        sw_triangle(&c, &a, &b, &d);
        CHECK(r16[0] == 0xFFFF && r16[63 * 64 + 63] == 0x07E0, "565 tri %04x %04x", r16[0], r16[63 * 64 + 63]);
    }
    printf(fails ? "FAILED %d\n" : "PASSED\n", fails);
    return fails != 0;
}
