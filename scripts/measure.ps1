param(
    [Parameter(Mandatory = $true)][int]$ClientProcessId,
    [Parameter(Mandatory = $true)][int]$ServerProcessId,
    [ValidateRange(5, 300)][int]$Seconds = 20,
    [string]$Output = 'artifacts/performance.json'
)
$ErrorActionPreference = 'Stop'

# Observe existing app processes. This script never starts, stops or sends messages.
$roots = @{ client = $ClientProcessId; server = $ServerProcessId }
foreach ($role in $roots.Keys) {
    $expected = if ($role -eq 'client') { 'wechat-lite' } else { 'wechat-server' }
    if ((Get-Process -Id $roots[$role]).ProcessName -ne $expected) { throw "Unexpected $role process" }
}
$parents = @{}
foreach ($entry in Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId) {
    $parents[[int]$entry.ProcessId] = [int]$entry.ParentProcessId
}
$members = @{}
foreach ($role in $roots.Keys) {
    $ids = [Collections.Generic.HashSet[int]]::new()
    [void]$ids.Add($roots[$role])
    do {
        $changed = $false
        foreach ($processId in @($parents.Keys)) {
            if ($ids.Contains($parents[$processId]) -and $ids.Add($processId)) { $changed = $true }
        }
    } while ($changed)
    $members[$role] = @($ids)
}
function Read-Sample([string]$Role) {
    [double]$cpu = 0
    [double]$rss = 0
    [double]$privateBytes = 0
    $processNames = @()
    foreach ($processId in $members[$Role]) {
        $process = Get-Process -Id $processId -ErrorAction SilentlyContinue
        if (-not $process) { throw "A measured $Role process exited; repeat after it is stable" }
        $cpu += $process.TotalProcessorTime.TotalSeconds
        $rss += $process.WorkingSet64
        $privateBytes += $process.PrivateMemorySize64
        $processNames += $process.ProcessName
    }
    return [pscustomobject]@{ cpu = $cpu; rss = $rss; private = $privateBytes; processes = @($processNames) }
}
$start = @{}
$samples = @{ client = @(); server = @() }
foreach ($role in $roots.Keys) { $start[$role] = Read-Sample $role }
$timer = [Diagnostics.Stopwatch]::StartNew()
for ($index = 0; $index -lt $Seconds; $index++) {
    Start-Sleep -Seconds 1
    foreach ($role in $roots.Keys) { $samples[$role] += Read-Sample $role }
}
$elapsed = $timer.Elapsed.TotalSeconds
$logicalProcessors = [Environment]::ProcessorCount
$summary = @{}
foreach ($role in $roots.Keys) {
    $last = $samples[$role][-1]
    $cpuPercent = ($last.cpu - $start[$role].cpu) / $elapsed * 100
    $summary[$role] = [ordered]@{
        processes = $last.processes
        average_rss_mib = [Math]::Round(($samples[$role] | Measure-Object rss -Average).Average / 1MB, 2)
        average_private_mib = [Math]::Round(($samples[$role] | Measure-Object private -Average).Average / 1MB, 2)
        cpu_one_core_percent = [Math]::Round($cpuPercent, 3)
        cpu_machine_percent = [Math]::Round($cpuPercent / $logicalProcessors, 3)
    }
}
$report = [ordered]@{
    scenario = 'Existing client/server process trees; no synthetic load or message submission'
    elapsed_seconds = [Math]::Round($elapsed, 2)
    logical_processors = $logicalProcessors
    summary = $summary
}
$outputPath = [IO.Path]::GetFullPath($Output)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outputPath)) | Out-Null
$json = $report | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText($outputPath, $json, [Text.UTF8Encoding]::new($false))
$json
