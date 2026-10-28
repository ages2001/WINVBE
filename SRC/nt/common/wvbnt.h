/*
 * WINVBE for Windows NT - definitions shared by the miniport (WINVBE.SYS),
 * the display driver (WINVBE.DLL) and WINVBECF.EXE.
 */
#ifndef WVBNT_H
#define WVBNT_H

#define WVB_VERSION_STR "1.1"

/* Registry values in the device key of the miniport
   (NT 3.x/4: Services\winvbe\Device0, 2000/XP: Control\Video\{..}\0000) */
#define WVB_REG_INFO     L"WinVbe.Info"     /* REG_BINARY WVBINFO, written by the miniport */
#define WVB_REG_REFRESH  L"WinVbe.Refresh"  /* DWORD: refresh rates offered: 0 = monitor (EDID), 1 = all, 2 = 60 Hz only */
#define WVB_REG_PATCH    L"WinVbe.Patch915" /* string: 915PATCH command(s), "" = off */
#define WVB_REG_DAC8     L"WinVbe.Dac8"     /* DWORD: 8 bit DAC in 256 colours (default 1) */
#define WVB_REG_DEBUG    L"WinVbe.Debug"    /* DWORD: debug output to port E9h */
#define WVB_REG_ACCESS   L"WinVbe.Access"   /* DWORD: 0 auto, 1 linear frame buffer only, 2 VGA window only */

/* refresh rates offered in the mode list (mode index = vbe mode * 8 + entry) */
#define WVB_NHZ 6
static const unsigned short wvb_hz[WVB_NHZ] = { 60, 70, 72, 75, 85, 100 };

#define WVB_MAXMODES 200

/* private IOCTL: debug text from the display driver to the miniport */
#define IOCTL_WVB_DEBUG 0x00232000UL   /* CTL_CODE(FILE_DEVICE_VIDEO, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS) */
/* private IOCTL: display start (DirectDraw flip), in: ULONG byte offset
   into video memory; the change is made during the vertical retrace */
#define IOCTL_WVB_SET_START 0x00232004UL   /* function 0x801 */
/* private IOCTL: banked modes (no linear frame buffer): in: ULONG position
   of the VGA window in units of the granularity (VBE 4F05h) */
#define IOCTL_WVB_SET_BANK  0x00232008UL   /* function 0x802 */
/* VIDEO_MODE_INFORMATION.DriverSpecificAttributeFlags of a banked mode:
   this flag | granularity in KB | window size in KB << 16 */
#define WVB_BANKED_FLAG     0x80000000UL

#pragma pack(push, 1)
typedef struct {
    unsigned short w, h;
    unsigned char  bpp;          /* 8, 15, 16, 24, 32 */
    unsigned char  hzmask;       /* bit n = wvb_hz[n] offered */
    unsigned short vbe;          /* VBE mode number */
} WVBMODE;

typedef struct {
    unsigned long  size;         /* sizeof(WVBINFO) */
    unsigned long  ntlevel;      /* 31 = NT 3.1, 35 = 3.5x, 40 = NT4, 50 = 2000/XP and later */
    unsigned short vbever;
    unsigned short pad0;
    unsigned long  memkb;
    char           oem[80];      /* OEM string + vendor/product */
    unsigned char  edid_ok;
    unsigned char  pad1[3];
    unsigned char  edid[128];
    char           patch[160];   /* result of the 915 patch (first line) */
    unsigned long  nmodes;
    WVBMODE        modes[WVB_MAXMODES];
} WVBINFO;
#pragma pack(pop)

#endif
