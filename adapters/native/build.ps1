param(
    [string]$BuildDirectory = "$PSScriptRoot\..\..\build\native",
    [string]$CMake = "$PSScriptRoot\..\..\.tools\Scripts\cmake.exe",
    [string]$CTest = "$PSScriptRoot\..\..\.tools\Scripts\ctest.exe",
    [string]$Generator = 'Visual Studio 18 2026'
)
$ErrorActionPreference = 'Stop'
& $CMake -S $PSScriptRoot -B $BuildDirectory -G $Generator -A x64
if ($LASTEXITCODE) { throw 'Actions configure failed' }
& $CMake --build $BuildDirectory --config Release --parallel 2
if ($LASTEXITCODE) { throw 'Actions build failed' }
& $CTest --test-dir $BuildDirectory -C Release --output-on-failure --timeout 180
if ($LASTEXITCODE) { throw 'Actions offline self-test failed' }
Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'Release\WeChatActions.dll') -Algorithm SHA256
Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'Release\WeChatQuote.dll') -Algorithm SHA256
Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'Release\WeChatMedia.dll') -Algorithm SHA256
