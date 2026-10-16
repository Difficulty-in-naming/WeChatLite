function Confirm-PortableWorkspacePath([string]$Workspace, [string]$Candidate) {
    $absolute = [IO.Path]::GetFullPath($Candidate)
    $prefix = $Workspace.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Portable build path leaves workspace: $absolute"
    }
    $ancestor = $absolute
    while ($ancestor -and -not $ancestor.Equals($Workspace, [StringComparison]::OrdinalIgnoreCase)) {
        if (Test-Path -LiteralPath $ancestor) {
            if ((Get-Item -LiteralPath $ancestor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "Portable build cannot traverse a link or junction: $ancestor"
            }
        }
        $ancestor = [IO.Path]::GetDirectoryName($ancestor)
    }
    return $absolute
}

function Add-PortablePayload([string]$Stub, [string]$Payload, [string]$Output) {
    $destination = [IO.File]::Open($Output, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $source = [IO.File]::OpenRead($Stub)
        try { $source.CopyTo($destination, 65536) } finally { $source.Dispose() }
        $offset = [UInt64]$destination.Position
        $source = [IO.File]::OpenRead($Payload)
        try {
            $length = [UInt64]$source.Length
            $source.CopyTo($destination, 65536)
        } finally { $source.Dispose() }
        $writer = [IO.BinaryWriter]::new($destination, [Text.UTF8Encoding]::new($false), $true)
        try {
            $writer.Write([Text.Encoding]::ASCII.GetBytes("WLPEND1`n"))
            $writer.Write($offset)
            $writer.Write($length)
            $writer.Flush()
        } finally { $writer.Dispose() }
        $destination.Flush($true)
    } finally { $destination.Dispose() }
}

function Assert-PortablePayload([string]$Tool, [string]$Executable, [string]$Hash) {
    $result = & $Tool verify $Executable $Hash
    if ($LASTEXITCODE -ne 0) { throw 'Portable EXE payload verification failed' }
    $report = $result | ConvertFrom-Json
    if (-not $report.verified -or -not $report.gui_subsystem -or -not $report.system_only_imports -or $report.sha256 -ne $Hash) {
        throw 'Portable EXE returned an invalid verification report'
    }
}
