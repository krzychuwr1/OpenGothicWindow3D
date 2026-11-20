<#
.SYNOPSIS
  Creates save games at well-known places on the headset (development and testing).

.DESCRIPTION
  For each place the game starts a new game without the menu, teleports the hero, sets the time
  and saves into a fixed slot, then quits. The saves appear in Load game with the place as name.
  Slots 1-9 are left alone; the places use slots 10 and up and overwrite earlier ones there.
  Runs at low clocks, one place at a time; the headset must be awake (the script keeps it on).

.EXAMPLE
  ./tools/make-saves.ps1
  ./tools/make-saves.ps1 -Only 18,19
#>
param(
  [string]$Package = 'org.opengothic.window3d',
  [int[]]$Only     = @()
)
$ErrorActionPreference = 'Stop'

# slot, name, waypoint, time, extra console commands
$places = @(
  @(10, 'Khorinis: main street (noon)',      'NW_CITY_MAINSTREET_02',      '12 0',  ''),
  @(11, 'Khorinis: merchant quarter (noon)', 'NW_CITY_MERCHANT_PATH_02',   '12 0',  ''),
  @(12, 'Khorinis: upper quarter (noon)',    'NW_CITY_UPTOWNPARADE_03',    '12 0',  ''),
  @(13, 'Khorinis: harbour (evening)',       'NW_CITY_HABOUR_03',          '19 30', ''),
  @(14, 'Khorinis: main street (night)',     'NW_CITY_MAINSTREET_02',      '23 0',  ''),
  @(15, 'Dead Harpy tavern (noon)',          'NW_TAVERNE',                 '12 0',  ''),
  @(16, 'Onar''s farm (afternoon)',          'NW_BIGFARM_HOUSE_01',        '15 0',  ''),
  @(17, 'Monastery (morning)',               'NW_MONASTERY_PLACE_01',      '9 0',   ''),
  @(18, 'Xardas'' tower (dusk)',             'NW_XARDAS_START',            '20 0',  ''),
  @(19, 'Fight: wolves at the tavern',       'NW_TAVERNE',                 '12 0',  'insert WOLF;insert WOLF;insert WOLF')
)

function Run-Place($p) {
  $slot, $name, $wp, $time, $extra = $p
  $exec = "goto waypoint $wp;set time $time"
  if ($extra) { $exec += ";$extra" }
  $exec += ";savegame $slot $name"
  $argsFile = [IO.Path]::GetTempFileName()
  "-nomenu -novideo -perf low -exec `"$exec`"" | Set-Content $argsFile -NoNewline -Encoding utf8
  adb push $argsFile /sdcard/OpenGothic/args.txt | Out-Null
  Remove-Item $argsFile

  $before = (adb shell run-as $Package ls -l "files/save_slot_$slot.sav" 2>$null) -join ''
  adb shell am force-stop $Package
  adb shell input keyevent KEYCODE_WAKEUP
  adb shell am broadcast -a com.oculus.vrpowermanager.prox_close | Out-Null
  adb shell am start -n "$Package/android.app.NativeActivity" | Out-Null

  # Wait for the save file to change and stop growing (load ~60 s, save a few seconds later)
  $deadline = (Get-Date).AddSeconds(180)
  $last = $before; $stable = 0; $ok = $false
  while ((Get-Date) -lt $deadline) {
    Start-Sleep 3
    $now = (adb shell run-as $Package ls -l "files/save_slot_$slot.sav" 2>$null) -join ''
    if ($now -and $now -ne $before) {
      if ($now -eq $last) { $stable++ } else { $stable = 0 }
      if ($stable -ge 2) { $ok = $true; break }
    }
    $last = $now
  }
  adb shell am force-stop $Package
  $log = adb shell run-as $Package cat files/log.txt | Select-String 'startup command|startup save'
  $failed = $log | Select-String 'failed'
  if ($ok -and -not $failed) { "slot ${slot}: $name" }
  elseif ($ok) { "slot ${slot}: $name (saved, but: $($failed -join '; '))" }
  else { "slot ${slot}: $name FAILED (no save)" }
}

adb shell setprop debug.oculus.cpuLevel 1
adb shell setprop debug.oculus.gpuLevel 0
try {
  foreach ($p in $places) {
    if ($Only.Count -gt 0 -and $Only -notcontains $p[0]) { continue }
    Run-Place $p
  }
}
finally {
  adb shell rm -f /sdcard/OpenGothic/args.txt
  adb shell am force-stop $Package
  adb shell am broadcast -a com.oculus.vrpowermanager.automation_disable | Out-Null
  adb shell setprop debug.oculus.cpuLevel "''"
  adb shell setprop debug.oculus.gpuLevel "''"
}
