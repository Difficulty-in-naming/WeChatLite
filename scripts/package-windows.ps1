param(
    [string]$QtRoot = "$PSScriptRoot\..\.qt\6.8.3\msvc2022_64",
    [string]$FFmpeg = "$PSScriptRoot\..\.tools\Lib\site-packages\imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v7.1.exe"
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Set-Location -LiteralPath $workspace
$QtRoot = [IO.Path]::GetFullPath($QtRoot)
$FFmpeg = (Resolve-Path -LiteralPath $FFmpeg).ProviderPath

function Confirm-WorkspacePath([string]$Candidate) {
    $absolute = [IO.Path]::GetFullPath($Candidate)
    $prefix = $workspace.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw "Path leaves workspace: $absolute" }
    $ancestor = $absolute
    while ($ancestor -and -not $ancestor.Equals($workspace, [StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $ancestor) {
            if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Packaging cannot traverse a link or junction: $ancestor"
            }
        }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    return $absolute
}

$distribution = Confirm-WorkspacePath (Join-Path $workspace 'dist')
New-Item -ItemType Directory -Force -Path $distribution | Out-Null

$stamp = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$staging = Confirm-WorkspacePath (Join-Path $workspace "build\package-staging\$stamp")
$client = Join-Path $staging 'client'
$server = Join-Path $staging 'server'
New-Item -ItemType Directory -Force -Path $client, $server | Out-Null
Copy-Item -LiteralPath 'build\client\Release\wechat-lite.exe' -Destination $client
Copy-Item -LiteralPath 'target\release\wechat-link.exe' -Destination $client
Copy-Item -LiteralPath 'build\server_gui\Release\wechat-server.exe' -Destination $server
Copy-Item -LiteralPath 'target\release\wechat-relay.exe' -Destination $server
$adapter = Join-Path $server 'adapter'
New-Item -ItemType Directory -Path $adapter | Out-Null
Copy-Item -LiteralPath 'target\release\wechat-adapter.exe' -Destination $adapter
Copy-Item -LiteralPath 'adapters\windows-rs\vendor\WeChatActions.dll' -Destination $adapter
Copy-Item -LiteralPath 'adapters\windows-rs\vendor\WeChatQuote.dll' -Destination $adapter
Copy-Item -LiteralPath $FFmpeg -Destination (Join-Path $adapter 'ffmpeg.exe')
& "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-d3d-compiler (Join-Path $client 'wechat-lite.exe')
if ($LASTEXITCODE) { throw 'Client Qt deployment failed' }
& "$QtRoot\bin\windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-d3d-compiler (Join-Path $server 'wechat-server.exe')
if ($LASTEXITCODE) { throw 'Server GUI Qt deployment failed' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
foreach ($package in @($client, $server)) {
    if (Test-Path -LiteralPath $vswhere) {
        $visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        $versions = Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC\Redist\MSVC') -Directory | Where-Object Name -Match '^\d+\.' | Sort-Object Name -Descending
        foreach ($version in $versions | Select-Object -First 1) {
            foreach ($runtime in Get-ChildItem -LiteralPath (Join-Path $version.FullName 'x64') -Directory | Where-Object Name -Match '^Microsoft\.VC\d+\.CRT$') {
                foreach ($dll in Get-ChildItem -LiteralPath $runtime.FullName -Filter '*.dll') {
                    Copy-Item -LiteralPath $dll.FullName -Destination $package
                }
            }
        }
    }
    Set-Content -LiteralPath (Join-Path $package 'qt.conf') -Value "[Paths]`nPlugins=." -Encoding utf8
    Copy-Item -LiteralPath 'THIRD_PARTY_NOTICES.md' -Destination $package
    $p2pLicenses = Join-Path $package 'licenses'
    New-Item -ItemType Directory -Force -Path $p2pLicenses | Out-Null
    foreach ($notice in Get-ChildItem -LiteralPath 'network\licenses' -Filter '*.txt' -File) {
        Copy-Item -LiteralPath $notice.FullName -Destination $p2pLicenses
    }
}
Copy-Item -LiteralPath 'docs\CLIENT.md' -Destination (Join-Path $client '使用说明.md')
Copy-Item -LiteralPath 'docs\SERVER.md' -Destination (Join-Path $server '使用说明.md')
$licenses = Join-Path $server 'licenses'
New-Item -ItemType Directory -Force -Path $licenses | Out-Null
Copy-Item -LiteralPath 'adapters\native\THIRD_PARTY_NOTICES.md' -Destination (Join-Path $licenses 'native-actions.md')
Copy-Item -LiteralPath 'adapters\native\third_party\minhook\LICENSE.txt' -Destination (Join-Path $licenses 'MinHook.txt')
Copy-Item -LiteralPath 'adapters\windows\vendor\WechatAuto-LICENSE.txt' -Destination (Join-Path $licenses 'WechatAuto-Apache-2.0.txt')
Copy-Item -LiteralPath 'adapters\windows-rs\vendor\pilk-0.2.4\LICENSE' -Destination (Join-Path $licenses 'GPL-3.0.txt')
Copy-Item -LiteralPath 'adapters\windows-rs\vendor\pilk-0.2.4\src\interface\SKP_Silk_SDK_API.h' -Destination (Join-Path $licenses 'SILK-SDK-source-notice.h')
$ffmpegVersion = & $FFmpeg -version 2>&1
if ($LASTEXITCODE) { throw 'Standalone FFmpeg version check failed' }
$ffmpegLicense = & $FFmpeg -hide_banner -L 2>&1
if ($LASTEXITCODE) { throw 'Standalone FFmpeg license check failed' }
[IO.File]::WriteAllLines((Join-Path $licenses 'ffmpeg-build.txt'), [string[]]@($ffmpegVersion + $ffmpegLicense), [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath 'thirdparty-sources\FFmpeg-COPYING.GPLv3' -Destination $licenses
$sourceDestination = Join-Path $server 'thirdparty-sources'
New-Item -ItemType Directory -Force -Path $sourceDestination | Out-Null
foreach ($source in Get-ChildItem -LiteralPath 'thirdparty-sources' -File) {
    Copy-Item -LiteralPath $source.FullName -Destination $sourceDestination
}
if (Test-Path -LiteralPath (Join-Path $client 'wechat-relay.exe')) { throw 'Client contains a relay process' }
if (Test-Path -LiteralPath (Join-Path $client 'adapter')) { throw 'Client contains a WeChat adapter' }
if (@(Get-ChildItem -LiteralPath $adapter -Force).Count -ne 4) { throw 'Server adapter runtime must have exactly four files' }
$qtFolders = @('generic', 'imageformats', 'networkinformation', 'platforms', 'styles', 'tls')
$runtimeFiles = @('qt.conf', 'THIRD_PARTY_NOTICES.md', '使用说明.md',
    'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Network.dll', 'Qt6WebSockets.dll', 'Qt6Widgets.dll',
    'concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vccorlib140.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll')
foreach ($role in @('client', 'server')) {
    $package = Join-Path $staging $role
    $folders = $qtFolders + @('licenses') + $(if ($role -eq 'server') { @('adapter', 'thirdparty-sources') })
    $files = $runtimeFiles + $(if ($role -eq 'client') { @('wechat-lite.exe', 'wechat-link.exe') } else { @('wechat-server.exe', 'wechat-relay.exe') })
    foreach ($entry in Get-ChildItem -LiteralPath $package -Force) {
        if (($entry.PSIsContainer -and $entry.Name -notin $folders) -or
            (-not $entry.PSIsContainer -and $entry.Name -notin $files)) {
            throw "Unexpected release entry: $role/$($entry.Name)"
        }
    }
    foreach ($file in Get-ChildItem -LiteralPath $package -File -Recurse -Force) {
        $relative = $file.FullName.Substring($package.Length + 1)
        $topFolder = $relative.Split([IO.Path]::DirectorySeparatorChar)[0]
        if ($topFolder -in $qtFolders -and $file.Extension -ne '.dll') {
            throw "Unexpected Qt runtime file: $relative"
        }
        if ($file.Extension -in @('.pdb', '.obj', '.lib', '.exp', '.ilk', '.log', '.tmp', '.pyc') -or
            $file.Name -match '(?i)(-tests?\.exe$|^Qt6Test\.dll$)') {
            throw "Development file cannot enter a runtime package: $($file.Name)"
        }
    }
}
foreach ($role in @('client', 'server')) {
    $destination = Confirm-WorkspacePath (Join-Path $workspace "dist\$role-windows-x64")
    if (Test-Path -LiteralPath $destination) {
        $previous = Confirm-WorkspacePath (Join-Path $workspace "build\package-history\$stamp-$role")
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $previous) | Out-Null
        $verifiedSource = Confirm-WorkspacePath $destination
        Move-Item -LiteralPath $verifiedSource -Destination $previous
    }
    $verifiedStage = Confirm-WorkspacePath (Join-Path $staging $role)
    Move-Item -LiteralPath $verifiedStage -Destination $destination
    Write-Output "$role package: $destination"
}

# Archive only the checked runtime directories; validation output stays in the workspace.
Add-Type -AssemblyName System.IO.Compression.FileSystem
foreach ($role in @('Client', 'Server')) {
    $package = Confirm-WorkspacePath (Join-Path $distribution "$($role.ToLowerInvariant())-windows-x64")
    $archiveName = "WeChatLite-$role-Windows-x64.zip"
    $temporaryArchive = Confirm-WorkspacePath (Join-Path $staging $archiveName)
    $archive = [IO.Compression.ZipFile]::Open($temporaryArchive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName) {
            $relative = $file.FullName.Substring($package.Length + 1).Replace('\', '/')
            [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName,
                "WeChatLite-$role/$relative", [IO.Compression.CompressionLevel]::Optimal) | Out-Null
        }
    } finally {
        $archive.Dispose()
    }
    $archivePath = Confirm-WorkspacePath (Join-Path $distribution $archiveName)
    Move-Item -LiteralPath $temporaryArchive -Destination $archivePath -Force
    Write-Output "Runtime archive: $archivePath"
}
