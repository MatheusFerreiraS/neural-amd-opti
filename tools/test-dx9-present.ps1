# Runs tests\dx9_present_smoke.cpp with exports\release-local\OptiScaler.dll as d3d9.dll beside it and checks
# the test's own output and OptiScaler.log. See test-dx9-present.cmd.
param(
    [string]$Case = "",
    [string]$Runtime = "exports\dx9-smoke\runtime",
    [string]$XeFG = "..\..\opti\dist\OptiScaler-0.4.9-amd-nr\OptiScaler",
    [string]$Lock = "",
    # Added to every case's OptiScaler.ini, for one-off comparisons: "[Section]~Key=value~..."
    [string]$ExtraIni = "",
    [int]$TimeoutSeconds = 240
)
$ErrorActionPreference = "Stop"
Set-Location (Split-Path -Parent $PSScriptRoot)

$out = [System.IO.Path]::GetFullPath("exports\dx9-smoke")
New-Item -ItemType Directory -Force -Path $out | Out-Null
$exe = Join-Path $out "dx9_present_smoke.exe"
& cl /nologo /std:c++20 /EHsc /O2 /W4 /utf-8 tests\dx9_present_smoke.cpp "/Fe$exe" "/Fo$(Join-Path $out 'dx9_present_smoke.obj')" user32.lib gdi32.lib
if ($LASTEXITCODE -ne 0) { throw "cl failed" }

$opti = [System.IO.Path]::GetFullPath("exports\release-local\OptiScaler.dll")
foreach ($file in @($opti, (Join-Path $Runtime "dlssnr_amd_pass1.dll"), (Join-Path $Runtime "dlssnr_on_amd_weights.bin"))) {
    if (-not (Test-Path $file)) { throw "missing $file" }
}

# Menu (Insert), NR (Numpad 5) and FG (End) keys, a Reset and screen samples. mark= starts a timed phase.
$nrScript = "90:mark=nr-on,150:motion=nr-on,170:menu,190:shot=menu-open,200:menu,215:shot=menu-closed," +
            "230:nr,240:mark=nr-off,300:motion=nr-off,310:nr,320:mark=nr-on-again,380:reset=1600x900," +
            "390:mark=after-reset,400:menu,415:shot=menu-after-reset,425:menu,450:motion=after-reset,460:shot=after-reset"
# With FG, the open menu's model scale slider (at 380,395 in the 1280x720 window) is dragged to the left end, then
# the menu's Frame Generation section (its sidebar button at 337,241) is opened: neither may stop generation.
$fgScript = "150:mark=fg-on,200:motion=fg-on,220:menu,232:shot=fg-menu-open,235:press=380x395,241:release," +
            "244:shot=fg-scale-set,246:press=337x241,249:release,256:shot=fg-section,262:menu," +
            "270:shot=fg-menu-closed,280:fg,290:mark=fg-off,330:motion=fg-off," +
            "340:fg,350:mark=fg-on-again,420:motion=fg-on-again,430:nr,440:mark=nr-off,480:motion=nr-off,490:nr," +
            "500:mark=nr-on,580:motion=nr-on,650:reset=1600x900,660:mark=after-reset,760:motion=after-reset," +
            "765:shot=fg-after-reset,780:minimize,820:restore,840:menu,850:shot=fg-menu-after-restore,860:menu," +
            "900:motion=after-restore"
$nrLog = @("OptiScaler working as d3d9.dll", "D3D9 device hooks installed", "D3D9 bridge: D3D12 device",
           "D3D9 menu ready", "D3D9 bridge frame \d+: menu open", "D3D9 bridge frame \d+: menu closed",
           "D3D9 bridge frame \d+: D3D9 present, NR true", "Final-image NR \(FidelityFX Optical Flow",
           "Reset.*: 0, 1600x900", "D3D9 bridge: 1600x900", "D3D9 bridge frame 4\d\d: menu open")
$fgLog = @("OptiScaler working as d3d9.dll", "D3D9 device hooks installed", "D3D9 bridge: XeFG presenter",
           "D3D9 bridge frame \d+: menu open", "D3D9 bridge frame \d+: menu closed",
           "D3D9 bridge frame \d+: NR scale 1.00 -> 0.[5-9]",
           "D3D9 bridge frame 2[89]\d: frame generation off", "D3D9 bridge frame 3[4-9]\d: frame generation on",
           "D3D9 bridge frame 4[3-9]\d: frame generation off", "D3D9 bridge frame [45]\d\d: frame generation on",
           "D3D9 bridge frame 601: XeFG presenter, NR true at scale 0.[5-9]\d, FG true, XeFG last present [2-9] frames",
           "Reset.*: 0, 1600x900", "XeFG presenter 1280x720 -> 1600x900: 0", "D3D9 bridge frame 8[4-9]\d: menu open")

# Generation stays on through the scale change and the visit to the Frame Generation section.
$fgNotLog = @("D3D9 bridge frame 2[0-7]\d: frame generation off")

