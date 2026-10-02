# Called by test-gl-present.cmd, which sets up the x64 MSVC environment.
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

$mode = if ($args.Count -gt 0) { $args[0] } else { "menu" }
$runtime = if ($args.Count -gt 1) { $args[1] } else { "" }
if ($mode -notin @("menu", "nr", "fg")) { throw "mode must be menu, nr or fg" }
if ($mode -ne "menu" -and -not (Test-Path (Join-Path $runtime "dlssnr_amd_pass1.dll"))) {
    throw "$mode needs a runtime folder with dlssnr_amd_pass1.dll and dlssnr_on_amd_weights.bin"
}

$out = [System.IO.Path]::GetFullPath("exports\gl-present\$mode")
New-Item -ItemType Directory -Force -Path $out | Out-Null

& cl /nologo /std:c++20 /EHsc /W4 /utf-8 tests\gl_present_smoke.cpp "/Fe$out\gl_present_smoke.exe" "/Fo$out\gl_present_smoke.obj" /link opengl32.lib gdi32.lib user32.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed" }

Copy-Item -Force "exports\release-local\OptiScaler.dll" "$out\opengl32.dll"
$files = @()
if ($mode -ne "menu") { $files += "dlssnr_amd_pass1.dll", "dlssnr_on_amd_weights.bin" }
if ($mode -eq "fg") { $files += "libxess_fg.dll", "libxell.dll" }
foreach ($name in $files) {
    $source = Get-Item (Join-Path $runtime $name)
    $target = Join-Path $out $name
    if (-not (Test-Path $target) -or (Get-Item $target).Length -ne $source.Length) { Copy-Item -Force $source.FullName $target }
}

$nr = if ($mode -eq "menu") { "false" } else { "true" }
$fg = if ($mode -eq "fg") { "true" } else { "false" }
@"
[Log]
LogToFile=true
LogLevel=2

[Menu]
DisableSplash=true

[Hotfix]
CheckForUpdate=false

[DlssNr]
Enabled=$nr
PresentWithoutUpscaler=$nr
NrBackend=daniel

[FrameGen]
Enabled=$fg
FGOutput=$(if ($mode -eq "fg") { "xefg" } else { "auto" })
"@ | Set-Content -Encoding ascii "$out\OptiScaler.ini"
Remove-Item -Force -ErrorAction SilentlyContinue "$out\OptiScaler.log"

# One GPU, shared by every worktree: a directory is the lock, stale after 20 minutes.
$lock = [System.IO.Path]::GetFullPath("..\locks\gpu")
while ($true) {
    try { New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null; break }
    catch {
        $existing = Get-Item $lock -ErrorAction SilentlyContinue
        if ($existing -and $existing.CreationTime -lt (Get-Date).AddMinutes(-20)) { Remove-Item -Force -Recurse $lock; continue }
        Write-Host "waiting for the GPU lock"
        Start-Sleep -Seconds 20
    }
}
try {
    $frames = if ($mode -eq "menu") { 300 } else { 900 }
    $process = Start-Process -FilePath "$out\gl_present_smoke.exe" -ArgumentList $frames, $(if ($mode -eq "menu") { "menu" } else { "nr" }) `
        -WorkingDirectory $out -PassThru -NoNewWindow -RedirectStandardOutput "$out\stdout.txt"
    $null = $process.Handle
    if (-not $process.WaitForExit(150000)) { $process.Kill(); throw "timeout: the app did not exit in 150 s" }
    $code = $process.ExitCode
}
finally { Remove-Item -Force -Recurse $lock -ErrorAction SilentlyContinue }

Get-Content "$out\stdout.txt"
$log = Get-Content "$out\OptiScaler.log" -Raw
$expected = @(
    "OptiScaler working as opengl32.dll",
    "OpenGL: wglSwapBuffers hooked",
    "OpenGL menu: ImGui renderer ready",
    "OpenGL menu: opened",
    "OpenGL menu: frame drawn with the menu open",
    "OpenGL menu: closed"
)
if ($mode -ne "menu") {
    $expected += "OpenGL final-image NR: back buffer 1280x720", "OpenGL final-image NR: first frame processed", "ms average over 120 presents"
}
if ($mode -eq "fg") { $expected += "OpenGL XeFG presenter created", "OpenGL XeFG paused while OptiScaler menu is open" }
$failed = $code -ne 0
foreach ($line in $expected) {
    if ($log.Contains($line)) { Write-Host "ok: $line" } else { Write-Host "MISSING: $line"; $failed = $true }
}
Select-String -Path "$out\OptiScaler.log" -Pattern "OpenGL|error" | Select-Object -First 60 | ForEach-Object { $_.Line }
if ($failed) { Write-Host "FAIL ($mode, exit $code)"; exit 1 }
Write-Host "PASS ($mode)"
exit 0
