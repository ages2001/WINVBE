@ECHO OFF
REM WINVBE v1.1 - prepare this folder for Windows 2.x Setup ("Other" display)
REM
REM   PREP2X C:\WINDOWS     copy from an installed Windows 2.x (installed with
REM                         an EGA or VGA display) or Windows/386 2.x
REM   PREP2X A:             copy from the Windows 2.x Displays disk
REM
REM The easiest way is INSTALL.EXE, which needs neither Setup nor disks.
REM For Windows Setup ("Other" display) this folder already holds
REM WINVBE.DRV, WINVBE.LGO and WINVBE.GRB (the Windows 2.03 EGA grabber).
REM Windows/386 Setup also wants WINVBE.3EX and WINVBE.386: PREP2X copies
REM them (and the grabber) from your own Windows/386 2.x.
REM Run this batch file in the WIN2X folder.
IF "%1"=="" GOTO usage
IF NOT EXIST WINVBE.DRV GOTO wrongdir
IF EXIST %1\WINOLDAP.GRB COPY %1\WINOLDAP.GRB WINVBE.GRB >NUL
IF NOT EXIST WINVBE.GRB IF EXIST %1\VGA.GRB COPY %1\VGA.GRB WINVBE.GRB >NUL
IF NOT EXIST WINVBE.GRB IF EXIST %1\EGA.GRB COPY %1\EGA.GRB WINVBE.GRB >NUL
IF NOT EXIST WINVBE.GRB IF EXIST %1\EGAHIRES.GRB COPY %1\EGAHIRES.GRB WINVBE.GRB >NUL
IF NOT EXIST WINVBE.GRB GOTO nogrb
ECHO WINVBE.GRB copied.
IF EXIST %1\WIN386.EXE COPY %1\WIN386.EXE WINVBE.3EX >NUL
IF NOT EXIST WINVBE.3EX IF EXIST %1\EGA.3EX COPY %1\EGA.3EX WINVBE.3EX >NUL
IF EXIST %1\WIN386.386 COPY %1\WIN386.386 WINVBE.386 >NUL
IF NOT EXIST WINVBE.386 IF EXIST %1\EGA.386 COPY %1\EGA.386 WINVBE.386 >NUL
IF EXIST WINVBE.3EX ECHO WINVBE.3EX and WINVBE.386 copied (Windows/386).
ECHO.
ECHO Now run Windows Setup from the Setup disk, choose the display
ECHO "Other (requires disk provided by a hardware manufacturer)" and give
ECHO the path of this folder.
GOTO end
:nogrb
ECHO No grabber found in %1 (WINOLDAP.GRB, VGA.GRB, EGA.GRB or EGAHIRES.GRB).
GOTO end
:wrongdir
ECHO Run PREP2X in the WIN2X folder of WINVBE (e.g. CD C:\WINVBE\WIN2X).
GOTO end
:usage
ECHO Usage: PREP2X C:\WINDOWS   (installed Windows 2.x)
ECHO        PREP2X A:           (Windows 2.x Displays disk)
:end
