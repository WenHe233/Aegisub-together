$ErrorActionPreference = 'Stop'

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildDir = Join-Path $projectRoot 'build-codex'

$buildSucceeded = $false
$wrapOverrides = & (Join-Path $PSScriptRoot 'subproject-overrides.ps1')
$wrapBackupDir = Join-Path ([System.IO.Path]::GetTempPath()) ("aegisub-wrap-{0}" -f [guid]::NewGuid())
$savedWraps = @()
$locationPushed = $false

# A source tree is only usable if it can actually be enumerated. The broken ones are not
# missing, they are present and unreadable, so Test-Path alone says nothing.
function Test-UsableSubproject([string] $path) {
	if (-not (Test-Path -LiteralPath $path)) { return $false }
	try { return $null -ne (Get-ChildItem -LiteralPath $path -Force -ErrorAction Stop | Select-Object -First 1) }
	catch { return $false }
}

try {
	[System.IO.Directory]::CreateDirectory($wrapBackupDir) | Out-Null
	$utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
	$overridden = @()
	foreach ($entry in $wrapOverrides.GetEnumerator()) {
		$wrapPath = Join-Path $projectRoot (Join-Path 'subprojects' $entry.Key)
		$localDependency = Join-Path $projectRoot (Join-Path 'subprojects' $entry.Value)
		if (-not (Test-Path -LiteralPath $wrapPath) -or
			-not (Test-UsableSubproject $localDependency)) { continue }

		$content = [System.IO.File]::ReadAllText($wrapPath)
		$stockMatch = [System.Text.RegularExpressions.Regex]::Match(
			$content, '(?m)^directory\s*=\s*(.+?)\s*$')
		# Leave the wrap alone when the directory it names is readable. The rewriting only
		# exists to route around the locked-out source trees, so it disappears by itself
		# once fix-subproject-permissions.ps1 has put the replacements under those names.
		if ($stockMatch.Success -and
			(Test-UsableSubproject (Join-Path $projectRoot (Join-Path 'subprojects' $stockMatch.Groups[1].Value)))) {
			continue
		}

		$backupPath = Join-Path $wrapBackupDir $entry.Key
		Copy-Item -LiteralPath $wrapPath -Destination $backupPath
		$savedWraps += [pscustomobject]@{ Source = $backupPath; Target = $wrapPath }
		$content = [System.Text.RegularExpressions.Regex]::Replace(
			$content, '(?m)^directory\s*=.*$', "directory = $($entry.Value)")
		[System.IO.File]::WriteAllText($wrapPath, $content, $utf8WithoutBom)
		$overridden += $entry.Key
	}

	if ($overridden.Count) {
		Write-Output ("Redirected {0} wrap(s) around unreadable source trees: {1}" -f
			$overridden.Count, ($overridden -join ', '))
		Write-Output 'Run tools\fix-subproject-permissions.ps1 from an elevated shell to retire this.'
	}

	Push-Location $projectRoot
	$locationPushed = $true

    # ffmpeg and dav1d reach the bundled assembler through the override_find_program in
    # meson.build, but libass picks its assembler up with add_languages('nasm'), and that
    # path only consults the machine file or the NASM variable. With neither set, libass
    # configured itself with "ASM optimizations: NO" while nasm sat unused in subprojects,
    # so its blur and rasterizer kernels were built as plain C.
    $nasmExe = $null
    foreach ($attempt in 1, 2) {
        $nasmExe = Get-ChildItem -Path (Join-Path $projectRoot 'subprojects') -Directory -Filter 'nasm-*' -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'nasm.exe' } |
            Where-Object { Test-Path -LiteralPath $_ } |
            Select-Object -First 1
        if ($nasmExe -or $attempt -eq 2) { break }
        & meson subprojects download nasm
    }
    if ($nasmExe) {
        $env:NASM = $nasmExe
        Write-Output "Assembler for libass: $nasmExe"
    }
    else {
        Write-Warning 'No bundled nasm found; libass will build without its SIMD kernels.'
    }

    # Both of these can only enter the configuration while meson is configuring: the
    # assembler because add_languages runs there, and the wrap redirection because it
    # decides where each subproject's sources live. A tree configured under different
    # answers keeps building the old way until it is reconfigured, so record them.
    $configStamp = Join-Path $buildDir '.aegisub-buildconfig'
    $configCurrent = "nasm=$(if ($nasmExe) { $nasmExe } else { 'none' })`nwraps=$($overridden -join ',')"
    $configRecorded = if (Test-Path -LiteralPath $configStamp) {
        [System.IO.File]::ReadAllText($configStamp).Trim()
    } else { '' }

    if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'meson-private\coredata.dat'))) {
        & meson setup $buildDir -Ddefault_library=static -Dtests=false -Dfreetype2:zlib=internal
        if ($LASTEXITCODE -ne 0) { throw "Meson setup failed with exit code $LASTEXITCODE" }
    }
    elseif ($configRecorded -ne $configCurrent.Trim()) {
        Write-Output 'Build configuration changed; reconfiguring...'
        & meson setup --reconfigure $buildDir
        if ($LASTEXITCODE -ne 0) { throw "Meson reconfigure failed with exit code $LASTEXITCODE" }
    }
    [System.IO.File]::WriteAllText($configStamp, $configCurrent, $utf8WithoutBom)

    & meson compile -C $buildDir
    if ($LASTEXITCODE -ne 0) { throw "Aegisub build failed with exit code $LASTEXITCODE" }
}
finally {
	if ($locationPushed) { Pop-Location }
	foreach ($saved in $savedWraps) {
		Copy-Item -LiteralPath $saved.Source -Destination $saved.Target -Force
	}
	if (Test-Path -LiteralPath $wrapBackupDir) {
		Remove-Item -LiteralPath $wrapBackupDir -Recurse -Force
	}
}

