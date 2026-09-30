# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
$ErrorActionPreference = 'Stop'
$script = Join-Path $PSScriptRoot '../promote_pending_updates.ps1'
$root = Join-Path ([System.IO.Path]::GetTempPath()) ('skyfire-promotion-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path "$root/sql/pending_updates/hub", "$root/sql/updates/hub" | Out-Null
function Assert($condition, $message) { if (-not $condition) { throw $message } }
function Reject($operation, $message) {
    $failed = $false
    try { & $operation } catch { $failed = $true }
    Assert $failed $message
}
try {
    [System.IO.File]::WriteAllText("$root/sql/pending_updates/hub/002_second.sql", "SELECT 2;`r`n")
    [System.IO.File]::WriteAllText("$root/sql/pending_updates/hub/001_first.sql", "SELECT 1;`n")
    [System.IO.File]::WriteAllText("$root/sql/updates/hub/2026_09_30_hub_04.sql", 'SELECT 0;')
    $hash = (Get-FileHash "$root/sql/pending_updates/hub/002_second.sql").Hash
    & $script -Root $root -Database hub -PromotionDate 2026_09_30
    Assert (Test-Path "$root/sql/pending_updates/hub/001_first.sql") 'Dry run moved a file'
    Assert (-not (Test-Path "$root/sql/updates/hub/2026_09_30_hub_05.sql")) 'Dry run created a release'
    & $script -Root $root -Database hub -PromotionDate 2026_09_30 -Apply
    Assert ((Get-Content "$root/sql/updates/hub/2026_09_30_hub_05.sql.pending-name") -eq '001_first.sql') 'Incorrect first identity'
    Assert ((Get-Content "$root/sql/updates/hub/2026_09_30_hub_06.sql.pending-name") -eq '002_second.sql') 'Incorrect second identity'
    Assert ((Get-FileHash "$root/sql/updates/hub/2026_09_30_hub_06.sql").Hash -eq $hash) 'SQL bytes changed'
    Assert (-not (Test-Path "$root/sql/pending_updates/hub/002_second.sql")) 'Source was not moved'
    [System.IO.File]::WriteAllText("$root/sql/pending_updates/hub/001_first.sql", 'SELECT 9;')
    Reject { & $script -Root $root -Database hub -PromotionDate 2026_09_30 -Apply } 'Reused identity accepted'
    Remove-Item -LiteralPath "$root/sql/pending_updates/hub/001_first.sql"
    [System.IO.File]::WriteAllText("$root/sql/pending_updates/hub/003_third.sql", 'SELECT 3;')
    [System.IO.File]::WriteAllText("$root/sql/updates/hub/2026_09_30_hub_07.sql.pending-name", 'orphan.sql')
    Reject { & $script -Root $root -Database hub -PromotionDate 2026_09_30 -Apply } 'Existing metadata overwritten'
    [System.IO.File]::WriteAllText("$root/sql/updates/hub/2026_09_30_hub_99.sql", 'SELECT 99;')
    Reject { & $script -Root $root -Database hub -PromotionDate 2026_09_30 -Apply } 'Sequence overflow accepted'
    Assert (Test-Path "$root/sql/pending_updates/hub/003_third.sql") 'Rejected promotion moved source'
    Write-Host 'SQL promotion tests passed.'
}
finally {
    # Only remove the unique fixture under the resolved system temporary directory.
    $resolved = [System.IO.Path]::GetFullPath($root)
    $temp = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath()).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    if ($resolved.StartsWith($temp, [System.StringComparison]::OrdinalIgnoreCase) -and
        (Split-Path -Leaf $resolved) -like 'skyfire-promotion-*') {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
