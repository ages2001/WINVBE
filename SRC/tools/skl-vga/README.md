# skl-vga: an Intel Skylake graphics look-alike for QEMU

A QEMU device for testing WINVBE the way it meets Intel HD Graphics of the
6th generation and later on real hardware. It is not an emulation of the
GPU itself, but of everything a VESA driver sees of it:

- PCI 8086:1912 (HD Graphics 530), revision 06, at 00:02.0, class 030000
  (038000 with `vga=off`), interrupt pin A
- BAR0 GTTMMADR: 64-bit memory, 16 MB (the EDID is at offset 0)
- BAR2 GMADR: 64-bit prefetchable aperture, at least 256 MB, holding the
  linear frame buffer
- BAR4 IOBAR: MMIO index (0) / data (4)
- no Bochs DISPI ports at 1CEh: the video BIOS programs the mode through
  the IOBAR, as the Intel BIOS reaches its registers
- `read-ns=N`: every read of the aperture costs N ns, as video memory reads
  do on real cards (uncached); writes stay fast in comparison
- `unreal=on`: the video BIOS reaches its registers above 1 MB in "unreal"
  mode during VBE 4F00h and 4F02h (a moment in protected mode, then a
  32-bit address), which works in real mode and fails in virtual 8086 mode

## Files

| File | |
| --- | --- |
| `qemu-8.2.2-skl-vga.patch` | the device, in `hw/display/vga-pci.c` (QEMU 8.2.2) |
| `seabios-skl-vga.patch` | SeaVGABIOS: DISPI through the IOBAR, LFB in BAR2, EDID in BAR0, 384-byte EDID (DisplayID), unreal mode |
| `config.vga-skl` | SeaBIOS `.config` for the video BIOS (PCI ID 8086:1912) |
| `vgabios-skl.bin` | the built video BIOS |

## Use

```sh
qemu-system-i386 ... -vga none \
  -device skl-vga,addr=02.0,romfile=vgabios-skl.bin[,read-ns=500][,unreal=on][,vgamem_mb=64]
```

## What it showed

- Scrolling (`test/misc/scrollb.exe`, 300 scrolls of 800x560 in 16 bpp,
  `read-ns=500`): without the shadow buffer native scrolling took 64 s on
  Windows 2000 and 85 s on Windows 98; with it 19 s on both - as fast as
  a program copying its own buffer to the screen.
- `unreal=on`: Windows 2000 stops with 0x0000000A as soon as a driver
  calls VBE in virtual 8086 mode; WINVBE 1.3 runs such a BIOS in its x86
  emulator. Windows 9x fails to start (safe mode at the next start).
