/*
 * WINVBE - ternary raster operations on byte buffers
 * D = rop3(P, S, D), applied bitwise (works for any pixel format).
 */
#include "drv.h"

BYTE rop2_to_rop3(WORD rop2)
{
    /* R2 code 1..16: (code-1) is a truth table indexed by (P<<1)|D */
    BYTE t = (BYTE)((rop2 - 1) & 0x0F);
    BYTE r = 0, i;
    for (i = 0; i < 8; i++) {
        BYTE p = (i >> 2) & 1, d = i & 1;
        if ((t >> ((p << 1) | d)) & 1)
            r |= (BYTE)(1 << i);
    }
    return r;
}

/* generic evaluation of one word */
static WORD rop_eval(BYTE rop, WORD p, WORD s, WORD d)
{
    WORD r = 0;
    if (rop & 0x01) r |= ~p & ~s & ~d;
    if (rop & 0x02) r |= ~p & ~s &  d;
    if (rop & 0x04) r |= ~p &  s & ~d;
    if (rop & 0x08) r |= ~p &  s &  d;
    if (rop & 0x10) r |=  p & ~s & ~d;
    if (rop & 0x20) r |=  p & ~s &  d;
    if (rop & 0x40) r |=  p &  s & ~d;
    if (rop & 0x80) r |=  p &  s &  d;
    return r;
}

void rop_apply(BYTE rop, BYTE *D, const BYTE *S, const BYTE *P, WORD n)
{
    WORD i, nw = n >> 1;
    WORD *d = (WORD *)D;
    const WORD *s = (const WORD *)S, *p = (const WORD *)P;

    switch (rop) {
    case 0x00: memset(D, 0, n); return;
    case 0xFF: memset(D, 0xFF, n); return;
    case 0xCC: memcpy(D, S, n); return;                       /* SRCCOPY */
    case 0xF0: memcpy(D, P, n); return;                       /* PATCOPY */
    case 0xAA: return;                                        /* D */
    case 0x33: for (i = 0; i < nw; i++) d[i] = ~s[i]; break;  /* NOTSRCCOPY */
    case 0x55: for (i = 0; i < nw; i++) d[i] = ~d[i]; break;  /* DSTINVERT */
    case 0x0F: for (i = 0; i < nw; i++) d[i] = ~p[i]; break;  /* Pn */
    case 0x5A: for (i = 0; i < nw; i++) d[i] ^= p[i]; break;  /* PATINVERT */
    case 0x66: for (i = 0; i < nw; i++) d[i] ^= s[i]; break;  /* SRCINVERT */
    case 0x88: for (i = 0; i < nw; i++) d[i] &= s[i]; break;  /* SRCAND */
    case 0xEE: for (i = 0; i < nw; i++) d[i] |= s[i]; break;  /* SRCPAINT */
    case 0xBB: for (i = 0; i < nw; i++) d[i] |= ~s[i]; break; /* MERGEPAINT */
    case 0x44: for (i = 0; i < nw; i++) d[i] = s[i] & ~d[i]; break; /* SRCERASE */
    case 0x11: for (i = 0; i < nw; i++) d[i] = ~(s[i] | d[i]); break; /* NOTSRCERASE */
    case 0x22: for (i = 0; i < nw; i++) d[i] &= ~s[i]; break; /* DSna */
    case 0xC0: for (i = 0; i < nw; i++) d[i] = s[i] & p[i]; break; /* MERGECOPY */
    case 0xA0: for (i = 0; i < nw; i++) d[i] &= p[i]; break;  /* DPa */
    case 0xFA: for (i = 0; i < nw; i++) d[i] |= p[i]; break;  /* DPo */
    case 0x0A: for (i = 0; i < nw; i++) d[i] &= ~p[i]; break; /* DPna */
    case 0xFB: for (i = 0; i < nw; i++) d[i] |= p[i] | ~s[i]; break; /* PATPAINT */
    case 0xB8: for (i = 0; i < nw; i++) d[i] = ((d[i] ^ p[i]) & s[i]) ^ p[i]; break; /* PSDPxax */
    case 0xE2: for (i = 0; i < nw; i++) d[i] = ((p[i] ^ d[i]) & s[i]) ^ d[i]; break; /* DSPDxax */
    case 0xCA: for (i = 0; i < nw; i++) d[i] = ((d[i] ^ s[i]) & p[i]) ^ d[i]; break; /* DPSDxax? */
    case 0xAC: for (i = 0; i < nw; i++) d[i] = ((s[i] ^ d[i]) & p[i]) ^ s[i]; break; /* SPDSxax */
    case 0x99: for (i = 0; i < nw; i++) d[i] = ~(s[i] ^ d[i]); break;
    case 0x96: for (i = 0; i < nw; i++) d[i] ^= p[i] ^ s[i]; break;
    default:
        for (i = 0; i < nw; i++)
            d[i] = rop_eval(rop, p ? p[i] : 0, s ? s[i] : 0, d[i]);
        break;
    }
    if (n & 1) {
        BYTE pp = P ? P[n - 1] : 0, ss = S ? S[n - 1] : 0;
        D[n - 1] = (BYTE)rop_eval(rop, pp, ss, D[n - 1]);
    }
}
