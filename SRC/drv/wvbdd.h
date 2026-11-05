/*
 * WINVBE - DirectDraw HAL for Windows 95/98/ME: the block shared by the
 * display driver (16-bit, WINVBE.DRV, ddhal.c) and its 32-bit part
 * (WVBDD32.DLL, dd9x/wvbdd32.c).
 *
 * The display driver allocates the block in global (shared) memory and
 * passes its linear address to DirectDraw as the context of the 32-bit
 * part (DDGET32BITDRIVERNAME). DriverInit of the 32-bit part puts the
 * addresses of its callbacks here; the display driver builds the HAL
 * information (DDHALINFO) for the current mode from them and hands it to
 * DirectDraw (DDCREATEDRIVEROBJECT -> DDHAL_SetInfo).
 *
 * The structures below have the layout of DDHALINFO and its parts, written
 * with fixed-size fields only, so that the 16-bit and the 32-bit compiler
 * read them alike; wvbdd32.c checks them against the DirectDraw headers.
 * Pointers to structures are 16:16 far pointers (DDHAL_SetInfo is a 16-bit
 * function), the callback addresses are 32-bit flat addresses.
 */
#ifndef WVBDD_H
#define WVBDD_H

typedef unsigned long  WD32;
typedef unsigned short WD16;

#pragma pack(push, 1)

typedef struct {
    WD32 dwSize, dwFlags, dwFourCC, dwRGBBitCount;
    WD32 dwRBitMask, dwGBitMask, dwBBitMask, dwRGBAlphaBitMask;
} WD_PIXFMT;

typedef struct {                    /* VIDMEM */
    WD32 dwFlags;
    WD32 fpStart;
    WD32 dwWidth;                   /* fpEnd for a linear heap */
    WD32 ddsCaps, ddsCapsAlt;
    WD32 dwHeight;
} WD_VIDMEM;

typedef struct {                    /* VIDMEMINFO */
    WD32 fpPrimary;
    WD32 dwFlags;
    WD32 dwDisplayWidth, dwDisplayHeight;
    WD32 lDisplayPitch;
    WD_PIXFMT ddpfDisplay;
    WD32 dwOffscreenAlign, dwOverlayAlign, dwTextureAlign, dwZBufferAlign, dwAlphaAlign;
    WD32 dwNumHeaps;
    WD32 pvmList;                   /* 16:16 */
} WD_VIDMEMINFO;

typedef struct {                    /* DDCORECAPS */
    WD32 dwSize, dwCaps, dwCaps2, dwCKeyCaps, dwFXCaps, dwFXAlphaCaps, dwPalCaps, dwSVCaps;
    WD32 dwAlphaBltConstBitDepths, dwAlphaBltPixelBitDepths, dwAlphaBltSurfaceBitDepths;
    WD32 dwAlphaOverlayConstBitDepths, dwAlphaOverlayPixelBitDepths, dwAlphaOverlaySurfaceBitDepths;
    WD32 dwZBufferBitDepths, dwVidMemTotal, dwVidMemFree, dwMaxVisibleOverlays, dwCurrVisibleOverlays;
    WD32 dwNumFourCCCodes, dwAlignBoundarySrc, dwAlignSizeSrc, dwAlignBoundaryDest, dwAlignSizeDest;
    WD32 dwAlignStrideAlign;
    WD32 dwRops[8];
    WD32 ddsCaps;
    WD32 dwMinOverlayStretch, dwMaxOverlayStretch, dwMinLiveVideoStretch, dwMaxLiveVideoStretch;
    WD32 dwMinHwCodecStretch, dwMaxHwCodecStretch;
    WD32 dwReserved1, dwReserved2, dwReserved3;
    WD32 dwSVBCaps, dwSVBCKeyCaps, dwSVBFXCaps, dwSVBRops[8];
    WD32 dwVSBCaps, dwVSBCKeyCaps, dwVSBFXCaps, dwVSBRops[8];
    WD32 dwSSBCaps, dwSSBCKeyCaps, dwSSBFXCaps, dwSSBRops[8];
    WD32 dwMaxVideoPorts, dwCurrVideoPorts, dwSVBCaps2;
} WD_CORECAPS;

typedef struct {                    /* DDHALINFO */
    WD32 dwSize;
    WD32 lpDDCallbacks, lpDDSurfaceCallbacks, lpDDPaletteCallbacks;    /* 16:16 */
    WD_VIDMEMINFO vmiData;
    WD_CORECAPS ddCaps;
    WD32 dwMonitorFrequency;
    WD32 GetDriverInfo;             /* 32-bit */
    WD32 dwModeIndex;
    WD32 lpdwFourCC;                /* 16:16 */
    WD32 dwNumModes;
    WD32 lpModeInfo;                /* 16:16 */
    WD32 dwFlags;
    WD32 lpPDevice;                 /* 16:16 */
    WD32 hInstance;
    WD32 lpD3DGlobalDriverData, lpD3DHALCallbacks, lpDDExeBufCallbacks;
} WD_HALINFO;

