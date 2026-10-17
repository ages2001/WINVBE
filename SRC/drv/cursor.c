/*
 * WINVBE - software mouse cursor with save-under and exclusion. The cursor
 * is drawn into video memory only: with a shadow frame buffer the copy
 * never holds it, and the save-under is read from the copy.
 */
#include "drv.h"

#define CMAX 32

volatile BYTE screen_busy;

static BYTE  cur_and[CMAX * 4], cur_xor[CMAX * 4];   /* 32 rows x 4 bytes */
static SHORT cur_hx, cur_hy, cur_w = CMAX, cur_h = CMAX;
static BYTE  cur_valid;          /* a shape is set */
static volatile SHORT cur_x, cur_y;
static BYTE  drawn;              /* save-under valid, cursor on screen */
static SHORT dl, dt, dw, dh;     /* drawn rect (clipped) */
static SHORT doffx, doffy;       /* offset of drawn rect inside the shape */
static BYTE  save[CMAX * CMAX * 4];
static BYTE  rowbuf[CMAX * 4];
static volatile BYTE pending;
static BYTE  excluded;

static void cur_hide(void)
{
    SHORT r;
    if (!drawn) return;
    svga_mark(dl, dt, dl + dw, dt + dh);
    if (g.shadow)               /* the copy is always up to date */
        shadow_flush(dl, dt, dl + dw, dt + dh);
    else
        for (r = 0; r < dh; r++)
            vram_write(dt + r, dl, dw, save + r * CMAX * 4);
    drawn = 0;
}

static void cur_draw(void)
{
    SHORT x0, y0, x1, y1, r, c;
    BYTE Bpp = g.Bpp;
    DWORD white = pix_of(phys_white());
    if (drawn || !cur_valid || !g.enabled || g.in_background) return;
    x0 = cur_x - cur_hx; y0 = cur_y - cur_hy;
    x1 = x0 + cur_w; y1 = y0 + cur_h;
    doffx = doffy = 0;
    if (x0 < 0) { doffx = -x0; x0 = 0; }
    if (y0 < 0) { doffy = -y0; y0 = 0; }
    if (x1 > (SHORT)g.xres) x1 = g.xres;
    if (y1 > (SHORT)g.yres) y1 = g.yres;
    if (x1 <= x0 || y1 <= y0) return;
    dl = x0; dt = y0; dw = x1 - x0; dh = y1 - y0;
    svga_mark(dl, dt, dl + dw, dt + dh);
    for (r = 0; r < dh; r++) {
        BYTE *sv = save + r * CMAX * 4;
        BYTE am, xm;
        WORD sr = r + doffy;
        scr_read(dt + r, dl, dw, sv);
        memcpy(rowbuf, sv, dw * Bpp);
        for (c = 0; c < dw; c++) {
            WORD sc = c + doffx;
            BYTE bit = (BYTE)(0x80 >> (sc & 7));
            am = cur_and[sr * 4 + (sc >> 3)] & bit;
            xm = cur_xor[sr * 4 + (sc >> 3)] & bit;
            if (!am || xm) {
                DWORD v = am ? pix_get(rowbuf + c * Bpp, Bpp) : 0;
                if (xm) v ^= white;
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
    svga_mark(l, t, r, b);
    if (!drawn) return;
    if (r <= dl || l >= dl + dw || b <= dt || t >= dt + dh) return;
    cur_hide();
    excluded = 1;
}

void cursor_unexclude(void)
{
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
    screen_busy++;
    cur_hide();
    if (!cs) {
        cur_valid = 0;
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
