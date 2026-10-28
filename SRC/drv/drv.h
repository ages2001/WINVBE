/*
 * WINVBE - Universal VESA/VGA/SVGA display driver for Windows 3.0 (and 3.1)
 * Common definitions.
 *
 * Copyright (c) 2026 Atakan & Claude.
 */
#ifndef DRV_H
#define DRV_H

#include <string.h>

#ifdef __WATCOMC__
#include <i86.h>
typedef unsigned char  BYTE;
typedef unsigned short WORD;
typedef unsigned long  DWORD;
typedef short          SHORT;
typedef long           LONG;
typedef int            BOOL;
typedef void __far    *LPVOID;
typedef BYTE __far    *LPBYTE;
typedef WORD __far    *LPWORD;
typedef DWORD __far   *LPDWORD;
typedef char __far    *LPSTR;
#else
#include "hostcompat.h"
#endif

#define TRUE  1
#define FALSE 0
#ifndef NULL
#define NULL  0
#endif

#ifdef __WATCOMC__
#define FAR     __far
#define NEAR    __near
#define PASCAL  __pascal
/* exported entry: far pascal, load DGROUP */
#define EXPORT  __far __pascal __loadds
#endif

#define LOWORD(l) ((WORD)(l))
#define HIWORD(l) ((WORD)((DWORD)(l) >> 16))
#define MAKELONG(lo,hi) ((DWORD)(((WORD)(lo)) | (((DWORD)(WORD)(hi)) << 16)))
#define SELOF(p)  FP_SEG(p)
#define OFFOF(p)  FP_OFF(p)

/* ------------------------------------------------------------------ */
/* GDI structures (Windows 3.0 DDK layout)                             */
/* ------------------------------------------------------------------ */
#pragma pack(push, 1)

typedef struct { SHORT x, y; } GPOINT;
typedef struct { SHORT left, top, right, bottom; } GRECT;
typedef GPOINT FAR *LPGPOINT;
typedef GRECT FAR *LPGRECT;

/* GDI physical bitmap (memory bitmaps) */
typedef struct {
    SHORT bmType;           /* 0 = memory bitmap, else physical device */
    WORD  bmWidth;
    WORD  bmHeight;
    WORD  bmWidthBytes;
    BYTE  bmPlanes;
    BYTE  bmBitsPixel;
    LPBYTE bmBits;
    DWORD bmWidthPlanes;
    LPVOID bmlpPDevice;
    WORD  bmSegmentIndex;
    WORD  bmScanSegment;
    WORD  bmFillBytes;
    WORD  futureUse4;
    WORD  futureUse5;
} GBITMAP;
typedef GBITMAP FAR *LPGBITMAP;

/* our screen PDEVICE - starts like a GBITMAP */
typedef struct {
    GBITMAP bm;
    WORD    magic;          /* 'VW' */
    WORD    pad;
} PDEVICE;
typedef PDEVICE FAR *LPPDEVICE;
#define PDEV_TYPE   0x2000
#define PDEV_MAGIC  0x5756

typedef struct {
    SHORT Rop2;
    SHORT bkMode;
    DWORD bkColor;
    DWORD TextColor;
    SHORT TBreakExtra;
    SHORT BreakExtra;
    SHORT BreakErr;
    SHORT BreakRem;
    SHORT BreakCount;
    SHORT CharacterExtra;
    DWORD LbkColor;
    DWORD LTextColor;
} DRAWMODE;
typedef DRAWMODE FAR *LPDRAWMODE;

typedef struct {
    WORD   dfType;
    WORD   dfPoints;
    WORD   dfVertRes;
    WORD   dfHorizRes;
    WORD   dfAscent;
    WORD   dfInternalLeading;
    WORD   dfExternalLeading;
    BYTE   dfItalic;
    BYTE   dfUnderline;
    BYTE   dfStrikeOut;
    WORD   dfWeight;
    BYTE   dfCharSet;
    WORD   dfPixWidth;
    WORD   dfPixHeight;
    BYTE   dfPitchAndFamily;
    WORD   dfAvgWidth;
    WORD   dfMaxWidth;
    BYTE   dfFirstChar;
    BYTE   dfLastChar;
    BYTE   dfDefaultChar;
    BYTE   dfBreakChar;
    WORD   dfWidthBytes;
    DWORD  dfDevice;
    DWORD  dfFace;
    DWORD  dfBitsPointer;
    DWORD  dfBitsOffset;
} FONTINFO;
typedef FONTINFO FAR *LPFONTINFO;

