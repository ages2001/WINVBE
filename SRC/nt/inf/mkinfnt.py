#!/usr/bin/env python3
"""mkinfnt.py outdir : WINVBE.INF for Windows NT 4.0/2000/XP/2003 (x86) and
for x64 (XP/2003 x64)."""
import sys, os

def inf(arch):
    x64 = arch == 'amd64'
    dec = ',NTamd64' if x64 else ''
    mfg = 'WINVBE.Mfg' + ('.NTamd64' if x64 else '')
    return f"""; WINVBE v1.1 - universal VESA/VGA/SVGA display driver
; {'Windows XP x64 / Server 2003 x64' if x64 else 'Windows NT 4.0, 2000, XP and Server 2003 (x86)'}
;
; Windows 2000/XP/2003: Device Manager > Display adapters > (adapter) >
;   Update Driver > Install from a specific location > Don't search >
;   Have Disk > this folder.
; Windows NT 4.0: Control Panel > Display > Settings > Display Type >
;   Change > Have Disk > this folder.

[Version]
Signature="$CHICAGO$"
Provider=%WVB%
ClassGUID={{4D36E968-E325-11CE-BFC1-08002BE10318}}
Class=Display
DriverVer=10/19/2026,1.1.0.0

[DestinationDirs]
DefaultDestDir   = 11
winvbe.Miniport  = 12
winvbe.Display   = 11
winvbe.Tools     = 11

[SourceDisksNames]
1 = %Disk%,,,

[SourceDisksFiles]
winvbe.sys   = 1
winvbe.dll   = 1
winvbecf.exe = 1

[Manufacturer]
%WVB% = WINVBE.Mfg{dec}

[{mfg}]
%WVB.Desc% = winvbe, PCI\\CC_0300
%WVB.Desc% = winvbe, PCI\\CC_0001

[winvbe]
CopyFiles = winvbe.Miniport, winvbe.Display, winvbe.Tools

[winvbe.Miniport]
winvbe.sys

[winvbe.Display]
winvbe.dll

[winvbe.Tools]
winvbecf.exe

[winvbe.SoftwareSettings]
AddReg = winvbe_SoftwareDeviceSettings

[winvbe_SoftwareDeviceSettings]
HKR,, InstalledDisplayDrivers,      %REG_MULTI_SZ%, winvbe
HKR,, VgaCompatible,                %REG_DWORD%,    0
HKR,, DefaultSettings.BitsPerPel,   %REG_DWORD%,    16
HKR,, DefaultSettings.XResolution,  %REG_DWORD%,    1024
HKR,, DefaultSettings.YResolution,  %REG_DWORD%,    768
HKR,, DefaultSettings.VRefresh,     %REG_DWORD%,    60

[winvbe.GeneralConfigData]
MaximumDeviceMemoryConfiguration = 128

[winvbe.Services]
AddService = winvbe, 0x00000002, winvbe_Service_Inst, winvbe_EventLog_Inst

[winvbe_Service_Inst]
ServiceType    = 1                  ; SERVICE_KERNEL_DRIVER
StartType      = 1                  ; SERVICE_SYSTEM_START
ErrorControl   = 0                  ; SERVICE_ERROR_IGNORE
LoadOrderGroup = Video
ServiceBinary  = %12%\\winvbe.sys

[winvbe_EventLog_Inst]
AddReg = winvbe_EventLog_AddReg

[winvbe_EventLog_AddReg]
HKR,,EventMessageFile,0x00020000,"%SystemRoot%\\System32\\IoLogMsg.dll;%SystemRoot%\\System32\\drivers\\winvbe.sys"
HKR,,TypesSupported,0x00010001,7

[Strings]
WVB          = "WINVBE"
WVB.Desc     = "WINVBE Universal VESA/VGA/SVGA"
Disk         = "WINVBE v1.1 for Windows NT"
REG_SZ       = 0x00000000
REG_MULTI_SZ = 0x00010000
REG_DWORD    = 0x00010001
"""

