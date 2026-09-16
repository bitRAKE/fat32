@echo off
setlocal
if defined VSCMD_VER goto ready
set "fat32_vswhere=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%fat32_vswhere%" goto missing
for /f "usebackq tokens=*" %%I in (`"%fat32_vswhere%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "fat32_vsroot=%%I"
if not defined fat32_vsroot goto missing
call "%fat32_vsroot%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
if errorlevel 1 exit /b %errorlevel%
:ready
cd /d "%~dp0..\.."
nmake /nologo /f tests\win32\makefile %*
exit /b %errorlevel%
:missing
echo Run from an x64 Visual Studio developer prompt, or install the C++ build tools.
exit /b 1
