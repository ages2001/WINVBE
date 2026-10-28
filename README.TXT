WINVBE v1.1 - Universal VESA/VGA/SVGA display driver for Windows 1.0-XP
========================================================================

WINVBE is a generic display driver that uses the VESA BIOS Extensions (VBE)
of the graphics card instead of card-specific code, in the spirit of VBEMP
and VBESVGA. It works with
  * Windows 3.0 and 3.1 in all operating modes: real (WIN /R), standard
    (WIN /S) and 386 enhanced (WIN /3),
  * Windows for Workgroups 3.1 and 3.11 (3.11 runs in 386 enhanced mode
    only); it is installed like Windows 3.1, with the WIN31 folder,
  * Windows 95, 98 and ME (installed with Device Manager),
  * Windows 2.x (2.03, 2.1x) and Windows/386 2.x,
  * Windows 1.0x (1.01 ... 1.04),
  * Windows NT 3.1, 3.5, 3.51, 4.0, 2000, XP and Server 2003 (x86), and
    Windows XP / Server 2003 x64 (miniport WINVBE.SYS + display driver
    WINVBE.DLL, see "Windows NT").

Files
-----
  INSTALL.EXE    DOS installer / configurator for every Windows version
  915PATCH.EXE   DOS tool that adds resolutions to the video BIOS of Intel
                 integrated graphics (like 915resolution on Linux)
  915PATCH.VXD   the same, run by Windows while it starts (SYSTEM.INI), for
                 Windows ME and other systems without an MS-DOS prompt
  GDITEST.EXE    GDI test program (optional, for testing)
  WIN31\         everything for Windows 3.1: WINVBE.DRV, WINVBE.386 (helper
                 VxD for 386 enhanced mode), WINVBECF.EXE (settings
                 program), WVBPOWER.SCR (monitor power saver), WVB*.FON
                 (large system fonts),
                 OEMSETUP.INF and the Microsoft files Setup would ask the
                 Windows 3.1 disks for (VGA fonts, grabbers, VDDVGA30.386)
  WIN30\         the same for Windows 3.0 (with the Windows 3.0 files)
  WIN9X\         Windows 95/98/ME: WINVBE.INF (for Device Manager),
                 WINVBE.DRV, WINVBE.386, WINVBECF.EXE, WVBPOWER.SCR,
                 915PATCH.VXD
  WIN2X\         Windows 2.x and Windows/386: WINVBE.DRV, WINVBECF.EXE,
                 WINVBE.LGO (start-up logo), WINVBE.GRB, PREP2X.BAT
  WIN1X\         Windows 1.0x: WINVBE.DRV, WINVBECF.EXE, WINVBE.LGO,
                 PATCH1X.BAT
  NT\            Windows NT:
   I386\NT3X\    Windows NT 3.1, 3.5, 3.51: WINVBE.SYS, WINVBE.DLL,
                 WINVBECF.EXE (settings and installation), OEMSETUP.INF
   I386\NT4-XP\  Windows NT 4.0, 2000, XP, Server 2003 (x86): WINVBE.SYS,
                 WINVBE.DLL, WINVBECF.EXE, WINVBE.INF
   AMD64\        Windows XP / Server 2003 x64: the same files, 64 bit
  BENIOKU.TXT    Turkish documentation (UTF-8)
  SRC\           complete source code and build scripts

  The WIN30, WIN31 and WIN2X folders contain Microsoft files (fonts, screen
  grabbers, VDDVGA30.386, WIN.CNF) so that no Windows disks are needed.
  They belong to Microsoft Windows and are only meant for use with your
  own licensed copy of Windows; do not redistribute them. The same applies
  to the standard Windows bitmaps, cursors and icons inside the drivers
  (see "Features").

