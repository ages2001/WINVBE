/* WINVBE - VBE / EDID query shared by the Windows configuration program
   (WINVBECF.EXE) and the DOS installer (INSTALL.EXE). */
#ifndef VBEQ_H
#define VBEQ_H

typedef unsigned char  QBYTE;
typedef unsigned short QWORD;
typedef unsigned long  QDWORD;

#define VQ_MAXMODES 160

typedef struct {
    QWORD mode;
    QWORD w, h;
    QBYTE bpp;            /* 8, 15, 16, 24, 32 */
    QBYTE linear;         /* LFB available */
    QBYTE banked;         /* windowed (bank switching) available */
    QBYTE pad;
    QWORD pitch;
    QDWORD lfb;
} VQMODE;

typedef struct {
    QWORD  ver;
    QDWORD memkb;
    char   oem[64];
    char   vendor[64];
    char   product[64];
    QWORD  nmodes;
    VQMODE modes[VQ_MAXMODES];
    /* EDID */
    QBYTE  edid_ok;
    QBYTE  edid[128];
    char   monname[16];
    char   mfg[4];
    QWORD  natw, nath, nathz;
    QBYTE  range_ok;
    QBYTE  vmin, vmax, hmin, hmax;      /* Hz / kHz */
    QWORD  maxclk;                      /* MHz */
    QBYTE  digital;                     /* digital input -> LCD/flat panel likely */
    /* Bochs/QEMU/VirtualBox DISPI */
    QWORD  dispi_id;
    QWORD  dispi_memkb;
} VQINFO;

int  vq_query(VQINFO __far *vi);        /* returns 0 if no VBE */
void vq_sort(VQINFO __far *vi);

#endif
