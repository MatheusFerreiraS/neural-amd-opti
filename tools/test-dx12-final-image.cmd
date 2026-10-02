@echo off
rem Final-image NR on a real D3D12 app: builds tests\dx12_final_image_smoke.cpp, stages it with the
rem built OptiScaler.dll as dxgi.dll, the danielblnc runtime and XeFG, and runs the scenarios.
rem Needs the AMD GPU; each scenario holds wt\locks\gpu while it renders when that folder exists.
rem   tools\test-dx12-final-image.cmd <dlssnr_amd_pass1.dll or version.dll> <dlssnr_on_amd_weights.bin>
rem                                   <folder with libxess_fg.dll and libxell.dll> [scenario args...]
rem Without scenario args it runs: nr, fg, fg --bgra, fg --cpu-ms 6 (a game's CPU time per frame),
rem fg --debug-layer.
setlocal
cd /d "%~dp0.."
if "%~3"=="" goto usage
set "SMOKE=exports\dx12-smoke"
if not exist "%SMOKE%\OptiScaler" mkdir "%SMOKE%\OptiScaler"
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /W3 /utf-8 tests\dx12_final_image_smoke.cpp /Fe"%SMOKE%\dx12_final_image_smoke.exe" /Fo"%SMOKE%\dx12_final_image_smoke.obj" /link d3dcompiler.lib user32.lib dbghelp.lib
if errorlevel 1 exit /b 1
copy /y "exports\release-local\OptiScaler.dll" "%SMOKE%\dxgi.dll" >nul || exit /b 1
copy /y "OptiScaler.ini" "%SMOKE%\OptiScaler.template.ini" >nul || exit /b 1
copy /y "%~1" "%SMOKE%\dlssnr_amd_pass1.dll" >nul || exit /b 1
if not exist "%SMOKE%\dlssnr_on_amd_weights.bin" copy /y "%~2" "%SMOKE%\dlssnr_on_amd_weights.bin" >nul || exit /b 1
copy /y "%~3\libxess_fg.dll" "%SMOKE%\OptiScaler\" >nul || exit /b 1
copy /y "%~3\libxell.dll" "%SMOKE%\OptiScaler\" >nul || exit /b 1
copy /y "%~3\amd_fidelityfx_*.dll" "%SMOKE%\OptiScaler\" >nul
set "LOCK="
if exist "%~dp0..\..\locks" set "LOCK=--lock %~dp0..\..\locks\gpu"
set "FAILED=0"
if not "%~4"=="" (
  call :run %4 %5 %6
  goto done
)
call :run nr
call :run fg
call :run fg --bgra
call :run fg --cpu-ms 6
call :run fg --debug-layer
:done
if "%FAILED%"=="0" (echo ALL PASSED) else (echo SOME FAILED)
exit /b %FAILED%
:run
echo === dx12_final_image_smoke %*
"%SMOKE%\dx12_final_image_smoke.exe" %* %LOCK%
if errorlevel 1 set "FAILED=1"
for %%L in (OptiScaler.log dx12_smoke_result.txt) do if exist "%SMOKE%\%%L" copy /y "%SMOKE%\%%L" "%SMOKE%\%~1%~2-%%L" >nul
exit /b 0
:usage
echo usage: %~nx0 ^<runtime dll^> ^<dlssnr_on_amd_weights.bin^> ^<XeFG folder^> [scenario args...]
exit /b 2
