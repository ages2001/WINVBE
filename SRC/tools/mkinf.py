#!/usr/bin/env python3
"""Generate the OEMSETUP.INF files of WINVBE.
   mkinf.py <outdir>  ->  <outdir>/WIN30/OEMSETUP.INF and <outdir>/WIN31/OEMSETUP.INF
Notes from testing with the original Setup programs:
 * descriptions must stay short (Setup silently fails on long ones),
 * empty logo fields make Setup build WIN.COM without a start-up logo, so
   it never asks for the logo files (DOS-mode Setup always looks for them
   on drive A:),
 * the grabbers have names of their own: Setup takes files with the
   standard names from the Windows disks (its SETUP.INF),
 * DOS-mode Setup resolves numbered disks (also those of the fonts it copies
   from its own SETUP.INF) only through this file's [disks], so all Windows
   disks are listed here,
 * the OEM disk uses a letter so it never collides with a Windows disk."""
import sys, os
outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
PROF = [('winvbe',   'Auto Mode (Small fonts)',  '100,96,96',   0,  96),
        ('winvbe8',  '256 Colors (Small fonts)', '100,96,96',   8,  96),
        ('winvbe16', '64K Colors (Small fonts)', '100,96,96',   16, 96),
        ('winvbe32', '16M Colors (Small fonts)', '100,96,96',   32, 96),
        ('winvbel',  'Auto Mode (Large fonts)',  '100,120,120', 0,  120)]

# Windows 3.0 Setup takes the font lists from the OEM INF itself (as in
# Microsoft's own OEMSETUP.INF on the Windows 3.0 Multimedia CD).
EXTRA30 = """
; ---- sections Windows 3.0 Setup reads from this file; the files are in
; this folder (W:), so Setup does not ask for the Windows disks ----

; copy these fonts depending on the 386 grabber being used
[wvb.gr3]
W:CGA40WOA.FON,W:CGA40850.FON
W:CGA80WOA.FON,W:CGA80850.FON
W:EGA40WOA.FON,W:EGA40850.FON
W:EGA80WOA.FON,W:EGA80850.FON

[codepages]
863 = W:xlat863.bin, W:vga863.fon, "French Canadian (863)"
861 = W:xlat861.bin, W:vga861.fon, "Icelandic (861)"
865 = W:xlat865.bin, W:vga865.fon, "Norway & Denmark (865)"
850 = W:xlat850.bin, W:vga850.fon, "International (850)"
860 = W:xlat860.bin, W:vga860.fon, "Portuguese (860)"
437 =              ,             , "Standard (437)"

[sysfonts]
W:vgasys.fon,"VGA (640x480) resolution System Font", "100,96,96"
W:wvbsys.fon,"WINVBE System Font (120 dpi)", "100,120,120"

[fixedfonts]
W:vgafix.fon,"VGA (640x480) resolution Fixed System Font", "100,96,96"
W:wvbfix.fon,"WINVBE Fixed System Font (120 dpi)", "100,120,120"

[oemfonts]
W:vgaoem.fon,"VGA (640x480) resolution Terminal Font (USA/Europe)", "100,96,96",1
W:wvboem.fon,"WINVBE Terminal Font (120 dpi)", "100,120,120",1

[fonts]
W:HELVE.FON, "Helv 8,10,12,14,18,24 (VGA res)", "100,96,96"
W:HELVF.FON, "Helv 8,10,12,14,18,24 (8514/a res)", "100,120,120"
W:COURE.FON, "Courier 10,12,15 (VGA res)", "100,96,96"
W:COURF.FON, "Courier 10,12,15 (8514/a res)", "100,120,120"
W:TMSRE.FON, "Tms Rmn 8,10,12,14,18,24 (VGA res)", "100,96,96"
W:TMSRF.FON, "Tms Rmn 8,10,12,14,18,24 (8514/a res)", "100,120,120"
W:SYMBOLE.FON, "Symbol 8,10,12,14,18,24 (VGA res)", "100,96,96"
W:SYMBOLF.FON, "Symbol 8,10,12,14,18,24 (8514/a res)", "100,120,120"
W:ROMAN.FON, "Roman (All res)",   "CONTINUOUSSCALING"
W:SCRIPT.FON, "Script (All res)", "CONTINUOUSSCALING"
W:MODERN.FON, "Modern (All res)", "CONTINUOUSSCALING"
""".split('\n')

