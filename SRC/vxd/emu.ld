/* the emulator blob of WINVBE.386: one flat image at BASE */
SECTIONS {
  . = BASE;
  .all : {
    *(.text.entry)
    *(.text) *(.text.*)
    *(.rdata) *(.rdata.*) *(.rodata*)
    *(.data) *(.data.*)
    *(.bss) *(.bss.*) *(COMMON)
    . = ALIGN(16);
  }
  /DISCARD/ : { *(.rdata$zzz) *(.eh_frame) *(.comment) *(.debug*) *(.drectve) *(.reloc) *(.note*) }
}
