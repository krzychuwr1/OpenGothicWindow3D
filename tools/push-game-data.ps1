<#
.SYNOPSIS
  Copies an owned Gothic II: Night of the Raven installation to the headset over adb.

.DESCRIPTION
  Only the folders the engine reads are copied: Data, _work and System. See install-quest.ps1
  for -GameTarget (Shared: /sdcard/OpenGothic/<Subdir> plus all-files access, the only option
  on Meta Quest; App: the app's own folder, no extra permission) and for -Serial / -AnyDevice
  (headsets other than a Quest).
#>
param(
  [Parameter(Mandatory)] [string]$GameDir,
  [string]$Subdir  = 'Gothic2',
  [string]$Package = 'org.opengothic.window3d',
  [string]$Serial  = '',
  [switch]$AnyDevice,
  [ValidateSet('Shared','App')]
  [string]$GameTarget = 'Shared'
)
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'install-quest.ps1') -SkipApp -GameDir $GameDir -Subdir $Subdir -Package $Package `
  -Serial $Serial -AnyDevice:$AnyDevice -GameTarget $GameTarget