# Windows NT 3.1 / 3.5 / 3.51: OEMSETUP.INF for Windows NT Setup > Options >
# Change System Settings > Display > Other (the script language of NT 3.x
# Setup; same entry points as its VIDEO.INF)
# every resolution x depth WINVBECF may set: Setup finds the option that
# WINVBECF writes to Control\Setup "Video" ("WINVBE <w>x<h>x<bpp>")
NT3_RES = [(640, 480), (800, 600), (1024, 768), (1152, 864), (1280, 720), (1280, 768), (1280, 800),
           (1280, 960), (1280, 1024), (1360, 768), (1366, 768), (1400, 1050), (1440, 900), (1600, 900),
           (1600, 1200), (1680, 1050), (1920, 1080), (1920, 1200), (2048, 1536), (2560, 1440),
           (2560, 1600), (3840, 2160)]
NT3_MODES = [(w, h, b) for w, h in NT3_RES for b in (8, 15, 16, 24, 32)]
COLORS = {8: '256 colors', 15: '32768 colors', 16: '65536 colors', 24: '16.7 million colors',
          32: '16.7 million colors'}

def oemsetup():
    opts = ''.join('    "WINVBE %ux%ux%u"%s= winvbe, %2u, %4u, %4u, 60, 0\n' %
                   (w, h, b, ' ' * (16 - len('%ux%ux%u' % (w, h, b))), b, w, h) for w, h, b in NT3_MODES)
    # padded so that Setup's sorted list is in order of size and depth
    txt = ''.join('    "WINVBE %ux%ux%u"%s= "WINVBE VESA %4u x %4u, %2u bit (%s)"\n' %
                  (w, h, b, ' ' * (16 - len('%ux%ux%u' % (w, h, b))), w, h, b, COLORS[b]) for w, h, b in NT3_MODES)
    return r"""; WINVBE v1.1 - universal VESA display driver for Windows NT 3.1, 3.5, 3.51
;
; Windows NT Setup (Main group) > Options > Change System Settings >
;   Display > Other > path of this folder (e.g. A:\ or C:\WINVBE\NT\I386\NT3X).
; The resolution can be changed later with WINVBECF.EXE (in SYSTEM32).
; WINVBE.SYS and WINVBE.DLL of this folder are the Windows NT 3.x ones.
;
; Tag 2: Windows NT 3.1 loads the Video group in the order of
; GroupOrderList\Video: 1 (VIDEOPRT itself), 2, 3, ... and VGA (tag 10)
; last - WINVBE after VIDEOPRT and before VGA gets \Device\Video0. VGA
; stays installed for full-screen MS-DOS.

[Identification]
    OptionType = VIDEO

[Options]
""" + opts + r"""
[MiniportDrivers]
    winvbe = !SERVICE_KERNEL_DRIVER, Video, !SERVICE_ERROR_IGNORE, 2, {winvbe}, 0, "%SystemRoot%\System32\IoLogMsg.dll;%SystemRoot%\System32\drivers\winvbe.sys", 7

[Identify]
    read-syms Identification
    set Status     = STATUS_SUCCESSFUL
    set Identifier = $(OptionType)
    set Media      = #("Source Media Descriptions", 1, 1)
    Return $(Status) $(Identifier) $(Media)

[ReturnOptions]
    set Status         = STATUS_FAILED
    set OptionList     = {}
    set OptionTextList = {}
    set LanguageList   = ^(LanguagesSupported, 1)
    Ifcontains(i) $($0) in $(LanguageList)
        goto returnoptions
    else
        set Status = STATUS_NOLANGUAGE
        goto finish_ReturnOptions
    endif
returnoptions = +
    set OptionList     = ^(Options, 0)
    set OptionTextList = ^(OptionsText$($0), 1)
    set Status         = STATUS_SUCCESSFUL
finish_ReturnOptions = +
    Return $(Status) $(OptionList) $(OptionTextList)

[ServicesEntry]
    CurrentEntry = "" ? $(!LIBHANDLE) GetDevicemapValue Video \Device\Video0

[InstallOption]
    set Status       = STATUS_FAILED
    set DrivesToFree = {}
    set Option   = $($1)
    set SrcDir   = $($2)
    set AddCopy  = $($3)
    set DoCopy   = $($4)
    set DoConfig = $($5)
    set LanguageList = ^(LanguagesSupported, 1)
    Ifcontains(i) $($0) in $(LanguageList)
    else
        set Status = STATUS_NOLANGUAGE
        goto finish_InstallOption
    endif
    read-syms Strings$($0)
    set OptionList = ^(Options, 0)
    ifcontains $(Option) in $(OptionList)
    else
        goto finish_InstallOption
    endif
    set OptionList = ""
    set MiniportDriver    = #(Options, $(Option), 1)
    set BitsPerPel        = #(Options, $(Option), 2)
    set XResolution       = #(Options, $(Option), 3)
    set YResolution       = #(Options, $(Option), 4)
    set VRefresh          = #(Options, $(Option), 5)
    set Interlaced        = #(Options, $(Option), 6)
    set Type              = $(#(MiniportDrivers, $(MiniportDriver), 1))
    set Group             =   #(MiniportDrivers, $(MiniportDriver), 2)
    set ErrorControl      = $(#(MiniportDrivers, $(MiniportDriver), 3))
    set Tag               =   #(MiniportDrivers, $(MiniportDriver), 4)
    set InstalledDisplays =   #(MiniportDrivers, $(MiniportDriver), 5)
    set VgaCompatible     =   #(MiniportDrivers, $(MiniportDriver), 6)
    set EventMessageFile  =   #(MiniportDrivers, $(MiniportDriver), 7)
    set TypesSupported    =   #(MiniportDrivers, $(MiniportDriver), 8)
    read-syms ServicesEntry
    detect    ServicesEntry
    ifstr(i) $(AddCopy) == "YES"
        shell "subroutn.inf" DoAskSourceEx $(SrcDir) $(String2)
        ifint $($ShellCode) != $(!SHELL_CODE_OK)
            Debug-Output "OEMSETUP.INF: shelling DoAskSourceEx failed"
            goto finish_InstallOption
        endif
        ifstr(i) $($R0) == STATUS_SUCCESSFUL
            set SrcDir = $($R1)
            ifstr(i) $($R2) != ""
                set DrivesToFree = >($(DrivesToFree), $($R2))
            endif
        else
            Debug-Output "OEMSETUP.INF: user cancelled asking source"
            goto finish_InstallOption
        endif
        install Install-AddCopyOption
        ifstr(i) $(STF_INSTALL_OUTCOME) != "STF_SUCCESS"
            Debug-Output "OEMSETUP.INF: adding files to the copy list failed"
            goto finish_InstallOption
        endif
    endif
    ifstr(i) $(DoCopy) == "YES"
        read-syms ProgressCopy$($0)
        install Install-DoCopyOption
        ifstr(i) $(STF_INSTALL_OUTCOME) == "STF_FAILURE"
            Debug-Output "OEMSETUP.INF: copying files failed"
            goto finish_InstallOption
        else-ifstr(i) $(STF_INSTALL_OUTCOME) == "STF_USERQUIT"
            set Status = STATUS_USERCANCEL
            goto finish_InstallOption
        endif
    endif
    ifstr(i) $(DoConfig) == "YES"
        shell "registry.inf" CheckSetupModify
        ifint $($ShellCode) != $(!SHELL_CODE_OK)
            goto finish_InstallOption
        endif
        ifstr(i) $($R0) != STATUS_SUCCESSFUL
            goto finish_InstallOption
        endif
        set ServiceNode   = $(MiniportDriver)
        set ServiceBinary = %SystemRoot%\System32\drivers\winvbe.sys
        set ServicesValues = { +
                {Type,           0, $(!REG_VT_DWORD),     $(Type)                  }, +
                {Start,          0, $(!REG_VT_DWORD),     $(!SERVICE_SYSTEM_START) }, +
                {Group,          0, $(!REG_VT_SZ),        $(Group)                 }, +
                {ErrorControl,   0, $(!REG_VT_DWORD),     $(ErrorControl)          }, +
                {Tag,            0, $(!REG_VT_DWORD),     $(Tag)                   }, +
                {BinaryPathName, 0, $(!REG_VT_EXPAND_SZ), $(ServiceBinary)         }  +
                }
        set ParametersValues = { +
                {InstalledDisplayDrivers,     0, $(!REG_VT_MULTI_SZ), $(InstalledDisplays) }, +
                {VgaCompatible,               0, $(!REG_VT_DWORD),    $(VgaCompatible)     }, +
                {DefaultSettings.BitsPerPel,  0, $(!REG_VT_DWORD),    $(BitsPerPel)        }, +
                {DefaultSettings.XResolution, 0, $(!REG_VT_DWORD),    $(XResolution)       }, +
                {DefaultSettings.YResolution, 0, $(!REG_VT_DWORD),    $(YResolution)       }, +
                {DefaultSettings.VRefresh,    0, $(!REG_VT_DWORD),    $(VRefresh)          }, +
                {DefaultSettings.Interlaced,  0, $(!REG_VT_DWORD),    $(Interlaced)        }  +
                }
        set DeviceValues   = {}
        set EventLogValues = { +
                {EventMessageFile, 0, $(!REG_VT_EXPAND_SZ), $(EventMessageFile) }, +
                {TypesSupported,   0, $(!REG_VT_DWORD),     $(TypesSupported)   }  +
                }
        shell "registry.inf" MakeServicesEntry $(ServiceNode)      +
                                               $(ServicesValues)   +
                                               $(ParametersValues) +
                                               $(DeviceValues)     +
                                               $(EventLogValues)   +
                                               Device0
        ifint $($ShellCode) != $(!SHELL_CODE_OK)
            Debug-Output "OEMSETUP.INF: couldn't execute MakeServicesEntry in registry.inf"
            goto errorconfig
        endif
        ifstr(i) $($R0) != STATUS_SUCCESSFUL
            Debug-Output "OEMSETUP.INF: MakeServicesEntry failed"
            goto errorconfig
        endif
        ; the driver used until now is switched off - VGA stays
        ifstr(i) $(CurrentEntry) != $(MiniportDriver)
            ifstr(i) $(CurrentEntry) != VGA
                ifstr(i) $(CurrentEntry) != VgaSave
                    ifstr(i) $(CurrentEntry) != ""
                        shell "registry.inf" ModifyServicesEntry $(CurrentEntry) $(!SERVICE_DISABLED)
                    endif
                endif
            endif
        endif
        goto configdone
errorconfig = +
        goto finish_InstallOption
configdone = +
    endif
    set Status = STATUS_SUCCESSFUL
finish_InstallOption = +
    ForListDo $(DrivesToFree)
        LibraryProcedure STATUS,$(!LIBHANDLE), DeleteNetConnection $($) "TRUE"
    EndForListDo
    Return $(Status)

[Install-AddCopyOption]
    set STF_VITAL = ""
    AddSectionKeyFileToCopyList Files-DisplayMiniportDrivers $(MiniportDriver) $(SrcDir) $(!STF_WINDOWSSYSPATH)\drivers
    ForListDo $(InstalledDisplays)
        AddSectionKeyFileToCopyList Files-DisplayDLLs $($) $(SrcDir) $(!STF_WINDOWSSYSPATH)
    EndForListDo
    AddSectionFilesToCopyList Files-Tools $(SrcDir) $(!STF_WINDOWSSYSPATH)
    exit

[Install-DoCopyOption]
    CopyFilesInCopyList
    exit

[Source Media Descriptions]
    1 = "WINVBE v1.1 for Windows NT 3.x", TAGFILE = winvbe.sys

[ProductType]
STF_PRODUCT  = Winnt
STF_PLATFORM = I386

[Files-DisplayDLLs]
winvbe = 1,WINVBE.DLL

[Files-DisplayMiniportDrivers]
winvbe = 1,WINVBE.SYS

[Files-Tools]
winvbecf = 1,WINVBECF.EXE

[LanguagesSupported]
    ENG

[OptionsTextENG]
""" + txt + r"""
[ProgressCopyENG]
    ProCaption   = "WINVBE Setup"
    ProCancel    = "Cancel"
    ProCancelMsg = "The display driver is not completely installed.  Are you sure you want "+
                   "to cancel copying files?"
    ProCancelCap = "Setup Message"
    ProText1     = "Copying:"
    ProText2     = "To:"

[StringsENG]
    String1 = "Display"
    String2 = "Please enter the full path of the WINVBE files for Windows NT 3.x "+
              "(the folder NT\I386\NT3X of the WINVBE package), then choose Continue."
"""

out = sys.argv[1]
for arch, d in (('i386', 'NT/I386/NT4-XP'), ('amd64', 'NT/AMD64')):
    p = os.path.join(out, d)
    os.makedirs(p, exist_ok=True)
    open(os.path.join(p, 'WINVBE.INF'), 'w', newline='\r\n').write(inf(arch))
p = os.path.join(out, 'NT/I386/NT3X')
os.makedirs(p, exist_ok=True)
open(os.path.join(p, 'OEMSETUP.INF'), 'w', newline='\r\n').write(oemsetup())