$cases = @(
    # Final image off (the default): the D3D9 route only draws the menu, no D3D12 device.
    @{ Name = "menu-only"; Args = @(); Fg = $false; Present = "false"; Frames = 260; Script = "170:menu,190:shot=menu-only,200:menu"
       Log = @("D3D9 menu ready", "D3D9 bridge frame \d+: menu open", "D3D9 bridge frame \d+: menu closed",
               "D3D9 bridge frame 1: D3D9 present, NR false")
       NotLog = @("D3D9 bridge: D3D12 device") },
    @{ Name = "classic-nr"; Args = @(); Fg = $false; Frames = 470; Script = $nrScript; Log = $nrLog },
    # With the scene still, the screen with NR differs from the screen without it, and two frames without NR match.
    @{ Name = "nr-visible"; Args = @(); Fg = $false; Frames = 200
       Script = "60:still,140:shot=nr-on,150:nr,165:shot=nr-off,170:diff=nr-off,175:diff=nr-on,180:nr"
       Log = @("D3D9 bridge frame 1: D3D9 present, NR true")
       Out = @("diff nr-off: [0-9.]+ of the pixels changed, mean difference 0\.[0-4]",
               "diff nr-on: [0-9.]+ of the pixels changed, mean difference (?:[1-9]|0\.[5-9])") },
    @{ Name = "ex-nr"; Args = @("--ex"); Fg = $false; Frames = 470; Script = $nrScript; Log = $nrLog },
    @{ Name = "chain-nr"; Args = @("--chain"); Fg = $false; Frames = 470; Script = $nrScript; Log = $nrLog },
    @{ Name = "msaa-nr"; Args = @("--msaa"); Fg = $false; Frames = 470; Script = $nrScript; Log = $nrLog },
    @{ Name = "classic-fg"; Args = @(); Fg = $true; Frames = 910; Script = $fgScript; Log = $fgLog; NotLog = $fgNotLog },
    @{ Name = "ex-fg"; Args = @("--ex"); Fg = $true; Frames = 910; Script = $fgScript; Log = $fgLog; NotLog = $fgNotLog },
    @{ Name = "msaa-fg"; Args = @("--msaa"); Fg = $true; Frames = 910; Script = $fgScript; Log = $fgLog; NotLog = $fgNotLog },
    # The game moves to a new window: after two quiet seconds on the old one the bridge follows it.
    @{ Name = "window-nr"; Args = @(); Fg = $false; Frames = 700
       Script = "150:window2,151:cpu=6,600:cpu=0,620:menu,635:shot=window2-menu,645:menu,680:motion=window2"
       Log = @("(?s)following device \w+, window \w+.*following device \w+, window \w+.*D3D9 menu ready.*menu open") },
    @{ Name = "window-fg"; Args = @(); Fg = $true; Frames = 700
       Script = "150:window2,151:cpu=6,600:cpu=0,620:menu,635:shot=window2-menu,645:menu,680:motion=window2"
       Log = @("(?s)XeFG presenter \w+ on window.*releasing the XeFG presenter.*XeFG presenter \w+ on window.*frame generation on.*menu open") },
    # Thousands of generated frames in a row (the 32-bit bridge lost its D3D12 device after about 2000), with two
    # generated frames per rendered frame.
    @{ Name = "soak-fg"; Args = @(); Fg = $true; Frames = 6000; Ini = "[XeFG]`r`nInterpolationCount=2`r`n"
       Script = "1000:motion=soak-1000,3000:motion=soak-3000,5900:motion=soak-5900"
       Log = @("D3D9 bridge frame 5701: XeFG presenter, NR true at scale 1.00, FG true, XeFG last present 3 frames") },
    # Frame times at 1920x1080 borderless with the scene moving and still, with 8 ms of game CPU work per frame,
    # NR or FG on and off.
    @{ Name = "timing-nr"; Args = @("--borderless", "--size", "1920x1080"); Fg = $false; Frames = 880
       Script = "100:mark=nr-moving,220:still,230:mark=nr-still,350:still,360:cpu=8,370:mark=nr-cpu8,490:nr," +
                "500:mark=off-cpu8,620:cpu=0,630:mark=off-moving,750:still,760:mark=off-still"
       Log = @("D3D9 bridge frame \d+: D3D9 present, NR true") },
    @{ Name = "timing-fg"; Args = @("--borderless", "--size", "1920x1080"); Fg = $true; Frames = 930
       Script = "150:mark=fg-moving,270:still,280:mark=fg-still,400:still,410:cpu=8,420:mark=fg-cpu8,540:fg," +
                "550:mark=nr-only-cpu8,670:cpu=0,680:mark=nr-only-moving,800:nr,810:mark=off-moving"
       Log = @("D3D9 bridge frame \d+: XeFG presenter, NR true at scale [0-9.]+, FG true") },
    @{ Name = "timing-fg-nopacing"; Args = @("--borderless", "--size", "1920x1080"); Fg = $true; Frames = 930
       Ini = "[XeFG]`r`nExtraPacing=false`r`n"
       Script = "150:mark=fg-moving,270:still,280:mark=fg-still,400:still,410:cpu=8,420:mark=fg-cpu8,540:fg," +
                "550:mark=nr-only-cpu8,670:cpu=0,680:mark=nr-only-moving,800:nr,810:mark=off-moving"
       Log = @("D3D9 bridge frame \d+: XeFG presenter, NR true at scale [0-9.]+, FG true") }
)
if ($Case) { $cases = @($cases | Where-Object { $_.Name -eq $Case }) }
if ($cases.Count -eq 0) { throw "unknown case $Case" }

function Enter-Lock {
    if (-not $Lock) { return }
    while ($true) {
        try { New-Item -ItemType Directory -Path $Lock -ErrorAction Stop | Out-Null; return } catch { }
        $item = Get-Item $Lock -ErrorAction SilentlyContinue
        if ($item -and $item.CreationTime -lt (Get-Date).AddMinutes(-20)) { Remove-Item $Lock -Force -Recurse }
        else { Start-Sleep -Seconds 20 }
    }
}

$failed = @()
foreach ($c in $cases) {
    $dir = Join-Path $out $c.Name
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Copy-Item $exe $dir
    Copy-Item $opti (Join-Path $dir "d3d9.dll")
    foreach ($name in @("dlssnr_amd_pass1.dll", "dlssnr_on_amd_weights.bin", "dlssnr_on_amd.ini")) {
        $source = Join-Path $Runtime $name
        if (Test-Path $source) { New-Item -ItemType HardLink -Path (Join-Path $dir $name) -Target ([System.IO.Path]::GetFullPath($source)) | Out-Null }
    }
    $fgSection = "[FrameGen]`r`nEnabled=false`r`n"
    if ($c.Fg) {
        foreach ($name in @("libxess_fg.dll", "libxell.dll")) { Copy-Item (Join-Path $XeFG $name) $dir }
        $fgSection = "[FrameGen]`r`nEnabled=true`r`nFGInput=upscaler`r`nFGOutput=xefg`r`n"
    }
    Set-Content -Path (Join-Path $dir "OptiScaler.ini") -Encoding ascii -Value (
        "[DlssNr]`r`nEnabled=true`r`nPresentWithoutUpscaler=$(if ($c.Present) { $c.Present } else { 'true' })`r`n" +
        "NrBackend=daniel`r`n" + $fgSection + $c.Ini + ($ExtraIni -replace '~', "`r`n") + "`r`n" +
        "[Log]`r`nLogToFile=true`r`nLogLevel=2`r`nSingleFile=true`r`n")

    $arguments = @($c.Args) + @("--frames", $c.Frames, "--shots", "`"$dir`"", "--script", "`"$($c.Script)`"")
    Enter-Lock
    try {
        $process = Start-Process -FilePath (Join-Path $dir "dx9_present_smoke.exe") -ArgumentList $arguments `
            -WorkingDirectory $dir -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $dir "stdout.txt") `
            -RedirectStandardError (Join-Path $dir "stderr.txt")
        $null = $process.Handle
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            $process.Kill()
            $process.WaitForExit()
            $failed += "$($c.Name): timed out"
            continue
        }
        $code = $process.ExitCode
    }
    finally {
        if ($Lock) { Remove-Item $Lock -Force -Recurse -ErrorAction SilentlyContinue }
    }

    $stdout = Get-Content (Join-Path $dir "stdout.txt") -Raw
    $log = Get-Content (Join-Path $dir "OptiScaler.log") -Raw -ErrorAction SilentlyContinue
    $problems = @()
    if ($code -ne 0) { $problems += "exit code $code" }
    if ($stdout -notmatch "DONE 0 present failures") { $problems += "present failures" }
    foreach ($pattern in $c.Log) { if ($log -notmatch $pattern) { $problems += "log lacks: $pattern" } }
    foreach ($pattern in $c.NotLog) { if ($log -match $pattern) { $problems += "log has: $pattern" } }
    foreach ($pattern in $c.Out) { if ($stdout -notmatch $pattern) { $problems += "output lacks: $pattern" } }
    foreach ($line in ($stdout -split "`r?`n" | Where-Object { $_ -match "^motion (\S+): ([0-9.]+)" })) {
        $null = $line -match "^motion (\S+): ([0-9.]+)"
        if ([double]$Matches[2] -lt 0.02) { $problems += "frozen on screen: $line" }
    }
    if ($log -match "\[error\].*D3D9") { $problems += "D3D9 error in the log" }

    Write-Host "== $($c.Name): $(if ($problems.Count) { 'FAIL' } else { 'PASS' })"
    $stdout -split "`r?`n" | Where-Object { $_ -match "^(phase|motion|shot|reset|diff)" } | ForEach-Object { Write-Host "   $_" }
    foreach ($p in $problems) { Write-Host "   $p" }
    if ($problems.Count) { $failed += $c.Name }
}

if ($failed.Count) { Write-Host "FAILED: $($failed -join ', ')"; exit 1 }
Write-Host "D3D9 present tests passed"
exit 0
