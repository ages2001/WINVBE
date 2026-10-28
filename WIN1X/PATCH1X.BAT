@ECHO OFF
REM WINVBE v1.1 - put the WINVBE driver on COPIES of the Windows 1.0x disks
REM
REM Windows 1.0x Setup has no "Other display" choice, so WINVBE takes the
REM place of "EGA (more than 64K) with Enhanced Color Display":
REM   1. Make copies of the Setup disk and the Build disk (DISKCOPY).
REM   2. PATCH1X A:   with the copy of the Setup disk in drive A:
REM      PATCH1X A:   with the copy of the Build disk in drive A:
REM      (a disk that holds both is patched in one go)
REM   3. Run SETUP from the copies and choose "EGA (more than 64K)".
REM Never use your original disks. Run this batch file in the WIN1X folder.
IF "%1"=="" GOTO usage
IF NOT EXIST WINVBE.DRV GOTO wrongdir
SET WVBDONE=
IF NOT EXIST %1\EGAHIRES.DRV GOTO build
REM the Setup disk is nearly full: make room by removing two EGA variants
IF EXIST %1\EGAHIBW.DRV DEL %1\EGAHIBW.DRV
IF EXIST %1\EGALORES.DRV DEL %1\EGALORES.DRV
DEL %1\EGAHIRES.DRV
COPY WINVBE.DRV %1\EGAHIRES.DRV >NUL
IF NOT EXIST %1\EGAHIRES.DRV GOTO full
ECHO Setup disk: WINVBE.DRV copied as EGAHIRES.DRV.
SET WVBDONE=1
:build
IF NOT EXIST %1\EGAHIRES.LGO GOTO check
COPY WINVBE.LGO %1\EGAHIRES.LGO >NUL
ECHO Build disk: WINVBE.LGO copied as EGAHIRES.LGO.
SET WVBDONE=1
:check
IF "%WVBDONE%"=="" ECHO %1 is neither the Setup nor the Build disk of Windows 1.0x.
SET WVBDONE=
GOTO end
:full
ECHO Not enough room on the disk in %1 - delete CGA.DRV or HERCULES.DRV there
ECHO (if you do not need them) and run PATCH1X again.
GOTO end
:wrongdir
ECHO Run PATCH1X in the WIN1X folder of WINVBE (e.g. CD C:\WINVBE\WIN1X).
GOTO end
:usage
ECHO Usage: PATCH1X A:   (copy of the Windows 1.0x Setup or Build disk in A:)
:end