# Windows 3.1: the font and grabber font lists with the files taken from
# this folder (W:), so Setup does not ask for the Windows disks.
EXTRA31 = """
; ---- files Setup would otherwise copy from the Windows 3.1 disks ----

; WIN.COM is built from WIN.CNF + logo code + logo data
[windows.system]
W:win.cnf

[wvb30.3gr]
W:CGA40WOA.FON,W:CGA40850.FON
W:CGA80WOA.FON,W:CGA80850.FON
W:EGA40WOA.FON,W:EGA40850.FON
W:EGA80WOA.FON,W:EGA80850.FON

[sysfonts]
W:vgasys.fon,"VGA (640x480) resolution System Font", "100,96,96"
W:8514sys.fon,"8514/a (1024x768) resolution System Font", "100,120,120"

[fixedfonts]
W:vgafix.fon,"VGA (640x480) resolution Fixed System Font", "100,96,96"
W:8514fix.fon,"8514/a (1024x768) resolution Fixed System Font", "100,120,120"

[oemfonts]
W:vgaoem.fon,"VGA (640x480) resolution Terminal Font (USA/Europe)", "100,96,96",1
W:8514oem.fon,"8514/a (1024x768) resolution Terminal Font (USA/Europe)", "100,120,120",1

[fonts]
W:SSERIFE.FON, "MS Sans Serif 8,10,12,14,18,24 (VGA res)", "100,96,96"
W:SSERIFF.FON, "MS Sans Serif 8,10,12,14,18,24 (8514/a res)", "100,120,120"
W:COURE.FON, "Courier 10,12,15 (VGA res)", "100,96,96"
W:COURF.FON, "Courier 10,12,15 (8514/a res)", "100,120,120"
W:SERIFE.FON, "MS Serif 8,10,12,14,18,24 (VGA res)", "100,96,96"
W:SERIFF.FON, "MS Serif 8,10,12,14,18,24 (8514/a res)", "100,120,120"
W:SYMBOLE.FON, "Symbol 8,10,12,14,18,24 (VGA res)", "100,96,96"
W:SYMBOLF.FON, "Symbol 8,10,12,14,18,24 (8514/a res)", "100,120,120"
W:SMALLE.FON, "Small Fonts (VGA res)", "100,96,96"
W:SMALLF.FON, "Small Fonts (8514/a res)", "100,120,120"
W:ROMAN.FON, "Roman (Plotter)",   "CONTINUOUSSCALING"
W:SCRIPT.FON, "Script (Plotter)", "CONTINUOUSSCALING"
W:MODERN.FON, "Modern (Plotter)", "CONTINUOUSSCALING"
""".split('\n')

VARIANTS = {
    # Windows 3.1: VDDVGA30.386 + VGA30.3GR is Microsoft's pairing for Windows
    # 3.0 style display drivers (Disk 1 / Disk 2 of the 1.44 MB set).
    'WIN31': dict(ver='3.1',
        disks=['%d =. ,"Microsoft Windows 3.1 Disk #%d",disk%d' % (i, i, i) for i in range(1, 7)],
        std='W:wvbcolor.2gr, , W:vddvga30.386, W:wvb30.3gr,, ',
        extra=EXTRA31),
    # Windows 3.0: file names and disk numbers of the Windows 3.0 SETUP.INF.
    'WIN30': dict(ver='3.0',
        # the tag (3rd field) is a file Setup checks for; Windows 3.0 media
        # (e.g. the Multimedia CD) have no DISKn tag files, so a file that the
        # Windows 3.0 SETUP.INF places on that disk is used instead
        disks=['1 =. ,"Microsoft Windows 3.0 Disk #1",vgasys.fon',
               '2 =. ,"Microsoft Windows 3.0 Disk #2",vga850.fon',
               '3 =. ,"Microsoft Windows 3.0 Disk #3",vgacolor.gr2',
               '4 =. ,"Microsoft Windows 3.0 Disk #4",cga40woa.fon'],
        std='W:wvbcolor.gr2, W:wvblogo.lgo, x:*vddvga, W:wvb.gr3,, W:wvblogo.rle',
        extra=EXTRA30,
        # Windows 3.0 Setup ignores the optional sections, so the colour depth
        # is chosen afterwards in WINVBECF.EXE and the driver takes the DPI
        # from the system font Setup installs
        prof=[('winvbe',  'Small fonts', '100,96,96',   0, 96),
              ('winvbel', 'Large fonts', '100,120,120', 0, 120)]),
}
for d, v in VARIANTS.items():
    prof = v.get('prof', PROF)
    L = ['; WINVBE v1.1 - Universal VESA/VGA/SVGA display driver',
         '; OEMSETUP.INF for Windows %s Setup:' % (v['ver'] + (' and Windows for Workgroups 3.1/3.11' if v['ver'] == '3.1' else '')),
         ';   Display -> "Other display (Requires disk from OEM)" -> path of this folder',
         '; All files Setup needs are in this folder - no Windows disks are needed.',
         '',
         '[disks]'] + v['disks'] + [
         'W =. ,"WINVBE v1.1 Display Driver Disk",winvbe.drv',
         '',
         '[display]',
         ';profile = driver, description, resolution, 286 grabber, logo code, VDD, 386 grabber, ega.sys, logo data, optional section']
    for k, desc, r, dep, dpi in prof:
        L.append('%-9s= W:winvbe.drv, "WINVBE v1.1 VESA, %s", "%s", %s, %s' % (k, desc, r, v['std'], k))
    L += ['', '; file, destination, .ini file, section, old line, new line']
    for k, desc, r, dep, dpi in prof:
        L += ['', '[%s]' % k,
              'W:winvbecf.exe, 0:, system.ini, WINVBE, "Depth=", "Depth=%d"' % dep,
              ',, system.ini, WINVBE, "DPI=", "DPI=%d"' % dpi]
        if v['ver'] == '3.1':       # monitor power saver (screen saver)
            L.append('W:wvbpower.scr, 0:')
    L += v.get('extra', [])
    os.makedirs(os.path.join(outdir, d), exist_ok=True)
    open(os.path.join(outdir, d, 'OEMSETUP.INF'), 'w', newline='').write('\r\n'.join(L) + '\r\n')

