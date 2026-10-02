@echo off
rem Builds the 32-bit D3D9 bridge into exports\dx9-x86: d3d9.dll (x86 proxy), the x86 smoke program,
rem and OptiScaler64\OptiScalerDx9Host.exe (x64 host). The host runs exports\release-local\OptiScaler.dll,
rem which tools\build-optiscaler-fast.cmd builds; copy it into OptiScaler64 with tools\test-dx9-x86.cmd
rem or by hand.
setlocal
cd /d "%~dp0.."
set "VS_ROOT="
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VS_ROOT=%%I"
if not defined VS_ROOT if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC" set "VS_ROOT=%ProgramFiles%\Microsoft Visual Studio\18\Community"
if not defined VS_ROOT (
  echo FAIL: no Visual Studio with the C++ tools found
  exit /b 1
)
set "OUT=%CD%\exports\dx9-x86"
if not exist "%OUT%\obj32" mkdir "%OUT%\obj32"
if not exist "%OUT%\obj64" mkdir "%OUT%\obj64"
if not exist "%OUT%\OptiScaler64" mkdir "%OUT%\OptiScaler64"
set "FLAGS=/nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE"

setlocal
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x86 -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
cl %FLAGS% /LD tools\dx9-x86\proxy.cpp /Fe"%OUT%\d3d9.dll" /Fo"%OUT%\obj32\\" /link /DEF:tools\dx9-x86\exports.def dxgi.lib user32.lib
if errorlevel 1 exit /b 1
cl %FLAGS% tests\dx9_x86_smoke.cpp /Fe"%OUT%\dx9_x86_smoke.exe" /Fo"%OUT%\obj32\\" /link user32.lib gdi32.lib
if errorlevel 1 exit /b 1
endlocal

setlocal
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
cl %FLAGS% tools\dx9-x86\host.cpp /Fe"%OUT%\OptiScaler64\OptiScalerDx9Host.exe" /Fo"%OUT%\obj64\\" /link d3d11.lib dxgi.lib user32.lib
if errorlevel 1 exit /b 1
endlocal

echo BUILD_OK %OUT%
exit /b 0
