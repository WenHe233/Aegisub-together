$ErrorActionPreference = 'Stop'

$projectRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildDir = Join-Path $projectRoot 'build-codex'
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

$wrapMappings = @(
    @{ File = 'boost.wrap'; Original = 'boost_1_83_0'; Local = 'boost_1_83_0-aegisub' },
    @{ File = 'zlib.wrap'; Original = 'zlib-1.3.1'; Local = 'zlib-1.3.1-aegisub' },
    @{ File = 'curl.wrap'; Original = 'curl-8.12.1'; Local = 'curl-8.12.1-aegisub' },
    @{ File = 'hunspell.wrap'; Original = 'hunspell-1.7.2'; Local = 'hunspell-1.7.2-aegisub' },
    @{ File = 'icu.wrap'; Original = 'icu'; Local = 'icu-aegisub' },
    @{ File = 'uchardet.wrap'; Original = 'uchardet-0.0.8'; Local = 'uchardet-0.0.8-aegisub' },
    @{ File = 'luajit.wrap'; Original = 'LuaJIT-04dca7911ea255f37be799c18d74c305b921c1a6'; Local = 'luajit-2.1.1720049189-aegisub' }
)

$originalWraps = @{}
$buildSucceeded = $false

try {
    foreach ($mapping in $wrapMappings) {
        $wrapPath = Join-Path $projectRoot (Join-Path 'subprojects' $mapping.File)
        $localDependency = Join-Path $projectRoot (Join-Path 'subprojects' $mapping.Local)
        if (-not (Test-Path -LiteralPath (Join-Path $localDependency 'meson.build'))) {
            throw "Missing local Meson dependency: $localDependency"
        }

        $content = [System.IO.File]::ReadAllText($wrapPath)
        $originalWraps[$wrapPath] = $content
        $pattern = '(?m)^directory\s*=\s*' + [regex]::Escape($mapping.Original) + '\s*$'
        $replacement = 'directory = ' + $mapping.Local
        $updated = [regex]::Replace($content, $pattern, $replacement, 1)
        if ($updated -eq $content -and $content -notmatch ('(?m)^directory\s*=\s*' + [regex]::Escape($mapping.Local) + '\s*$')) {
            throw "Could not update dependency directory in $wrapPath"
        }
        [System.IO.File]::WriteAllText($wrapPath, $updated, $utf8NoBom)
    }

    Push-Location $projectRoot
    try {
        if (-not (Test-Path -LiteralPath (Join-Path $buildDir 'meson-private\coredata.dat'))) {
            & meson setup $buildDir -Ddefault_library=static -Dtests=false -Dfreetype2:zlib=internal
            if ($LASTEXITCODE -ne 0) { throw "Meson setup failed with exit code $LASTEXITCODE" }
        }
        else {
            & meson setup $buildDir --reconfigure
            if ($LASTEXITCODE -ne 0) { throw "Meson reconfigure failed with exit code $LASTEXITCODE" }
        }

        & meson compile -C $buildDir
        if ($LASTEXITCODE -ne 0) { throw "Aegisub build failed with exit code $LASTEXITCODE" }
    }
    finally {
        Pop-Location
    }

    $freshExe = Join-Path $buildDir 'aegisub.exe'
    $freshMo = Join-Path $buildDir 'po\hu\LC_MESSAGES\aegisub.mo'
    if (-not (Test-Path -LiteralPath $freshExe)) { throw "Built executable is missing: $freshExe" }
    if (-not (Test-Path -LiteralPath $freshMo)) { throw "Built Hungarian translation is missing: $freshMo" }

    $targetExe = Join-Path $projectRoot 'build\aegisub.exe'
    $targetExeDir = Split-Path -Parent $targetExe
    $targetMoDir = Join-Path $projectRoot 'build\po\hu\LC_MESSAGES'
    [System.IO.Directory]::CreateDirectory($targetExeDir) | Out-Null
    [System.IO.Directory]::CreateDirectory($targetMoDir) | Out-Null

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

    $freshHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $freshExe).Hash
    $targetHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $targetExe).Hash
    if ($freshHash -ne $targetHash) { throw 'The copied Aegisub executable failed SHA-256 verification.' }

    $builtFile = Get-Item -LiteralPath $targetExe
    Write-Output "Build succeeded: $($builtFile.FullName)"
    Write-Output "Size: $($builtFile.Length) bytes"
    Write-Output "SHA256: $targetHash"
    $buildSucceeded = $true
}
finally {
    foreach ($entry in $originalWraps.GetEnumerator()) {
        [System.IO.File]::WriteAllText($entry.Key, $entry.Value, $utf8NoBom)
    }
}

if (-not $buildSucceeded) { exit 1 }
