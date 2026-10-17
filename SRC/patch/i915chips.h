/* 915PATCH - supported Intel chipsets (host bridge 0:0.0 device IDs) and
   how their shadow RAM is unlocked; shared by 915PATCH.EXE/.VXD and the NT
   miniport WINVBE.SYS */
#ifndef I915CHIPS_H
#define I915CHIPS_H
#define PAM_NONE 0
#define PAM_5A   1      /* 830 / 852 / 855: one byte at 5Ah */
#define PAM_90   2      /* 845G and later: PAM1/PAM2 at 91h/92h */

static const struct { u16 id; const char *name; u8 pam; } i915_chips[] = {
    { 0x3575, "830M/MG",            PAM_5A },
    { 0x2560, "845G",               PAM_90 },
    { 0x3580, "852GM/855GM",        PAM_5A },
    { 0x2570, "865G",               PAM_90 },
    { 0x2580, "915G",               PAM_90 },
    { 0x2590, "915GM",              PAM_90 },
    { 0x2770, "945G",               PAM_90 },
    { 0x27A0, "945GM",              PAM_90 },
    { 0x27AC, "945GME",             PAM_90 },
    { 0x2970, "946GZ",              PAM_90 },
    { 0x2990, "Q963/Q965",          PAM_90 },
    { 0x29A0, "G965",               PAM_90 },
    { 0x2A00, "GM965",              PAM_90 },
    { 0x2A10, "GME965",             PAM_90 },
    { 0x29B0, "Q35",                PAM_90 },
    { 0x29C0, "G31/G33",            PAM_90 },
    { 0x29D0, "Q33",                PAM_90 },
    { 0x2A40, "GM45",               PAM_90 },
    { 0x2E00, "G45 class",          PAM_90 },
    { 0x2E10, "Q45/Q43",            PAM_90 },
    { 0x2E20, "G45/G43",            PAM_90 },
    { 0x2E30, "G41",                PAM_90 },
    { 0x2E40, "B43",                PAM_90 },
    { 0xA000, "Atom D4xx/D5xx",     PAM_90 },
    { 0xA010, "Atom N4xx/N5xx",     PAM_90 },
#ifdef TEST440FX
    { 0x1237, "440FX (QEMU test)",  PAM_5A },  /* PAM1/PAM2 at 5Ah/5Bh */
#endif
    { 0, NULL, 0 }
};
#endif
