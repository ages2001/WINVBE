; WINVBE - low level entry points (NASM, OMF output)
;   LibEntry, INT 2Fh screen switch hook, real-mode INT 10h thunk,
;   real-mode VBE window function call.

        cpu     386
        bits    16

segment _DATA public class=DATA use16 align=2
segment _BSS public class=BSS use16 align=2
group DGROUP _DATA _BSS

        extern  rmr                 ; RMREGS (C)
        extern  rm_stack_seg
        extern  rm_stack_top
        extern  rm_winfunc_ptr      ; DWORD
        extern  rm_winfunc_both     ; BYTE
        extern  old_int2f           ; DWORD
        extern  _driver_load        ; void far cdecl driver_load(WORD hInst)
        extern  _on_int2f           ; void far cdecl on_int2f(WORD ax)

segment _DATA
save_ss dw      0
save_sp dw      0
vdd_entry dd    0
i2f_ss  dw      0
i2f_sp  dw      0
i2f_busy db     0

%ifndef OLDWIN
; own stack for the INT 2Fh notifications (a segment of its own, DGROUP is
; full): they arrive on whatever stack the VDD or WINVBE.386 uses (the VMM's
; small locked stack), and a mode set through the BIOS goes deep
segment I2F_STACK public class=FAR_DATA use16 align=16
i2f_stack times 4096 db 0
i2f_top:
%endif

segment ENTRY_TEXT public class=CODE use16 align=16

        global  LibEntry
..start:
LibEntry:
%ifdef MARK
        mov     al,'L'
        out     0E9h,al
%endif
        push    bp
        mov     bp,sp
        push    ds
        push    si
        push    di
        push    di                  ; hInstance
        call    far _driver_load
        add     sp,2
        pop     di
        pop     si
        pop     ds
        pop     bp
        mov     ax,1
        retf

; ---------------------------------------------------------------------
; void far rm_int10(void)  -- real mode only, uses global rmr
        global  rm_int10
rm_int10:
        pushf                       ; keep the caller's interrupt flag
        pushad
        push    ds
        push    es
        mov     ax,DGROUP
        mov     ds,ax
        mov     [save_ss wrt DGROUP],ss
        mov     [save_sp wrt DGROUP],sp
        mov     ax,[rm_stack_seg wrt DGROUP]
        or      ax,ax
        jz      .nostk
        cli
        mov     ss,ax
        mov     sp,[rm_stack_top wrt DGROUP]
        sti
.nostk:
        push    ds
        mov     edi,[rmr+0 wrt DGROUP]
        mov     esi,[rmr+4 wrt DGROUP]
        mov     ebp,[rmr+8 wrt DGROUP]
        mov     ebx,[rmr+16 wrt DGROUP]
        mov     edx,[rmr+20 wrt DGROUP]
        mov     ecx,[rmr+24 wrt DGROUP]
        mov     eax,[rmr+28 wrt DGROUP]
        mov     es,[rmr+34 wrt DGROUP]
        int     10h
        pop     ds                  ; DGROUP
        pushf
        mov     [rmr+0 wrt DGROUP],edi
        mov     [rmr+4 wrt DGROUP],esi
        mov     [rmr+8 wrt DGROUP],ebp
        mov     [rmr+16 wrt DGROUP],ebx
        mov     [rmr+20 wrt DGROUP],edx
        mov     [rmr+24 wrt DGROUP],ecx
        mov     [rmr+28 wrt DGROUP],eax
        mov     [rmr+34 wrt DGROUP],es
        pop     ax
        mov     [rmr+32 wrt DGROUP],ax
        cli
        mov     ss,[save_ss wrt DGROUP]
        mov     sp,[save_sp wrt DGROUP]
        pop     es
        pop     ds
        popad
        popf
        retf

; ---------------------------------------------------------------------
; void far rm_winfunc(WORD pos)  -- pos in DX, real mode only
        global  rm_winfunc
rm_winfunc:
        pushf                       ; some BIOS window functions leave IF clear
        pushad
        push    ds
        push    es
        mov     ax,DGROUP
        mov     ds,ax
        push    dx
        xor     bx,bx
        call    far [rm_winfunc_ptr wrt DGROUP]
        pop     dx
        cmp     byte [rm_winfunc_both wrt DGROUP],0
        je      .done
        mov     bx,1
        call    far [rm_winfunc_ptr wrt DGROUP]
.done:
        pop     es
        pop     ds
        popad
        popf
        retf

