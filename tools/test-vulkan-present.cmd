@echo off
rem Vulkan final-image regression: NR through the shared D3D12 image, XeFG on the D3D12 bridge, the
rem menu with frame generation on, focus loss and return, minimize, resize and the NR and FG keys,
rem then the same INI with PresentWithoutUpscaler=false, which must leave all of that alone.
rem Not part of the release set: it needs an AMD GPU, the danielblnc runtime, XeFG and an unlocked
rem desktop, since it moves the real cursor and types into its own window.
rem   tools\test-vulkan-present.cmd <runtime folder> <XeFG folder> [OptiScaler.dll]
rem The runtime folder holds dlssnr_amd_pass1.dll (or version.dll) and dlssnr_on_amd_weights.bin, the XeFG
rem folder libxess_fg.dll and libxell.dll. Set GPU_LOCK to a lock directory to serialise GPU tests.
setlocal
cd /d "%~dp0.."
if "%~2"=="" goto usage
set "OUT=exports\vulkan-present"
set "DLL=%~3"
if not defined DLL set "DLL=exports\release-local\OptiScaler.dll"
if not exist "%OUT%" mkdir "%OUT%"
where cl >nul 2>nul && goto compile
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_ROOT=%%I"
call "%VS_ROOT%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>nul || exit /b 1
:compile
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /Iexternal\vulkan\include tests\vulkan_present_smoke.cpp /Fe"%OUT%\vulkan_present_smoke.exe" /Fo"%OUT%\vulkan_present_smoke.obj" /link OptiScaler\library\vulkan\vulkan-1.lib user32.lib gdi32.lib
if not %errorlevel%==0 exit /b 1
copy /y "%DLL%" "%OUT%\OptiScaler.dll" >nul || exit /b 1
if exist "%~1\dlssnr_amd_pass1.dll" (copy /y "%~1\dlssnr_amd_pass1.dll" "%OUT%\" >nul) else copy /y "%~1\version.dll" "%OUT%\dlssnr_amd_pass1.dll" >nul
xcopy /d /y "%~1\dlssnr_on_amd_weights.bin" "%OUT%\" >nul || exit /b 1
copy /y "%~2\libxess_fg.dll" "%OUT%\" >nul || exit /b 1
copy /y "%~2\libxell.dll" "%OUT%\" >nul || exit /b 1
set "OFF=%OUT%-off"
if not exist "%OFF%" mkdir "%OFF%"
copy /y "%OUT%\*.dll" "%OFF%\" >nul
copy /y "%OUT%\*.bin" "%OFF%\" >nul
copy /y "%OUT%\vulkan_present_smoke.exe" "%OFF%\" >nul || exit /b 1
call :ini true > "%OUT%\OptiScaler.ini"
call :ini false > "%OFF%\OptiScaler.ini"
for %%D in ("%OUT%" "%OFF%") do del /q "%%~D\OptiScaler.log" "%%~D\amd_presr.log" "%%~D\dlssnr_on_amd.log" 2>nul
if not defined GPU_LOCK goto run
:lock
mkdir "%GPU_LOCK%" 2>nul && goto run
powershell -NoProfile -Command "if (((Get-Date) - (Get-Item '%GPU_LOCK%').CreationTime).TotalMinutes -gt 20) { Remove-Item '%GPU_LOCK%' -Recurse -Force }; Start-Sleep 20"
goto lock
:run
call :smoke "%OUT%" ""
set "RESULT=%errorlevel%"
echo final image on: exit %RESULT%, log %OUT%\OptiScaler.log
call :smoke "%OFF%" "off"
set "RESULT_OFF=%errorlevel%"
echo final image off: exit %RESULT_OFF%, log %OFF%\OptiScaler.log
if defined GPU_LOCK rmdir "%GPU_LOCK%" 2>nul
if not %RESULT%==0 exit /b %RESULT%
exit /b %RESULT_OFF%
:smoke
powershell -NoProfile -Command "$s = @{ FilePath = '%~1\vulkan_present_smoke.exe'; WorkingDirectory = '%~1'; NoNewWindow = $true; PassThru = $true }; if ('%~2') { $s.ArgumentList = '%~2' }; $p = Start-Process @s; $null = $p.Handle; if (-not $p.WaitForExit(300000)) { $p.Kill(); Write-Output 'FAIL timeout'; exit 124 }; exit $p.ExitCode"
exit /b %errorlevel%
:ini
echo [FrameGen]
echo Enabled=true
echo FGInput=upscaler
echo FGOutput=xefg
echo [XeFG]
echo InterpolationCount=2
echo [DlssNr]
echo Enabled=true
echo PresentWithoutUpscaler=%1
echo NrBackend=daniel
echo AmdModelScale=0.5
echo [Log]
echo LogToFile=true
echo LogLevel=2
exit /b 0
:usage
echo usage: %~nx0 ^<runtime folder^> ^<XeFG folder^> [OptiScaler.dll]
exit /b 2
