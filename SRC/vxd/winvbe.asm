; WINVBE.386 - Windows 3.0/3.1/3.11 386 enhanced mode and Windows 95/98/ME
; helper VxD of the WINVBE display driver ([386Enh] device=winvbe.386).
;
; The standard VDD (VDDVGA) knows only the VGA registers: when a full-screen
; MS-DOS box gets the screen it reloads the box's VGA state but leaves the
; Bochs/QEMU/VirtualBox "DISPI" VBE interface on, so the box is shown in the
; Windows graphics mode (and Windows' screen after the switch back is wrong).
; This VxD watches the display focus (Set_Device_Focus). Its init order is
; just before the VDD's, so it runs first: it saves the DISPI state of the
; VM losing the screen and switches DISPI off - the VDD then saves and
; reloads VGA state and video memory of a real VGA (through the A000h window,
; which DISPI would otherwise redirect into the frame buffer). A VM with a
; VBE mode (Windows, or a DOS program using VESA in a box) gets its DISPI
; state back in a VM event, after the VDD. VGA registers QEMU/Bochs rewrite
; as a side effect of DISPI writes are put back for the VDD (vga_fix).
; Message mode (SHELL's text screens in the system VM) is handled like a
; DOS box.
; The VMware SVGA II device (VMware, VirtualBox VMSVGA, QEMU -vga vmware)
; is handled the same way: its SVGA mode is switched off for a DOS box and
; on again for the VM that had it.
; Power management (all hardware): on Power_Event (APM / ACPI suspend and
; resume, Windows 3.1 VPOWERD and Windows 9x) the VBE/SVGA state of the VM
; owning the screen is saved before suspend and loaded after resume, and the
; display driver is told (INT 2Fh AX=B57Eh, BX=1, in the system VM) to set
; its mode again and repaint - BIOSes often lose a VBE mode in suspend.
; The call is made when Windows is idle (Call_When_Idle): the driver, GDI
; and USER are not re-entered in the middle of drawing.
; Other hardware (real video cards): a VBE mode is extended state the VDD
; does not know. When a full-screen VM gets the screen, its video mode is
; set again through the video BIOS (no clear, BIOS data kept), which takes
; the card out of Windows' mode; when Windows gets the screen back, and when
; a VM in the background (a windowed MS-DOS box) has called the BIOS to set
; a mode - which modern video BIOSes do on the hardware, past the VDD - the
; display driver is told to set its mode again (INT 2Fh AX=B57Eh, BX=3/4).
; Write combining (all hardware): the display driver asks with INT 2Fh
; AX=B57Fh (reflected to V86 mode, where this VxD hooks INT 2Fh) for its
; linear frame buffer mapping to be write-combined. With PAT (Pentium III,
; Athlon and later) a free PAT entry is set to WC and the page table entries
; of the mapping select it; otherwise a free variable MTRR is used, when no
; other MTRR covers the range. Frame buffer writes then go over the bus as
; bursts instead of single transfers.
;
; Built with NASM as a flat binary twice (org 0 / org 10000h) - tools/mkvxd32.py
; derives the relocations from the difference and writes the LE file.

        bits 32
%ifndef ORG
%define ORG 0
%endif
        org ORG

%macro VMMcall 1
        int     20h
        dd      00010000h + %1
%endmacro
Get_Sys_VM_Handle   equ 03h
Call_When_VM_Returns equ 0Dh
Schedule_VM_Event   equ 0Fh
Call_When_Idle      equ 3Ah
Hook_V86_Int_Chain  equ 41h
Begin_Nest_Exec     equ 83h
Begin_Use_Locked_PM_Stack equ 8Bh
End_Use_Locked_PM_Stack equ 8Ch
Begin_Nest_V86_Exec equ 82h
Exec_Int            equ 84h
End_Nest_Exec       equ 86h
Save_Client_State   equ 8Dh
MapPhysToLinear     equ 6Ch
Restore_Client_State equ 8Eh

DISPI_IDX       equ 1CEh
DISPI_DAT       equ 1CFh
VDD_ID          equ 0Ah
NSLOT           equ 8

Device_Init     equ 1
Sys_VM_Init     equ 3
Destroy_VM      equ 0Ch
Set_Device_Focus equ 0Fh
Begin_Message_Mode equ 10h
End_Message_Mode equ 11h
Power_Event     equ 1Ah

PWR_SUSPENDREQUEST equ 1
PWR_SUSPENDRESUME  equ 2
PWR_CRITICALRESUME equ 3
WVB_NOTIFY      equ 0B57Eh          ; INT 2Fh to the display driver
WVBN_RESUME     equ 1
WVBN_MODECHECK  equ 3               ; a background VM's BIOS set a video mode
WVBN_FOREGROUND equ 4               ; Windows has the screen back

ddb:    dd 0                    ; DDB_Next
        dw 0300h                ; DDB_SDK_Version (Windows 3.0)
        dw 0                    ; DDB_Req_Device_Number: undefined
        db 1, 0                 ; version 1.0
        dw 0                    ; DDB_Flags
        db 'WINVBE  '           ; DDB_Name
        dd 27FFFFFFh            ; DDB_Init_Order: just before the VDD (28000000h)
        dd control              ; DDB_Control_Proc
        dd 0, 0, 0, 0, 0, 0, 0  ; APIs, reference data, service table

; ---------------------------------------------------------------------
control:
%ifdef DEBUGE9
        push    eax
        add     al, 'a'
        out     0E9h, al
        pop     eax
%endif
        cmp     eax, Set_Device_Focus
        je      focus
        cmp     eax, Begin_Message_Mode
        je      msg_begin
        cmp     eax, End_Message_Mode
        je      msg_end
        cmp     eax, Destroy_VM
        je      destroy
        cmp     eax, Power_Event
        je      power
        cmp     eax, Device_Init
        je      dev_init
        cmp     eax, Sys_VM_Init
        je      init
        clc
        ret

dev_init:
        push    ebx
        mov     esi, idle_cb
        VMMcall Call_When_Idle
        mov     eax, 2Fh                ; write combining requests (B57Fh)
        mov     esi, int2f_hook
        VMMcall Hook_V86_Int_Chain
        mov     eax, 10h                ; mode sets of background VMs
        mov     esi, int10_hook
        VMMcall Hook_V86_Int_Chain
        pop     ebx
init:   mov     [sysvm], ebx
        mov     [focus_vm], ebx
        call    detect
        clc
        ret

Get_VMM_Version equ 0

; EBX = VM getting the focus, EDX = device (0 = all devices)
focus:
%ifdef DEBUGE9
        push    eax
        mov     al, '<'
        out     0E9h, al
        mov     al, dl
        add     al, '0'
        out     0E9h, al
        mov     al, 'd'
        cmp     ebx, [sysvm]
        jne     .q
        mov     al, 's'
.q:     out     0E9h, al
        mov     al, '>'
        out     0E9h, al
        pop     eax
%endif
        test    edx, edx
        jz      .dev
        cmp     edx, VDD_ID
        jne     .out
.dev:   cmp     byte [inmsg], 0
        jne     .fm
        mov     eax, [focus_vm]
        mov     [focus_vm], ebx
        cmp     byte [present], 0       ; real hardware (see the top)
        jne     .fm
        cmp     eax, ebx
        je      .out
        cmp     ebx, [sysvm]
        je      .back
        cmp     eax, [sysvm]
        jne     .out
%ifdef NOTEXTEV
        jmp     .out
%endif
        pushad                          ; Windows -> full-screen VM: its
        mov     esi, text_event         ; mode through the BIOS, after the VDD
        xor     edx, edx
        VMMcall Schedule_VM_Event
        popad
        jmp     .out
.back:  mov     byte [fg_pending], 1    ; the display driver sets its mode
        mov     byte [modechk_pending], 0
        jmp     .out
.fm:    cmp     byte [present], 0
        je      .out
        cmp     ebx, [owner]
        je      .out
        cmp     byte [msgmode], 0
        je      .sw
        mov     [owner], ebx            ; message mode: hardware is SHELL's,
        jmp     .out                    ; End_Message_Mode loads the owner
.sw:    pushad
        push    ebx
        mov     esi, [owner]
        call    save_state              ; the VM losing the screen
        pop     ebx
        mov     [owner], ebx
        ; before the VDD: the VGA must be a VGA again when it saves and
        ; reloads the VGA state and memory of the VMs
        mov     byte [svga_was], 0
        call    dispi_off
        ; Windows 9x keeps a DOS box's font in video memory, where the
        ; SVGA II frame buffer (which starts there) has overwritten it:
        ; the box loads the ROM font again (after the VDD)
        cmp     byte [svga_was], 0
        je      .nf
        cmp     byte [win9x], 0
        je      .nf
        cmp     ebx, [sysvm]
        je      .nf
        push    ebx
        mov     esi, font_event
        xor     edx, edx
        VMMcall Schedule_VM_Event
        pop     ebx
.nf:
        ; a VM with a VBE (DISPI) mode gets it back after the VDD is done
        mov     esi, ebx
        call    slot
        test    edi, edi
        jz      .nl
        cmp     byte [edi + 22], 0
        jne     .ld
        test    byte [edi + 2 + 4 * 2], 1
        jz      .nl
.ld:
        mov     esi, load_event
        xor     edx, edx
        VMMcall Schedule_VM_Event
.nl:
%ifdef DEBUGE9
        mov     al, 'D'
        cmp     ebx, [sysvm]
        jne     .d1
        mov     al, 'S'
.d1:    out     0E9h, al
%endif
        popad
.out:   clc
        ret

msg_begin:
        mov     byte [inmsg], 1
        cmp     byte [present], 0
        je      .out
        pushad
        mov     esi, [owner]
        call    save_state
        call    dispi_off
        mov     byte [msgmode], 1
        popad
.out:   clc
        ret

msg_end:
        mov     byte [inmsg], 0
        cmp     byte [present], 0
        je      .out
        cmp     byte [msgmode], 0
        je      .out
        pushad
        mov     byte [msgmode], 0
        mov     ebx, [owner]
        test    ebx, ebx
        jz      .no
        mov     esi, load_event
        xor     edx, edx
        VMMcall Schedule_VM_Event
.no:    popad
.out:   clc
        ret

; ---------------------------------------------------------------------
; Power_Event: ESI = event, EDI -> return code (left as it is: never deny)
power:
%ifdef DEBUGE9
        push    eax
        mov     al, 'W'
        out     0E9h, al
        mov     eax, esi
        call    hexb
        pop     eax
%endif
        pushad
        cmp     esi, PWR_SUSPENDREQUEST
        jne     .r
        cmp     byte [present], 0       ; DISPI / SVGA II state of the
        je      .x                      ; VM that has the screen
        cmp     byte [msgmode], 0
        jne     .x
        mov     esi, [owner]
        call    save_state
        jmp     .x
.r:     cmp     esi, PWR_SUSPENDRESUME
        je      .res
        cmp     esi, PWR_CRITICALRESUME
        jne     .x
.res:   cmp     byte [present], 0
        je      .nd
%ifdef DEBUGE9
        cmp     byte [test_noload], 0           ; test: the BIOS path
        mov     byte [test_noload], 0
        jne     .nd
%endif
        cmp     byte [msgmode], 0
        jne     .nd
        mov     esi, [owner]
        call    load_state
.nd:    ; Windows has the screen: its display driver sets the mode again
        ; (when Windows is idle next, see idle_cb)
        mov     ebx, [focus_vm]
        cmp     ebx, [sysvm]
        jne     .x
        mov     byte [resume_pending], 1
.x:     popad
        clc
        ret

; Call_When_Idle callback: EBX = system VM, EBP = its client registers.
; Windows (KERNEL) is idle: a resume is passed on to the display driver
; with INT 2Fh AX=B57Eh in protected mode, as the VDD sends 4002h.
idle_cb:
        mov     al, [resume_pending]
        or      al, [fg_pending]
        or      al, [modechk_pending]
        jz      .out
        cmp     byte [inmsg], 0                 ; message mode: later
        jne     .out
        test    dword [ebp + 2Ch], 20000h       ; V86 mode now: later
        jnz     .out
        cmp     ebx, [focus_vm]                 ; a DOS box has the screen:
        jne     .wait                           ; when Windows gets it back
        mov     dx, WVBN_RESUME
        cmp     byte [resume_pending], 0
        jne     .go
        mov     dx, WVBN_FOREGROUND
        cmp     byte [fg_pending], 0
        jne     .go
        mov     dx, WVBN_MODECHECK
.go:    mov     byte [resume_pending], 0
        mov     byte [fg_pending], 0
        mov     byte [modechk_pending], 0
        call    notify
.out:   clc
        ret
.wait:  mov     byte [resume_pending], 0        ; the switch back does it
        clc
        ret

; DX = event for the display driver (EBX = system VM, EBP = client)
notify:
        pushad
        mov     [notify_ev], dx
        mov     edi, client_save
        VMMcall Save_Client_State
        VMMcall Begin_Use_Locked_PM_Stack       ; a known 16-bit stack
        VMMcall Begin_Nest_Exec
%ifdef DEBUGE9
        mov     eax, [ebp + 2Ch]                ; client EFLAGS, CS
        shr     eax, 16
        call    hexb
        mov     eax, [ebp + 28h]
        xchg    al, ah
        call    hexb
        xchg    al, ah
        call    hexb
%endif
        mov     word [ebp + 1Ch], WVB_NOTIFY    ; Client_AX
        mov     ax, [notify_ev]
        mov     word [ebp + 10h], ax            ; Client_BX
%ifdef DEBUGE9
        call    hexb
%endif
        mov     eax, 2Fh
        VMMcall Exec_Int
        VMMcall End_Nest_Exec
        VMMcall End_Use_Locked_PM_Stack
        mov     esi, client_save
        VMMcall Restore_Client_State
%ifdef DEBUGE9
        mov     al, 'N'
        out     0E9h, al
%endif
        popad
        ret

; V86 INT 10h hook: a VM without the screen (a windowed MS-DOS box) sets a
; video mode -> when the BIOS call has returned, the display driver checks
; its mode (real hardware only: DISPI/SVGA II are repaired otherwise).
; Debug build: AX=4F7Eh/4F7Fh test triggers, AX=4F10h calls logged.
int10_hook:
%ifdef DEBUGE9
        cmp     word [ebp + 1Ch], 4F7Fh         ; test hook: the adapter lost
        jne     .sr                             ; its mode, then a resume
        pushad
        mov     eax, 4
        xor     ecx, ecx
        call    wr                              ; DISPI off
        mov     dx, 3CEh
        mov     ax, 0E06h                       ; GR6: text mode
        out     dx, ax
        mov     byte [test_noload], 1
        popad
        jmp     .rs
.sr:    cmp     word [ebp + 1Ch], 4F7Eh         ; test hook: simulate a resume
        jne     .pm
.rs:    pushad
        mov     esi, PWR_SUSPENDRESUME
        call    power
        popad
        clc
        ret
.pm:    cmp     word [ebp + 1Ch], 4F10h
        jne     .ms
        push    eax
        mov     al, 'P'
        out     0E9h, al
        mov     al, [ebp + 10h]                 ; BL
        call    hexb
        mov     al, [ebp + 11h]                 ; BH
        call    hexb
        pop     eax
.ms:
%endif
        cmp     ebx, [focus_vm]
        je      .pass
        cmp     byte [present], 0
        jne     .pass
        mov     ax, [ebp + 1Ch]
        test    ah, ah                          ; set mode
        jz      .mode
        cmp     ax, 4F02h                       ; VBE set mode
        je      .mode
        cmp     ax, 4F04h                       ; VBE state save/restore
        je      .mode
        cmp     ax, 1C02h                       ; state restore
        jne     .pass
.mode:  pushad
%ifdef DEBUGE9
        mov     al, 'M'
        out     0E9h, al
%endif
        mov     esi, int10_ret
        xor     edx, edx
        xor     eax, eax
        VMMcall Call_When_VM_Returns
        popad
.pass:  stc
        ret

int10_ret:
        mov     byte [modechk_pending], 1
        ret

; VM event in the VM that got the screen (after the VDD): set its BIOS mode
; again without clearing the screen - the card leaves Windows' VBE mode,
; which the VDD's VGA register reload does not do. BIOS data (cursor, page,
; rows, font height) kept; 8x8 / 8x14 fonts loaded again.
text_event:
        cmp     ebx, [focus_vm]
        jne     .out
        pushad
        mov     esi, [ebx + 4]                  ; CB_High_Linear
        add     esi, 400h                       ; BIOS data area
        mov     al, [esi + 49h]
        cmp     al, 13h
        ja      .x                              ; VESA or OEM mode: not ours
        mov     [tmode], al
        push    esi
        lea     esi, [esi + 49h]
        mov     edi, bda_a
        mov     ecx, 1Eh
        rep     movsb
        pop     esi
        push    esi
        lea     esi, [esi + 84h]
        mov     edi, bda_b
        mov     ecx, 7
        rep     movsb
        pop     esi
        mov     [bda_lin], esi
        mov     edi, client_save
        VMMcall Save_Client_State
        VMMcall Begin_Nest_V86_Exec
        movzx   eax, byte [tmode]
        or      al, 80h                         ; keep video memory
        mov     [ebp + 1Ch], ax
        mov     eax, 10h
        VMMcall Exec_Int
        mov     al, [tmode]                     ; text modes: the font height
        cmp     al, 3
        jbe     .t
        cmp     al, 7
        jne     .nf
.t:     mov     ax, 1112h                       ; 8x8
        cmp     byte [bda_b + 1], 8
        je      .f
        mov     ax, 1111h                       ; 8x14
        cmp     byte [bda_b + 1], 14
        jne     .nf
.f:     mov     [ebp + 1Ch], ax
        mov     word [ebp + 10h], 0
        mov     eax, 10h
        VMMcall Exec_Int
.nf:    mov     edi, [bda_lin]                  ; BIOS data back
        push    edi
        lea     edi, [edi + 49h]
        mov     esi, bda_a
        mov     ecx, 1Eh
        rep     movsb
        pop     edi
        lea     edi, [edi + 84h]
        mov     esi, bda_b
        mov     ecx, 7
        rep     movsb
        mov     word [ebp + 1Ch], 0100h         ; cursor shape
        mov     ax, [bda_a + 60h - 49h]
        mov     [ebp + 18h], ax
        mov     eax, 10h
        VMMcall Exec_Int
        movzx   eax, byte [bda_a + 62h - 49h]   ; page
        and     eax, 7
        mov     [tpage], al
        or      ah, 05h
        mov     [ebp + 1Ch], ax
        mov     eax, 10h
        VMMcall Exec_Int
        movzx   ecx, byte [tpage]               ; cursor position of the page
        mov     ax, [bda_a + 50h - 49h + ecx * 2]
        mov     [ebp + 14h], ax                 ; DX
        mov     byte [ebp + 11h], cl            ; BH
        mov     word [ebp + 1Ch], 0200h
        mov     eax, 10h
        VMMcall Exec_Int
        VMMcall End_Nest_Exec
        mov     esi, client_save
        VMMcall Restore_Client_State
%ifdef DEBUGE9
        mov     al, 'T'
        out     0E9h, al
%endif
.x:     popad
.out:   ret

; ---------------------------------------------------------------------
; V86 INT 2Fh hook: AX=B57Fh, BX=1: write combining for EDX = linear
; address, ECX = size, ESI = physical address of a frame buffer mapping.
; Returns AX = 0 not possible, 1 done with PAT, 2 done with an MTRR, 3 on
; already. Everything else goes on down the chain.
int2f_hook:
        cmp     word [ebp + 1Ch], 0B57Fh
        jne     .pass
        cmp     word [ebp + 10h], 1
        jne     .pass
        pushad
        mov     edx, [ebp + 14h]
        mov     ecx, [ebp + 18h]
        mov     esi, [ebp + 04h]
        call    set_wc
        mov     [ebp + 1Ch], ax
%ifdef DEBUGE9
        push    eax
        mov     al, 'C'
        out     0E9h, al
        pop     eax
        call    hexb
%endif
        popad
        clc
        ret
.pass:  stc
        ret

; EDX = linear, ECX = size, ESI = physical -> EAX
set_wc: cmp     edx, [wc_lin]
        jne     .new
        cmp     ecx, [wc_size]
        ja      .new
        mov     eax, 3
        ret
.new:   mov     [req_lin], edx
        mov     [req_size], ecx
        mov     [req_phys], esi
        call    cpu_feat
        mov     [feat], edx
        test    edx, 10000h             ; PAT
        jz      .mtrr
        test    dword [req_lin], 0FFFh
        jnz     .mtrr
        call    pat_entry
        cmp     eax, -1
        je      .mtrr
        call    pte_wc
        test    eax, eax
        jz      .mtrr
        mov     eax, 1
        jmp     .ok
.mtrr:  test    dword [feat], 1000h     ; MTRR
        jz      .no
        call    mtrr_wc
        test    eax, eax
        jz      .no
.ok:    mov     edx, [req_lin]
        mov     [wc_lin], edx
        mov     edx, [req_size]
        mov     [wc_size], edx
        ret
.no:    xor     eax, eax
        ret

; -> EDX = CPUID 1 feature flags (0 without CPUID)
cpu_feat:
        pushfd
        pop     eax
        mov     ecx, eax
        xor     eax, 200000h
        push    eax
        popfd
        pushfd
        pop     eax
        push    ecx
        popfd
        xor     eax, ecx
        test    eax, 200000h
        jz      .none
        xor     eax, eax
        cpuid
        test    eax, eax
        jz      .none
        mov     eax, 1
        cpuid
        ret
.none:  xor     edx, edx
        ret

; TLB flush, global pages included
flush_tlb:
        mov     eax, cr3
        mov     cr3, eax
        test    dword [feat], 2000h     ; PGE
        jz      .x
        mov     eax, cr4
        test    al, 80h
        jz      .x
        and     al, 7Fh
        mov     cr4, eax
        or      al, 80h
        mov     cr4, eax
.x:     ret

; caches off for a memory type change (Intel SDM "MTRR considerations");
; interrupts stay off until cache_on
cache_off:
        pop     dword [cache_ret]
        pushfd
        pop     dword [cache_fl]
        cli
        mov     eax, cr0
        mov     [cache_cr0], eax
        or      eax, 40000000h          ; CD
        and     eax, ~20000000h         ; NW
        mov     cr0, eax
        wbinvd
        call    flush_tlb
        jmp     dword [cache_ret]
cache_on:
        wbinvd
        call    flush_tlb
        mov     eax, [cache_cr0]
        mov     cr0, eax
        push    dword [cache_fl]
        popfd
        ret

; PAT entry 4-7 that is WC: an existing one, else one still at its power-on
; value (nothing in Windows 3.x/9x sets the PAT bit of a page) -> EAX =
; index, -1 = none
pat_entry:
        mov     ecx, 277h
        rdmsr
        mov     [pat_lo], eax
        mov     [pat_hi], edx
        xor     ebx, ebx
.f:     mov     al, [pat_hi + ebx]
        cmp     al, 1
        je      .got
        inc     ebx
        cmp     ebx, 4
        jb      .f
        xor     ebx, ebx
.d:     mov     al, [pat_hi + ebx]
        cmp     al, [pat_def + ebx]
        je      .set
        inc     ebx
        cmp     ebx, 4
        jb      .d
        mov     eax, -1
        ret
.set:   mov     byte [pat_hi + ebx], 1  ; WC
        push    ebx
        call    cache_off
        mov     eax, [pat_lo]
        mov     edx, [pat_hi]
        mov     ecx, 277h
        wrmsr
        call    cache_on
        pop     ebx
.got:   lea     eax, [ebx + 4]
        ret

; EAX = PAT index 4-7: the page table entries of [req_lin, +req_size) select
; it -> EAX = pages changed
pte_wc: mov     ecx, eax
        xor     edx, edx
        test    cl, 1
        jz      .a
        or      edx, 08h                ; PWT
.a:     test    cl, 2
        jz      .b
        or      edx, 10h                ; PCD
.b:     mov     eax, edx
        or      eax, 80h                ; PAT bit of a 4 KB page
        mov     [or4k], eax
        or      edx, 1000h              ; PAT bit of a 4 MB page
        mov     [or4m], edx
        mov     eax, cr3
        and     eax, 0FFFFF000h
        call    map_page
        test    eax, eax
        jz      .fail
        mov     [pd_lin], eax
        mov     dword [nchg], 0
        mov     dword [pt_phys], -1
        mov     esi, [req_lin]
        mov     edi, esi
        add     edi, [req_size]
.pg:    cmp     esi, edi
        jae     .done
        mov     ebx, esi
        shr     ebx, 22
        mov     eax, [pd_lin]
        mov     eax, [eax + ebx * 4]
        test    al, 1
        jz      .next4m
        test    al, 80h                 ; 4 MB page
        jz      .pt
        and     eax, ~(1018h)
        or      eax, [or4m]
        mov     ecx, [pd_lin]
        mov     [ecx + ebx * 4], eax
        inc     dword [nchg]
.next4m:
        add     esi, 400000h
        and     esi, 0FFC00000h
        jmp     .pg
.pt:    and     eax, 0FFFFF000h
        cmp     eax, [pt_phys]
        je      .have
        mov     [pt_phys], eax
        call    map_page
        test    eax, eax
        jz      .fail
        mov     [pt_lin], eax
.have:  mov     ebx, esi
        shr     ebx, 12
        and     ebx, 3FFh
        mov     ecx, [pt_lin]
        mov     eax, [ecx + ebx * 4]
        test    al, 1
        jz      .np
        and     eax, ~98h
        or      eax, [or4k]
        mov     [ecx + ebx * 4], eax
        inc     dword [nchg]
.np:    add     esi, 1000h
        jmp     .pg
.done:  call    cache_off               ; no stale lines or TLB entries
        call    cache_on
        mov     eax, [nchg]
        ret
.fail:  xor     eax, eax
        ret

; EAX = physical page -> EAX = linear address of a mapping (0 = failed);
; the last few mappings are kept, _MapPhysToLinear mappings are permanent
map_page:
        push    esi
        push    edi
        mov     esi, maps
        mov     ecx, NMAPS
.f:     cmp     [esi], eax
        je      .hit
        add     esi, 8
        loop    .f
        push    eax
        push    0
        push    1000h
        push    eax
        VMMcall MapPhysToLinear
        add     esp, 12
        pop     edx
        cmp     eax, -1
        je      .no
        mov     ecx, [map_next]
        mov     [maps + ecx * 8], edx
        mov     [maps + ecx * 8 + 4], eax
        inc     ecx
        cmp     ecx, NMAPS
        jb      .w
        xor     ecx, ecx
.w:     mov     [map_next], ecx
        jmp     .out
.hit:   mov     eax, [esi + 4]
        jmp     .out
.no:    xor     eax, eax
.out:   pop     edi
        pop     esi
        ret

; variable range MTRR set to WC for [req_phys, +req_size) rounded up to a
; power of two; only when it is aligned, a free MTRR exists and no MTRR
; already covers part of it -> EAX = 2 done, 3 already WC, 0 not possible
mtrr_wc:
        mov     esi, [req_phys]
        test    esi, esi
        jz      .no
        mov     eax, 1000h
.s:     cmp     eax, [req_size]
        jae     .sz
        shl     eax, 1
        jnc     .s
        jmp     .no
.sz:    mov     [msize], eax
        dec     eax
        test    esi, eax
        jnz     .no
        mov     ecx, 0FEh               ; MTRRcap
        rdmsr
        test    eax, 400h               ; WC supported
        jz      .no
        movzx   eax, al
        mov     [vcnt], eax
        mov     ecx, 2FFh               ; MTRRdefType
        rdmsr
        test    eax, 800h               ; enabled
        jz      .no
        ; physical address width -> high mask bits
        mov     dword [mhi], 0Fh        ; 36 bits
        mov     eax, 80000000h
        cpuid
        cmp     eax, 80000008h
        jb      .w
        mov     eax, 80000008h
        cpuid
        movzx   ecx, al
        sub     ecx, 32
        jbe     .w
        mov     eax, 1
        shl     eax, cl
        dec     eax
        mov     [mhi], eax
.w:     mov     dword [mfree], -1
        xor     ebx, ebx
.l:     cmp     ebx, [vcnt]
        jae     .chk
        lea     ecx, [ebx * 2 + 201h]   ; PHYSMASK
        rdmsr
        test    eax, 800h
        jnz     .used
        cmp     dword [mfree], -1
        jne     .nx
        mov     [mfree], ebx
        jmp     .nx
.used:  and     eax, 0FFFFF000h
        mov     edi, eax                ; its mask
        lea     ecx, [ebx * 2 + 200h]   ; PHYSBASE
        rdmsr
        mov     [mtype], al
        and     eax, edi
        mov     [mbase], eax            ; its base
        ; overlap: equal on the address bits both masks care about
        mov     ecx, [msize]
        neg     ecx
        and     ecx, edi
        mov     edx, esi
        and     edx, ecx
        and     ecx, eax
        cmp     edx, ecx
        jne     .nx
        ; overlapping: fine only if it is WC and holds all of ours
        cmp     byte [mtype], 1
        jne     .no
        mov     ecx, esi
        and     ecx, edi
        cmp     ecx, [mbase]
        jne     .no
        mov     eax, 3
        ret
.nx:    inc     ebx
        jmp     .l
.chk:   mov     ebx, [mfree]
        cmp     ebx, -1
        je      .no
        call    cache_off
        mov     ecx, 2FFh
        rdmsr
        mov     [mdef], eax
        and     eax, ~800h              ; MTRRs off while changing
        wrmsr
        lea     ecx, [ebx * 2 + 200h]
        mov     eax, esi
        or      eax, 1                  ; WC
        xor     edx, edx
        wrmsr
        lea     ecx, [ebx * 2 + 201h]
        mov     eax, [msize]
        neg     eax
        or      eax, 800h
        mov     edx, [mhi]
        wrmsr
        mov     ecx, 2FFh
        rdmsr
        mov     eax, [mdef]
        wrmsr
        call    cache_on
        mov     eax, 2
        ret
.no:    xor     eax, eax
        ret

; EBX = VM being destroyed: free its slot
destroy:
        push    edi
        mov     edi, slots
        mov     ecx, NSLOT
.l:     cmp     [edi], ebx
        jne     .n
        mov     dword [edi], 0
.n:     add     edi, 24
        loop    .l
        pop     edi
        clc
        ret

; Runs in the VM that got the screen, after the VDD (EBX = VM)
load_event:
        cmp     ebx, [owner]
        jne     .out
        cmp     byte [msgmode], 0
        jne     .out
        pushad
        mov     esi, ebx
        call    load_state
%ifdef DEBUGE9
        mov     al, 'L'
        out     0E9h, al
%endif
        popad
.out:   ret

; ---------------------------------------------------------------------
; DISPI present?  ID B0C0h - B0C5h.  VMware SVGA II (PCI 15ADh:0405h)?
detect: mov     byte [present], 0
        mov     byte [dispi], 0
        VMMcall Get_VMM_Version         ; AH = 3 (Windows 3.x) or 4 (9x)
        cmp     ah, 4                   ; Windows 9x (the font of a DOS box
        jb      .w3                     ; stays in video memory)
        mov     byte [win9x], 1
.w3:
%ifdef NODISPI
        ret                             ; test: behave as on real hardware
%endif
        xor     eax, eax
        call    rd
        cmp     ax, 0B0C0h
        jb      .sv
        cmp     ax, 0B0C5h
        ja      .sv
        mov     byte [dispi], 1
        mov     byte [present], 1
.sv:    xor     ecx, ecx                ; bus 0, devices 0-31
.p:     mov     eax, ecx
        shl     eax, 11
        or      eax, 80000000h
        mov     dx, 0CF8h
        out     dx, eax
        mov     dx, 0CFCh
        in      eax, dx
        cmp     eax, 040515ADh
        je      .f
        inc     ecx
        cmp     ecx, 32
        jb      .p
        jmp     .d
.f:     mov     eax, ecx
        shl     eax, 11
        or      eax, 80000010h          ; BAR0: I/O ports
        mov     dx, 0CF8h
        out     dx, eax
        mov     dx, 0CFCh
        in      eax, dx
        test    al, 1
        jz      .d
        and     eax, 0FFFCh
        mov     [svgaport], ax
        mov     byte [present], 1
.d:     cmp     byte [present], 0
        je      .no
        mov     eax, [sysvm]
        mov     [owner], eax
.no:    ret

; SVGA II: EAX = register -> EAX = value / EAX = register, ECX = value
svrd:   push    edx
        mov     dx, [svgaport]
        out     dx, eax
        inc     dx
        in      eax, dx
        pop     edx
        ret
svwr:   push    edx
        mov     dx, [svgaport]
        out     dx, eax
        inc     dx
        mov     eax, ecx
        out     dx, eax
        pop     edx
        ret

; SVGA mode off (the VGA shows again); waits for the command FIFO first
svga_off:
        cmp     word [svgaport], 0
        je      .x
        mov     eax, 1                  ; SVGA_REG_ENABLE
        call    svrd
        test    eax, eax
        jz      .x
        mov     eax, 21                 ; SVGA_REG_SYNC
        mov     ecx, 1
        call    svwr
        mov     ecx, 100000
.w:     push    ecx
        mov     eax, 22                 ; SVGA_REG_BUSY
        call    svrd
        pop     ecx
        test    eax, eax
        loopnz  .w
        mov     eax, 1
        xor     ecx, ecx
        call    svwr
        mov     byte [svga_was], 1
.x:     ret

; VM event in a DOS box (EBX = VM, EBP = its client registers): INT 10h
; AX=1104h, the 8x16 ROM font into font block 0
font_event:
        cmp     ebx, [owner]
        jne     .out
        pushad
        mov     edi, client_save
        VMMcall Save_Client_State
        VMMcall Begin_Nest_V86_Exec
        mov     word [ebp + 1Ch], 1104h ; Client_AX
        mov     word [ebp + 10h], 0     ; Client_BX: block 0
        mov     eax, 10h
        VMMcall Exec_Int
        VMMcall End_Nest_Exec
        mov     esi, client_save
        VMMcall Restore_Client_State
%ifdef DEBUGE9
        mov     al, 'F'
        out     0E9h, al
%endif
        popad
.out:   ret

; AX = index -> AX = value
rd:     push    edx
        mov     dx, DISPI_IDX
        out     dx, ax
        mov     dx, DISPI_DAT
        in      ax, dx
        pop     edx
        ret

; AX = index, CX = value
wr:     push    edx
        mov     dx, DISPI_IDX
        out     dx, ax
        mov     ax, cx
        mov     dx, DISPI_DAT
        out     dx, ax
        pop     edx
        ret

dispi_off:
        call    svga_off
        cmp     byte [dispi], 0
        je      .x
        mov     eax, 4
        call    rd
        test    al, 1
        jz      .x
        mov     eax, 4
        xor     ecx, ecx
        call    wr
        call    vga_fix
.x:     ret

%ifdef DEBUGE9
hexb:   push    eax
        shr     al, 4
        call    .d
        pop     eax
        push    eax
        and     al, 0Fh
        call    .d
        mov     al, ' '
        out     0E9h, al
        pop     eax
        ret
.d:     add     al, '0'
        cmp     al, '9'
        jbe     .o
        add     al, 7
.o:     out     0E9h, al
        ret
%endif

; QEMU (and Bochs) rewrite some VGA registers whenever DISPI registers are
; written (graphics mode, CRTC width/offset/height). The VDD does not know:
; a register it believes to hold the DOS box's value is not written again.
; vga_p = values before our DISPI writes, vga_q = after; at switch-off,
; registers still holding the vga_q value get their vga_p value back.
VGAREGS equ 9
vga_tab:                                ; port, index
        dw 3CEh, 5, 3CEh, 6, 3D4h, 1, 3D4h, 7, 3D4h, 9, 3D4h, 12h, 3D4h, 13h, 3D4h, 17h, 3D4h, 18h

; EDI = buffer (VGAREGS bytes)
vga_snap:
        push    esi
        push    ecx
        mov     esi, vga_tab
        mov     ecx, VGAREGS
.l:     mov     dx, [esi]
        in      al, dx
        mov     ah, al                  ; old index
        mov     al, [esi + 2]
        out     dx, al
        inc     dx
        in      al, dx
        stosb
        dec     dx
        mov     al, ah
        out     dx, al
        add     esi, 4
        loop    .l
        pop     ecx
        pop     esi
        ret

vga_fix:
        cmp     byte [vga_ok], 0
        je      .out
        mov     byte [vga_ok], 0
        push    edi
        mov     edi, vga_n
        call    vga_snap                ; now
        pop     edi
%ifdef DEBUGE9
        pushad
        mov     esi, vga_p
        mov     ecx, VGAREGS * 3
.h:     lodsb
        call    hexb
        loop    .h
        mov     al, '|'
        out     0E9h, al
        popad
%endif
        ; unlock CRTC 0-7
        mov     dx, 3D4h
        in      al, dx
        mov     [crtidx], al
        mov     al, 11h
        out     dx, al
        inc     dx
        in      al, dx
        mov     [v_cr11], al
        and     al, 7Fh
        out     dx, al
        xor     ecx, ecx
.l:     mov     al, [vga_n + ecx]
        cmp     al, [vga_q + ecx]
        jne     .n
        mov     dx, [vga_tab + ecx * 4]
        mov     al, [vga_tab + ecx * 4 + 2]
        mov     ah, [vga_p + ecx]
        out     dx, ax
.n:     inc     ecx
        cmp     ecx, VGAREGS
        jb      .l
        mov     dx, 3D4h
        mov     al, 11h
        mov     ah, [v_cr11]
        out     dx, ax
        mov     al, [crtidx]
        out     dx, al
.out:   ret

; ESI = VM -> EDI = its slot (created when missing; 0 = none free)
slot:   mov     edi, slots
        mov     ecx, NSLOT
.f:     cmp     [edi], esi
        je      .ok
        add     edi, 24
        loop    .f
        mov     edi, slots              ; new slot
        mov     ecx, NSLOT
.e:     cmp     dword [edi], 0
        je      .new
        add     edi, 24
        loop    .e
        xor     edi, edi
        ret
.new:   mov     [edi], esi
        mov     word [edi + 10], 0      ; register 4 (enable) = off
        mov     byte [edi + 22], 0      ; SVGA II mode off
.ok:    ret

; slot layout: dd vm, dw reg[1..9] (reg n at offset 4 + 2*(n-1)), db SVGA
; II enable, db pad
; ESI = VM: save the DISPI registers into its slot
save_state:
        test    esi, esi
        jz      .out
        call    slot
        test    edi, edi
        jz      .out
        mov     byte [edi + 22], 0
        cmp     word [svgaport], 0
        je      .ds
        mov     eax, 1                  ; SVGA_REG_ENABLE
        call    svrd
        mov     [edi + 22], al
.ds:    cmp     byte [dispi], 0
        jne     .dd
        mov     word [edi + 10], 0
        jmp     .out
.dd:    mov     ebx, 1
.l:     mov     eax, ebx
        call    rd
        mov     [edi + 2 + ebx * 2], ax
        inc     ebx
        cmp     ebx, 9
        jbe     .l
.out:   ret

; ESI = VM: switch DISPI to its state (off when unknown or off)
load_state:
        mov     byte [vesa_vm], 0
        call    dispi_off
        test    esi, esi
        jz      .out
        call    slot
        test    edi, edi
        jz      .out
        cmp     byte [edi + 22], 0
        je      .nsv
        mov     eax, 1                  ; SVGA II mode on again
        mov     ecx, 1
        call    svwr
        jmp     .out
.nsv:   test    byte [edi + 2 + 4 * 2], 1
        jz      .out
        mov     byte [vesa_vm], 1
        push    edi
        mov     edi, vga_p
        call    vga_snap                ; VGA registers before (see vga_fix)
        pop     edi
        mov     ebx, 1                  ; XRES, YRES, BPP
.a:     mov     eax, ebx
        mov     cx, [edi + 2 + ebx * 2]
        call    wr
        inc     ebx
        cmp     ebx, 3
        jbe     .a
        mov     eax, 4                  ; enable, keep the video memory
        mov     cx, [edi + 2 + 4 * 2]
        or      cx, 80h
        call    wr
        mov     ebx, 5                  ; bank, virtual width, offsets
.b:     cmp     ebx, 7
        je      .c
        mov     eax, ebx
        mov     cx, [edi + 2 + ebx * 2]
        call    wr
.c:     inc     ebx
        cmp     ebx, 9
        jbe     .b
        mov     edi, vga_q
        call    vga_snap                ; ... and after the DISPI writes
        mov     byte [vga_ok], 1
.out:   ret

; ---------------------------------------------------------------------
        align 4
sysvm   dd 0
owner   dd 0
focus_vm dd 0                   ; VM with the display focus (any hardware)
inmsg   db 0                    ; message mode (any hardware)
resume_pending db 0
fg_pending db 0
modechk_pending db 0
tmode   db 0
tpage   db 0
bda_a   times 1Eh db 0                  ; 40:49 - 40:66
bda_b   times 7 db 0                    ; 40:84 - 40:8A
        align 4
bda_lin dd 0
test_noload db 0
notify_ev dw 0
present db 0
dispi   db 0
win9x   db 0
svga_was db 0
msgmode db 0
        align 2
svgaport dw 0
vga_ok  db 0
vesa_vm db 0
crtidx  db 0
v_cr11  db 0
vga_p   times VGAREGS db 0
vga_q   times VGAREGS db 0
vga_n   times VGAREGS db 0
        align 4
slots   times NSLOT * 24 db 0
client_save times 80h db 0
NMAPS   equ 8
wc_lin  dd 0
wc_size dd 0
req_lin dd 0
req_size dd 0
req_phys dd 0
feat    dd 0
cache_ret dd 0
cache_fl dd 0
cache_cr0 dd 0
pat_lo  dd 0
pat_hi  dd 0
pat_def db 6, 4, 7, 0                   ; power-on values of PAT entries 4-7
or4k    dd 0
or4m    dd 0
pd_lin  dd 0
pt_phys dd 0
pt_lin  dd 0
nchg    dd 0
maps    times NMAPS * 2 dd -1
map_next dd 0
msize   dd 0
vcnt    dd 0
mhi     dd 0
mfree   dd 0
mdef    dd 0
mbase   dd 0
mtype   db 0
