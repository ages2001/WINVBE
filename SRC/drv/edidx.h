/*
 * WINVBE - EDID timings beyond the 128-byte base block
 *
 * The detailed timing descriptors of the base EDID block store the active
 * size in 12 bits, so a monitor wider or taller than 4095 pixels (ultrawide
 * 32:9 panels at 5120 or more, 8K at 7680x4320) reports its native timing
 * in an extension block instead:
 *   - DisplayID 1.x  Type I  detailed timing (data block tag 03h)
 *   - DisplayID 2.x  Type VII detailed timing (data block tag 22h)
 *   - CTA-861 video data block: the first (preferred) short video
 *     descriptor, for the VICs wider than 4095 pixels
 *
 * Shared, as plain C, by the Windows 3.x/9x driver, the NT miniport and
 * the configuration tools. The caller copies each 128-byte block into
 * memory it can address with a near (default) pointer.
 */
#ifndef WVB_EDIDX_H
#define WVB_EDIDX_H

typedef struct {
    unsigned long  pclk10k;         /* pixel clock, 10 kHz units (0: no exact timing) */
    unsigned short ha, hb, hso, hsw;
    unsigned short va, vb, vso, vsw;
    unsigned char  hpos, vpos;      /* sync polarity: 1 positive */
    unsigned short hz;              /* refresh rate */
    unsigned char  src;             /* 0 none, 1 base DTD, 2 DisplayID, 3 CTA VIC */
    unsigned char  pref;            /* DisplayID: marked as the preferred timing */
} EDIDTM;

/* the base block's first detailed timing descriptor */
static int edidx_dtd(const unsigned char *d, EDIDTM *t)
{
    unsigned long tot;
    unsigned short clk = (unsigned short)(d[0] | (d[1] << 8));
    if (!clk) return 0;
    t->pclk10k = clk;
    t->ha  = (unsigned short)(d[2] | ((d[4] & 0xF0) << 4));
    t->hb  = (unsigned short)(d[3] | ((d[4] & 0x0F) << 8));
    t->va  = (unsigned short)(d[5] | ((d[7] & 0xF0) << 4));
    t->vb  = (unsigned short)(d[6] | ((d[7] & 0x0F) << 8));
    t->hso = (unsigned short)(d[8] | ((d[11] & 0xC0) << 2));
    t->hsw = (unsigned short)(d[9] | ((d[11] & 0x30) << 4));
    t->vso = (unsigned short)((d[10] >> 4) | ((d[11] & 0x0C) << 2));
    t->vsw = (unsigned short)((d[10] & 0x0F) | ((d[11] & 0x03) << 4));
    t->hpos = t->vpos = 1;                      /* as the base block gives them */
    if ((d[17] & 0x18) == 0x18) {
        t->hpos = (unsigned char)((d[17] & 0x02) ? 1 : 0);
        t->vpos = (unsigned char)((d[17] & 0x04) ? 1 : 0);
    }
    tot = (unsigned long)(t->ha + t->hb) * (t->va + t->vb);
    t->hz = tot ? (unsigned short)((t->pclk10k * 10000UL + tot / 2) / tot) : 60;
    t->src = 1;
    return 1;
}

#ifndef EDIDX_DTD_ONLY
static unsigned short edidx_w16(const unsigned char *p) { return (unsigned short)(p[0] | (p[1] << 8)); }

/* a DisplayID Type I (khz10 = 1) or Type VII (khz10 = 0) timing */
static int edidx_did(const unsigned char *p, int khz10, EDIDTM *t)
{
    unsigned long clk = ((unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16)) + 1;
    unsigned long tot;
    t->pclk10k = khz10 ? clk : (clk + 5) / 10;
    t->ha  = (unsigned short)(edidx_w16(p + 4) + 1);
    t->hb  = (unsigned short)(edidx_w16(p + 6) + 1);
    t->hso = (unsigned short)((edidx_w16(p + 8) & 0x7FFF) + 1);
    t->hpos = (unsigned char)((p[9] & 0x80) ? 1 : 0);
    t->hsw = (unsigned short)(edidx_w16(p + 10) + 1);
    t->va  = (unsigned short)(edidx_w16(p + 12) + 1);
    t->vb  = (unsigned short)(edidx_w16(p + 14) + 1);
    t->vso = (unsigned short)((edidx_w16(p + 16) & 0x7FFF) + 1);
    t->vpos = (unsigned char)((p[17] & 0x80) ? 1 : 0);
    t->vsw = (unsigned short)(edidx_w16(p + 18) + 1);
    if (p[3] & 0x10) return 0;                  /* interlaced */
    tot = (unsigned long)(t->ha + t->hb) * (t->va + t->vb);
    if (!tot) return 0;
    t->hz = (unsigned short)((t->pclk10k * 10000UL + tot / 2) / tot);
    t->src = 2;
    return 1;
}

