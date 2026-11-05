/*
 * WINVBE - software mouse cursor with save-under and exclusion. The cursor
 * is drawn into video memory only: with a shadow frame buffer the copy
 * never holds it, and the save-under is read from the copy.
 */
#include "drv.h"

#define CMAX 32

volatile BYTE screen_busy;
static void cur_hide(void);
static void cur_draw(void);
static int  hwc_define(void);
static BYTE hwc;                 /* VMware SVGA II hardware pointer in use */

static BYTE  cur_and[CMAX * 4], cur_xor[CMAX * 4];   /* 32 rows x 4 bytes */
static SHORT cur_hx, cur_hy, cur_w = CMAX, cur_h = CMAX;
static BYTE  cur_valid;          /* a shape is set */
static volatile SHORT cur_x, cur_y;
static BYTE  drawn;              /* save-under valid, cursor on screen */
static SHORT dl, dt, dw, dh;     /* drawn rect (clipped) */
static SHORT doffx, doffy;       /* offset of drawn rect inside the shape */
/* pointer options (WINVBE Mouse settings, [WINVBE] CursorColor/CursorSize):
   color of the white parts (0 = standard, 1 = black with a white outline,
   2.. = colors, black outline), size 1 or 2 (double: save-under from DOS
   memory, the default data group is full) */
static BYTE  save_small[CMAX * CMAX * 4];
static BYTE FAR *save = save_small;
static WORD  save_stride = CMAX * 4;
static BYTE  rowbuf[CMAX * 2 * 4];
BYTE  cur_scale = 1, cur_color;
const DWORD cur_rgb[8] = {
    0xFFFFFFUL, 0x000000UL, 0x0000FFUL, 0x00C000UL, 0xFF0000UL, 0x00FFFFUL, 0xFF00FFUL, 0xFFFF00UL
};
#ifndef OLDWIN
extern DWORD __far __pascal GlobalDosAlloc(DWORD);
#endif

/* new options (Escape WINVBE_CURSOR or the INI at start) */
void cursor_options(WORD color, WORD size)
{
    if (color > 7) color = 0;
#ifndef OLDWIN
    if (size == 2 && save == save_small) {
        DWORD db = GlobalDosAlloc((DWORD)CMAX * 2 * CMAX * 2 * 4);
        if (db) save = (BYTE FAR *)MK_FP((WORD)db, 0);
    }
#endif
#ifndef OLDWIN
    if (dibeng_on) {
        cur_color = (BYTE)color;
        cur_scale = (BYTE)(size == 2 ? 2 : 1);
        dibeng_cursor_again();
        return;
    }
#endif
    screen_busy++;
    cur_hide();
    cur_color = (BYTE)color;
    cur_scale = (size == 2 && save != save_small) ? 2 : 1;
    save_stride = CMAX * cur_scale * 4;
    if (hwc && !hwc_define()) svga_cursor_move(0, 0, 0);
    cur_draw();
    screen_busy--;
}
static volatile BYTE pending;
static BYTE  excluded;

/* VMware SVGA II: the host's hardware pointer (vbe.c) instead of drawing */

/* one pixel of the shape for the host, with the options of cur_draw */
static int hwc_px(WORD x, WORD y, DWORD *xorv)
{
    WORD sx = x / cur_scale, sy = y / cur_scale;
    BYTE bit = (BYTE)(0x80 >> (sx & 7)), am, xm;
    if (sx >= (WORD)cur_w || sy >= (WORD)cur_h) { *xorv = 0; return 1; }
    am = cur_and[sy * 4 + (sx >> 3)] & bit;
    xm = cur_xor[sy * 4 + (sx >> 3)] & bit;
    if (!am && cur_color) *xorv = xm ? cur_rgb[cur_color] : (cur_color == 1 ? 0xFFFFFFUL : 0);
    else if (am && xm && !svga_cursor_inverts()) { *xorv = 0; return 0; }   /* inverted: black */
    else *xorv = xm ? 0xFFFFFFUL : 0;
    return am != 0;
}

/* (re)define the hardware pointer if possible; 1 = it is used */
static int hwc_define(void)
{
    hwc = 0;
    if (!cur_valid || !svga_cursor_possible()) return 0;
    if (!svga_cursor_define(cur_hx * cur_scale, cur_hy * cur_scale, CMAX * cur_scale, CMAX * cur_scale, hwc_px))
        return 0;
    hwc = 1;
    return 1;
}

