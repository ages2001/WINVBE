/* host (Linux) emulation of 16-bit segmented environment for unit tests */
#ifndef HOSTCOMPAT_H
#define HOSTCOMPAT_H
#include <stdint.h>
#include <stdio.h>
typedef uint8_t  BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int16_t  SHORT;
typedef int32_t  LONG;
typedef int      BOOL;
typedef void    *LPVOID;
typedef BYTE    *LPBYTE;
typedef WORD    *LPWORD;
typedef DWORD   *LPDWORD;
typedef char    *LPSTR;
#define FAR
#define NEAR
#define PASCAL
#define EXPORT
#define __far
#define __pascal
#define __cdecl
#define __loadds
/* segment emulation */
void *host_mkfp(WORD sel, WORD off);
WORD host_seg(const void *p);
WORD host_off(const void *p);
#define MK_FP(s,o) host_mkfp((WORD)(s),(WORD)(o))
#define FP_SEG(p) host_seg((const void *)(p))
#define FP_OFF(p) host_off((const void *)(p))
#define far_memcpy(d,s,n) memmove((void*)(d),(const void*)(s),(n))
#define far_memset(d,v,n) memset((void*)(d),(v),(n))
static inline void far_memset32(void *d, DWORD v, WORD n) { DWORD *p = (DWORD *)d; while (n--) *p++ = v; }
#define outp(p,v) ((void)0)
#define inp(p) 0
#define outpw(p,v) ((void)0)
#define inpw(p) 0
#endif