/* CTA-861 VICs wider than 4095 pixels (the base block cannot hold them) */
static int edidx_vic(unsigned char vic, EDIDTM *t)
{
    static const unsigned char hz8[8] = { 24, 25, 30, 48, 50, 60, 100, 120 };
    unsigned short w = 0, h = 0, hz = 0;
    if (vic >= 121 && vic <= 127) { w = 5120; h = 2160; hz = hz8[vic - 121]; }
    else if (vic == 193)          { w = 5120; h = 2160; hz = 120; }
    else if (vic >= 194 && vic <= 201) { w = 7680; h = 4320; hz = hz8[vic - 194]; }
    else if (vic >= 202 && vic <= 209) { w = 7680; h = 4320; hz = hz8[vic - 202]; }
    else if (vic >= 210 && vic <= 217) { w = 10240; h = 4320; hz = hz8[vic - 210]; }
    if (!w) return 0;
    t->pclk10k = 0;             /* no exact timing: CVT when a rate is asked */
    t->ha = w; t->va = h; t->hz = hz;
    t->hb = t->hso = t->hsw = t->vb = t->vso = t->vsw = 0;
    t->hpos = t->vpos = 0;
    t->src = 3;
    return 1;
}

/* the native timing an extension block reports, if any: the preferred
   DisplayID timing (else its first one), or the preferred CTA VIC when it
   is wider than 4095 pixels */
static int edidx_ext(const unsigned char *b, EDIDTM *t)
{
    if (b[0] == 0x70) {                         /* DisplayID */
        int end = 5 + b[2], i = 5, got = 0;
        if (end > 127) end = 127;
        while (i + 3 <= end) {
            unsigned char tag = b[i], len = b[i + 2];
            const unsigned char *p = b + i + 3;
            int k;
            if (!tag && !len) break;
            if (i + 3 + len > end) break;
            if (tag == 0x03 || tag == 0x22)
                for (k = 0; k + 20 <= len; k += 20) {
                    EDIDTM c;
                    if (!edidx_did(p + k, tag == 0x03, &c)) continue;
                    c.pref = (unsigned char)((p[k + 3] & 0x80) ? 1 : 0);
                    if (!got || (p[k + 3] & 0x80)) { *t = c; got = 1; }
                    if (p[k + 3] & 0x80) return 1;
                }
            i += 3 + len;
        }
        return got;
    }
    if (b[0] == 0x02 && b[1] >= 3) {            /* CTA-861 */
        int end = b[2], i = 4;
        if (end <= 4) return 0;                 /* 0: nothing, 4: no data blocks */
        if (end > 127) end = 127;
        while (i < end) {
            unsigned char tag = (unsigned char)(b[i] >> 5), len = (unsigned char)(b[i] & 31);
            if (tag == 2 && len) {
                unsigned char v = b[i + 1];
                if (v >= 1 && v <= 64) v &= 0x7F;   /* bit 7: native flag (VIC 1-64) */
                return edidx_vic(v, t);
            }
            i += 1 + len;
        }
    }
    return 0;
}

/* The preferred timing is the base block's first detailed timing. An
   extension replaces it only when the base block has none (a monitor beyond
   4095 pixels leaves a dummy there) or DisplayID marks a larger timing as
   preferred. A CTA-861 video data block lists supported formats, not the
   preferred one, so it counts only without a base timing. */
static void edidx_merge(EDIDTM *best, const EDIDTM *ext)
{
    if (!ext->src) return;
    if (!best->src) { *best = *ext; return; }
    if (ext->src == 2 && ext->pref && (unsigned long)ext->ha * ext->va > (unsigned long)best->ha * best->va)
        *best = *ext;
}

#endif /* EDIDX_DTD_ONLY */

#endif
