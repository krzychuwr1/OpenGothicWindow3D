<#
.SYNOPSIS
  Copies an owned Gothic II: Night of the Raven installation to the headset over adb.

.DESCRIPTION
  Only the folders the engine reads are copied: Data, _work and System. They go to
  /sdcard/OpenGothic/Gothic2. Files that adb pushes into Android/data stay unreadable to the
  app on Meta Quest, so the app reads the shared folder with the all-files access that this
  script grants (sideloaded apps only; uninstalling the app keeps the game files).
#>
param(
  [Parameter(Mandatory)] [string]$GameDir,
  [string]$Subdir  = 'Gothic2',
  [string]$Package = 'org.opengothic.window3d'
)
$ErrorActionPreference = 'Stop'
$questSerial = & (Join-Path $PSScriptRoot 'install-quest.ps1') -GetQuestSerial
foreach ($d in 'Data', '_work', 'System') {
  if (-not (Test-Path (Join-Path $GameDir $d))) { throw "Not a Gothic installation (missing $d): $GameDir" }
}
$target = "/sdcard/OpenGothic/$Subdir"
adb -s $questSerial shell mkdir -p $target
foreach ($d in 'Data', '_work', 'System') {
  Write-Host "Copying $d ..."
  adb -s $questSerial push --sync (Join-Path $GameDir $d) "$target/"
  if ($LASTEXITCODE) { throw "adb -s $questSerial push failed for $d" }
}
adb -s $questSerial shell appops set $Package MANAGE_EXTERNAL_STORAGE allow
Write-Host "Game files are in $target"
