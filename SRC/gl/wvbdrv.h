/*
 * wvbdrv.h - WINVBE OpenGL software driver: interface between the Mesa GDI
 * back end (wvbdrv.c) and the ICD entry layer (icd.c).
 *
 * Rendering goes into 32-bit BGRA DIB sections (one per colour buffer) and
 * reaches the screen with GDI BitBlt, so it works on top of any display
 * driver and at any desktop colour depth.
 */
#ifndef WVBDRV_H
#define WVBDRV_H

#include <windows.h>

#define WVB_VENDOR   "WINVBE (Mesa project)"
/* the build in the renderer string: x64, SSE2 (WVBGLS.DLL) or x87 (WVBGLX.DLL) */
#if defined(_WIN64)
#define WVB_RENDERER "WINVBE software (Mesa 7.8.2, x64)"
#elif defined(__SSE2__)
#define WVB_RENDERER "WINVBE software (Mesa 7.8.2, SSE2)"
#else
#define WVB_RENDERER "WINVBE software (Mesa 7.8.2, x87)"
#endif

struct wvb_context;

/* Process/thread notifications from the DLL entry point. */
void wvb_process_attach(void);
void wvb_process_detach(void);

/* Pixel formats (1-based, as seen by opengl32). */
int  wvb_pixelformat_count(void);
int  wvb_describe_pixelformat(HDC hdc, int index, PIXELFORMATDESCRIPTOR *pfd);
BOOL wvb_set_pixelformat(HDC hdc, int index);
int  wvb_get_pixelformat(HDC hdc);

/* Contexts. */
struct wvb_context *wvb_create_context(HDC hdc);
BOOL wvb_destroy_context(struct wvb_context *c);
BOOL wvb_make_current(struct wvb_context *c, HDC hdc);  /* c == NULL: release */
BOOL wvb_release_current(struct wvb_context *c);
BOOL wvb_copy_context(struct wvb_context *src, struct wvb_context *dst, UINT mask);
BOOL wvb_share_lists(struct wvb_context *c1, struct wvb_context *c2);
BOOL wvb_swap_buffers(HDC hdc);

/* NT 3.x helpers (wvbdrv.c) */
int wvb_nt3(void);
void wvb_flush_vertices(void);

#endif /* WVBDRV_H */
