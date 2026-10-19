param(
    [string]$QtRoot = "$PSScriptRoot\..\.qt\6.8.3\msvc2022_64",
    [string]$FFmpeg = "$PSScriptRoot\..\.tools\Lib\site-packages\imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v7.1.exe",
    [string]$CMake = "$PSScriptRoot\..\.tools\Scripts\cmake.exe",
    [string]$CTest = "$PSScriptRoot\..\.tools\Scripts\ctest.exe",
    [string]$Generator = 'Visual Studio 18 2026'
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $workspace
if (-not (Test-Path -LiteralPath $CMake -PathType Leaf)) { $CMake = 'cmake' }
if (-not (Test-Path -LiteralPath $CTest -PathType Leaf)) { $CTest = 'ctest' }
if (-not (Test-Path -LiteralPath $FFmpeg -PathType Leaf)) {
    throw 'Provide the standalone FFmpeg EXE with -FFmpeg <path>. Python and PyInstaller are not build dependencies.'
}
$FFmpeg = (Resolve-Path -LiteralPath $FFmpeg).ProviderPath
$QtRoot = [IO.Path]::GetFullPath($QtRoot)
if (-not (Test-Path -LiteralPath (Join-Path $QtRoot 'bin\windeployqt.exe') -PathType Leaf)) {
    throw 'Provide an MSVC x64 Qt installation with -QtRoot <path>.'
}
$env:PATH = "$QtRoot\bin;$workspace\.tools\Scripts;$env:PATH"

# Build the independent C++ action module and pin the exact binary before Rust embeds its hash.
& (Join-Path $workspace 'adapters\native\build.ps1') -CMake $CMake -CTest $CTest -Generator $Generator
if ($LASTEXITCODE) { throw 'Native actions build failed' }
$actionsBinary = Join-Path $workspace 'build\native\Release\WeChatActions.dll'
$actionsHash = (Get-FileHash -LiteralPath $actionsBinary -Algorithm SHA256).Hash.ToLowerInvariant()
$quoteBinary = Join-Path $workspace 'build\native\Release\WeChatQuote.dll'
$quoteHash = (Get-FileHash -LiteralPath $quoteBinary -Algorithm SHA256).Hash.ToLowerInvariant()
$adapterVendor = Join-Path $workspace 'adapters\windows-rs\vendor'
New-Item -ItemType Directory -Force -Path $adapterVendor | Out-Null
Copy-Item -LiteralPath $actionsBinary -Destination (Join-Path $adapterVendor 'WeChatActions.dll') -Force
[IO.File]::WriteAllText((Join-Path $adapterVendor 'WeChatActions.sha256'), "$actionsHash`n", [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath $quoteBinary -Destination (Join-Path $adapterVendor 'WeChatQuote.dll') -Force
[IO.File]::WriteAllText((Join-Path $adapterVendor 'WeChatQuote.sha256'), "$quoteHash`n", [Text.UTF8Encoding]::new($false))
$mediaBinary = Join-Path $workspace 'build\native\Release\WeChatMedia.dll'
$mediaHash = (Get-FileHash -LiteralPath $mediaBinary -Algorithm SHA256).Hash.ToLowerInvariant()
Copy-Item -LiteralPath $mediaBinary -Destination (Join-Path $adapterVendor 'WeChatMedia.dll') -Force
[IO.File]::WriteAllText((Join-Path $adapterVendor 'WeChatMedia.sha256'), "$mediaHash`n", [Text.UTF8Encoding]::new($false))

# Cargo media fixtures locate these standalone runtime files next to the test executable.
$testRuntime = Join-Path $workspace 'target\debug\deps'
New-Item -ItemType Directory -Force -Path $testRuntime | Out-Null
Copy-Item -LiteralPath $FFmpeg -Destination (Join-Path $testRuntime 'ffmpeg.exe') -Force
Copy-Item -LiteralPath $actionsBinary -Destination (Join-Path $testRuntime 'WeChatActions.dll') -Force
Copy-Item -LiteralPath $quoteBinary -Destination (Join-Path $testRuntime 'WeChatQuote.dll') -Force
cargo test --locked -p wechat-relay -p wechat-adapter -p wechat-p2p
if ($LASTEXITCODE) { throw 'Rust relay/adapter tests failed' }
cargo build --release --locked -p wechat-relay -p wechat-adapter -p wechat-p2p
if ($LASTEXITCODE) { throw 'Rust relay/adapter build failed' }
$previousRelayBinary = $env:WECHAT_RELAY_BINARY
try {
    $env:WECHAT_RELAY_BINARY = Join-Path $workspace 'target\release\wechat-relay.exe'
    cargo test --locked -p wechat-p2p --test p2p_e2e -- --ignored --test-threads=1
    if ($LASTEXITCODE) { throw 'P2P direct/relay application integration failed' }
} finally {
    $env:WECHAT_RELAY_BINARY = $previousRelayBinary
}
$releaseRuntime = Join-Path $workspace 'target\release'
Copy-Item -LiteralPath $FFmpeg -Destination (Join-Path $releaseRuntime 'ffmpeg.exe') -Force
Copy-Item -LiteralPath $actionsBinary -Destination (Join-Path $releaseRuntime 'WeChatActions.dll') -Force
Copy-Item -LiteralPath $quoteBinary -Destination (Join-Path $releaseRuntime 'WeChatQuote.dll') -Force

& $CMake -S . -B build -G $Generator -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot"
if ($LASTEXITCODE) { throw 'Client configure failed' }
& $CMake --build build --config Release --parallel 4
if ($LASTEXITCODE) { throw 'Client build failed' }
& $CTest --test-dir build -C Release --output-on-failure --timeout 180
if ($LASTEXITCODE) { throw 'Client tests failed' }

& (Join-Path $PSScriptRoot 'package-windows.ps1') -QtRoot $QtRoot -FFmpeg $FFmpeg
if ($LASTEXITCODE) { throw 'Split deployment failed' }
& (Join-Path $PSScriptRoot 'package-portable-windows.ps1')
if ($LASTEXITCODE) { throw 'Single-file portable packaging failed' }