static void cur_hide(void)
{
    SHORT r;
    if (hwc) return;                    /* the host's pointer stays */
    if (!drawn) return;
    svga_mark(dl, dt, dl + dw, dt + dh);
    if (g.shadow)               /* the copy is always up to date */
        shadow_flush(dl, dt, dl + dw, dt + dh);
    else
        for (r = 0; r < dh; r++)
            vram_write(dt + r, dl, dw, save + r * save_stride);
    drawn = 0;
}

static void cur_draw(void)
{
    SHORT x0, y0, x1, y1, r, c;
    BYTE Bpp = g.Bpp;
    DWORD white = pix_of(phys_white());
    DWORD body = 0;
    BYTE sc = cur_scale;
    if (hwc || (cur_valid && svga_cursor_possible())) {
        /* first time, or a new mode (the host forgot the shape) */
        if ((!hwc || !svga_cursor_defined()) && !hwc_define()) goto soft;
        svga_cursor_move(cur_x, cur_y, (BYTE)(cur_valid && g.enabled && !g.in_background));
        return;
    }
soft:
    if (drawn || !cur_valid || !g.enabled || g.in_background) return;
    if (cur_color) {
        body = pix_of(rgb_to_phys(cur_rgb[cur_color]));   /* the white parts */
    }
    x0 = cur_x - cur_hx * sc; y0 = cur_y - cur_hy * sc;
    x1 = x0 + cur_w * sc; y1 = y0 + cur_h * sc;
    doffx = doffy = 0;
    if (x0 < 0) { doffx = -x0; x0 = 0; }
    if (y0 < 0) { doffy = -y0; y0 = 0; }
    if (x1 > (SHORT)g.lw) x1 = g.lw;
    if (y1 > (SHORT)g.lh) y1 = g.lh;
    if (x1 <= x0 || y1 <= y0) return;
    dl = x0; dt = y0; dw = x1 - x0; dh = y1 - y0;
    svga_mark(dl, dt, dl + dw, dt + dh);
    for (r = 0; r < dh; r++) {
        BYTE FAR *sv = save + r * save_stride;
        BYTE am, xm;
        WORD sr = (r + doffy) / sc;
        scr_read(dt + r, dl, dw, sv);
        far_memcpy(rowbuf, sv, dw * Bpp);
        for (c = 0; c < dw; c++) {
            WORD scol = (c + doffx) / sc;
            BYTE bit = (BYTE)(0x80 >> (scol & 7));
            am = cur_and[sr * 4 + (scol >> 3)] & bit;
            xm = cur_xor[sr * 4 + (scol >> 3)] & bit;
            if (!am || xm) {
                DWORD v;
                if (!am && cur_color)               /* the pointer's own colors */
                    v = xm ? body : (cur_color == 1 ? white : 0);
                else {
                    v = am ? pix_get(rowbuf + c * Bpp, Bpp) : 0;
                    if (xm) v ^= white;
                }
                pix_put(rowbuf + c * Bpp, v, Bpp);
            }
        }
        vram_write(dt + r, dl, dw, rowbuf);
    }
    drawn = 1;
}

/* the same with 32-bit coordinates: GDI passes rectangles up to 32767
   (e.g. ExtTextOut's opaque rectangle), where r + 8 would wrap around */
void cursor_exclude_l(LONG l, LONG t, LONG r, LONG b)
{
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > 32767) r = 32767;
    if (b > 32767) b = 32767;
    if (r <= l || b <= t) return;
    cursor_exclude((SHORT)l, (SHORT)t, (SHORT)r, (SHORT)b);
}

/* called with screen_busy already incremented */
void cursor_exclude(SHORT l, SHORT t, SHORT r, SHORT b)
{
#ifndef OLDWIN
    if (dibeng_on) { dibeng_begin(l, t, r, b); return; }   /* the DIB engine's cursor */
#endif
    svga_mark(l, t, r, b);
    if (!drawn) return;
    if (r <= dl || l >= dl + dw || b <= dt || t >= dt + dh) return;
    cur_hide();
    excluded = 1;
}

