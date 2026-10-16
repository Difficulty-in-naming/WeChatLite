param([switch]$Offline)
$ErrorActionPreference = 'Stop'
$crate = $PSScriptRoot
$workspace = [IO.Path]::GetFullPath((Join-Path $crate '..\..'))
. (Join-Path $crate 'portable-functions.ps1')
$stamp = [Guid]::NewGuid().ToString('N')
$stage = Confirm-PortableWorkspacePath $workspace (Join-Path $workspace "build\portable-fixtures\$stamp")
New-Item -ItemType Directory -Path $stage | Out-Null
$oldRustFlags = $env:RUSTFLAGS
$oldEncodedFlags = $env:CARGO_ENCODED_RUSTFLAGS
$oldTarget = $env:CARGO_TARGET_DIR
$oldPayload = $env:WECHAT_PORTABLE_PAYLOAD
try {
    $env:RUSTFLAGS = '-C target-feature=+crt-static'
    $env:CARGO_ENCODED_RUSTFLAGS = $null
    $env:CARGO_TARGET_DIR = Join-Path $crate 'target'
    $env:WECHAT_PORTABLE_PAYLOAD = $null
    $arguments = @('build', '--manifest-path', (Join-Path $crate 'Cargo.toml'), '--release', '--locked')
    if ($Offline) { $arguments += '--offline' }
    & cargo @arguments --bin portable-fixture --bin portable-pack
    if ($LASTEXITCODE -ne 0) { throw 'Fixture binaries did not build' }
    $payload = Join-Path $stage 'fixture.payload'
    $hash = & (Join-Path $crate 'target\release\portable-fixture.exe') --make-payload $payload
    if ($LASTEXITCODE -ne 0 -or $hash -notmatch '^[a-f0-9]{64}$') { throw 'Fixture payload failed' }
    $env:WECHAT_PORTABLE_PAYLOAD = $payload
    & cargo @arguments --bin portable-launcher
    if ($LASTEXITCODE -ne 0) { throw 'Fixture launcher did not build' }
    $exe = Join-Path $stage 'fixture-portable.exe'
    Add-PortablePayload (Join-Path $crate 'target\release\portable-launcher.exe') $payload $exe
    Assert-PortablePayload (Join-Path $crate 'target\release\portable-pack.exe') $exe $hash
    # ProcessStartInfo.Arguments is deliberately a fixed test string: Windows quoting is part of this fixture.
    $processes = @()
    foreach ($index in 0..3) {
        $report = Join-Path $stage "report-$index.json"
        $start = [Diagnostics.ProcessStartInfo]::new()
        $start.FileName = $exe
        $start.WorkingDirectory = $stage
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.Arguments = '"' + $report + '" "" "带 空格" "a\"b" "tail\\"'
        $processes += [Diagnostics.Process]::Start($start)
    }
    foreach ($process in $processes) {
        if (-not $process.WaitForExit(15000) -or $process.ExitCode -ne 0) { throw 'Portable fixture launcher did not exit successfully' }
        $process.Dispose()
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $ready = @(Get-ChildItem -LiteralPath $stage -Filter 'report-*.json').Count -eq 4
        if (-not $ready) { Start-Sleep -Milliseconds 30 }
    } while (-not $ready -and [DateTime]::UtcNow -lt $deadline)
    if (-not $ready) { throw 'Detached fixture child did not produce its report' }
    foreach ($index in 0..3) {
        $report = Get-Content -LiteralPath (Join-Path $stage "report-$index.json") -Raw | ConvertFrom-Json
        if ($report.args.Count -ne 5 -or $report.args[1] -ne '' -or $report.args[2] -ne '带 空格' -or
            $report.args[3] -ne 'a"b' -or $report.args[4] -ne 'tail\' -or
            -not $report.cwd.Equals($stage, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Portable launcher changed arguments or working directory'
        }
    }
    Write-Output 'PASS: complete EXE hash/footer, static-CRT Windows launcher, four concurrent first launches, detached children, Unicode/empty/quotes/trailing-slash arguments and original working directory.'
    Write-Output "Private fixture reports: $stage"
} finally {
    $env:RUSTFLAGS = $oldRustFlags
    $env:CARGO_ENCODED_RUSTFLAGS = $oldEncodedFlags
    $env:CARGO_TARGET_DIR = $oldTarget
    $env:WECHAT_PORTABLE_PAYLOAD = $oldPayload
}
