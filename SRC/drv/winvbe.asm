; WINVBE.LGO - start-up logo code for Windows 1.x/2.x (WIN.COM)
;
; WIN.COM is built by Setup from WIN.CNF + <display>.LGO + <oem>.LGD.  The
; logo code is entered with CS = DS = its own segment; the logo data (.LGD)
; follows the code directly.  Layout required by WIN.CNF:
;   +0  'LOGO'
;   +4  jmp show        show the logo, return AX = bytes to keep resident
;   +7  restore         (far) restore the video mode saved by show
; The .LGD starts with 8 zero terminated strings (product, version, ...,
; copyright); they are shown on a blue text screen together with the
; WINVBE line.  Text mode works on every VGA compatible card.

        bits    16
        org     0

        db      'LOGO'
        jmp     near show
restore:                                ; +7
        xor     ax,ax
        mov     es,ax
        mov     al,[cs:save_equip]
        mov     [es:0410h],al
        mov     al,[cs:save_mode]       ; AH = 0: set mode
        int     10h
        retf
save_mode       db      3
save_equip      db      0
resident_end:

show:
        push    bp
        push    si
        push    di
        push    es
        push    ds
        push    cs
        pop     ds
        mov     ah,0Fh
        int     10h
        and     al,7Fh
        mov     [save_mode],al
        xor     ax,ax
        mov     es,ax
        mov     al,[es:0410h]
        mov     [save_equip],al
        or      byte [es:0410h],20h     ; colour 80x25
        and     byte [es:0410h],0EFh
        mov     ax,0003h
        int     10h
        mov     ah,01h                  ; hide the cursor
        mov     cx,2000h
        int     10h
        ; blue screen
        mov     ax,0B800h
        mov     es,ax
        xor     di,di
        mov     cx,80*25
        mov     ax,1F20h
        cld
        rep     stosw
        ; frame rows 5..19, columns 2..77
        mov     dh,5
        mov     dl,9
        mov     al,0C9h
        mov     bl,0CDh
        mov     ah,0BBh
        call    hline
        mov     dh,19
        mov     al,0C8h
        mov     ah,0BCh
        call    hline
        mov     dh,6
.side:  call    rowaddr
        mov     word [es:di+2*2],1FBAh
        mov     word [es:di+77*2],1FBAh
        inc     dh
        cmp     dh,19
        jb      .side
        ; strings from the .LGD
        mov     si,lgd
        mov     dh,7
        mov     cx,8
.str:   cmp     byte [si],0
        je      .skip
        mov     bl,1Fh
        cmp     dh,7
        jne     .norm
        mov     bl,1Eh                  ; product name in yellow
.norm:  call    center
        inc     dh
        cmp     dh,9
        jne     .skip
        inc     dh                      ; blank line after the version
.skip:  lodsb                           ; skip to the next string
        or      al,al
        jnz     .skip
        loop    .str
        ; our line
        mov     dh,17
        mov     si,msg
        mov     bl,1Bh
        call    center
        pop     ds
        pop     es
        pop     di
        pop     si
        pop     bp
        mov     ax,resident_end
        retf

; DH = row -> DI = offset
rowaddr:
        push    ax
        mov     al,dh
        mov     ah,160
        mul     ah
        mov     di,ax
        pop     ax
        ret

; DH = row: AL left corner, BL fill, AH right corner (columns 2..77)
hline:
        call    rowaddr
        add     di,2*2
        mov     [es:di],al
        mov     byte [es:di+1],1Fh
        add     di,2
        mov     cx,74
.l:     mov     [es:di],bl
        mov     byte [es:di+1],1Fh
        add     di,2
        loop    .l
        mov     [es:di],ah
        mov     byte [es:di+1],1Fh
        ret

; DS:SI string (kept), DH row, BL attribute: centred between the frame
center:
        push    si
        push    cx
        xor     cx,cx
.len:   cmp     byte [si],0
        je      .got
        inc     si
        inc     cx
        jmp     .len
.got:   pop     ax
        pop     si
        push    si
        push    ax
        jcxz    .done
        cmp     cx,74
        jbe     .fit
        mov     cx,74
.fit:   call    rowaddr
        mov     ax,80
        sub     ax,cx
        shr     ax,1
        shl     ax,1
        add     di,ax
.put:   lodsb
        mov     [es:di],al
        mov     [es:di+1],bl
        add     di,2
        loop    .put
.done:  pop     cx
        pop     si
        ret

msg     db      'WINVBE v1.0 - universal VESA/VGA/SVGA display driver',0

        align   2
lgd:                                    ; the .LGD data follows here
