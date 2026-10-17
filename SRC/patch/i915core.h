/*
 * 915PATCH - Intel video BIOS mode table patcher (core, hardware independent)
 *
 * In the spirit of 915resolution: the video BIOS of Intel 830 ... G45 class
 * integrated graphics keeps a table of its modes (5 byte entries, Intel mode
 * numbers 30h..5Fh) with a pointer to a resolution/timing block for each.
 * Overwriting one entry with another resolution makes the VESA BIOS offer
 * that resolution (e.g. the 1280x800 or 1400x1050 panel of a notebook).
 * The video BIOS lives in shadow RAM, so the patch lasts until the next
 * reset and has to be applied at every boot (AUTOEXEC.BAT).
 */
#ifndef I915CORE_H
#define I915CORE_H

#ifdef __WATCOMC__
#define FARP __far
#else
#define FARP
#endif

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;

#define BT_UNKNOWN 0
#define BT_1       1    /* 830..865G, GM965: 8 byte block, packed 12 bit sizes */
#define BT_2       2    /* 915..945: xchars, ychars, 4 bytes, 3 modelines of 32 bytes */
#define BT_3       3    /* 946GZ/965/G3x/G4x: 6 bytes, 3 modelines of 38 bytes */
#define BT_4       4    /* GM45 class: EDID detailed timings (18 bytes), one per refresh rate */

typedef struct {
    u8 FARP *bios;      /* video BIOS image (segment C000h) */
    u32 size;           /* its size in bytes */
    u16 table;          /* offset of the mode table, 0 = not found */
    u16 count;          /* number of entries */
    int type;           /* BT_x */
} VBMAP;

typedef struct {
    u8  mode;           /* Intel mode number */
    u8  bpp;
    u16 x, y;           /* current resolution */
    u16 res;            /* offset of the resolution block */
} VBMODE;

int  vb_find_table(VBMAP *m);
int  vb_detect_type(VBMAP *m);
int  vb_locate(VBMAP *m);          /* find + detect, skipping false matches */
int  vb_force_type(VBMAP *m, int t);  /* /TYPE:n as 915resolution -t */
int  vb_find_next(VBMAP *m, u32 start); /* next table candidate from offset start */
int  vb_get_mode(VBMAP *m, u16 i, VBMODE *out);
int  vb_set_mode(VBMAP *m, u8 mode, u16 x, u16 y, u8 bpp, u16 htotal, u16 vtotal);
const char *vb_type_name(int t);

/* GTF timings (as used by 915resolution for the modelines) */
void vb_gtf(u16 x, u16 y, u16 freq, u32 *clock_khz, u16 *hss, u16 *hse, u16 *hbl,
            u16 *vss, u16 *vse, u16 *vbl);

#endif
