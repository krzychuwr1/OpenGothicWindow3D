<#
.SYNOPSIS
  Repeatable performance run on the headset in a fixed scene.

.DESCRIPTION
  Starts a new game without the menu, teleports the hero to a waypoint at noon (through
  /sdcard/OpenGothic/args.txt and the -exec console hook), then averages the runtime's
  per-second statistics (VrApi) and the game's own frame CPU breakdown.
  The headset must be awake (proximity sensor covered or disabled). args.txt is removed after the run.

.EXAMPLE
  ./tools/bench-quest.ps1 -Waypoint NW_CITY_MAINSTREET_02
#>
param(
  [string]$Waypoint = 'NW_CITY_MAINSTREET_02',
  [string]$Time     = '12 0',
  [string]$Package  = 'org.opengothic.window3d',
  [int]$LoadSeconds = 50,
  [int]$Samples     = 12,
  [string]$Extra    = ''
)
$ErrorActionPreference = 'Stop'
$argsFile = [IO.Path]::GetTempFileName()
"-nomenu -novideo -gpuprofile $Extra -exec `"goto waypoint $Waypoint;set time $Time`"" | Set-Content $argsFile -NoNewline -Encoding ascii
adb push $argsFile /sdcard/OpenGothic/args.txt | Out-Null
Remove-Item $argsFile

adb shell am force-stop $Package
adb shell am start -n "$Package/android.app.NativeActivity" | Out-Null
Start-Sleep $LoadSeconds
adb logcat -c
Start-Sleep ($Samples + 1)
$vr  = adb logcat -d -s VrApi:* | Select-String 'App='
$cpu = adb logcat -d -s app:* | Select-String 'Frame CPU ms|Update:|Draw CPU|eye 1|Per frame|XR submit|GPU ms per view|^.*app\s+:\s+[ A-Za-z#0-9&\-]+ [\d.]+;' | Select-Object -Last 8
adb shell rm -f /sdcard/OpenGothic/args.txt

$fps = @(); $gpu = @(); $cg = @()
foreach ($l in $vr) {
  if ($l -match 'FPS=(\d+)/')          { $fps += [int]$Matches[1] }
  if ($l -match 'GPU%=([\d.]+)')       { $gpu += [double]$Matches[1] }
  if ($l -match 'CPU&GPU=([\d.]+)ms')  { $cg  += [double]$Matches[1] }
}
if ($fps.Count -eq 0) { throw 'No VrApi samples: is the app in front and the headset awake?' }
'{0}: fps {1:N1}  GPU busy {2:P0}  CPU&GPU {3:N1} ms ({4} samples)' -f $Waypoint,
  ($fps | Measure-Object -Average).Average, ($gpu | Measure-Object -Average).Average,
  ($cg | Measure-Object -Average).Average, $fps.Count
if ($vr[-1] -match '(CPU4/GPU=[^,]+,[^,]+)') { "clocks: $($Matches[1])" }
$cpu | ForEach-Object { ($_ -split 'app\s+:\s+')[-1] }