typedef struct {
    SHORT ftHeight;
    SHORT ftWidth;
    SHORT ftEscapement;
    SHORT ftOrientation;
    SHORT ftWeight;
    BYTE  ftItalic;
    BYTE  ftUnderline;
    BYTE  ftStrikeOut;
    BYTE  ftOutPrecision;
    BYTE  ftClipPrecision;
    WORD  ftAccelerator;
    SHORT ftOverhang;
} TEXTXFORM;
typedef TEXTXFORM FAR *LPTEXTXFORM;

typedef struct {
    WORD  lopnStyle;
    GPOINT lopnWidth;
    DWORD lopnColor;
} LOGPEN;
typedef LOGPEN FAR *LPLOGPEN;

typedef struct {
    WORD  lbStyle;
    DWORD lbColor;          /* for BS_PATTERN: far pointer to GBITMAP */
    WORD  lbHatch;
    DWORD lbBkColor;
} LOGBRUSH;
typedef LOGBRUSH FAR *LPLOGBRUSH;

typedef struct {
    SHORT csHotX;
    SHORT csHotY;
    SHORT csWidth;
    SHORT csHeight;
    SHORT csWidthBytes;
    SHORT csColor;
} CURSORSHAPE;
typedef CURSORSHAPE FAR *LPCURSORSHAPE;

