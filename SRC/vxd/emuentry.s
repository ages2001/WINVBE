# WINVBE.386: jump table at the start of the emulator blob (see emuvxd.c)
        .text
        .globl _ve_entry
_ve_entry:
        jmp     _ve_init
        jmp     _ve_int10
        jmp     _ve_priv
        jmp     _ve_clear_priv