What's new in v1.1
------------------
  * Standard Windows mouse pointers, icons and window bitmaps on every
    version: each driver carries the ones of Microsoft's own VGA driver
    for that Windows (1.01, 2.x, 3.0, 3.1, NT 3.x); the Windows 9x driver
    carries none, so Windows 95/98/ME use their own, exactly as with
    Microsoft's drivers. WIN30 and WIN31 now have separate WINVBE.DRV
    files. NT 4.0 and later always used the system's own.
  * NT 3.1: the default colours and the colour dialog table are those of
    NT 3.1's VGA driver.
  * Windows NT: graphics cards without a usable linear frame buffer (VBE
    1.2, ISA/VL-Bus cards, many cards emulated by 86Box/PCem) work: the
    banked VGA window at A000h is used, with a copy of the screen in system
    memory. Without any VBE graphics mode WINVBE starts in VGA 320x200x256
    - before, Windows NT stopped at boot with "The required system file is
    bad or missing" when the card had no LFB modes.
  * Windows NT: WINVBECF "Install driver" from a CD failed the next time
    ("Cannot copy WINVBE.DLL", NT Setup: "Unable to do the specified file
    copy operation"): files copied from a CD are read-only. The copies are
    now made writable (also existing read-only ones), and the temporary
    name for a file in use is a valid 8.3 name (NT 3.1 FAT).
  * Windows 3.x 386 enhanced mode: Windows stopped at its logo on current
    graphics cards (e.g. GeForce RTX, Radeon RX 7000): the standard VDD
    virtualizes the CRTC registers, through which their video BIOS talks to
    the card. WINVBE.386 now lets the video BIOS calls of Windows reach the
    CRTC and status registers directly, and the BIOS gets a real mode stack
    of its own (8 KB).
  * Windows 9x: big DIBs of Win32 programs, which GDI converts in bands,
    were drawn with a black line at every band; memory bitmaps and DIB bits
    in one big (32-bit) segment are addressed correctly.
  * Windows 98/95/ME (GitHub issue #1): Delphi programs such as ConText and
    NetMeter stopped at start ("This program has performed an illegal
    operation", exception 0EEDFADEh) in 32-bit colour. GDI hands a DIB in
    the screen's own format (32 bpp on a 32-bpp screen) to the driver
    unconverted, and WINVBE refused it, so SetDIBits and CreateDIBitmap
    failed - and with them the image lists (toolbar pictures) that Delphi
    programs load at start. 16 and 32 bpp DIBs (BI_RGB and BI_BITFIELDS)
    are now supported in SetDIBits, GetDIBits, SetDIBitsToDevice and
    StretchDIBits; GetDIBits also works for bitmaps wider than 2048 pixels.

Features
--------
  * VBE 1.2, 2.0 and 3.0 BIOSes; linear frame buffer (LFB) or the VGA
    window at A000h (64K banks), chosen automatically. The LFB address is
    verified before use (PCI BAR check and a read-back through the VGA
    window), so a frame buffer that is not reachable - e.g. moved above
    4 GB by "Above 4G decoding" / Resizable BAR - falls back to the VGA
    window instead of writing to a wrong address.
  * Shadow frame buffer (Windows 3.x 386 enhanced mode, 95/98/ME): a copy
    of the screen in system memory. Reading video memory over PCI/AGP/
    PCIe is very slow on modern cards (uncached, one bus round trip per
    access); scrolling, raster operations and the mouse cursor read the
    copy, writes go to both. Write combining (WINVBE.386, PAT or MTRR) lets
    the CPU send frame buffer writes as bursts. Together they make large
    True Color modes (e.g. 1920x1080x32) fast on GeForce/Radeon cards.
  * 256 colors (8 bpp, palette), 32K (15 bpp), 64K (16 bpp), 16.7M (24 and
    32 bpp) colors.
  * Every resolution the BIOS offers (640x480 ... 3840x2160 and beyond, up
    to 4096x4096), plus any custom resolution on Bochs/QEMU/VirtualBox
    adapters (DISPI interface).
  * VMware SVGA II adapter (VMware, VirtualBox "VMSVGA", QEMU -vga vmware),
    Windows 3.x (Windows 9x: SYSTEM.INI VMware=1): any resolution in True
    Color (32 bit), screen-to-screen copies and solid fills done by the host
    (2D acceleration), screen updates through the command FIFO.
  * DirectDraw on Windows NT 4.0, 2000, XP and XP x64: primary surface,
    page flipping in video memory, off-screen surfaces, vertical blank
    (games and video players get full-screen exclusive mode with flipping).
  * 3D: Direct3D (DirectX 5-7 software "RGB" device) renders into flip
    chains in video memory, OpenGL runs with Microsoft's
    software OpenGL (Windows NT 3.51 and later) - both computed by the CPU;
    there is no hardware 3D (VBE has none, see Known limitations).
  * Power management, Windows 3.1/WfW and 95/98/ME: monitor stand by and
    off through VBE/PM (DPMS) with the WINVBE Power Saver screen saver
    (Windows 95/98/ME also with their own energy saving settings); after a
    suspend (APM or ACPI) WINVBE.386 and the driver set the mode again and
    repaint the screen. See "Power management (Windows 3.x/9x/ME)".
  * Power management (Windows 2000, XP, Server 2003, x64): monitor power
    saving through VBE/PM (DPMS stand by, suspend, off), Stand By (S3) and
    Hibernate (S4). On resume WINVBE restores the PCI registers of the
    adapter, runs the video BIOS again (POST) when it was switched off and,
    on Bochs/QEMU/VirtualBox adapters, sets the mode directly when the BIOS
    is not usable.
  * DCI primary surface (Windows 3.1 with the DCI runtime, Windows 95/98/
    ME): direct frame buffer access for Video for Windows and other DCI
    clients.
  * Monitor detection through EDID (VBE/DDC): monitor name, native
    resolution and refresh range; "Automatic" picks the native resolution.
  * Refresh rate control on VBE 3.0 BIOSes: monitor native timing (EDID),
    or 56-120 Hz computed with VESA CVT (CRT) or CVT reduced blanking (LCD).
  * Small (96 dpi) and large (120 dpi) fonts; WINVBE brings its own large
    system fonts (WVB*.FON) and uses Microsoft's 8514 fonts when Windows
    already has them.
  * The standard bitmaps (window buttons, scroll bar arrows, check boxes),
    mouse pointers and message box icons of each Windows version, taken
    from its own VGA driver: Windows 1.01 (EGA), 2.x, 3.0, 3.1 and NT 3.x;
    Windows 95/98/ME and NT 4.0 and later use their built-in ones.
  * Sky blue default colours (as in the Windows 3.x 256-color drivers) on
    both Windows 3.0 and 3.1, with the cream application workspace on 3.1;
    the classic navy scheme is available as an option. Windows 2.x gets its
    classic look (blue caption, cyan menu bar and desktop), Windows 1.0x its
    own (blue caption, yellow menu bar, green desktop); selected files are
    black, selected menu items red, as with Microsoft's EGA/VGA drivers.
  * Full-screen DOS sessions in standard and 386 enhanced mode.
  * Device-independent bitmaps, stretching, palettes, dithered brushes,
    styled lines, text output with all Windows 3.0 fonts, software cursor.
  * Falls back to standard VGA 320x200x256 when no VBE is present.

Installation
------------
Method 1 - DOS installer (recommended, all Windows versions, no disks)
  Copy the WINVBE11 folder to the hard disk (e.g. C:\WINVBE11), exit
  Windows and run
      C:\WINVBE11\INSTALL
  INSTALL looks for Windows in the folders of the hard disks (one level,
  e.g. C:\WINDOWS, C:\WIN30, D:\WIN31), lists what it finds (Windows 1.0x,
  2.xx, 3.0, 3.1x, Windows for Workgroups 3.1x, 95/98/ME, NT) and asks for
  the number of the one to install for; C:\WINDOWS is the default. Another
  directory can be typed instead (e.g. E:\OLD\WIN30).
  It recognises the Windows version and takes the files of the matching
  folder: 1.x -> WIN1X, 2.x -> WIN2X, 3.0 -> WIN30, 3.1 and Windows for
  Workgroups 3.1/3.11 -> WIN31. How: SYSTEM.INI means Windows 3.x,
  WIN200.BIN 2.x, WIN100.BIN 1.x; for 3.x the version of SYSTEM\USER.EXE
  (version resource, else the Windows version in its header: 3.00 or
  3.10) and, for Windows for Workgroups, of SYSTEM\WFWNET.DRV. It then
  lists the available video modes and monitor data and asks for the mode,
  refresh rate and font size. Start Windows with WIN.

  Unattended use:
      INSTALL /W:1024 /H:768 /D:16 [/R:75] [/A:0|1|2] [/LARGE] [/DIR:C:\WIN30]
      INSTALL /AUTO          automatic (monitor native) mode
      INSTALL /LIST          show modes and monitor information only
      INSTALL /VGA           switch Windows back to its original driver

  Windows 3.x: INSTALL copies WINVBE.DRV and the WVB fonts to the SYSTEM
    directory, WINVBECF.EXE to the Windows directory and updates SYSTEM.INI
    (the original is saved once as SYSTEM.WVB). VGA fonts and grabbers that
    Windows does not have yet (Windows installed for another display) are
    copied from the WIN30/WIN31 folder.
  Windows 1.x/2.x: the display driver is part of WIN100.BIN/WIN200.BIN
    (built by Windows Setup). INSTALL links WINVBE.DRV into these files
    itself - no Setup, no disks - copies WINVBECF.EXE to the Windows
    directory and writes the settings to WIN.INI [WINVBE]. The files of the
    last Windows Setup are kept as WIN100.BAK/.OBK or WIN200.BAK/.OBK;
    INSTALL /VGA puts them back (the original EGA/VGA/CGA... driver).
    Windows/386 2.x is handled the same way (start it with WIN386).
  Windows 3.x in 386 enhanced mode: INSTALL also copies WINVBE.386 and adds
    device=winvbe.386 to [386Enh]. This helper VxD switches the Bochs/QEMU/
    VirtualBox (DISPI) and VMware SVGA II modes off for full-screen MS-DOS
    boxes and on again for Windows, and brings the screen back after a
    suspend (on every adapter). INSTALL also copies WVBPOWER.SCR to the
    Windows directory (Windows 3.1).
  Intel integrated graphics (830 ... G45, Atom) whose video BIOS lacks the
    monitor resolution: INSTALL also sets up 915PATCH (AUTOEXEC.BAT), see
    "915PATCH" - on other PCs it does not.
  Windows NT: INSTALL refuses NT directories and NTVDM; use WINVBECF.EXE
    (see "Windows NT").

Windows 95, 98 and ME
  The driver is installed in Windows with Device Manager (INSTALL refuses
  these versions and says so):
  1. Control Panel > System > Device Manager > Display adapters > (your
     adapter, e.g. Standard Display Adapter (VGA)) > Properties > Driver >
     Change Driver (Windows 98/ME: Update Driver, "Display a list of all the
     drivers" ...) > Have Disk, and give the WIN9X folder.
  2. Choose "WINVBE Universal VESA/VGA/SVGA" and restart the computer.
  3. Resolution and colours: Display Properties > Settings (Windows offers
     the modes listed in WINVBE.INF; after a change Windows restarts) or
     WINVBECF.EXE (Start > Run > winvbecf), which also sets the refresh
     rate, memory access and custom sizes and puts the modes of your card
     into the Display Properties list. Both write the same setting.
     Font size: Display Properties (Small / Large Fonts).
  WINVBE is a Windows 3.1 style driver for these versions (no DirectDraw
  HAL - DirectDraw runs in emulation, with WINVBE's DCI primary surface;
  no change of resolution without a restart). The INF also installs
  WINVBE.386 ([386Enh] device=winvbe.386) and, on Intel graphics whose
  video BIOS 915PATCH can patch, 915PATCH.VXD (see "915PATCH"). MS-DOS
  Prompt windows and full-screen sessions work: WINVBE and WINVBE.386
  restore Windows' mode after the video BIOS calls of a DOS box, on real
  cards as well as on Bochs, QEMU and VirtualBox adapters (Windows 9x has
  no mini-VDD for VBE modes; see Known limitations). Graphics programs in
  a window may still not be shown correctly; run them full screen.

Method 2 - from inside Windows 3.x
  Run WINVBECF.EXE from the WIN31 or WIN30 folder (File > Run in Program
  Manager), tick "Use WINVBE as the display driver", choose the settings
  and press OK. The files are copied and Windows restarts.

Method 3 - Windows Setup (OEMSETUP.INF), no disks needed
  Windows 3.1: run SETUP from DOS in the Windows directory, or Windows
    Setup in Main group > Options > Change System Settings. Display ->
    "Other display (Requires disk from OEM)", path: the WIN31 folder (e.g.
    C:\WINVBE11\WIN31). Choose a colour depth / font size. All files Setup
    needs are in the folder; it installs VDDVGA30.386 + WVB30.3GR (the VGA
    grabber pair for Windows 3.0 style display drivers). DOS Setup builds
    WIN.COM without the start-up logo (it would look for the logo files on
    drive A:).
  Windows 3.0: only the DOS Setup has the "Other" entry (Windows Setup of
    3.0 cannot install OEM display drivers). Run SETUP in the Windows
    directory, Display -> "Other (Requires disk provided by a hardware
    manufacturer)", path: the WIN30 folder. Profiles: small or large fonts.
    Windows 3.0 Setup cannot write extra settings, so choose resolution and
    colours afterwards in WINVBECF.EXE (run it from the WIN30 folder once;
    it copies itself to the Windows directory).

To add an icon: Program Manager > File > New > Program Item, command line
WINVBECF.EXE.

Windows 2.x / Windows/386 with Windows Setup (optional)
  INSTALL is the easy way. To use Setup's "Other" display choice instead:
  run SETUP from the Windows 2.x Setup disk, choose "Other (requires disk
  provided by a hardware manufacturer)" and type C:\WINVBE11\WIN2X (the
  folder holds WINVBE.DRV, WINVBE.LGO and the grabber WINVBE.GRB).
  Windows/386 Setup also wants WINVBE.3EX and WINVBE.386 from your own
  Windows/386: run PREP2X C:\WIN386 (or PREP2X A:) in the WIN2X folder.
  Windows/386 also needs a CONFIG.SYS without DOS=HIGH.

Windows 1.0x with Windows Setup (optional)
  INSTALL is the easy way. Windows 1.0x Setup has a fixed display list and
  no "Other" choice; PATCH1X puts WINVBE in the place of "EGA (more than
  64K) with Enhanced Color Display" on COPIES of the Setup and Build disks
  (PATCH1X A: for each copy, then run SETUP from the copies).
  On MS-DOS 5 or later Windows 1.0x cannot start programs unless DOS
  reports an older version: add DEVICE=C:\DOS\SETVER.EXE to CONFIG.SYS and
  run SETVER WIN100.BIN 3.30 once (INSTALL reminds you). This is a
  property of Windows 1.0x, not of WINVBE.
  The mouse driver of Windows 1.0x supports Microsoft serial and bus mice
  (not PS/2 mice).