typedef struct {
    DWORD biSize;
    LONG  biWidth;
    LONG  biHeight;
    WORD  biPlanes;
    WORD  biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG  biXPelsPerMeter;
    LONG  biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BMIHEADER;
typedef struct {
    DWORD bcSize;
    WORD  bcWidth;
    WORD  bcHeight;
    WORD  bcPlanes;
    WORD  bcBitCount;
} BMCHEADER;

/* GDIINFO for Windows 3.0 */
typedef struct {
    SHORT dpVersion;
    SHORT dpTechnology;
    SHORT dpHorzSize;
    SHORT dpVertSize;
    SHORT dpHorzRes;
    SHORT dpVertRes;
    SHORT dpBitsPixel;
    SHORT dpPlanes;
    SHORT dpNumBrushes;
    SHORT dpNumPens;
    SHORT futureuse;
    SHORT dpNumFonts;
    SHORT dpNumColors;
    SHORT dpDEVICEsize;
    WORD  dpCurves;
    WORD  dpLines;
    WORD  dpPolygonals;
    WORD  dpText;
    WORD  dpClip;
    WORD  dpRaster;
    SHORT dpAspectX;
    SHORT dpAspectY;
    SHORT dpAspectXY;
    SHORT dpStyleLen;
    GPOINT dpMLoWin;
    GPOINT dpMLoVpt;
    GPOINT dpMHiWin;
    GPOINT dpMHiVpt;
    GPOINT dpELoWin;
    GPOINT dpELoVpt;
    GPOINT dpEHiWin;
    GPOINT dpEHiVpt;
    GPOINT dpTwpWin;
    GPOINT dpTwpVpt;
    SHORT dpLogPixelsX;
    SHORT dpLogPixelsY;
    SHORT dpDCManage;
    SHORT dpCaps1;
    SHORT futureuse4;
    SHORT futureuse5;
    SHORT futureuse6;
    SHORT futureuse7;
    SHORT dpPalColors;
    SHORT dpPalReserved;
    SHORT dpPalResolution;
} GDIINFO;
typedef GDIINFO FAR *LPGDIINFO;

/* physical pen */
typedef struct {
    DWORD color;            /* physical color */
    WORD  style;
    WORD  width;
} PPEN;
typedef PPEN FAR *LPPPEN;

/* physical brush */
#define PB_COLORBYTES   (8*8*4)
typedef struct {
    WORD  style;            /* BS_xxx */
    BYTE  flags;            /* PBF_xxx */
    BYTE  hatch;
    DWORD fg;               /* physical fg color (solid/hatch) */
    DWORD bg;               /* physical bg color (hatch) */
    BYTE  mono[8];          /* rendering for monochrome destinations */
    BYTE  mask[8];          /* hatch mask (1 = fg) / raw bits of mono pattern */
    BYTE  pat[PB_COLORBYTES]; /* color pattern, 8 rows, 32 bytes/row */
} PBRUSH;
typedef PBRUSH FAR *LPPBRUSH;

#define PBF_SOLID     0x01  /* solid color, pat uniform */
#define PBF_MONOPAT   0x02  /* mono pattern brush, colors from DRAWMODE */
#define PBF_HATCH     0x04  /* hatched: mono[]=hatch mask */
#define PBF_DITHER    0x08  /* dithered (8bpp) */

#pragma pack(pop)

/* GDI constants */
#define OBJ_PEN     1
#define OBJ_BRUSH   2
#define OBJ_FONT    3

#define BS_SOLID    0
#define BS_HOLLOW   1
#define BS_HATCHED  2
#define BS_PATTERN  3

#define PS_SOLID       0
#define PS_DASH        1
#define PS_DOT         2
#define PS_DASHDOT     3
#define PS_DASHDOTDOT  4
#define PS_NULL        5
#define PS_INSIDEFRAME 6

#define TRANSPARENT 1
#define OPAQUE      2

#define OS_ARC          3
#define OS_SCANLINES    4
#define OS_RECTANGLE    6
#define OS_ELLIPSE      7
#define OS_MARKER       8
#define OS_POLYLINE     18
#define OS_WINDPOLYGON  20
#define OS_POLYGON      22
#define OS_PIE          23
#define OS_POLYMARKER   24
#define OS_CHORD        39
#define OS_CIRCLE       55
#define OS_BEGINNSCAN   80
#define OS_ENDNSCAN     81

#define ETO_OPAQUE      0x0002
#define ETO_CLIPPED     0x0004

/* ------------------------------------------------------------------ */
/* driver internal                                                     */
/* ------------------------------------------------------------------ */

/* access methods */
#define ACC_NONE    0
#define ACC_LFB     1       /* linear frame buffer through selector tiles */
#define ACC_BANKED  2       /* VBE window, bank switching */
#define ACC_VGA13   3       /* plain VGA mode 13h */

/* pixel formats */
#define PF_8        0
#define PF_555      1
#define PF_565      2
#define PF_24       3
#define PF_32       4

typedef struct {
    WORD  mode;         /* VBE mode number, 0x13 = VGA */
    WORD  xres, yres;
    BYTE  bpp;          /* 8,15,16,24,32 */
    BYTE  Bpp;          /* bytes per pixel */
    BYTE  pf;           /* PF_xx */
    BYTE  memmodel;
    WORD  attr;
    WORD  pitch;        /* banked pitch */
    WORD  lpitch;       /* linear pitch (VBE3) */
    BYTE  winAattr, winBattr;
    WORD  wingran;      /* KB */
    WORD  winsize;      /* KB */
    WORD  winAseg, winBseg;
    DWORD winfunc;
    DWORD physbase;
    BYTE  rsize, rpos, gsize, gpos, bsize, bpos;
    WORD  pages;
} MODEINFO;

#define MAX_MODES 96

/* row buffer size in bytes; smaller for Windows 1.x/2.x (real mode, every
   byte of the fixed data segment is taken from the 640K) */
#ifdef OLDWIN
#define ROWBUF   4096
#define CHUNKPX  1024
#else
#define ROWBUF   8192
#define CHUNKPX  2048
#endif

/* global driver state */
typedef struct {
    /* environment */
    WORD  winflags;
    BYTE  pmode;            /* protected mode? */
    BYTE  enhanced;
    BYTE  cpu386;
    WORD  ahincr;
    WORD  a000sel;
    WORD  hInst;
    WORD  dosbuf_sel, dosbuf_seg;

    /* vbe */
    WORD  vbever;
    WORD  vbecaps_lo;
    WORD  totalmem64k;
    char  oemstr[64];
    BYTE  have_edid;
    BYTE  edid[128];
    WORD  edid_w, edid_h, edid_hz;

    /* current mode */
    MODEINFO mi;
    BYTE  access;
    BYTE  enabled;
    BYTE  dacbits;
    BYTE  palettized;
    WORD  xres, yres, pitch;
    BYTE  Bpp, pf, bpp;
    DWORD fbsize;
    DWORD fbofs;                /* screen start in video memory (VGA planes kept free) */

    /* banked */
    WORD  winsel;           /* selector of the window used for writing */
    WORD  winrsel;          /* ... for reading (window B on cards with a read-only B) */
    BYTE  wshift;           /* log2 of the window size (16 = 64 KB) */
    WORD  bankmul;          /* 64K / granularity */
    WORD  curbank;
    BYTE  use_winfunc;      /* real-mode direct far call */
    BYTE  winread_ok, winwrite_ok;

    /* lfb */
    WORD  lfbsel;           /* first selector of tile array */
    WORD  lfbtiles;
    WORD  lfbinc;           /* selector increment of tile array */
    DWORD lfblin;
    BYTE  lfb_state;        /* 0 = not checked, 1 = verified, 2 = unusable */
    BYTE  lfb_check;        /* SYSTEM.INI LfbCheck (default 1) */

    /* palette (8bpp) */
    BYTE  pal[256*4];       /* R,G,B,flags */
    WORD  xlat[256];        /* palette translate */
    BYTE  xlat_identity;

    /* settings */
    WORD  req_w, req_h, req_bpp, req_mode, req_hz;
    BYTE  req_access;       /* 0 auto,1 lfb,2 banked */
    WORD  dpi;
    BYTE  debug;
    BYTE  refresh_set;
    WORD  refresh_cur;      /* achieved refresh, 0.01 Hz */
    BYTE  use_dispi;
    BYTE  dibwords;         /* GDI 3.0: DIB color tables hold WORD physical colors */
    WORD  winver;
    BYTE  win9x;            /* Windows 95/98/ME (registry settings) */
    DWORD devcolormatch;    /* GDI.449 */

    /* repaint */
    DWORD repaint_proc;
    BYTE  repaint_disable;
    BYTE  repaint_pending;
    BYTE  in_background;
    BYTE  noleave;
    BYTE  vddmode;
    BYTE  vdd_assist;           /* 3.x enhanced: INT 2Fh 4000h done */
    BYTE  svga;                 /* VMware SVGA II mode */
    BYTE  dpms;                 /* monitor power state set by the DPMS escape */
    BYTE  fg_4002;              /* INT 2Fh 4002h has set the mode (WVBN_FOREGROUND not needed) */
    /* settings read once at start: mode sets also run in contexts where
       KERNEL (profile/registry) may not be called, e.g. after a resume */
    BYTE  ini_timing;           /* Timing: 0 CVT, 1 reduced blanking, 2 auto */
    BYTE  ini_vgareserve, ini_dac8, ini_repost, ini_vmaccel, ini_vmfifo;
    BYTE  ini_shadow, ini_wc;

    /* shadow frame buffer: a copy of the screen in system memory. Reading
       video memory over PCI/AGP/PCIe is very slow (uncached, one bus round
       trip per access): all reads (scrolling, ROPs, cursor) use the copy,
       writes go to both */
    BYTE  shadow;               /* in use */
    BYTE  wc;                   /* frame buffer write combining: 1 PAT, 2 MTRR, 3 was on */
    WORD  shsel, shtiles;       /* tile selectors, 64 KB each (increment lfbinc) */
    DWORD shlin, shsize, shhandle;
} DRVSTATE;

extern DRVSTATE g;
extern MODEINFO modes[MAX_MODES];
extern WORD nmodes;

/* surface descriptor */
typedef struct {
    BYTE  screen;   /* 1 = screen */
    BYTE  mono;     /* 1 = 1bpp */
    BYTE  Bpp;      /* bytes per pixel in working format (mono: 1) */
    BYTE  planar;   /* number of planes of a foreign planar (EGA/VGA) bitmap */
    WORD  width, height;
    WORD  pitch;
    WORD  sel, off;     /* memory bitmap base */
    WORD  scanseg;      /* scans per segment (0: unlimited) */
    WORD  seginc;
    DWORD flat;         /* > 64 KB in one (32-bit) segment: linear base */
    WORD  rsel;         /* ... and a selector for its rows */
} SURF;

/* row buffers */
extern BYTE bufD[ROWBUF], bufS[ROWBUF], bufP[ROWBUF], bufT[ROWBUF];
void load_src(SURF *s, SURF *d, WORD y, WORD x, WORD n, LPDRAWMODE dm);
void stretch_row(BYTE *out, const BYTE *src, WORD sw, WORD dw, WORD dfirst, WORD dn, BYTE flip, BYTE Bpp);

/* screen busy / cursor */
extern volatile BYTE screen_busy;

/* ------------------------------------------------------------------ */
/* prototypes                                                          */
/* ------------------------------------------------------------------ */
/* util.c */
void dbg_puts(const char *s);
void dbg_printf(const char *fmt, ...);
/* safe in if/else: "if (a) DBG(...); else b;" */
#define DBG if (!g.debug) {} else dbg_printf
extern WORD trace_n;
WORD trace_fn(const char *s);
#define TRACE(s) WORD trace_dummy = trace_fn(s)
#ifdef __WATCOMC__
/* 32-bit moves: a PCI/PCIe frame buffer takes one bus cycle per access */
void far_memcpy(void FAR *d, const void FAR *s, WORD n);
#pragma aux far_memcpy = \
    "push ds" "mov ds,dx" "mov ax,cx" "shr cx,2" "rep movsd" "mov cx,ax" "and cx,3" "rep movsb" "pop ds" \
    parm [es di] [dx si] [cx] modify [ax cx si di];
void far_memset(void FAR *d, BYTE v, WORD n);
#pragma aux far_memset = \
    "push eax" "mov ah,al" "mov bx,ax" "shl eax,16" "mov ax,bx" "mov bx,cx" "shr cx,2" "rep stosd" "mov cx,bx" \
    "and cx,3" "rep stosb" "pop eax" parm [es di] [al] [cx] modify [ax bx cx di];
/* n DWORDs of value v */
void far_memset32(void FAR *d, DWORD v, WORD n);
#pragma aux far_memset32 = \
    "push eax" "push edx" "shl edx,16" "mov dx,ax" "mov eax,edx" "rep stosd" "pop edx" "pop eax" \
    parm [es di] [dx ax] [cx] modify [cx di];
extern void __far rm_int10(void);       /* real-mode INT 10h with rmr */
extern void __far rm_winfunc(WORD bank); /* call VBE window function */
extern void __far hook_int2f(void);
extern void __far unhook_int2f(void);
extern WORD __far vdd_assist(WORD on);
#pragma aux rm_int10 "*"
#pragma aux rm_winfunc "*" parm [dx]
#pragma aux hook_int2f "*"
#pragma aux unhook_int2f "*"
#pragma aux vdd_assist "*" parm caller [] value [ax] modify [ax]
#endif
WORD get_ini_int(const char *key, WORD def);
void get_ini_str(const char *key, const char *def, char *buf, WORD n);
DWORD isqrt32(DWORD v);

/* vbe.c */
typedef struct {
    DWORD edi, esi, ebp, res, ebx, edx, ecx, eax;
    WORD flags, es, ds, fs, gs, ip, cs, sp, ss;
} RMREGS;
extern RMREGS rmr;
int  int10(RMREGS *r);
LPBYTE rm_ptr(WORD seg, WORD off);
int  vbe_probe(void);
int  vbe_select_mode(void);
int  vbe_set_mode(void);
void vbe_text_mode(void);
void vbe_leave_bg(void);
void vga_snapshot(void);
int vga_changed(void);
int vga_restore(void);
void vbe_dispi_off(void);
void svga_sync(void);
void svga_mark(SHORT l, SHORT t, SHORT r, SHORT b);
void svga_flush(void);
int  svga_copy(SHORT sx, SHORT sy, SHORT dx, SHORT dy, SHORT w, SHORT h);
int  svga_fill(DWORD color, SHORT x, SHORT y, SHORT w, SHORT h);
void svga_off(void);
int  svga_repair(void);
void svga_reverify(void);
void win9x_check_screen(void);
void vbe_dispi_snapshot(void);
int vbe_dispi_repair(void);
void vbe_set_bank(WORD bank);
void vbe_set_dac(WORD start, WORD count, const BYTE *rgbx);
/* INT 2Fh AX = WVB_NOTIFY from WINVBE.386, BX = event */
#define WVB_NOTIFY   0xB57E
#define WVBN_RESUME  1
#define WVBN_SUSPEND 2
#define WVBN_MODECHECK  3   /* a background VM (windowed DOS box) set a mode through the BIOS */
#define WVBN_FOREGROUND 4   /* Windows has the screen back from a full-screen VM */
WORD vbe_pm_caps(void);
void vbe_dpms(WORD state);
int  vbe_repost(void);
int  vbe_mode_intact(void);
int  vbe_virtual_hw(void);
void vbe_bda_save(void);
void vbe_bda_restore(void);
void hw_restore_state(void);
WORD sel_alloc(WORD count);
void sel_free(WORD sel, WORD count);
int  sel_setbase(WORD sel, DWORD base);
DWORD sel_getbase(WORD sel);
int  sel_setlimit(WORD sel, DWORD limit);
DWORD map_phys(DWORD phys, DWORD size);

/* screen access (surf.c) */
void surf_from_dev(SURF *s, LPVOID lpDev);
LPBYTE mem_row(SURF *s, WORD y);
void scr_read(WORD y, WORD x, WORD n, BYTE *buf);
void scr_write(WORD y, WORD x, WORD n, const BYTE *buf);
void scr_fill(WORD y, WORD x, WORD n, DWORD pix);
void vram_write(WORD y, WORD x, WORD n, const BYTE *buf);   /* not the shadow (cursor) */
void shadow_flush(WORD l, WORD t, WORD r, WORD b);           /* shadow -> video memory */
int  shadow_alloc(DWORD size);
void shadow_free(void);
WORD vxd_wc(DWORD lin, DWORD size, DWORD phys);
void row_read(SURF *s, WORD y, WORD x, WORD n, BYTE *buf);  /* to working format */
void row_write(SURF *s, WORD y, WORD x, WORD n, const BYTE *buf);
void pix_fill(BYTE *buf, WORD n, DWORD pix, BYTE Bpp);
DWORD pix_get(const BYTE *p, BYTE Bpp);
void pix_put(BYTE *p, DWORD v, BYTE Bpp);

/* color.c */
DWORD rgb_to_phys(DWORD rgb);
DWORD phys_to_rgb(DWORD phys);
DWORD color_for_dev(LPVOID lpDev, DWORD rgb);
BYTE  rgb_is_white(DWORD rgb);
void  palette_init(void);
DWORD phys_white(void);
#define PHYS_MONO   0x01000000UL    /* mono flag (white) in physical colors */
#define PIXMASK     0x00FFFFFFUL
BYTE  mono_of(DWORD phys);
DWORD pix_of(DWORD phys);
BYTE  nearest_index(BYTE r, BYTE gg, BYTE b);
BYTE  dither_index(DWORD rgb, WORD x, WORD y);
DWORD pixel_to_rgb(DWORD pixel);
DWORD pixel_to_phys(DWORD pixel);
DWORD word_to_phys(WORD w);
void  invcache_reset(void);

/* rop.c */
void rop_apply(BYTE rop3, BYTE *D, const BYTE *S, const BYTE *P, WORD nbytes);
BYTE rop2_to_rop3(WORD rop2);
#define ROP_NEEDS_S(r) ((((r) >> 2) ^ (r)) & 0x33)
#define ROP_NEEDS_P(r) ((((r) >> 4) ^ (r)) & 0x0F)
#define ROP_NEEDS_D(r) ((((r) >> 1) ^ (r)) & 0x55)

/* blt.c */
void pattern_row(BYTE *dst, LPPBRUSH br, LPDRAWMODE dm, SURF *d, WORD y, WORD x, WORD n);
void fill_rect(SURF *d, SHORT x, SHORT y, SHORT w, SHORT h, BYTE rop3, LPPBRUSH br, LPDRAWMODE dm);
void solid_rect(SURF *d, SHORT x, SHORT y, SHORT w, SHORT h, DWORD phys, BYTE rop3);

/* cursor.c */
void cursor_exclude(SHORT l, SHORT t, SHORT r, SHORT b);
void cursor_exclude_l(LONG l, LONG t, LONG r, LONG b);
void cursor_unexclude(void);
void cursor_hide_all(void);
void cursor_show(void);
#define BEGIN_SCREEN(s,l,t,r,b) if ((s)->screen) { screen_busy++; cursor_exclude(l,t,r,b); }
#define END_SCREEN(s) if ((s)->screen) { cursor_unexclude(); screen_busy--; }

/* enable.c */
int  enable_hw(BYTE restore);
extern BYTE static_pal[20*4];

#endif
