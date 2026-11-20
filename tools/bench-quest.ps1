<#
.SYNOPSIS
  Repeatable performance run on the headset in a fixed scene.

.DESCRIPTION
  Starts a new game without the menu, teleports the hero to a waypoint at noon (through
  /sdcard/OpenGothic/args.txt and the -exec console hook), then averages the runtime's
  per-second statistics (VrApi) and the game's own frame CPU breakdown.
  The headset must be awake (proximity sensor covered or disabled). args.txt is removed after the run.

  -Scenario adds action to the scene (the hero is made invulnerable):
    combat  wolves and an orc warrior attack (animation, weapon swings, blood)
    spells  skeleton mages and a fire lizard attack (spell effects, particles, light)
  -Captures N takes N headset screenshots (what the compositor shows) during sampling and
  copies them to captures/ for checking the picture.

.EXAMPLE
  ./tools/bench-quest.ps1 -Waypoint NW_CITY_MAINSTREET_02
  ./tools/bench-quest.ps1 -Waypoint NW_TAVERNE -Scenario spells -Captures 3
#>
param(
  [string]$Waypoint = 'NW_CITY_MAINSTREET_02',
  [string]$Time     = '12 0',
  [string]$Package  = 'org.opengothic.window3d',
  [int]$LoadSeconds = 50,
  [int]$Samples     = 12,
  [string]$Extra    = '',
  [ValidateSet('none','combat','spells')]
  [string]$Scenario = 'none',
  [int]$Captures    = 0,
  # Test runs ask for low clocks: less heat and battery drain (compare runs at the same level)
  [ValidateSet('low','high')]
  [string]$Perf     = 'low'
)
$ErrorActionPreference = 'Stop'
$exec = "goto waypoint $Waypoint;set time $Time"
switch ($Scenario) {
  'combat' { $exec += ';cheat god;insert WOLF;insert WOLF;insert WOLF;insert ORCWARRIOR_ROAM' }
  'spells' { $exec += ';cheat god;insert SKELETONMAGE;insert SKELETONMAGE;insert FIREWARAN' }
}
$argsFile = [IO.Path]::GetTempFileName()
"-nomenu -novideo -gpuprofile -perf $Perf $Extra -exec `"$exec`"" | Set-Content $argsFile -NoNewline -Encoding ascii
adb push $argsFile /sdcard/OpenGothic/args.txt | Out-Null
Remove-Item $argsFile

adb shell am force-stop $Package
# Low forced clocks for unattended runs (heat, battery); cleared again after the run
if ($Perf -eq 'low') {
  adb shell setprop debug.oculus.cpuLevel 1
  adb shell setprop debug.oculus.gpuLevel 0
}
# Keep the display on for this run only (proximity sensor override)
adb shell input keyevent KEYCODE_WAKEUP
adb shell am broadcast -a com.oculus.vrpowermanager.prox_close | Out-Null
adb shell am start -n "$Package/android.app.NativeActivity" | Out-Null
Start-Sleep $LoadSeconds
adb logcat -c
Start-Sleep ($Samples + 1)
$vr  = adb logcat -d -s VrApi:* | Select-String 'App='
$cpu = adb logcat -d -s app:* | Select-String 'Frame CPU ms|Point lights:|Update:|Draw CPU|eye 1|Per frame|XR submit|GPU ms per view|^.*app\s+:\s+[ A-Za-z#0-9&\-]+ [\d.]+;' | Select-Object -Last 8
# Screenshots after sampling: the capture service's logging would push the statistics out of the log
$shots = @()
if ($Captures -gt 0) {
  $before = @(adb shell ls /sdcard/Oculus/Screenshots 2>$null)
  for ($i = 0; $i -lt $Captures; $i++) {
    adb shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT | Out-Null
    Start-Sleep 4
  }
  $shots = @(adb shell ls /sdcard/Oculus/Screenshots) | Where-Object { $_ -and ($before -notcontains $_) }
}
adb shell rm -f /sdcard/OpenGothic/args.txt
# Stop the game and let the headset sleep again between runs
adb shell am force-stop $Package
adb shell am broadcast -a com.oculus.vrpowermanager.automation_disable | Out-Null
adb shell setprop debug.oculus.cpuLevel "''"
adb shell setprop debug.oculus.gpuLevel "''"

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
if ($shots.Count -gt 0) {
  $dir = Join-Path (Split-Path $PSScriptRoot) 'captures'
  New-Item -ItemType Directory -Force $dir | Out-Null
  $stamp = Get-Date -Format 'MMdd-HHmmss'
  foreach ($f in $shots) {
    $dst = Join-Path $dir "$stamp-$Waypoint-$Scenario-$f"
    adb pull "/sdcard/Oculus/Screenshots/$f" $dst | Out-Null
    adb shell rm -f "/sdcard/Oculus/Screenshots/$f"
    "capture: $dst"
  }
}
