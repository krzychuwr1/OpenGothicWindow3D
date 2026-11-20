<#
.SYNOPSIS
  Installs OpenGothic Window3D on a Meta Quest and copies your Gothic II game files to it.

.DESCRIPTION
  For players: no build tools needed. Requires
  - a Meta Quest in developer mode, connected over USB with USB debugging allowed;
  - Android platform-tools (adb) on PATH or in a "platform-tools" folder next to this script;
  - an owned Gothic II Gold / Night of the Raven installation (Steam or GOG are found
    automatically; pass -GameDir otherwise);
  - OpenGothicWindow3D.apk next to this script, or its path in -Apk.

  The game files go to /sdcard/OpenGothic/Gothic2 on the headset; nothing is uploaded anywhere
  else. Uninstalling the app keeps them; delete that folder to remove them.

.EXAMPLE
  ./install-quest.ps1
  ./install-quest.ps1 -GameDir 'E:\Games\Gothic II'
#>
param(
  [string]$Apk     = (Join-Path $PSScriptRoot 'OpenGothicWindow3D.apk'),
  [string]$GameDir = '',
  [switch]$SkipGameFiles
)
$ErrorActionPreference = 'Stop'
$package = 'org.opengothic.window3d'

function Find-Adb {
  $local = Join-Path $PSScriptRoot 'platform-tools\adb.exe'
  if (Test-Path $local) { return $local }
  $cmd = Get-Command adb -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  throw "adb not found. Download Android SDK Platform-Tools from https://developer.android.com/tools/releases/platform-tools and unzip it next to this script (platform-tools\adb.exe)."
}

function Test-GothicDir([string]$dir) {
  return $dir -and (Test-Path (Join-Path $dir 'Data')) -and (Test-Path (Join-Path $dir '_work')) -and (Test-Path (Join-Path $dir 'System'))
}

function Find-Gothic2 {
  $candidates = @()
  # Steam: every library folder
  $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
  if ($steam) {
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    $libs = @($steam)
    if (Test-Path $vdf) {
      $libs += Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\','\' }
    }
    foreach ($lib in ($libs | Select-Object -Unique)) { $candidates += Join-Path $lib 'steamapps\common\Gothic II' }
  }
  # GOG Galaxy / offline installers
  foreach ($root in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games') {
    Get-ChildItem $root -ErrorAction SilentlyContinue | ForEach-Object {
      $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
      if ($p.gameName -like 'Gothic II*' -or $p.gameName -like 'Gothic 2*') { $candidates += $p.path }
    }
  }
  $candidates += 'C:\Program Files (x86)\Steam\steamapps\common\Gothic II', 'C:\GOG Games\Gothic 2 Gold'
  foreach ($c in $candidates) { if (Test-GothicDir $c) { return $c } }
  return $null
}

$adb = Find-Adb
Write-Host "adb: $adb"

$devices = & $adb devices | Select-Object -Skip 1 | Where-Object { $_ -match '\S' }
if (-not $devices) { throw 'No headset found. Connect the Quest with a USB cable, put it on and allow USB debugging.' }
if ($devices -match 'unauthorized') { throw 'The headset has not allowed USB debugging yet: put it on and accept the prompt.' }

if (-not (Test-Path $Apk)) { throw "APK not found: $Apk" }
Write-Host 'Installing the app...'
& $adb install -r $Apk
if ($LASTEXITCODE) { throw 'adb install failed' }

if (-not $SkipGameFiles) {
  if (-not $GameDir) { $GameDir = Find-Gothic2 }
  if (-not (Test-GothicDir $GameDir)) {
    throw 'Gothic II was not found. Pass the installation folder (the one with Data, _work and System): -GameDir "path"'
  }
  if (-not (Test-Path (Join-Path $GameDir 'Data\Worlds_Addon.vdf'))) {
    Write-Warning 'Data\Worlds_Addon.vdf is missing: OpenGothic needs Gothic II Gold Edition / Night of the Raven.'
  }
  Write-Host "Game files: $GameDir"
  $target = '/sdcard/OpenGothic/Gothic2'
  & $adb shell mkdir -p $target
  foreach ($d in 'Data', '_work', 'System') {
    Write-Host "Copying $d (this can take a few minutes)..."
    & $adb push --sync (Join-Path $GameDir $d) "$target/"
    if ($LASTEXITCODE) { throw "Copying $d failed" }
  }
}

# The app reads the game files from the shared OpenGothic folder
& $adb shell appops set $package MANAGE_EXTERNAL_STORAGE allow
Write-Host ''
Write-Host 'Done. On the headset: Library > Unknown Sources > OpenGothic Window3D.'
