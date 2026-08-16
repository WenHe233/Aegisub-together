$ErrorActionPreference = 'Stop'

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildDir = Join-Path $projectRoot 'build-codex'

$buildSucceeded = $false
$locationPushed = $false

try {
	$utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)

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

    # The assembler can only enter the configuration while meson is configuring, since that
    # is when add_languages runs. A tree set up before NASM was visible therefore keeps
    # building the scalar libass until it is reconfigured, so record what it was set up with.
    $configStamp = Join-Path $buildDir '.aegisub-buildconfig'
    $configCurrent = "nasm=$(if ($nasmExe) { $nasmExe } else { 'none' })"
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