typedef struct {                    /* DDHAL_DDCALLBACKS */
    WD32 dwSize, dwFlags;
    WD32 DestroyDriver, CreateSurface, SetColorKey, SetMode, WaitForVerticalBlank;
    WD32 CanCreateSurface, CreatePalette, GetScanLine, SetExclusiveMode, FlipToGDISurface;
} WD_DDCB;

typedef struct {                    /* DDHAL_DDSURFACECALLBACKS */
    WD32 dwSize, dwFlags;
    WD32 DestroySurface, Flip, SetClipList, Lock, Unlock, Blt, SetColorKey, AddAttachedSurface;
    WD32 GetBltStatus, GetFlipStatus, UpdateOverlay, SetOverlayPosition, reserved4, SetPalette;
} WD_SURFCB;

typedef struct {                    /* DDHAL_DDPALETTECALLBACKS */
    WD32 dwSize, dwFlags;
    WD32 DestroyPalette, SetEntries;
} WD_PALCB;

typedef struct {                    /* DDHAL_DDEXEBUFCALLBACKS */
    WD32 dwSize, dwFlags;
    WD32 CanCreateExecuteBuffer, CreateExecuteBuffer, DestroyExecuteBuffer, LockExecuteBuffer, UnlockExecuteBuffer;
} WD_EXEBUFCB;

typedef struct {                    /* DDHALMODEINFO */
    WD32 dwWidth, dwHeight;
    WD32 lPitch;
    WD32 dwBPP;
    WD16 wFlags, wRefreshRate;
    WD32 dwRBitMask, dwGBitMask, dwBBitMask, dwAlphaBitMask;
} WD_MODE;

#define WD_MAXMODES 128
#define WD_VERSION  0x0102

typedef struct {
    WD32 size, version;
    /* by the 32-bit part (DriverInit) */
    WD32 hinst;                     /* its module */
    WD_DDCB cb;                     /* its callbacks (flags: which) */
    WD_SURFCB scb;
    WD32 d3dglobal, d3dcb;          /* Direct3D: D3DHAL_GLOBALDRIVERDATA, D3DHAL_CALLBACKS (flat) */
    WD32 getdriverinfo;             /* GetDriverInfo (flat) */
    /* by the display driver: the current mode */
    WD32 valid;                     /* the mode below may be used */
    WD32 fblin;                     /* linear address of the visible screen */
    WD32 vramlin, vramsize;         /* the video memory mapped for DirectDraw */
    WD32 width, height, bpp, Bpp, pitch;
    WD32 rmask, gmask, bmask;
    WD32 fourcc_ok;                 /* YUV surfaces and the overlay (15 bpp and more) */
    WD32 debug;                     /* [WINVBE] Debug: the 32-bit part logs to port E9h too */
    WD32 flip;                      /* flips of the screen: 0 by copying, 1 DISPI display start */
    WD32 shown;                     /* linear address of the surface shown (flips) */
    WD32 d3d_on;                    /* the Direct3D HAL is offered in this mode */
    /* the HAL information handed to DirectDraw (its pointers point here) */
    WD_HALINFO hal;
    WD_DDCB ddcb;
    WD_SURFCB ddscb;
    WD_PALCB ddpcb;
    WD_EXEBUFCB ddxcb;
    WD_VIDMEM heap;
    WD32 fourcc[4];
    WD_MODE modes[WD_MAXMODES];
} WVBDD;

#pragma pack(pop)

/* DCICOMMAND for DirectDraw (DDK ddrawi.h) */
#define DD_VERSION              0x0200
#define DD_HAL_VERSION          0x0100
#define DD_RUNTIME_VERSION      0x0700
#define DDCREATEDRIVEROBJECT    10
#define DDGET32BITDRIVERNAME    11
#define DDNEWCALLBACKFNS        12
#define DDVERSIONINFO           13

/* callback flags */
#define WD_CB_DESTROYDRIVER     0x0001
#define WD_CB_CREATESURFACE     0x0002
#define WD_CB_SETCOLORKEY       0x0004
#define WD_CB_SETMODE           0x0008
#define WD_CB_WAITFORVBLANK     0x0010
#define WD_CB_CANCREATESURFACE  0x0020
#define WD_CB_FLIPTOGDISURFACE  0x0200
#define WD_SCB_DESTROYSURFACE   0x0001
#define WD_SCB_FLIP             0x0002
#define WD_SCB_LOCK             0x0008
#define WD_SCB_UNLOCK           0x0010
#define WD_SCB_BLT              0x0020
#define WD_SCB_GETBLTSTATUS     0x0100
#define WD_SCB_GETFLIPSTATUS    0x0200
#define WD_SCB_UPDATEOVERLAY    0x0400
#define WD_SCB_SETOVERLAYPOS    0x0800

#define WD_DLLNAME              "wvbdd32.dll"

#endif
