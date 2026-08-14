#!/usr/bin/env powershell

param (
  [Parameter(Position = 0)]
  [string]$BuildRoot,
  [Parameter(Position = 1)]
  [string]$SourceRoot
)

$ErrorActionPreference = "Stop"

$InstallerDir = Join-Path $SourceRoot "packages\win_installer" | Resolve-Path
$DepsDir = Join-Path $BuildRoot "installer-deps"
if (!(Test-Path $DepsDir)) {
	New-Item -ItemType Directory -Path $DepsDir
}

$Env:BUILD_ROOT = $BuildRoot
$Env:SOURCE_ROOT = $SourceRoot

Set-Location $DepsDir

$GitHeaders = @{}
if (Test-Path 'Env:GITHUB_TOKEN') {
	$GitHeaders = @{ 'Authorization' = 'Bearer ' + $Env:GITHUB_TOKEN }
}

# DepCtrl
if (!(Test-Path DependencyControl)) {
	git clone https://github.com/TypesettingTools/DependencyControl.git
	if ($LASTEXITCODE -ne 0) { throw "DependencyControl clone failed." }
	Set-Location DependencyControl
	git checkout v0.6.3-alpha
	if ($LASTEXITCODE -ne 0) { throw "DependencyControl checkout failed." }
	Set-Location $DepsDir
}

# YUtils
if (!(Test-Path YUtils)) {
	git clone https://github.com/TypesettingTools/YUtils.git
	if ($LASTEXITCODE -ne 0) { throw "YUtils clone failed." }
}

# luajson
if (!(Test-Path luajson)) {
	git clone https://github.com/harningt/luajson.git
	if ($LASTEXITCODE -ne 0) { throw "luajson clone failed." }
}

# Avisynth
# if (!(Test-Path AviSynthPlus64)) {
# 	$avsReleases = Invoke-WebRequest "https://api.github.com/repos/AviSynth/AviSynthPlus/releases/latest" -Headers $GitHeaders -UseBasicParsing | ConvertFrom-Json
# 	$avsUrl = $avsReleases.assets[0].browser_download_url
# 	Invoke-WebRequest $avsUrl -OutFile AviSynthPlus.7z -UseBasicParsing
# 	7z x AviSynthPlus.7z
# 	Rename-Item (Get-ChildItem -Filter "AviSynthPlus_*" -Directory) AviSynthPlus64
# 	Remove-Item AviSynthPlus.7z
# }

# VSFilter
if (!(Test-Path VSFilter\x64\VSFilter.dll)) {
	Remove-Item VSFilter -Recurse -Force -ErrorAction SilentlyContinue
	$vsFilterDir = New-Item -ItemType Directory VSFilter
	Set-Location $vsFilterDir
	$vsFilterReleases = Invoke-WebRequest "https://api.github.com/repos/pinterf/xy-VSFilter/releases/latest" -Headers $GitHeaders -UseBasicParsing | ConvertFrom-Json
	$vsFilterUrl = $vsFilterReleases.assets[0].browser_download_url
	Invoke-WebRequest $vsFilterUrl -OutFile VSFilter.7z -UseBasicParsing
	7z x VSFilter.7z
	if ($LASTEXITCODE -ne 0) { throw "VSFilter extraction failed." }
	Remove-Item VSFilter.7z
	Set-Location $DepsDir
}

# ffi-experiments
if (!(Test-Path ffi-experiments)) {
	Get-Command "moonc" # check to ensure Moonscript is present
	git clone https://github.com/TypesettingTools/ffi-experiments.git
	if ($LASTEXITCODE -ne 0) { throw "ffi-experiments clone failed." }
	Set-Location ffi-experiments
	meson setup build -Ddefault_library=static
	if ($LASTEXITCODE -ne 0) { throw "ffi-experiments setup failed." }
	Set-Location $DepsDir
}
if (!(Test-Path ffi-experiments\build\requireffi\requireffi.lua)) {
	Get-Command "moonc"
	Set-Location ffi-experiments
	if (!(Test-Path build\meson-private\coredata.dat)) {
		meson setup build -Ddefault_library=static
		if ($LASTEXITCODE -ne 0) { throw "ffi-experiments setup failed." }
	}
	meson compile -C build
	if ($LASTEXITCODE -ne 0) { throw "ffi-experiments build failed." }
	Set-Location $DepsDir
}

# VC++ redistributable
if (!(Test-Path VC_redist)) {
	$redistDir = New-Item -ItemType Directory VC_redist
	Invoke-WebRequest https://aka.ms/vs/17/release/VC_redist.x64.exe -OutFile "$redistDir\VC_redist.x64.exe" -UseBasicParsing
}

# Dictionaries
if (!(Test-Path dictionaries)) {
	New-Item -ItemType Directory dictionaries
	Invoke-WebRequest https://raw.githubusercontent.com/TypesettingTools/Aegisub-dictionaries/master/dicts/en_US.aff -OutFile dictionaries/en_US.aff -UseBasicParsing
	Invoke-WebRequest https://raw.githubusercontent.com/TypesettingTools/Aegisub-dictionaries/master/dicts/en_US.dic -OutFile dictionaries/en_US.dic -UseBasicParsing
}

# Installer localization. Chinese became an official Inno Setup translation,
# while the other languages used here remain under Unofficial.
if (!(Test-Path innosetup-langs)) {
	New-Item -ItemType Directory innosetup-langs
}
$InnoLanguages = @{
	"Greek.isl" = "Unofficial/Greek.isl"
	"Basque.isl" = "Unofficial/Basque.isl"
	"Galician.isl" = "Unofficial/Galician.isl"
	"Indonesian.isl" = "Unofficial/Indonesian.isl"
	"SerbianCyrillic.isl" = "Unofficial/SerbianCyrillic.isl"
	"SerbianLatin.isl" = "Unofficial/SerbianLatin.isl"
	"ChineseSimplified.isl" = "ChineseSimplified.isl"
	"ChineseTraditional.isl" = "ChineseTraditional.isl"
}
foreach ($Language in $InnoLanguages.GetEnumerator()) {
	$Destination = Join-Path "innosetup-langs" $Language.Key
	if (!(Test-Path $Destination)) {
		$Url = "https://raw.githubusercontent.com/jrsoftware/issrc/main/Files/Languages/$($Language.Value)"
		Invoke-WebRequest $Url -OutFile $Destination -UseBasicParsing
	}
}

# Aegisub localization
Set-Location $BuildRoot
meson compile aegisub-gmo
if ($LASTEXITCODE -ne 0) { throw "Translation build failed." }

# Invoke InnoSetup
$IssUrl = Join-Path $InstallerDir "aegisub_depctrl.iss"
iscc $IssUrl
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed." }
