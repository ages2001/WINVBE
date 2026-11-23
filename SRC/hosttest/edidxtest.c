/* host test of drv/edidx.h: DisplayID and CTA-861 native timings */
#include <stdio.h>
#include <string.h>
#include "../drv/edidx.h"
static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } else printf("ok: %s\n", m); } while (0)
static void put16(unsigned char *p, unsigned v) { p[0] = v & 255; p[1] = v >> 8; }
/* DisplayID Type I/VII timing: w x h, blanking 80/62, clock for ~60 Hz */
static void did_timing(unsigned char *p, unsigned w, unsigned h, int pref, int type7)
{
    unsigned long clk = (unsigned long)(w + 80) * (h + 62) * 60;           /* Hz */
    unsigned long v = type7 ? clk / 1000 - 1 : clk / 10000 - 1;
    p[0] = v & 255; p[1] = (v >> 8) & 255; p[2] = (v >> 16) & 255;
    p[3] = pref ? 0x80 : 0;
    put16(p + 4, w - 1); put16(p + 6, 80 - 1); put16(p + 8, (48 - 1) | 0x8000); put16(p + 10, 32 - 1);
    put16(p + 12, h - 1); put16(p + 14, 62 - 1); put16(p + 16, 3 - 1); put16(p + 18, 5 - 1);
}
int main(void)
{
    unsigned char b[128];
    EDIDTM t, best;
    /* DisplayID 1.3, Type I block with two timings, the second preferred */
    memset(b, 0, sizeof(b)); memset(&t, 0, sizeof(t));
    b[0] = 0x70; b[1] = 0x13; b[2] = 3 + 40; b[5] = 0x03; b[6] = 0; b[7] = 40;
    did_timing(b + 8, 3840, 2160, 0, 0); did_timing(b + 28, 7680, 4320, 1, 0);
    CHECK(edidx_ext(b, &t) && t.ha == 7680 && t.va == 4320 && t.src == 2, "DisplayID Type I preferred 7680x4320");
    CHECK(t.hz >= 59 && t.hz <= 61 && t.pclk10k && t.hpos == 1, "DisplayID Type I clock / refresh / polarity");
    /* DisplayID 2.0, Type VII 5120x1440 */
    memset(b, 0, sizeof(b)); memset(&t, 0, sizeof(t));
    b[0] = 0x70; b[1] = 0x20; b[2] = 3 + 20; b[5] = 0x22; b[7] = 20;
    did_timing(b + 8, 5120, 1440, 1, 1);
    CHECK(edidx_ext(b, &t) && t.ha == 5120 && t.va == 1440 && t.hz >= 59 && t.hz <= 61, "DisplayID 2.0 Type VII 5120x1440");
    /* CTA-861: first SVD 7680x4320@60 (VIC 199) */
    memset(b, 0, sizeof(b)); memset(&t, 0, sizeof(t));
    b[0] = 0x02; b[1] = 3; b[2] = 4 + 3; b[4] = (2 << 5) | 2; b[5] = 199; b[6] = 16;
    CHECK(edidx_ext(b, &t) && t.ha == 7680 && t.va == 4320 && t.hz == 60 && t.src == 3, "CTA VIC 199 (8K 60 Hz)");
    /* CTA-861: first SVD 1080p (native flag) -> nothing beyond 4095 */
    b[5] = 0x80 | 16;
    CHECK(!edidx_ext(b, &t), "CTA preferred 1080p is ignored");
    /* CTA-861 without data blocks (d = 0 and d = 4: QEMU) */
    memset(b, 0, sizeof(b)); b[0] = 0x02; b[1] = 3; b[2] = 0; b[60] = 0x40; b[61] = 121;
    CHECK(!edidx_ext(b, &t), "CTA d=0: no data blocks");
    b[2] = 4;
    CHECK(!edidx_ext(b, &t), "CTA d=4: no data blocks");
    /* merge: a preferred DisplayID timing wins over the base DTD */
    memset(&best, 0, sizeof(best)); best.ha = 3840; best.va = 2160; best.src = 1;
    memset(&t, 0, sizeof(t)); t.ha = 7680; t.va = 4320; t.src = 2; t.pref = 1;
    edidx_merge(&best, &t);
    CHECK(best.ha == 7680 && best.src == 2, "merge: preferred DisplayID timing replaces the base DTD");
    /* merge: a CTA format list does not replace the preferred base DTD (QEMU lists 5120x2160) */
    memset(&best, 0, sizeof(best)); best.ha = 1280; best.va = 800; best.src = 1;
    memset(&t, 0, sizeof(t)); t.ha = 5120; t.va = 2160; t.src = 3;
    edidx_merge(&best, &t);
    CHECK(best.ha == 1280 && best.src == 1, "merge: CTA VIC does not replace the base DTD");
    /* merge: without a base timing the extension is used */
    memset(&best, 0, sizeof(best));
    edidx_merge(&best, &t);
    CHECK(best.ha == 5120 && best.src == 3, "merge: CTA VIC used when the base block has no timing");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
