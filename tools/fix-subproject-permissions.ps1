#Requires -RunAsAdministrator
<#
.SYNOPSIS
Puts the replacement subproject sources under the names the wraps already use.

.DESCRIPTION
Several extracted subprojects carry an ACL that shuts this account out completely: they
cannot be read, renamed or deleted, only seen in a directory listing. Working copies were
made beside them under suffixed names, and build-aegisub.ps1 rewrites the wraps to point
at those for the duration of a build.

This script retires that workaround. For each entry in subproject-overrides.ps1 it takes
ownership of the unreadable directory, moves it aside, and renames the working copy into
its place. Afterwards the wraps resolve on their own and the build stops touching them.

Ownership has to be taken before the move, which is why this needs an elevated shell.

.PARAMETER RemoveLocked
Also delete the directories that were moved aside instead of leaving them in place. This
is the only irreversible part, so it is off by default.
#>
[CmdletBinding()]
param(
    [switch] $RemoveLocked
)

$ErrorActionPreference = 'Stop'

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$subprojects = Join-Path $projectRoot 'subprojects'
$overrides = & (Join-Path $PSScriptRoot 'subproject-overrides.ps1')
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$account = [Security.Principal.WindowsIdentity]::GetCurrent().Name

# Present but unreadable is the case this whole script is about, so presence proves nothing.
function Test-UsableSubproject([string] $path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    try { return $null -ne (Get-ChildItem -LiteralPath $path -Force -ErrorAction Stop | Select-Object -First 1) }
    catch { return $false }
}

$repaired = @()
$skipped = @()

foreach ($entry in $overrides.GetEnumerator()) {
    $wrapPath = Join-Path $subprojects $entry.Key
    if (-not (Test-Path -LiteralPath $wrapPath)) {
        $skipped += "$($entry.Key): no such wrap"
        continue
    }

    $stockMatch = [System.Text.RegularExpressions.Regex]::Match(
        [System.IO.File]::ReadAllText($wrapPath), '(?m)^directory\s*=\s*(.+?)\s*$')
    if (-not $stockMatch.Success) {
        $skipped += "$($entry.Key): no directory field"
        continue
    }

    $stockName = $stockMatch.Groups[1].Value
    $stockPath = Join-Path $subprojects $stockName
    $replacementPath = Join-Path $subprojects $entry.Value

    if (Test-UsableSubproject $stockPath) {
        $skipped += "$stockName is already readable"
        continue
    }
    if (-not (Test-UsableSubproject $replacementPath)) {
        Write-Warning "$($entry.Value) is missing or unreadable; leaving $stockName alone."
        continue
    }

    if (Test-Path -LiteralPath $stockPath) {
        # Renaming a directory within its parent only touches its own entry, so ownership
        # of the top level is enough and the contents can keep their ACLs. Recursing here
        # would mean walking every file of boost and icu for no gain.
        # Ownership goes to whoever runs this, not to the Administrators group: /A would
        # leave directories that an unelevated shell still cannot clean up later.
        & takeown.exe /F $stockPath | Out-Null
        & icacls.exe $stockPath /grant "$($account):(OI)(CI)F" /C /Q | Out-Null

        $lockedPath = Join-Path $subprojects "$stockName.locked-$stamp"
        Move-Item -LiteralPath $stockPath -Destination $lockedPath
        Write-Output "Moved aside: $stockName -> $stockName.locked-$stamp"

        if ($RemoveLocked) {
            # Deleting does need every child, hence the recursive pass this time.
            & takeown.exe /F $lockedPath /R /D Y | Out-Null
            & icacls.exe $lockedPath /grant "$($account):(OI)(CI)F" /T /C /Q | Out-Null
            Remove-Item -LiteralPath $lockedPath -Recurse -Force
            Write-Output "Deleted: $stockName.locked-$stamp"
        }
    }

    Move-Item -LiteralPath $replacementPath -Destination $stockPath
    $repaired += "$($entry.Value) -> $stockName"
}

Write-Output ''
if ($repaired.Count) {
    Write-Output "Repaired $($repaired.Count) subproject(s):"
    $repaired | ForEach-Object { Write-Output "  $_" }
    Write-Output ''
    Write-Output 'The next build will reconfigure itself and will no longer rewrite the wraps.'
}
else {
    Write-Output 'Nothing to repair.'
}
if ($skipped.Count) {
    Write-Output ''
    Write-Output 'Skipped:'
    $skipped | ForEach-Object { Write-Output "  $_" }
}