; ---------------------------------------------------------------------
; INT 2Fh hook: 4001h = going to background, 4002h = coming to foreground
int2f_handler:
        cmp     ax,4001h
        je      .ours
        cmp     ax,4002h
        je      .ours
        cmp     ax,0B57Eh           ; WVB_NOTIFY from WINVBE.386 (BX = event)
        je      .ours
        ; chain to old handler
        sub     sp,4
        push    bp
        mov     bp,sp
        push    ds
        push    ax
        mov     ax,DGROUP
        mov     ds,ax
        mov     ax,[old_int2f wrt DGROUP]
        mov     [bp+2],ax
        mov     ax,[old_int2f+2 wrt DGROUP]
        mov     [bp+4],ax
        pop     ax
        pop     ds
        pop     bp
        retf
.ours:
        push    bp
        mov     bp,sp
        pushad
        push    ds
        push    es
        push    fs
        push    gs
        mov     cx,DGROUP
        mov     ds,cx
%ifndef OLDWIN
        cmp     byte [i2f_busy wrt DGROUP],0
        jne     .same                       ; nested: stay on this stack
        mov     byte [i2f_busy wrt DGROUP],1
        mov     [i2f_ss wrt DGROUP],ss
        mov     [i2f_sp wrt DGROUP],sp
        mov     dx,seg i2f_top
        cli
        mov     ss,dx
        mov     sp,i2f_top
        sti
        push    bx
        push    ax
        call    far _on_int2f
        add     sp,4
        cli
        mov     ss,[i2f_ss wrt DGROUP]
        mov     sp,[i2f_sp wrt DGROUP]
        sti
        mov     byte [i2f_busy wrt DGROUP],0
        jmp     .done
%endif
.same:  push    bx
        push    ax
        call    far _on_int2f
        add     sp,4
.done:  pop     gs
        pop     fs
        pop     es
        pop     ds
        popad
        and     word [bp+6],0FFFEh  ; clear carry in iret frame
        pop     bp
        iret

        global  hook_int2f
hook_int2f:
        push    ds
        push    es
        push    bx
        push    dx
        mov     ax,DGROUP
        mov     ds,ax
        mov     ax,352Fh
        int     21h
        mov     [old_int2f wrt DGROUP],bx
        mov     [old_int2f+2 wrt DGROUP],es
        push    ds
        mov     dx,int2f_handler
        mov     ax,cs
        mov     ds,ax
        mov     ax,252Fh
        int     21h
        pop     ds
        pop     dx
        pop     bx
        pop     es
        pop     ds
        retf

; ---------------------------------------------------------------------
; vdd_assist(on): Windows 3.x 386 enhanced mode only (INT 2Fh 1600h).
; AX=4000h asks the VDD for VM-assisted save/restore: it then sends us
; 4001h/4002h instead of trying to restore a SVGA mode it does not know.
; AX=4007h ends it.  Returns AX = 1 when enhanced mode was found.
        global  vdd_assist
        extern  _screen_busy
vdd_assist:
        push    bp
        mov     bp,sp
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es
        push    ds
        mov     ax,DGROUP
        mov     ds,ax
        mov     ax,1600h
        int     2Fh
        test    al,7Fh
        jz      .none
        cmp     word [bp+6],0
        je      .off
        cmp     word [bp+6],1
        je      .i4000
        ; register with the VDD like VGA.DRV does (VDD PM API 0Ch):
        ; BX = latch byte offset (none), DX = 0, SI = our "screen busy" byte
        xor     di,di
        mov     es,di
        mov     ax,1684h
        mov     bx,000Ah
        int     2Fh
        mov     ax,es
        or      ax,di
        jz      .i4000
        mov     [vdd_entry wrt DGROUP],di
        mov     [vdd_entry+2 wrt DGROUP],es
        mov     ax,000Ch
        mov     bx,0FFFFh
        xor     dx,dx
        mov     si,_screen_busy
        call    far [vdd_entry wrt DGROUP]
        mov     ax,2
        jmp     .out
.i4000: mov     ax,4000h
        int     2Fh
        mov     ax,1
        jmp     .out
.off:   mov     ax,4007h
        int     2Fh
        mov     ax,1
        jmp     .out
.none:  xor     ax,ax
.out:   pop     ds
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     bp
        retf

        global  unhook_int2f
unhook_int2f:
        push    ds
        push    dx
        mov     ax,DGROUP
        mov     ds,ax
        mov     dx,[old_int2f wrt DGROUP]
        mov     ax,[old_int2f+2 wrt DGROUP]
        or      ax,dx
        jz      .no
        mov     ax,[old_int2f+2 wrt DGROUP]
        push    ds
        mov     ds,ax
        mov     ax,252Fh
        int     21h
        pop     ds
        mov     word [old_int2f wrt DGROUP],0
        mov     word [old_int2f+2 wrt DGROUP],0
.no:
        pop     dx
        pop     ds
        retf