void cursor_unexclude(void)
{
#ifndef OLDWIN
    if (dibeng_on) { dibeng_end(); return; }
#endif
    if (screen_busy != 1) return;
    if (excluded || pending) {
        excluded = 0;
        pending = 0;
        cur_hide();
        cur_draw();
    }
    svga_flush();
}

void cursor_hide_all(void)
{
    screen_busy++;
    if (hwc) svga_cursor_move(0, 0, 0);
    cur_hide();
    svga_flush();
    screen_busy--;
}

void cursor_show(void)
{
    screen_busy++;
    cur_hide();
    cur_draw();
    svga_flush();
    screen_busy--;
}

/* debug: does a span of a screen write hit the drawn cursor? */
int cursor_hit(WORD y, WORD x, WORD n)
{
    if (!drawn || (SHORT)y < dt || (SHORT)y >= dt + dh) return 0;
    if ((SHORT)(x + n) > dl && (SHORT)x < dl + dw) {
        dbg_printf("[busy %u excl %u pend %u cur %d,%d %dx%d]", screen_busy, excluded, pending, dl, dt, dw, dh);
        return 1;
    }
    return 0;
}

void cursor_invalidate(void)
{
    drawn = 0;
}

void EXPORT SetCursor(LPCURSORSHAPE cs)
{
    win30_icon_fix();
#ifndef OLDWIN
    if (dibeng_on) { dibeng_setcursor(cs); return; }
#endif
    screen_busy++;
    cur_hide();
    if (!cs) {
        cur_valid = 0;
        if (hwc) svga_cursor_move(0, 0, 0);
    } else {
        SHORT w = cs->csWidth, h = cs->csHeight, wb = cs->csWidthBytes, r, i;
        LPBYTE andm = (LPBYTE)(cs + 1);
        LPBYTE xorm = andm + wb * h;
        if (g.debug) {
            LPBYTE q = (LPBYTE)cs;
            WORD k;
            dbg_printf("SetCursor:");
            for (k = 0; k < 48; k++) dbg_printf(" %02x", q[k]);
            dbg_printf("\n");
        }
        if (w > CMAX) w = CMAX;
        if (h > CMAX) h = CMAX;
        memset(cur_and, 0xFF, sizeof(cur_and));
        memset(cur_xor, 0, sizeof(cur_xor));
        for (r = 0; r < h; r++)
            for (i = 0; i < 4 && i < wb; i++) {
                cur_and[r * 4 + i] = andm[r * wb + i];
                cur_xor[r * 4 + i] = xorm[r * wb + i];
            }
        cur_hx = cs->csHotX; cur_hy = cs->csHotY;
        cur_w = w; cur_h = h;
        cur_valid = 1;
        {
            BYTE had = hwc;
            if (!hwc_define() && had) svga_cursor_move(0, 0, 0);
        }
        cur_draw();
    }
    svga_flush();
    screen_busy--;
}

void EXPORT MoveCursor(SHORT x, SHORT y)
{
    cur_x = x; cur_y = y;
    if (screen_busy) { pending = 1; return; }
    screen_busy++;
    cur_hide();
    cur_draw();
    pending = 0;
    svga_flush();
    screen_busy--;
}

void EXPORT CheckCursor(void)
{
#ifndef OLDWIN
    if (dibeng_on) {
        /* the DIB engine's cursor; WINVBE still watches the mode (USER
           calls this from its timer) */
        typedef void (__far __pascal *PF_CHECK)(LPVOID);
        if (!screen_busy) win9x_check_screen();
        ((PF_CHECK)dibe_tab[29])((LPVOID)scr_pdev);
        return;
    }
#endif
    if (screen_busy) return;
    win9x_check_screen();
    if (pending || (!drawn && cur_valid)) {
        screen_busy++;
        pending = 0;
        cur_hide();
        cur_draw();
        svga_flush();
        screen_busy--;
    }
}

typedef struct { SHORT dpXRate, dpYRate; } CURSORINFO;

WORD EXPORT Inquire(CURSORINFO FAR *ci)
{
    TRACE("Inquire\n");
    ci->dpXRate = 1;
    ci->dpYRate = 1;
    return sizeof(CURSORINFO);
}
