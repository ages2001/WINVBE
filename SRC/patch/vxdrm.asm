; 915PATCH.VXD - real mode initialization entry (start address of the VxD).
; The Windows loader calls it in real mode, before Windows switches to
; protected mode, with the object at CS:0 and a NEAR return address (like
; the real mode init of Microsoft's VxDs, which end with RET).
; The Windows 95 loader ignores a real mode init object over 16 KB (code,
; data, BSS and stack together).
; Entry: AX = VMM version, BX = flags, ECX = loader services, EDX = INT 2Fh
; reference data, SI = environment segment.
; We switch to our own stack (the C code is small model, SS = DS), run
; vxd_main and return Abort_Device_Load + No_Fail_Message: the work is done,
; the protected mode part is not needed.
        bits 16
        cpu 386

segment _TEXT  public align=16 use16 class=CODE
segment CONST  public align=2  use16 class=DATA
segment CONST2 public align=2  use16 class=DATA
segment _DATA  public align=2  use16 class=DATA
segment _BSS   public align=2  use16 class=BSS
segment STACK  public align=16 use16 class=STACK
group DGROUP _TEXT CONST CONST2 _DATA _BSS STACK

Abort_Device_Load equ 1
No_Fail_Message   equ 8000h

segment _TEXT
        extern  vxd_main_
        global  _cstart_, _small_code_, __DOSseg__
_cstart_:
_small_code_:
__DOSseg__:
..start:
entry:
        pushf
        push    ds
        push    es
        pushad
%ifdef DEBUGE9
        mov     al, 'R'
        out     0E9h, al
%endif
        test    bx, 3                   ; Duplicate_Device_ID / _From_INT2F
        jnz     .out
        ; CS:0 must be the start of this object
        call    .here
.here:  pop     bx
        cmp     bx, .here
        jne     .out
        mov     [cs:old_ss], ss
        mov     [cs:old_sp], sp
        mov     [cs:env_si], si
        mov     ax, cs
        mov     ds, ax
        mov     es, ax
        cli
        mov     ss, ax
        mov     sp, stack_top
        sti
        cld
%ifdef DEBUGE9
        mov     al, 'S'
        out     0E9h, al
%endif
        mov     ax, [env_si]
        call    vxd_main_
        cli
        mov     ss, [cs:old_ss]
        mov     sp, [cs:old_sp]
        sti
.out:   popad
        pop     es
        pop     ds
        popf
        mov     ax, Abort_Device_Load | No_Fail_Message
        xor     bx, bx                  ; no excluded pages
        xor     si, si                  ; no instance data
        xor     edx, edx                ; reference data
        ret

old_ss  dw      0
old_sp  dw      0
env_si  dw      0

segment STACK
        resb    2048
stack_top:
