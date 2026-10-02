# Runs tests\dx9_x86_smoke.cpp against the 32-bit D3D9 bridge staged in exports\dx9-x86 (see
# tools\test-dx9-x86.cmd, which builds it first). Each case writes its own OptiScaler.ini beside the
# host, clears the logs, runs the smoke program and checks its exit code and the logs. The whole set
# holds the GPU lock (wt\locks\gpu).
param(
    [string]$Runtime = 'E:\Games\The Sims 4\Game\Bin\dlssnr_amd_pass1.dll',
    [string]$Weights = 'E:\Games\The Sims 4\Game\Bin\dlssnr_on_amd_weights.bin',
    [string]$XeFG = 'E:\Projetos\opti_rr_dlss5\opti\dist\OptiScaler-0.4.9-amd-nr\OptiScaler',
    [string[]]$Only = @()
)
$ErrorActionPreference = 'Stop'
$Only = @($Only | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$root = Split-Path $PSScriptRoot -Parent
$stage = Join-Path $root 'exports\dx9-x86'
$host64 = Join-Path $stage 'OptiScaler64'
$smoke = Join-Path $stage 'dx9_x86_smoke.exe'
$lock = 'E:\Projetos\opti_rr_dlss5\wt\locks\gpu'

Copy-Item (Join-Path $root 'exports\release-local\OptiScaler.dll') $host64 -Force
foreach ($pair in @(@($Runtime, 'dlssnr_amd_pass1.dll'), @($Weights, 'dlssnr_on_amd_weights.bin'),
        @((Join-Path $XeFG 'libxess_fg.dll'), 'libxess_fg.dll'), @((Join-Path $XeFG 'libxell.dll'), 'libxell.dll'))) {
    $target = Join-Path $host64 $pair[1]
    if (-not (Test-Path $target) -or (Get-Item $pair[0]).Length -ne (Get-Item $target).Length) {
        Copy-Item $pair[0] $target -Force
    }
}

function Write-Ini([string[]]$lines) {
    $base = @('[Log]', 'LogToFile=true', 'LogLevel=1', '[Menu]', 'ShortcutKey=auto')
    Set-Content -Path (Join-Path $host64 'OptiScaler.ini') -Value ($base + $lines) -Encoding ascii
}
function Clear-Logs {
    foreach ($name in 'OptiScaler.log', 'amd_presr.log', 'dx9-host.log', 'dx9-x86.log', 'dlssnr_on_amd.log') {
        Remove-Item (Join-Path $host64 $name) -ErrorAction SilentlyContinue
    }
}
function Count([string]$log, [string]$text) {
    $path = Join-Path $host64 $log
    if (-not (Test-Path $path)) { return 0 }
    return @(Select-String -Path $path -SimpleMatch -Pattern $text).Count
}

$results = [ordered]@{}
$screen = $false
# A case that needs keys runs again (twice at most) when another program held the foreground.
function Run-Case([string]$name, [string[]]$ini, [string[]]$arguments, [scriptblock]$check) {
    if ($Only.Count -and $Only -notcontains $name) { return }
    for ($try = 0; $try -lt 3; $try++) {
        Run-Once $name $ini $arguments $check
        if ($results[$name] -notlike '*skipped*') { return }
        Write-Host "${name}: the test window was not in front; running it again"
    }
}
function Run-Once([string]$name, [string[]]$ini, [string[]]$arguments, [scriptblock]$check) {
    Write-Ini $ini
    Clear-Logs
    $out = Join-Path $stage "$name.out.txt"
    $process = Start-Process -FilePath $smoke -ArgumentList $arguments -WorkingDirectory $stage -PassThru `
        -NoNewWindow -RedirectStandardOutput $out
    $null = $process.Handle
    if (-not $process.WaitForExit(240000)) {
        $process.Kill()
        Get-Process OptiScalerDx9Host -ErrorAction SilentlyContinue | Stop-Process -Force
        $results[$name] = 'FAIL (no exit in 240 s)'
        return
    }
    Get-Process OptiScalerDx9Host -ErrorAction SilentlyContinue | Wait-Process -Timeout 15 -ErrorAction SilentlyContinue
    $text = Get-Content $out -Raw
    $bmp = Join-Path $stage "$name.bmp"
    if (Test-Path $bmp) {
        Add-Type -AssemblyName System.Drawing
        $image = [Drawing.Image]::FromFile($bmp)
        $image.Save((Join-Path $stage "$name.png"), [Drawing.Imaging.ImageFormat]::Png)
        $image.Dispose()
        Remove-Item $bmp
    }
    Start-Sleep -Milliseconds 500
    Get-ChildItem $host64 -Filter *.log | ForEach-Object {
        Copy-Item $_.FullName (Join-Path $stage "$name.$($_.Name)") -Force -ErrorAction SilentlyContinue }
    $problems = @()
    if ($process.ExitCode -ne 0) { $problems += "exit $($process.ExitCode)" }
    if ($check) { $problems += @(& $check $text | Where-Object { $_ }) }
    if ($text -match 'SKIP ') { $problems += 'skipped: the test window could not take the foreground' }
    $results[$name] = if ($problems.Count) { 'FAIL (' + ($problems -join '; ') + ')' } else { 'PASS' }
    if ($text -match 'ms_per_frame=([0-9.]+)') { $results[$name] += " ($($Matches[1]) ms per frame)" }
    Write-Host "== $name : $($results[$name])"
    Write-Host $text
}

while ($true) {
    try { New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null; break }
    catch {
        if ((Test-Path $lock) -and ((Get-Date) - (Get-Item $lock).CreationTime).TotalMinutes -gt 20) {
            Remove-Item $lock -Force -Recurse -ErrorAction SilentlyContinue
        }
        else { Start-Sleep -Seconds 20 }
    }
}
try {
    Run-Case 'native' @() @('--native', '--frames', '150') {
        param($text)
        $script:screen = $text -match 'screen=ok'
    }
    $screenArg = if ($screen) { @('--screen') } else { @() }
    if (-not $screen) { Write-Host 'Screen capture shows no change even for the native D3D9 path; screen checks are skipped.' }

    Run-Case 'bridge' @() (@('--frames', '300', '--expect-bridged', '--shot-at', '250', 'bridge.bmp') + $screenArg) {
        param($text)
        if ((Count 'OptiScaler.log' 'OptiScaler working as OptiScaler.dll') -eq 0) { 'OptiScaler.dll did not start in the host' }
        if ((Count 'dx9-host.log' 'presented') -eq 0) { 'the host logged no presented frame' }
    }
    Run-Case 'bridge-ex' @() (@('--ex', '--frames', '300', '--expect-bridged') + $screenArg) {
        param($text)
        if ($text -notmatch 'transport=1') { 'D3D9Ex did not use a shared texture' }
    }
    Run-Case 'shared-memory' @('[Dx9X86]', 'SharedMemory=1') (@('--frames', '300', '--expect-bridged') + $screenArg) {
        param($text)
        if ($text -notmatch 'transport=2') { 'the shared-memory transport was not used' }
    }
    $nr = @('[DlssNr]', 'Enabled=true', 'PresentWithoutUpscaler=true', 'NrBackend=daniel')
    Run-Case 'nr' $nr (@('--frames', '500', '--expect-bridged', '--key-at', '250', '0x65', '--key-at', '350', '0x65') + $screenArg) {
        param($text)
        $done = Count 'amd_presr.log' 'Completed AMD pre-SR passes'
        if ($done -lt 5) { "fewer than 240 NR frames completed ($done log lines)" }
        if ((Count 'OptiScaler.log' 'setting DlssNrEnabled to false') -eq 0) { 'NR was not turned off' }
    }
    # The window on a thread that is blocked while the game renders and presents (an engine with a render
    # thread): the child window must still come, and a lost host must still be replaced.
    Run-Case 'window-thread' $nr (@('--ex', '--window-thread', '--frames', '300', '--expect-bridged') + $screenArg) {
        param($text)
        if ((Count 'amd_presr.log' 'Completed AMD pre-SR passes') -lt 3) { 'NR did not run' }
    }
    Run-Case 'window-thread-restart' $nr @('--window-thread', '--frames', '900', '--vsync', '--work', '10', '--kill-host-at',
        '200') {
        param($text)
        if ((Count 'dx9-x86.log' 'host 2 connected') -eq 0) { 'no second host' }
    }
    $fg = $nr + @('[FrameGen]', 'Enabled=true', 'FGInput=upscaler', 'FGOutput=xefg', '[XeFG]', 'InterpolationCount=2')
    # At the display's rate (--vsync), so the frames after the Reset span the host restart that
    # XeFG's refused resize costs.
    Run-Case 'nr-fg' $fg (@('--frames', '1300', '--vsync', '--expect-bridged', '--expect-fg', '--key-at', '300',
            '0x23', '--key-at', '400', '0x23', '--reset-at', '550', '1600x900') + $screenArg) {
        param($text)
        if ((Count 'OptiScaler.log' 'Created Dx11wDx12SC') -eq 0) { 'no Dx11wDx12 swap chain' }
        if ((Count 'OptiScaler.log' 'XeFG swapchain created') -eq 0) { 'no XeFG swap chain' }
        $done = Count 'amd_presr.log' 'Completed AMD pre-SR passes'
        if ($done -lt 5) { "fewer than 240 NR frames completed ($done log lines)" }
    }
    # The archived bridge froze the picture when NR and frame generation were both turned off: turn
    # both off and sample the screen afterwards.
    Run-Case 'nr-fg-off' $fg (@('--frames', '500', '--expect-bridged', '--key-at', '300', '0x23', '--key-at', '320',
            '0x65') + $screenArg) {
        param($text)
        if ((Count 'OptiScaler.log' 'setting DlssNrEnabled to false') -eq 0) { 'NR was not turned off' }
        if ((Count 'OptiScaler.log' 'setting FGEnabled to false') -eq 0) { 'frame generation was not turned off' }
    }
    # The archived bridge lost frame generation when the NR model scale changed: open the menu, drag the
    # final-image Model scale slider (where the menu draws it at 1280x720; twice, as a scripted click is
    # sometimes missed) and keep going with XeFG on.
    Run-Case 'nr-scale-fg' $fg @('--frames', '900', '--expect-bridged', '--expect-fg', '--key-at', '300', '0x2D',
        '--drag-at', '340', '470', '395', '420', '395', '--drag-at', '380', '470', '395', '420', '395', '--shot-at', '420',
        'nr-scale-fg.bmp', '--key-at', '440', '0x2D') {
        param($text)
        $last = Select-String -Path (Join-Path $host64 'amd_presr.log') -Pattern 'Completed AMD pre-SR passes=\d+ at (\d+x\d+)' |
            Select-Object -Last 1
        if (-not $last -or $last.Matches[0].Groups[1].Value -eq '1280x720') { 'NR did not run at a new model scale' }
    }
    Run-Case 'menu' $nr @('--frames', '300', '--menu-at', '100', '--shot-at', '150', 'menu.bmp') $null
    Run-Case 'reset' $nr (@('--frames', '400', '--expect-bridged', '--reset-at', '200', '1600x900') + $screenArg) {
        param($text)
        if ($text -notmatch 'hostStarts=1 ') { 'the host was restarted for a resize without frame generation' }
    }
    Run-Case 'restart-nr' $nr @('--frames', '900', '--vsync', '--work', '10', '--kill-host-at', '200') {
        param($text)
        if ((Count 'dx9-x86.log' 'host 2 connected') -eq 0) { 'no second host' }
    }
    Run-Case 'restart' $fg @('--frames', '900', '--vsync', '--work', '10', '--kill-host-at', '200', '--expect-fg') {
        param($text)
        if ((Count 'dx9-x86.log' 'host 2 connected') -eq 0) { 'no second host' }
    }
    # Frame time at 1920x1080 with 8 ms of game CPU per frame: native, the bridge, NR, each pipelined
    # and waiting for the host at every Present. Reported, not judged.
    $timing = @('--size', '1920x1080', '--frames', '300', '--work', '8')
    Run-Case 'time-native' @() (@('--native') + $timing) $null
    Run-Case 'time-bridge' @() ($timing + @('--expect-bridged')) $null
    Run-Case 'time-bridge-sync' @('[Dx9X86]', 'Pipelined=false') ($timing + @('--expect-bridged')) $null
    Run-Case 'time-nr' $nr ($timing + @('--expect-bridged')) $null
    Run-Case 'time-nr-sync' ($nr + @('[Dx9X86]', 'Pipelined=false')) ($timing + @('--expect-bridged')) $null
    Run-Case 'time-nr-ex' $nr ($timing + @('--ex', '--expect-bridged')) $null
    Run-Case 'time-nr-fg' $fg ($timing + @('--expect-bridged')) $null
}
finally {
    Remove-Item $lock -Force -Recurse -ErrorAction SilentlyContinue
}

Write-Host ''
$failed = 0
foreach ($entry in $results.GetEnumerator()) {
    Write-Host ("{0,-18} {1}" -f $entry.Key, $entry.Value)
    if ($entry.Value -notlike 'PASS*') { $failed++ }
}
exit [int]($failed -ne 0)
