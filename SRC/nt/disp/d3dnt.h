/*
 * WINVBE.DLL - the Direct3D HAL of Windows 2000/XP (d3dnt.c), as winvbe.c
 * calls it.
 */
#ifndef D3DNT_H
#define D3DNT_H

int  d3dnt_init(void);                  /* 0: no floating point state to save, no Direct3D */
void d3dnt_mode(void *fb, ULONG vram, ULONG bpp, ULONG r, ULONG g, ULONG b, int on);
void d3dnt_halinfo(DD_HALINFO *h);      /* capabilities and tables */
int  d3dnt_info(PDD_GETDRIVERINFODATA d);   /* 1: a Direct3D GUID, answered */
int  d3dnt_can_create(DDSURFACEDESC *sd);   /* textures and Z buffers */
void d3dnt_destroy(PDD_SURFACE_LOCAL s);    /* DestroySurface: forget its handles */
int  d3dnt_dxt(PDD_SURFACE_GLOBAL g, DWORD fourcc, LONG stride);   /* a DXT texture's block size */

extern void (*d3dnt_log)(const char *f, ULONG a, ULONG b);   /* debug builds */

#endif
