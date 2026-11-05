WINVBE OpenGL for Windows NT 3.1, 3.5 and 3.51
==============================================

This folder adds OpenGL to Windows NT 3.1, 3.5 and 3.51. Rendering is the
WINVBE software ICD (Mesa 7.8.2): full OpenGL 2.1 with GLSL 1.20, the same as
on NT 4.0 / 2000 / XP. There is no hardware acceleration (there is none on
any Windows here - it is a software renderer).

Files
-----
  OPENGL32.DLL   WINVBE's OpenGL for NT 3.x. Exports OpenGL 1.1 directly and
                 everything up to OpenGL 2.1 through wglGetProcAddress, exactly
                 as a program expects on any Windows.
  WVGDI.DLL      NT 3.1 only: a GDI32 stand-in (see below).
  GL31PAT.EXE    NT 3.1 only: prepares a program (see below).

The OpenGL ICD itself - WVBGL32.DLL, WVBGLX.DLL and WVBGLS.DLL - is in the
folder above this one (NT3X). Put those three, plus OPENGL32.DLL, where the
program can find them (the program's own folder, or SYSTEM32).

Windows NT 3.5 and 3.51
-----------------------
Copy OPENGL32.DLL (and the three ICD DLLs) next to the OpenGL program, or into
SYSTEM32. NT 3.5x's own OPENGL32.DLL is only OpenGL 1.0, so programs built for
1.1 or later need this one. Nothing else is required.

Windows NT 3.1
--------------
NT 3.1's GDI32.DLL has none of the OpenGL pixel-format or buffer functions
(ChoosePixelFormat, SetPixelFormat, DescribePixelFormat, GetPixelFormat,
SwapBuffers) - they arrived in NT 3.5 - so a program that imports them from
GDI32 will not even start on NT 3.1 ("the procedure entry point
ChoosePixelFormat could not be located in GDI32.DLL").

To run such a program on NT 3.1:

  1. Copy OPENGL32.DLL, WVGDI.DLL and the three ICD DLLs (WVBGL32.DLL,
     WVBGLX.DLL, WVBGLS.DLL) next to the program, or into SYSTEM32.
  2. Run the patcher once on the program, from a command prompt:
         GL31PAT  MYGLAPP.EXE
     It keeps a copy as MYGLAPP.EXE.BAK and reports what it did.
  3. Run the program.

GL31PAT points the program's five GDI32 OpenGL imports at WVGDI.DLL, which
forwards every ordinary GDI call to the real GDI32 and the five OpenGL
functions to OPENGL32.DLL. Nothing else about the program changes.

On an NT 3.1 FAT drive use the 8.3 file names as given here (GL31PAT.EXE,
WVGDI.DLL); that is why the tool is not called GL31PATCH.EXE.
