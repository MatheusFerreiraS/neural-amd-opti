# Runs tests\final_image_gate_smoke.cpp against the built OptiScaler.dll (as dxgi.dll) in five cases and
# checks OptiScaler.log and amd_presr.log:
#   off       PresentWithoutUpscaler=false: OptiScaler hooks the game, the final image never starts.
#   lmxxf     PresentWithoutUpscaler=true with NrBackend=lmxxf: the final image stays off.
#   auto      PresentWithoutUpscaler=true, NrBackend unset and only the lmxxf runtime beside the game, which
#             makes the session lmxxf: the final image stays off.
#   daniel    PresentWithoutUpscaler=true with danielblnc: the runtime processes frames.
#   stall     as daniel at 1080p with AmdStallStandDownMs=1: the stall watch stands the final image down.
# The two danielblnc cases run on the GPU and take the shared GPU lock. Needs an x64 MSVC environment.
param(
    [string]$OptiDll = 'exports\release-local\OptiScaler.dll',
    [string]$OutDir = 'exports\final-image-gate',
    [string]$RuntimeDll = 'E:\Games\The Sims 4\Game\Bin\dlssnr_amd_pass1.dll',
    [string]$Weights = 'E:\Games\The Sims 4\Game\Bin\dlssnr_on_amd_weights.bin',
    [string]$LmxxfDll = 'E:\Projetos\opti_rr_dlss5\opti\exports\lmxxf-runtime\LmxxfNrRuntime.dll',
    [string]$GpuLock = 'E:\Projetos\opti_rr_dlss5\wt\locks\gpu',
    [int]$Frames = 300,
    [int]$TimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$OptiDll = (Resolve-Path $OptiDll).Path
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$exe = Join-Path $OutDir 'final_image_gate_smoke.exe'
& cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\final_image_gate_smoke.cpp "/Fe$exe" "/Fo$OutDir\final_image_gate_smoke.obj" /link d3d11.lib dxgi.lib user32.lib | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'smoke build failed' }

$failures = [System.Collections.Generic.List[string]]::new()
function Expect([bool]$ok, [string]$what) {
    $tag = '  FAIL '
    if ($ok) { $tag = '  ok   ' }
    Write-Host ($tag + $what)
    if (-not $ok) { $failures.Add($what) }
}

function Run-Case([string]$name, [string]$dlssNr, [bool]$runtime, [bool]$gpu, [string]$size = '', [bool]$lmxxf = $false) {
    $dir = Join-Path $OutDir $name
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Path $dir | Out-Null
    Copy-Item $exe $dir
    Copy-Item $OptiDll (Join-Path $dir 'dxgi.dll')
    if ($runtime) {
        Copy-Item $RuntimeDll (Join-Path $dir 'dlssnr_amd_pass1.dll')
        Copy-Item $Weights (Join-Path $dir 'dlssnr_on_amd_weights.bin')
    }
    if ($lmxxf) { Copy-Item $LmxxfDll (Join-Path $dir 'LmxxfNrRuntime.dll') }
    $ini = "[Log]`r`nLogToFile=true`r`nLogLevel=1`r`nLogToConsole=false`r`n[DlssNr]`r`nEnabled=true`r`n$dlssNr`r`n"
    [IO.File]::WriteAllText((Join-Path $dir 'OptiScaler.ini'), $ini)
    $locked = $false
    if ($gpu) {
        while (-not $locked) {
            try { New-Item -ItemType Directory -Path $GpuLock -ErrorAction Stop | Out-Null; $locked = $true }
            catch {
                $age = (Get-Date) - (Get-Item $GpuLock).CreationTime
                if ($age.TotalMinutes -gt 20) { Remove-Item -Recurse -Force $GpuLock -ErrorAction SilentlyContinue }
                else { Start-Sleep -Seconds 20 }
            }
        }
    }
    try {
        $p = Start-Process -FilePath (Join-Path $dir 'final_image_gate_smoke.exe') -ArgumentList "$Frames $size".Trim() `
            -WorkingDirectory $dir -PassThru -RedirectStandardOutput (Join-Path $dir 'stdout.txt')
        $null = $p.Handle  # keeps the exit code readable after WaitForExit
        if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
            Stop-Process -Id $p.Id -Force
            Expect $false "${name}: the smoke exits by itself within $TimeoutSeconds s"
            return $dir
        }
        Expect ($p.ExitCode -eq 0) "${name}: the smoke exits 0 (got $($p.ExitCode))"
    }
    finally { if ($locked) { Remove-Item -Recurse -Force $GpuLock -ErrorAction SilentlyContinue } }
    return $dir
}
function Text([string]$path) { if (Test-Path $path) { Get-Content -Raw $path } else { '' } }

Write-Host 'case off'
$d = Run-Case 'off' "PresentWithoutUpscaler=false`r`nNrBackend=daniel" $true $false
$log = Text (Join-Path $d 'OptiScaler.log')
Expect ($log -match 'working as dxgi\.dll') 'off: OptiScaler loaded as dxgi.dll'
Expect ($log -match 'D3D11CreateDevice|Device captured') 'off: OptiScaler saw the D3D11 device'
Expect (-not (Test-Path (Join-Path $d 'amd_presr.log'))) 'off: no AMD runtime started (no amd_presr.log)'
Expect ($log -cnotmatch 'Final-image') 'off: no final-image line in OptiScaler.log'

Write-Host 'case lmxxf'
$d = Run-Case 'lmxxf' "PresentWithoutUpscaler=true`r`nNrBackend=lmxxf" $true $false
$log = Text (Join-Path $d 'OptiScaler.log')
Expect ($log -match 'working as dxgi\.dll') 'lmxxf: OptiScaler loaded as dxgi.dll'
Expect (-not (Test-Path (Join-Path $d 'amd_presr.log'))) 'lmxxf: the final image did not start the danielblnc runtime'

Write-Host 'case auto'
$d = Run-Case 'auto' "PresentWithoutUpscaler=true" $false $false '' $true
$log = Text (Join-Path $d 'OptiScaler.log')
Expect ($log -match 'working as dxgi\.dll') 'auto: OptiScaler loaded as dxgi.dll'
Expect (-not (Test-Path (Join-Path $d 'amd_presr.log'))) 'auto: an lmxxf session did not start the final image'

Write-Host 'case daniel'
$d = Run-Case 'daniel' "PresentWithoutUpscaler=true`r`nNrBackend=daniel" $true $true
$presr = Text (Join-Path $d 'amd_presr.log')
$log = Text (Join-Path $d 'OptiScaler.log')
Expect ($presr -cmatch 'AMD history reset: frame=0') 'daniel: the runtime recorded its first final-image frame'
Expect ($presr -match 'frame=[1-9]\d') 'daniel: the runtime kept recording frames'
Expect ($log -cnotmatch 'off until the game restarts') 'daniel: no stall stand-down at the default limit'

Write-Host 'case stall'
# The watch looks every 250 ms at the oldest unfinished job; at 1080p the queue spends most of each frame on
# the runtime, so a 1 ms limit is met within the run.
$d = Run-Case 'stall' "PresentWithoutUpscaler=true`r`nNrBackend=daniel`r`nAmdStallStandDownMs=1" $true $true '1920 1080'
$log = Text (Join-Path $d 'OptiScaler.log')
Expect ($log -cmatch 'Final-image NR: off until the game restarts') 'stall: a 1 ms limit stands the final image down'

if ($failures.Count) { Write-Host "FAIL: $($failures.Count) check(s)"; exit 1 }
Write-Host 'PASS: final-image gate smoke'
exit 0
