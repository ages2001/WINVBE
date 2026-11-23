/*
 * WINVBE - video BIOS calls through an x86 emulator (Windows NT x86)
 *
 * Windows NT 3.x - XP/2003 (x86) run the video BIOS in virtual 8086 mode
 * (VideoPortInt10 / the Int10 interface). A video BIOS that leaves real
 * mode - "unreal" (flat) addressing of its memory-mapped registers above
 * 1 MB, protected mode, MSRs - fails there: the call returns an error or
 * garbage. Newer Intel graphics (HD Graphics of Skylake and later) are
 * known for it. Linux runs such BIOSes in x86emu (uvesafb's v86d, the X
 * server's Int10 "x86emu" backend) and 64-bit Windows always uses an
 * emulator; this is the same for the 32-bit miniport, with libx86emu.
 *
 * The emulated machine: a private copy of the first 640 KB (interrupt
 * vectors, BIOS data area and EBDA from physical memory), the VGA memory
 * A0000-BFFFF and the ROMs C0000-FFFFF as they are in physical memory,
 * memory above 1 MB (memory-mapped registers) mapped on demand, and the
 * real I/O ports.
 */
#ifndef WVB_EMU10_H
#define WVB_EMU10_H

typedef struct {
    unsigned long eax, ebx, ecx, edx, esi, edi;
    unsigned short es, ds;
} EMUREGS;

#define EMU_BUFSEG  0x2000      /* buffer for BIOS calls: 2000:0000, 8 KB */
#define EMU_BUFSIZE 0x2000

/* the emulator, or NULL (no memory, or the physical memory not mapped) */
void *emu_create(void);
void emu_destroy(void *h);
/* INT n with the registers; 1 = the BIOS returned (IRET), 0 = it did not
   (an exception, an endless loop, too many instructions) */
int emu_int(void *h, unsigned char n, EMUREGS *r);
/* call a far routine (the video BIOS initialization C000:0003) */
int emu_call(void *h, unsigned short cs, unsigned short ip, EMUREGS *r);
/* memory below 1 MB as the emulated BIOS sees it */
int emu_write(void *h, unsigned long lin, const void *src, unsigned long len);
int emu_read(void *h, unsigned long lin, void *dst, unsigned long len);
/* the BIOS ran instructions that fail in virtual 8086 mode (MOV CRn,
   LGDT/LIDT, RDMSR/WRMSR, protected mode) since the last emu_clear_priv */
int emu_priv(void *h);
void emu_clear_priv(void *h);
/* debug output (set by the miniport) */
extern void (*emu_dbg)(const char *fmt, ...);

#endif