Windows NT
----------
WINVBE for Windows NT is a video miniport (WINVBE.SYS) that sets the VBE
modes through the video BIOS (Int 10h of the video port driver; the x64
version uses the BIOS emulator of Windows XP/2003 x64) and a frame buffer
display driver (WINVBE.DLL). Cards without a usable linear frame buffer
use the banked VGA window at A000h (slower; no DirectDraw); without VBE
graphics modes WINVBE uses VGA 320x200x256, so that Windows still starts.
Use the folder for your system:
  NT\I386\NT3X     Windows NT 3.1, 3.5, 3.51 (WINVBE.DLL runs in CSRSS)
  NT\I386\NT4-XP   Windows NT 4.0, 2000, XP, Server 2003
  NT\AMD64         Windows XP x64, Server 2003 x64
Installation (log on as Administrator):
  * All versions: run WINVBECF.EXE from the folder and press "Install
    driver", then restart. Windows NT 3.x/4.0: WINVBECF copies the files,
    creates the winvbe service (the old display service is disabled; NT 3.1
    keeps VGA for WOW and full-screen DOS) and sets 800x600, 16 bit.
    Windows 2000/XP/2003: WINVBECF installs WINVBE.INF for the display
    adapter (Plug and Play).
  * Windows 2000/XP/2003, x64: alternatively Device Manager > Display
    adapters > (adapter) > Update Driver > Install from a specific location
    > Don't search > Have Disk > the folder.
  * Windows NT 4.0: alternatively Control Panel > Display > Settings >
    Display Type > Change > Have Disk > NT\I386\NT4-XP.
  * Windows NT 3.1: alternatively Windows NT Setup > Options > Change
    System Settings > Display > Other > NT\I386\NT3X (OEMSETUP.INF), and
    choose a resolution. Windows NT 3.5/3.51: Control Panel > Display >
    Change Display Type > Change > Other > NT\I386\NT3X.
  WINVBECF keeps the display entry of Windows NT 3.x Setup ("WINVBE VESA
  1024 x 768, 16 bit ...") up to date when it changes the mode.
Settings: WINVBECF.EXE (copied to the Windows directory) shows adapter and
monitor, lists the modes and sets resolution, colours and refresh rate
(NT 3.51 and later at once; NT 3.1/3.5 after a restart), the 8-bit DAC,
the debug log, which refresh rates are offered (monitor/EDID, all, 60 Hz)
and the built-in 915 patch (the same commands as 915PATCH.EXE, applied by
the miniport at every start). The Display control panel works as well.
Modes: 8, 15, 16, 24 and 32 bit, every VBE mode (linear frame buffer, or
the VGA window when there is none), refresh rates through VBE 3.0 (EDID
limits). Registry value WinVbe.Access (DWORD, in the device key, e.g.
Services\winvbe\Device0): 0 automatic, 1 linear frame buffer only, 2 VGA
window only.
DirectDraw (4.0/2000/XP/x64), Direct3D, OpenGL and power management: see
Features. NT 3.x has no DirectDraw; OpenGL works on NT 3.51 (Microsoft's
or Mesa's OPENGL32.DLL). The 915 patch box of WINVBECF is shown only on
Intel chipsets 915PATCH supports; elsewhere the adapter box says "not
supported".

Display Settings (WINVBECF.EXE)
-------------------------------
  Graphics adapter and monitor   VBE version, video memory, adapter name,
                                 EDID monitor name, type, native mode and
                                 supported frequency range.
  Screen resolution              Automatic (monitor native), 320x200 VGA,
                                 every VBE resolution; "Custom" on
                                 Bochs/QEMU/VirtualBox.
  Colors                         256 / 32K / 64K / 16M (24 bit) / 16M (32 bit)
  Refresh                        BIOS default, monitor native, 56..120 Hz
                                 (VBE 3.0 BIOS needed)
  Monitor                        CRT (CVT) or LCD (CVT reduced blanking)
  Memory                         Automatic / Linear (LFB) / VGA window
  Font size                      Small (96 dpi) / Large (120 dpi)
  Debug log                      driver log to port E9h or a serial port

Windows 1.x/2.x have their own WINVBECF.EXE (in WIN1X/WIN2X, copied to
the Windows directory by INSTALL): the same settings without the font size
and the linear frame buffer (Windows 1.x/2.x run in real mode), stored in
WIN.INI. After OK it offers to end the Windows session; the new settings
are used when Windows is started again. Run it from MS-DOS Executive.

Large fonts use Microsoft's 8514SYS/8514FIX/8514OEM.FON when they are in
the SYSTEM directory, else WINVBE's own WVBSYS/WVBFIX/WVBOEM.FON (copied
by INSTALL and WINVBECF), so the Windows disks are not needed.

Refresh rate and monitor type can only be changed on VBE 3.0 BIOSes. The
virtual graphics cards of VirtualBox, QEMU and Bochs report VBE 2.0 (or have
no CRTC timing support), so there these fields show "Fixed (virtual)" /
"Not used" - the refresh rate of a virtual screen is set by the host.

SYSTEM.INI settings ([WINVBE] section)
--------------------------------------
(Windows 1.x/2.x: the same keys in the [WINVBE] section of WIN.INI; DPI,
Scheme and LfbCheck/LfbAddr are not used there.)
  Width=1024        screen width  (0 = automatic: EDID native or 1024)
  Height=768        screen height (0 = automatic: EDID native or 768)
  Depth=16          8, 15, 16, 24 or 32 (0 = automatic = 16)
  Mode=0            explicit VBE mode number, e.g. 0x117; 0x13 = VGA 320x200
  Refresh=0         0 = BIOS default, 1 = monitor native (EDID),
                    50..240 = refresh rate in Hz (VBE 3.0)
  Timing=1          0 = CVT (CRT monitors), 1 = CVT reduced blanking (LCD);
                    default: 1 for digital EDID monitors, else 0
  Access=0          0 = automatic, 1 = linear frame buffer only,
                    2 = VGA window at A000h only (64K banks)
  LfbCheck=1        verify the LFB address before use (0 = trust the BIOS)
  LfbAddr=          override the LFB physical address, e.g. 0xE0000000
                    (only for BIOSes that report a wrong address)
  Dispi=1           allow custom resolutions on Bochs/QEMU/VirtualBox
  DPI=96            96 = small fonts, 120 = large fonts; when missing, 120
                    if [boot] fonts.fon is 8514SYS.FON, else 96
  Scheme=0          default system colours: 0 = by Windows version,
                    1 = Windows 3.0 sky blue, 2 = Windows 3.1 sky blue
                    (cream workspace), 3 = classic navy;
                    used only for colours not set in WIN.INI [colors]
                    (Control Panel > Color overrides them)
  Dac8=1            use the 8-bit DAC in 256-color modes when available
  ShadowBuffer=1    copy of the screen in system memory (386 enhanced mode,
                    95/98/ME; 0 = draw in video memory only)
  WriteCombining=1  frame buffer write combining through WINVBE.386
  VddMode=          Windows 3.x full-screen DOS boxes: 1 = the VDD tells
                    the driver to leave and set its mode (4001h/4002h);
                    default: 1 on real cards, 0 on Bochs/QEMU/VirtualBox/
                    VMware (WINVBE.386 handles those)
  VgaReserve=1      keep the first 256 KB of video memory (VGA text and
                    font planes) free: full-screen MS-DOS boxes keep their
                    font after Alt+Tab (0 = screen starts at offset 0)
  Forcevga=0        1 = always use VGA 320x200x256
  Repost=1          after a resume: run the video BIOS initialisation again
                    when the mode cannot be set (0 = never)
  VMware=1          use the VMware SVGA II adapter when present (True
                    Color: Depth=24 or 32); default 1, Windows 9x: 0
  VMwareAccel=1     host copies/fills (0 = CPU only)
  VMwareFifo=1      command FIFO and screen updates (0 = the host traces
                    the frame buffer itself; no acceleration)
  Debug=0           0 = off, 1 = log, 2 = verbose
  DebugPort=0xE9    E9h (Bochs/QEMU debug console) or a COM port, e.g. 0x3F8

Power management (Windows 3.x/9x/ME)
-------------------------------------
Monitor (DPMS): WVBPOWER.SCR, "WINVBE Power Saver", is a screen saver for
Windows 3.1 / Windows for Workgroups 3.1x (also usable on 95/98/ME). It is
installed by INSTALL, Windows Setup (OEMSETUP.INF) and the Windows 9x INF;
choose it in Control Panel > Desktop > Screen Saver (95/98/ME: Display
Properties > Screen Saver). When it starts, the screen goes black; Setup
sets after how many minutes the monitor goes to stand by and is switched
off (default: stand by at once, off after 10 minutes). A key or a mouse
movement switches the monitor on again. Setup shows which states the video
BIOS supports (VBE/PM); "Test" switches the monitor off for 5 seconds.
The driver blanks the picture itself (sequencer and attribute controller)
and signals the monitor through VBE/PM 4F10h; without VBE/PM the screen is
only blanked. Programs can use the driver escape 7702h (WINVBE_DPMS,
WORD 0 on, 1 stand by, 2 suspend, 4 off, FFFFh query).
Windows 95/98/ME: the energy saving settings of Windows (Display
Properties > Screen Saver, "Monitor is Energy Star compliant" in Change
Display Type; 98/ME: Power Management) work with WINVBE as well - Windows
switches the display out and back as for a full-screen MS-DOS box.
Suspend / resume: WINVBE.386 receives the power events of Windows (Windows
3.1 with APM - POWER.DRV and VPOWERD.386 - and Windows 95/98/ME with APM
or ACPI). Before suspend it notes the Bochs/QEMU/VirtualBox and VMware
SVGA II state, after resume it restores it and, when Windows is idle,
asks the display driver to check the screen: if the adapter has lost the
mode, the driver sets it again (running the video BIOS initialisation once
more when the mode set fails, Repost=1) and repaints the screen. The BIOS
data of windowed MS-DOS boxes is kept unchanged. Standard and real mode
Windows and Windows 3.0 have no power events: there the BIOS restores the
screen (APM BIOSes usually do).

Hardware notes
--------------
  * Older cards (S3 Trio32/64, Cirrus Logic, Tseng ET4000, Trident, ATI
    Mach, Matrox ...): most have VBE 1.2/2.0 in ROM. With VBE 1.2 or no LFB
    the banked window is used. If a card has no VBE in ROM, load a VBE TSR
    such as UniVBE or S3VBE first.
  * Modern cards (AMD Radeon RX 7800, NVIDIA GeForce, Intel ...): the
    driver needs the legacy video BIOS, so enable CSM / Legacy boot in the
    firmware setup. Cards or boards that only offer UEFI GOP have no VBE and
    cannot run any DOS/Windows 3.x VESA driver. Many modern BIOSes only
    offer 32 bpp modes, sometimes also 16 bpp; 8 bpp may be missing.
  * "Above 4G decoding" / "Resizable BAR": Windows 3.x can only address
    physical memory below 4 GB. If the frame buffer was moved above 4 GB
    WINVBE detects it and uses the VGA window at A000h instead (slower -
    every 64 KB a bank switch through the BIOS, no write combining - but
    works at every resolution the BIOS can reach through the window). If
    you can, turn these options off for full speed; with CSM enabled most
    boards disable Resizable BAR anyway. WINVBECF shows what is used
    ("linear frame buffer + WC + shadow" / "VGA window ...").
  * Refresh rates other than the BIOS default need a VBE 3.0 BIOS that
    supports CRTC timings. If the picture is lost after changing the rate,
    start INSTALL from DOS and choose refresh 0, or set Refresh=0.
  * Real mode (WIN /R) and Windows 1.x/2.x cannot use the linear frame
    buffer; the banked window is used automatically, so the BIOS must
    support banked modes.
  * Intel integrated graphics (830 ... G45, Atom) often lack the native
    resolution of notebook panels and wide monitors (e.g. 1280x800,
    1440x900, 1680x1050). 915PATCH adds it, see below.

Known limitations
-----------------
  * Windows 9x in 32 bpp: GDI itself (not the driver) cannot create a
    device-dependent bitmap whose width is a multiple of 2048 pixels, or
    one of more than about 16 MB (e.g. 2560x1600). Programs that make a
    screen-sized bitmap can fail at 2048x1536x32 and 2560x1600x32; use
    24 or 16 bpp for those resolutions.
  * MS-DOS boxes on real video cards: Windows' standard VGA virtual
    display device does not know the VBE modes. WINVBE.386 sets the BIOS
    mode of a box that goes full screen again (screen contents kept), and
    when a box in a window sets a video mode through the BIOS - modern
    video BIOSes do that on the hardware - the driver sets its own mode
    again (the screen flashes once). Graphics programs in a window may
    still not be shown correctly; run them full screen (Alt+Enter).
  * No hardware acceleration (except copies and fills on the VMware SVGA II
    adapter) and a software mouse cursor; speed depends on the CPU and the
    bus (see Shadow frame buffer above). Windows NT draws directly into the
    frame buffer (write combined on NT 4.0 and later). 3D (Direct3D,
    OpenGL) is computed by the CPU: VBE has no 3D, and the VMware SVGA3D
    interface would need a complete Direct3D/OpenGL hardware driver of its
    own.
  * Power management: Windows NT 3.x/4.0 have none for display adapters.
    Windows 3.0 has no screen saver and no power events. QEMU's BIOS does
    nothing on an APM suspend and reports no resume event, so the resume
    path of Windows 3.1/9x was tested with simulated events (also with a
    lost mode); Windows 98/ME were not available for tests. In QEMU,
    Windows 2000 did not wake from Stand By (it stops before the display
    driver is called again); Hibernate works there, and XP x64 wakes from
    both.
  * Windows 3.x standard mode on QEMU: going back to an existing
    full-screen DOS program can show a blank screen; Microsoft's VGA.DRV
    behaves the same there (an emulator issue, not of WINVBE).
  * Windows 9x with the VMware SVGA II mode (VMware=1) on QEMU: the SVGA
    frame buffer shares the VGA memory, so full-screen MS-DOS boxes can be
    damaged; the default (VBE/DISPI mode) has no such problem.
  * 16-color (4 bpp planar) VBE modes are not supported; use the standard
    Windows VGA driver for 16 colors.
  * Windows 3.0 in 256 colors shows some Program Manager icons in muted
    colours; Windows 3.0 converts the 16-colour icons itself (Microsoft's
    MCGA256 driver shows the same). 15/16/24/32 bpp are not affected.

  * Windows 1.x/2.x run in real mode: the driver (about 100 KB with its
    buffers) uses conventional memory, so less memory is free for
    applications than with Microsoft's EGA/VGA drivers. 256 colours use a
    fixed palette (6x6x6 colour cube + greys); 2.x has no palette manager.
  * Windows/386 2.x: DOS programs in a window are not shown correctly; run
    them full screen (COMMANDF.PIF etc.). Do not load SMARTDRV (MS-DOS 5/6)
    with Windows/386 2.x: after large file copies (e.g. INSTALL's first
    run) Windows/386 may stop with "Internal stack overflow"; reboot.
  * Windows 1.0 Developer Release 5 (1984) is a pre-release with a
    different driver interface and module format; it is not supported.

915PATCH - resolutions for Intel integrated graphics
----------------------------------------------------
The video BIOS of Intel 830, 845G, 852/855GM, 865G, 915G/GM, 945G/GM/GME,
946GZ, G965/Q965/GM965, G31/G33/Q33/Q35, GM45/G41/G43/G45/Q45/B43 and
Atom D4xx/D5xx/N4xx/N5xx graphics contains a table of its modes. 915PATCH
overwrites entries of this table with the resolution you need, like
915resolution does on Linux:
    915PATCH /LIST                       chipset, BIOS type, mode table
    915PATCH /AUTO                       monitor native resolution (EDID)
    915PATCH /AUTO 1280 800              given resolution
    915PATCH 5C 1280 800 [32]            replace Intel mode 5Ch
    915PATCH /DUMP [file]                save the video BIOS (VBIOS.ROM)
    915PATCH /CHECK                      no output; exit code 0 = this PC
                                         has a video BIOS 915PATCH can patch
    /LOG with any command                also write 915PATCH.LOG next to
                                         915PATCH.EXE (/AUTO always does it,
                                         /NOLOG: not);  /Q: no screen output
The table and its layout are found by themselves, also when the blocks are
not evenly spaced (GM965 and later); a table is only used when its modes
are real ones (bits per pixel 8-32, 640x480/800x600/1024x768 present), so
look-alike bytes elsewhere in the BIOS are never patched. GM45/GL40/GS45
(Cantiga) BIOSes keep their timings as EDID detailed timings, one per
refresh rate (type 4): 915PATCH writes the new size into each with GTF
timings for 60, 75 and 85 Hz; the empty run-time slots of these BIOSes
(modes 60h-71h) are shown as "(empty)" and left alone. /TYPE:1 ... /TYPE:4
forces a layout (like 915resolution -t). When a BIOS has no known table,
915PATCH says so and changes nothing; please send the VBIOS.ROM of /DUMP
with a report.
/AUTO replaces, for 8, 16 and 32 bits, the mode with the largest resolution
(usually one you do not need, e.g. 1920x1440). The video BIOS runs from
shadow RAM, so the change lasts until the next reset: put the command in
AUTOEXEC.BAT, before Windows is started. On other hardware (other chipsets,
a separate graphics card, Core i graphics) 915PATCH changes nothing and
ends with an error message (exit code 2).

Automatic set-up (only on PCs whose video BIOS 915PATCH can patch; on all
others nothing of it is installed):
  * INSTALL (Windows 1.0x - 3.1x / WfW): runs 915PATCH /AUTO at once (the
    monitor resolution is then in its mode list), copies 915PATCH.EXE to
    the Windows directory and adds "915PATCH.EXE /AUTO" to
    AUTOEXEC.BAT (its messages show while the PC starts); INSTALL /VGA
    removes the line again.
  * Windows 95/98/ME (Device Manager, WINVBE.INF): after the restart
    WINVBECF /915 (RunOnce) puts device=915patch.vxd into SYSTEM.INI and
    offers to restart; on other PCs it deletes 915PATCH.VXD again. The VxD
    runs /AUTO silently at every start of Windows.
  * Windows NT: the miniport patches the BIOS itself (/AUTO by default, the
    "915 patch" box of WINVBECF), without messages.
  WINVBECF shows the result in its adapter box ("915 patch: ..."), on
  Windows 3.x/9x from the 915PATCH.LOG of the Windows directory.

915PATCH.VXD does the same while Windows starts, before the display driver
uses the video BIOS. Use it on Windows ME, which cannot start in MS-DOS
mode or run AUTOEXEC.BAT programs; it also works with Windows 95/98 and
Windows 3.1/WfW 3.11 in 386 enhanced mode.
  1. Copy 915PATCH.VXD to the SYSTEM folder of Windows (C:\WINDOWS\SYSTEM).
  2. In SYSTEM.INI (C:\WINDOWS\SYSTEM.INI, e.g. with Start > Run > sysedit)
     add a line under [386Enh]:
         device=915patch.vxd
  3. Optional, a section with the same commands as 915PATCH.EXE (several
     ones separated by ;). Without it the VxD runs /AUTO:
         [915PATCH]
         Patch=/AUTO 1280 800
     or  Patch=5C 1280 800 32; 5A 1280 800 16
         Log=0          no 915PATCH.LOG
  4. Restart. 915PATCH.LOG in the Windows folder shows what was done. On
     Windows 9x/ME then choose the resolution in Display Properties or in
     WINVBECF.
The VxD unloads itself after the patch (it uses no memory in Windows). To
remove it, delete the device=915patch.vxd line.

Test programs (TEST folder)
---------------------------
  DDTEST [w h bpp [seconds [1]]]  DirectDraw full screen: flip chain in
                                   video memory, locks, vertical blank,
                                   palette (1 = 3D-capable flip chain)
  D3DTEST [w h bpp [seconds]]     Direct3D 7, rotating triangle in a flip
                                   chain in video memory (HAL or RGB device)
  GLTEST [seconds]                OpenGL window, rotating triangle
  DCITEST                         DCI primary surface (Windows 9x)
  W32GDI [rounds]                 Win32 GDI drawing as applications do it
                                  (DIB sections, GetDIBits, image lists,
                                  icons, pens, regions, fonts, SetDIBits/
                                  GetDIBits round trips); W32GDI.TXT names
                                  the last step after a crash and ends
                                  with the number of failed checks
  DBGRUN program.exe [args]       runs a program under the Win32 debugger
                                  and logs its DLLs and exceptions (with
                                  the class and message of Delphi
                                  exceptions) to DBGRUN.TXT - for reports
                                  of programs that fail to start
Each writes a report (DDTEST.TXT etc.) next to the program. They are
Win32 programs (Windows 95 and NT 3.51 or later; DDTEST/D3DTEST need
DirectX, NT 4.0 SP3 or later).

Troubleshooting
---------------
  * Black or garbled screen: exit with Ctrl+Alt+Del or reboot, then run
    INSTALL /VGA from DOS to return to the VGA driver (Windows 1.x/2.x:
    the original driver), or edit SYSTEM.INI: [boot] display.drv=vga.drv.
  * Try Access=2 (VGA window), Refresh=0, a lower Depth, or Forcevga=1.
  * Debug=1 writes a log to port E9h (Bochs/QEMU: -debugcon file:log.txt)
    or to the serial port given in DebugPort.
  * A Win32 program fails or crashes at start (Windows 9x): run it with
    TEST\DBGRUN.EXE and TEST\W32GDI.EXE and attach DBGRUN.TXT and
    W32GDI.TXT to the report.

Tested
------
v1.1: standard bitmaps, cursors and icons checked (message boxes, window
buttons, scroll bars, I-beam) on Windows 1.01, 2.03, 3.0, 3.1, 95, NT 3.1
and NT 3.51; the resources in the drivers compared byte by byte with
those of Microsoft's drivers. NT banked modes (no LFB, forced) on NT 3.1,
4.0 and XP x64, the VGA 320x200 fallback on NT 3.1. Windows 3.1 with the
CRTC untrapping in the simulation of real hardware, with full-screen and
windowed MS-DOS boxes. A Win32 GDI test (TEST\W32GDI.EXE: DIB sections in
every format, GetDIBits, image lists, icons, pens, regions, fonts) on
Windows 95 at 8, 16, 24 and 32 bpp.
QEMU (SeaBIOS VBE 3.0 / Bochs DISPI) with Windows 3.0 (with Multimedia
Extensions) in real, standard and 386 enhanced mode, and Windows 3.1 in
386 enhanced mode: 8/15/16/24/32 bpp, LFB and banked access, 320x200 to
1600x1200 and custom DISPI sizes, VBE 3.0 refresh rates, EDID, full-screen
DOS switching, configuration program and both installers. LFB checks
with simulated wrong addresses. Windows 3.1 Setup (DOS and Windows mode)
and Windows 3.0 DOS Setup from the WIN31/WIN30 folders without any disk.
Windows 95 (RTM) installed with Device Manager: 8/16/24 bpp, Display
Properties and WINVBECF mode changes, Explorer, Paint, MS-DOS Prompt in a
window and full screen.
Windows 98 SE installed with Device Manager (Have Disk): 8/16/24/32 bpp,
800x600 to 2560x1440; ConText (Delphi) at 1920x1080x32, which failed with
1.0 and the first 1.1 build (issue #1); ImageList_Read of its toolbar data
and SetDIBits/GetDIBits round trips in 16/24/32 bpp DIBs (BI_RGB and
BI_BITFIELDS, 7 to 2500 pixels wide) on every screen depth; W32GDI.EXE
(also in the simulation of real hardware, without DISPI); MS-DOS Prompt in
a window and full screen; WINVBECF.
Windows 2.03 and Windows/386 2.11, installed with INSTALL and with their
Setup ("Other" display): 8/16/24/32 bpp up to 1280x800, menus, Paint,
Calculator, Reversi, full-screen DOS sessions, WINVBECF, INSTALL /VGA.
Windows 1.01 installed with INSTALL (EGA installation changed to WINVBE)
and from patched disk copies: 8/16 bpp up to 1280x800, keyboard, serial
mouse, Clock, WINVBECF. 915PATCH: unit tests with synthetic BIOS images of
all four table layouts; the real video BIOS of a GM45 notebook (mode table
with empty slots, EDID timings, look-alike bytes at C000:B808); QEMU Q35
with a test ROM carrying that GM45 table: 915PATCH.EXE (shadow RAM unlock,
/LIST, /AUTO, mode replacement, /DUMP), 915PATCH.VXD (Windows 3.1) and the
miniport of Windows NT 3.51; the error paths on unsupported hardware.
Automatic 915PATCH set-up: INSTALL on Windows 3.1 (Q35 test ROM: /AUTO,
AUTOEXEC.BAT, log, WINVBECF line; nothing on a PC without Intel graphics;
/VGA removes the line), WINVBECF /915 on Windows 95 (adds the VxD and
restarts, silent VxD, WINVBECF line; removal on other PCs). 915PATCH.VXD:
Windows 95 and Windows 3.1 (386 enhanced mode) with a test ROM (patch with
EDID and given resolution, SYSTEM.INI settings, log) and on unsupported
hardware.
VMware SVGA II (QEMU -vga vmware): Windows 3.1 at 1152x864x32 with host
copies/fills, full-screen DOS switching with WINVBE.386; Windows 95 with
WINVBE.386 (DISPI and SVGA II). DirectDraw: own test program (flip chain,
locks, palette, vertical blank) at 8/16/32 bit on Windows 2000 and XP x64;
DCI primary surface with DCIMAN32 on Windows 95.
Windows NT in QEMU: 3.1, 3.51, 4.0, 2000 and XP x64 run with WINVBE;
WINVBECF "Install driver" on NT 4.0 and Windows 2000. DOS windows: a short
MS-DOS Prompt window / full-screen (where available) / switch-back test on
every Windows version (1.0x, 2.x, 3.0 real/standard/enhanced, 3.1, 95, NT
3.1, 3.51, 4.0, 2000, XP x64). DirectDraw flip chains on NT 4.0, 2000 and
XP x64; Direct3D 7 (RGB device, flipping in video memory) on 2000 and XP
x64; OpenGL on NT 3.51 (Mesa), NT 4.0, 2000 and XP x64. Power: Hibernate
on Windows 2000 and XP x64, Stand By (S3) with BIOS re-POST and PCI
restore on XP x64, monitor stand by / off (DPMS) and wake-up. Windows 3.1:
WINVBE Power Saver (Control Panel, Setup, stand by, off, wake-up),
installed by INSTALL and by Windows Setup; Windows 95: the same screen
saver, Windows' own energy saving (low-power standby, shut off monitor,
wake-up), APM Suspend, resume handling with simulated events (mode kept /
mode lost, windowed MS-DOS box open), also on Windows 3.1. Windows 3.0
386 enhanced mode with the new WINVBE.386.
Shadow frame buffer and write combining (PAT; page table entries checked
in QEMU): Windows 95 and 3.1 in LFB and VGA-window modes, scrolling,
window moves, DCI through the shadow. MS-DOS boxes as on real cards
(QEMU with WINVBE.386's and the driver's Bochs/QEMU help switched off,
modes through the BIOS): Windows 95 window / full screen / Alt+Tab / exit,
Windows 3.1 the same with VddMode chosen automatically. A Windows 95
stress run (Explorer views, Control Panel, Display Properties, Paint,
Notepad, help, GDI and DCI tests, MS-DOS boxes; six rounds) - it found
the Explorer crash fixed in this version (GetDIBits in bands), and a
host unit test covers it. Windows NT 3.1 (on a Pentium) and 3.51:
installation with WINVBECF and with OEMSETUP.INF (NT Setup / Display
Type), WINVBECF updating NT Setup's display entry, full-screen console
and MS-DOS; regression on NT 4.0 (DirectDraw), 2000, XP x64, Windows
1.01, 2.03, 3.0 standard and real mode.

Building from source
--------------------
Requires Open Watcom C 2.0, NASM and Python 3. The standard bitmaps,
cursors and icons are taken from Microsoft's display drivers by
SRC\TOOLS\STOCKRES.PY at build time; these files are not part of the
source and must be put into msfiles/stock next to build.sh (from your
Windows disks; the names are case sensitive):
    VGA30.DRV    VGA.DRV of Windows 3.0
    VGA31.DRV    VGA.DRV of Windows 3.1
    VGA211.DRV   VGA.DRV of Windows 2.11 (IBM PS/2, "VGA450")
    EGA101.DRV   EGAHIRES.DRV of Windows 1.01
    VGANT31.DLL  VGA.DLL of Windows NT 3.1
Without them the drivers get WINVBE's own artwork instead (MKRES.PY,
MKRES2.PY for 1.x/2.x). The OEM tables are generated by MKRES.PY and
MKRES2.PY, the large fonts by MKFON.PY. NEFIX.PY gives the 1.x/2.x driver
the module layout their Setup programs expect; BINPATCH.PY is a Python
model of the WIN100/WIN200.BIN linking done by INSTALL.
    export WATCOM=/opt/watcom
    sh build.sh
WINVBE.386 is assembled with NASM (SRC\VXD, MKVXD32.PY writes the LE
file); the Windows NT files need mingw-w64 (i686 and x86_64) and are built
by SRC\NT\BUILD.SH.
The binaries are written to SRC\BIN; SRC\TOOLS\MKINF.PY writes the
OEMSETUP.INF files.

WINVBE v1.1 - (c) 2026
