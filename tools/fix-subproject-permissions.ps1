#Requires -RunAsAdministrator
<#
.SYNOPSIS
Gives your account back access to subproject source trees that lock it out.

.DESCRIPTION
Extracted subprojects occasionally end up with inheritance switched off and a DACL that
lists BUILTIN\Administrators but not your own account. An elevated shell reads them without
complaint while meson, running unelevated, cannot get in at all -- so a wrap resolves to a
directory that is present but unusable, and the build fails or has to be routed around.

This turns inheritance back on and grants the account explicit access. Every wrap under
subprojects/ is checked; the wrap files themselves are never touched, and nothing is moved
or deleted.

Elevation is required: an ACL you are neither on nor own cannot be rewritten without it.

.PARAMETER Account
Who to grant access to. Defaults to whoever runs the script, which is the right answer when
you elevate your own account rather than switching to a separate administrator.
#>
[CmdletBinding()]
param(
    [string] $Account = [Security.Principal.WindowsIdentity]::GetCurrent().Name
)

$ErrorActionPreference = 'Stop'

$subprojects = Join-Path ([System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))) 'subprojects'

# Whether the ACL hands this account what it needs. Reading the directory is the wrong
# question: run elevated, the Administrators entry answers it and everything looks healthy
# while the unelevated build still cannot get in.
function Test-AccountHasAccess([string] $path, [string] $account) {
    try { $acl = Get-Acl -LiteralPath $path -ErrorAction Stop }
    catch { return $false }
    foreach ($ace in $acl.Access) {
        if ($ace.AccessControlType -ne [Security.AccessControl.AccessControlType]::Allow) { continue }
        if ($ace.IdentityReference.Value -ne $account) { continue }
        if ($ace.FileSystemRights -band [Security.AccessControl.FileSystemRights]::ReadAndExecute) { return $true }
    }
    return $false
}

Write-Output "Granting access to: $Account"
Write-Output ''

$repaired = @()
$failed = @()

foreach ($wrap in Get-ChildItem -LiteralPath $subprojects -Filter '*.wrap' -File) {
    $match = [System.Text.RegularExpressions.Regex]::Match(
        [System.IO.File]::ReadAllText($wrap.FullName), '(?m)^directory\s*=\s*(.+?)\s*$')
    if (-not $match.Success) { continue }

    $name = $match.Groups[1].Value
    $path = Join-Path $subprojects $name
    if (-not (Test-Path -LiteralPath $path)) { continue }
    if (Test-AccountHasAccess $path $Account) { continue }

    Write-Output "Repairing $name ..."
    # Ownership first, so the ACL becomes writable at all.
    & takeown.exe /F $path | Out-Null
    # Inheritance pulls in the Full Control the parent already grants; the explicit
    # recursive grant then covers children that broke inheritance on their own.
    & icacls.exe $path /inheritance:e /C /Q | Out-Null
    & icacls.exe $path /grant "$($Account):(OI)(CI)F" /T /C /Q | Out-Null

    if (Test-AccountHasAccess $path $Account) { $repaired += $name } else { $failed += $name }
}

Write-Output ''
if ($repaired.Count) {
    Write-Output "Access restored on $($repaired.Count):"
    $repaired | ForEach-Object { Write-Output "  $_" }
}
if ($failed.Count) {
    Write-Output "Could not repair $($failed.Count):"
    $failed | ForEach-Object { Write-Output "  $_" }
}
if (-not $repaired.Count -and -not $failed.Count) {
    Write-Output 'Every subproject already grants access. Nothing to do.'
}