# ---------------------------------------------------------------------------
# Windows 95/98/ME: display class INF for Device Manager ("Have Disk").
# The modes listed under MODES are offered by Display Properties; WINVBECF
# replaces them with the modes of the actual graphics card.
RES9X = ['640,480', '800,600', '1024,768', '1152,864', '1280,720', '1280,768', '1280,800',
         '1280,960', '1280,1024', '1360,768', '1366,768', '1400,1050', '1440,900', '1600,900',
         '1600,1200', '1680,1050', '1920,1080', '1920,1200', '2048,1536', '2560,1440', '2560,1600']
L = ['; WINVBE v1.1 - Universal VESA/VGA/SVGA display driver',
     '; Windows 95/98/ME: Device Manager > Display adapters > (adapter) > Properties >',
     ';   Driver > Change Driver (Update Driver) > Have Disk > path of this folder',
     '',
     '[Version]',
     'Signature="$CHICAGO$"',
     'Class=Display',
     'Provider=%WVB%',
     '',
     '[DestinationDirs]',
     'DefaultDestDir=11',
     'WINVBE.CopyCF=10',
     '',
     '[SourceDisksNames]',
     '1="WINVBE v1.1",,0',
     '',
     '[SourceDisksFiles]',
     'winvbe.drv=1',
     'winvbe.386=1',
     'winvbecf.exe=1',
     'wvbpower.scr=1',
     '915patch.vxd=1',
     '',
     '[Manufacturer]',
     '%WVB%=WINVBE.Models',
     '',
     '[WINVBE.Models]',
     '%WINVBE.DeviceDesc%=WINVBE.Install, *PNP0900',
     '%WINVBE.DeviceDesc%=WINVBE.Install, PCI\\CC_0300',
     '',
     '[WINVBE.Install]',
     'CopyFiles=WINVBE.Copy, WINVBE.CopyCF',
     'DelReg=WINVBE.DelReg',
     'AddReg=WINVBE.AddReg, WINVBE.RunOnce',
     'UpdateInis=WINVBE.Inis',
     '',
     '[WINVBE.Copy]',
     'winvbe.drv',
     'winvbe.386',
     '915patch.vxd',
     '',
     '; WINVBE.386: switches the VMware SVGA II mode off for full-screen MS-DOS',
     '[WINVBE.Inis]',
     'system.ini,386Enh,,"device=winvbe.386"',
     '',
     '[WINVBE.CopyCF]',
     'winvbecf.exe',
     'wvbpower.scr',
     '',
     '; after the restart: WINVBECF /915 puts device=915patch.vxd into SYSTEM.INI',
     '; on Intel graphics whose video BIOS 915PATCH can patch (else it removes',
     '; 915PATCH.VXD again)',
     '[WINVBE.RunOnce]',
     'HKLM,"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",WINVBE915,,"%10%\\winvbecf.exe /915"',
     '',
     '[WINVBE.DelReg]',
     'HKR,,Ver',
     'HKR,,DevLoader',
     'HKR,DEFAULT',
     'HKR,MODES',
     'HKR,CURRENT',
     '',
     '[WINVBE.AddReg]',
     'HKR,,Ver,,4.0',
     'HKR,,DevLoader,,*vdd',
     'HKR,DEFAULT,Mode,,"16,800,600"',
     'HKR,DEFAULT,drv,,winvbe.drv',
     'HKR,DEFAULT,vdd,,*vdd',
     'HKR,DEFAULT,RefreshRate,,0']
for bpp in (8, 16, 24, 32):
    for r in RES9X:
        L.append('HKR,"MODES\\%d\\%s"' % (bpp, r))
L += ['',
      '[Strings]',
      'WVB="WINVBE"',
      'WINVBE.DeviceDesc="WINVBE Universal VESA/VGA/SVGA"']
os.makedirs(os.path.join(outdir, 'WIN9X'), exist_ok=True)
open(os.path.join(outdir, 'WIN9X', 'WINVBE.INF'), 'w', newline='').write('\r\n'.join(L) + '\r\n')
