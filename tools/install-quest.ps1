<#
.SYNOPSIS
  Installs OpenGothic Window3D on a Meta Quest (or another Android OpenXR headset) and copies
  your Gothic II game files to it.

.DESCRIPTION
  For players: no build tools needed. Requires
  - a headset in developer mode, connected over USB (or wireless adb) with USB debugging allowed;
  - Android platform-tools (adb) on PATH or in a "platform-tools" folder next to this script;
  - an owned Gothic II Gold / Night of the Raven installation (Steam or GOG are found
    automatically; pass -GameDir otherwise);
  - OpenGothicWindow3D.apk next to this script, or its path in -Apk.

  By default exactly one connected Meta Quest is used. For other headsets pass -AnyDevice (uses
  the only authorized adb device) or -Serial <serial> (as listed by "adb devices").

  -GameTarget Shared (default) copies the game files to /sdcard/OpenGothic/<Subdir> and grants
  the app all-files access over adb; uninstalling the app keeps them. -GameTarget App copies them
  into the app's own folder (/sdcard/Android/data/<package>/files/<Subdir>), which needs no
  extra permission but is deleted when the app is uninstalled. Meta Quest needs Shared: files
  that adb pushes into Android/data stay unreadable to the app there.

  Nothing is uploaded anywhere but the selected device.

.EXAMPLE
  ./install-quest.ps1
  ./install-quest.ps1 -GameDir 'E:\Games\Gothic II'
  ./install-quest.ps1 -AnyDevice -GameTarget App
  ./install-quest.ps1 -Serial 192.168.1.20:5555 -SkipApp       # game files only
#>
param(
  [string]$Apk     = (Join-Path $PSScriptRoot 'OpenGothicWindow3D.apk'),
  [string]$GameDir = '',
  [switch]$SkipGameFiles,
  [switch]$SkipApp,
  [string]$Serial  = '',
  [switch]$AnyDevice,
  [ValidateSet('Shared','App')]
  [string]$GameTarget = 'Shared',
  [string]$Package = 'org.opengothic.window3d',
  [string]$Subdir  = 'Gothic2',
  [switch]$GetQuestSerial
)
$ErrorActionPreference = 'Stop'

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
# Read the host inventory before sending any command to a device.
$inventory = & $adb devices -l
if ($LASTEXITCODE) { throw 'Could not read the adb device inventory.' }
$devices = @(foreach ($line in $inventory) {
  if ($line -match '^(\S+)\s+device(?:\s|$)') {
    $model = if ($line -match '\bmodel:(\S+)') { $Matches[1] } else { '' }
    [pscustomobject]@{
      Serial = ($line -split '\s+')[0]
      Model  = $model
      IsQuest = $model -match '^(?:Meta_)?Quest(?:_\S+)?$'
    }
  }
})

# An explicit -Serial picks any device. ANDROID_SERIAL may be left over from other work, so it
# must still name a Quest unless -AnyDevice is given.
$wanted = if ($Serial) { $Serial } else { $env:ANDROID_SERIAL }
$pool = if ($Serial -or $AnyDevice) { $devices } else { @($devices | Where-Object IsQuest) }
if ($wanted) { $pool = @($pool | Where-Object { $_.Serial -ceq $wanted }) }
if ($pool.Count -ne 1) {
  $list = if ($devices.Count) { ($devices | ForEach-Object { "  $($_.Serial)  $($_.Model)" }) -join "`n" } else { '  (none)' }
  $hint = if ($Serial -or $AnyDevice) {
    'Pass -Serial <serial> to pick one connected, USB-authorized device.'
  } else {
    'Connect one Quest with USB debugging allowed, or use -AnyDevice / -Serial <serial> for another headset.'
  }
  throw "No single device selected. $hint`nAuthorized adb devices:`n$list"
}
$device = $pool[0]
if ($GetQuestSerial) { return $device.Serial }
$serialArg = @('-s', $device.Serial)
Write-Host "Using $($device.Model) ($($device.Serial))"
if ($GameTarget -eq 'App' -and $device.IsQuest) {
  Write-Warning 'On Meta Quest the app cannot read files adb pushes into its own folder: use -GameTarget Shared.'
}

if (-not $SkipApp) {
  if (-not (Test-Path $Apk)) { throw "APK not found: $Apk" }
  Write-Host 'Installing the app...'
  & $adb @serialArg install -r $Apk
  if ($LASTEXITCODE) { throw 'adb install failed' }
}

if (-not $SkipGameFiles) {
  if (-not $GameDir) { $GameDir = Find-Gothic2 }
  if (-not (Test-GothicDir $GameDir)) {
    throw 'Gothic II was not found. Pass the installation folder (the one with Data, _work and System): -GameDir "path"'
  }
  if (-not (Test-Path (Join-Path $GameDir 'Data\Worlds_Addon.vdf'))) {
    Write-Warning 'Data\Worlds_Addon.vdf is missing: OpenGothic needs Gothic II Gold Edition / Night of the Raven.'
  }
  Write-Host "Game files: $GameDir"
  $target = if ($GameTarget -eq 'App') { "/sdcard/Android/data/$Package/files/$Subdir" } else { "/sdcard/OpenGothic/$Subdir" }
  & $adb @serialArg shell mkdir -p $target
  if ($LASTEXITCODE) { throw "Could not create $target on the device" }
  foreach ($d in 'Data', '_work', 'System') {
    Write-Host "Copying $d to $target (this can take a few minutes)..."
    & $adb @serialArg push --sync (Join-Path $GameDir $d) "$target/"
    if ($LASTEXITCODE) { throw "Copying $d failed" }
  }
}

if ($GameTarget -eq 'Shared') {
  # The app reads the game files from the shared OpenGothic folder
  & $adb @serialArg shell appops set $Package MANAGE_EXTERNAL_STORAGE allow
  if ($LASTEXITCODE) {
    Write-Warning "Could not grant all-files access. Allow it for the app in the device's settings, or copy the game files with -GameTarget App."
  }
}
Write-Host ''
if ($device.IsQuest) {
  Write-Host 'Done. On the headset: Library > Unknown Sources > OpenGothic Window3D.'
} else {
  Write-Host "Done. Start OpenGothic Window3D from the headset's app library, or run: adb -s $($device.Serial) shell am start -n $Package/android.app.NativeActivity"
}
