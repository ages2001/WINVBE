; WINVBECF for Windows 1.x/2.x - start-up code (NASM, OMF output)
;
; The application start-up of the Windows 1.x/2.x SDKs: INITTASK sets up
; the task (DS = SS = the data segment, local heap, stack), WAITEVENT(0)
; and INITAPP(hInstance) create the message queue, then WinMain is called.
; Only functions that Windows 1.01 already has are used, so the program
; does not need the C library start-up of Open Watcom (which is Windows 3.x
; only).  Also the real mode INT 10h call used by vbeq.c.

        cpu     8086
        bits    16

; Windows writes its task data into the first 16 bytes of the data segment
segment _NULL public class=BEGDATA use16 align=16
        dw      0, 0, 0, 0, 0, 0, 0, 0
segment _DATA public class=DATA use16 align=2
segment _BSS public class=BSS use16 align=2
group DGROUP _NULL _DATA _BSS

        extern  INITTASK            ; KERNEL.91
        extern  WAITEVENT           ; KERNEL.30
        extern  INITAPP             ; USER.5
        extern  WINMAIN             ; int PASCAL WinMain(hInst, hPrev, lpCmd, nShow)

segment _DATA
hinst   dw      0

segment CFGOLD_TEXT public class=CODE use16 align=16

        global  _wstart_
_wstart_:                           ; referenced by Open Watcom for WinMain
..start:
        xor     bp,bp
        push    bp
        call    far INITTASK
%ifdef MARK
        push    ax
        mov     al,'A'
        out     0E9h,al
        pop     ax
%endif
        or      ax,ax
        jz      .fail
        push    es                  ; command line ES:BX
        push    bx
        push    dx                  ; nCmdShow
        push    si                  ; hPrevInstance
        push    di                  ; hInstance
        mov     [hinst],di
        xor     ax,ax
        push    ax
        call    far WAITEVENT
        push    word [hinst]
        call    far INITAPP
%ifdef MARK
        push    ax
        mov     al,'B'
        out     0E9h,al
        pop     ax
%endif
        pop     di                  ; INITAPP of Windows 1.x returns no status
        pop     si
        pop     dx
        pop     bx
        pop     es
        push    di                  ; hInstance
        push    si                  ; hPrevInstance
        push    es                  ; lpCmdLine
        push    bx
        push    dx                  ; nCmdShow
%ifdef MARK
        mov     al,'W'
        out     0E9h,al
%endif
        push    cs                  ; same code segment: a near call with CS
        call    WINMAIN             ; pushed needs no entry table reference
%ifdef MARK
        mov     al,'X'
        out     0E9h,al
%endif
        mov     ah,4Ch
        int     21h
.fail:
        mov     ax,4CFFh
        int     21h

; void far pascal OLDINT10(WORD far *r)
;   r[0..6] = AX BX CX DX SI DI ES, in and out
        global  OLDINT10
OLDINT10:
        push    bp
        mov     bp,sp
        push    si
        push    di
        push    ds
        push    es
        lds     si,[bp+6]
        mov     ax,[si]
        mov     bx,[si+2]
        mov     cx,[si+4]
        mov     dx,[si+6]
        mov     di,[si+10]
        mov     es,[si+12]
        mov     si,[si+8]
        push    bp
        pushf
        int     10h
        popf                        ; keep the caller's interrupt flag
        pop     bp
        push    es
        push    di
        push    si
        push    dx
        push    cx
        push    bx
        push    ax
        lds     si,[bp+6]
        pop     word [si]
        pop     word [si+2]
        pop     word [si+4]
        pop     word [si+6]
        pop     word [si+8]
        pop     word [si+10]
        pop     word [si+12]
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     bp
        retf    4
