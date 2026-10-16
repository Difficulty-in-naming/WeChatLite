param(
    [ValidateSet('All', 'Client', 'Server')][string]$Role = 'All',
    [string]$RuntimeRoot = "$PSScriptRoot\..\dist",
    [string]$OutputDirectory = "$PSScriptRoot\..\dist",
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$crate = Join-Path $workspace 'packaging\windows-portable'
. (Join-Path $crate 'portable-functions.ps1')
$runtime = Confirm-PortableWorkspacePath $workspace $RuntimeRoot
$output = Confirm-PortableWorkspacePath $workspace $OutputDirectory
$stamp = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$stage = Confirm-PortableWorkspacePath $workspace (Join-Path $workspace "build\portable-packaging\$stamp")
New-Item -ItemType Directory -Force -Path $stage, $output | Out-Null
$hostInfo = & rustc -vV
if ($LASTEXITCODE -ne 0 -or -not ($hostInfo -match '^host: x86_64-pc-windows-msvc$')) {
    throw 'Windows x64 MSVC Rust toolchain is required for this package'
}

$oldRustFlags = $env:RUSTFLAGS
$oldEncodedFlags = $env:CARGO_ENCODED_RUSTFLAGS
$oldTarget = $env:CARGO_TARGET_DIR
$oldPayload = $env:WECHAT_PORTABLE_PAYLOAD
try {
    # Only this independent crate uses these build settings; the project workspace is unchanged.
    $env:RUSTFLAGS = '-C target-feature=+crt-static'
    $env:CARGO_ENCODED_RUSTFLAGS = $null
    $env:CARGO_TARGET_DIR = Join-Path $crate 'target'
    $env:WECHAT_PORTABLE_PAYLOAD = $null
    $cargoArguments = @('build', '--manifest-path', (Join-Path $crate 'Cargo.toml'), '--release', '--locked')
    if ($Offline) { $cargoArguments += '--offline' }
    & cargo @cargoArguments --bin portable-pack
    if ($LASTEXITCODE -ne 0) { throw 'Portable packer build failed' }
    $tool = Join-Path $crate 'target\release\portable-pack.exe'
    $roles = if ($Role -eq 'All') { @('Client', 'Server') } else { @($Role) }
    foreach ($packageRole in $roles) {
        $lower = $packageRole.ToLowerInvariant()
        $source = Confirm-PortableWorkspacePath $workspace (Join-Path $runtime "$lower-windows-x64")
        $payload = Confirm-PortableWorkspacePath $workspace (Join-Path $stage "$lower.payload")
        $metadata = & $tool $lower $source $payload
        if ($LASTEXITCODE -ne 0) { throw "Checked $packageRole runtime cannot enter a portable package" }
        $manifest = $metadata | ConvertFrom-Json
        if ($manifest.sha256 -notmatch '^[a-f0-9]{64}$' -or $manifest.role -ne $lower) {
            throw 'Portable packer returned invalid metadata'
        }
        $env:WECHAT_PORTABLE_PAYLOAD = $payload
        & cargo @cargoArguments --bin portable-launcher
        if ($LASTEXITCODE -ne 0) { throw "$packageRole portable launcher build failed" }
        $temporaryExe = Confirm-PortableWorkspacePath $workspace (Join-Path $stage "$lower.exe")
        Add-PortablePayload (Join-Path $crate 'target\release\portable-launcher.exe') $payload $temporaryExe
        Assert-PortablePayload $tool $temporaryExe $manifest.sha256
        $destination = Confirm-PortableWorkspacePath $workspace (Join-Path $output "WeChatLite-$packageRole-Windows-x64.exe")
        if (Test-Path -LiteralPath $destination -PathType Container) { throw 'Portable output is a directory' }
        Move-Item -LiteralPath $temporaryExe -Destination $destination -Force
        $exeHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
        Write-Output "$packageRole portable EXE: $destination"
        Write-Output "Payload files: $($manifest.files); payload SHA256: $($manifest.sha256); EXE SHA256: $exeHash"
    }
} finally {
    $env:RUSTFLAGS = $oldRustFlags
    $env:CARGO_ENCODED_RUSTFLAGS = $oldEncodedFlags
    $env:CARGO_TARGET_DIR = $oldTarget
    $env:WECHAT_PORTABLE_PAYLOAD = $oldPayload
}