$freshExe = Join-Path $buildDir 'aegisub.exe'
$freshMo = Join-Path $buildDir 'po\hu\LC_MESSAGES\aegisub.mo'
if (-not (Test-Path -LiteralPath $freshExe)) { throw "Built executable is missing: $freshExe" }
if (-not (Test-Path -LiteralPath $freshMo)) { throw "Built Hungarian translation is missing: $freshMo" }

$targetExe = Join-Path $projectRoot 'build\aegisub.exe'
$targetExeDir = Split-Path -Parent $targetExe
$targetMoDir = Join-Path $projectRoot 'build\po\hu\LC_MESSAGES'
$runtimeMoDirs = @(
    (Join-Path $projectRoot 'build\locale\hu\LC_MESSAGES'),
    (Join-Path $projectRoot 'build-codex\locale\hu\LC_MESSAGES')
)
[System.IO.Directory]::CreateDirectory($targetExeDir) | Out-Null
[System.IO.Directory]::CreateDirectory($targetMoDir) | Out-Null
foreach ($runtimeMoDir in $runtimeMoDirs) {
    [System.IO.Directory]::CreateDirectory($runtimeMoDir) | Out-Null
}

$stagedExe = Join-Path $targetExeDir 'aegisub.new.exe'
Copy-Item -LiteralPath $freshExe -Destination $stagedExe -Force
try {
    Copy-Item -LiteralPath $stagedExe -Destination $targetExe -Force
}
catch [System.IO.IOException] {
    $runningExe = Join-Path $targetExeDir ("aegisub.running-{0}.exe" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $oldExeMoved = $false
    try {
        Move-Item -LiteralPath $targetExe -Destination $runningExe
        $oldExeMoved = $true
        Move-Item -LiteralPath $stagedExe -Destination $targetExe
        Write-Output "Previous running executable preserved as: $runningExe"
    }
    catch {
        if ($oldExeMoved -and -not (Test-Path -LiteralPath $targetExe)) {
            Move-Item -LiteralPath $runningExe -Destination $targetExe
        }
        throw
    }
}
finally {
    if (Test-Path -LiteralPath $stagedExe) {
        Remove-Item -LiteralPath $stagedExe
    }
}
Copy-Item -LiteralPath $freshMo -Destination (Join-Path $targetMoDir 'aegisub.mo') -Force
foreach ($runtimeMoDir in $runtimeMoDirs) {
    Copy-Item -LiteralPath $freshMo -Destination (Join-Path $runtimeMoDir 'aegisub.mo') -Force
}

$freshHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $freshExe).Hash
$targetHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $targetExe).Hash
if ($freshHash -ne $targetHash) { throw 'The copied Aegisub executable failed SHA-256 verification.' }
$freshMoHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $freshMo).Hash
foreach ($deployedMo in @((Join-Path $targetMoDir 'aegisub.mo')) +
        ($runtimeMoDirs | ForEach-Object { Join-Path $_ 'aegisub.mo' })) {
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $deployedMo).Hash -ne $freshMoHash) {
        throw "The deployed Hungarian translation failed SHA-256 verification: $deployedMo"
    }
}

$builtFile = Get-Item -LiteralPath $targetExe
Write-Output "Build succeeded: $($builtFile.FullName)"
Write-Output "Size: $($builtFile.Length) bytes"
Write-Output "SHA256: $targetHash"
$buildSucceeded = $true

if (-not $buildSucceeded) { exit 1 }
