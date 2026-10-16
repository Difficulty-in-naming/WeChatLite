param(
    [string]$BuildDirectory = "$PSScriptRoot\..\..\build\native",
    [string]$CMake = "$PSScriptRoot\..\..\.tools\Scripts\cmake.exe",
    [string]$CTest = "$PSScriptRoot\..\..\.tools\Scripts\ctest.exe"
)
$ErrorActionPreference = 'Stop'
& $CMake -S $PSScriptRoot -B $BuildDirectory -G 'Visual Studio 18 2026' -A x64
if ($LASTEXITCODE) { throw 'Actions configure failed' }
& $CMake --build $BuildDirectory --config Release --parallel 2
if ($LASTEXITCODE) { throw 'Actions build failed' }
& $CTest --test-dir $BuildDirectory -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Actions offline self-test failed' }
Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'Release\WeChatActions.dll') -Algorithm SHA256
Get-FileHash -LiteralPath (Join-Path $BuildDirectory 'Release\WeChatQuote.dll') -Algorithm SHA256
