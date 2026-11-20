<#
.SYNOPSIS
  Creates save games at well-known places on the headset (development and testing).

.DESCRIPTION
  For each place the game starts a new game without the menu, teleports the hero, sets the time
  and saves into a fixed slot, then quits. The saves appear in Load game with the place as name.
  Slots 1-4 are left alone; slots 5-9 hold later-chapter heroes with gear, 10-19 places.
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
$questSerial = & (Join-Path $PSScriptRoot 'install-quest.ps1') -GetQuestSerial

# Hero setups for the later-chapter saves (startup commands: give, equip, guild, talent, attr)
$paladin = 'chapter 3;guild 1;attr str 120;attr dex 60;attr hp 450;fight 2 75;fight 4 50;' +
           'equip ITAR_PAL_M;equip ITMW_2H_PAL_SWORD;equip ITRW_CROSSBOW_H_02;give ITRW_BOLT 200;give ITPO_HEALTH_03 10'
$mage    = 'chapter 4;guild 4;talent 7 6;attr mana 320;attr hp 320;fight 1 40;equip ITAR_KDF_H;' +
           'equip ITRU_FIREBOLT;equip ITRU_INSTANTFIREBALL;equip ITRU_ICEBOLT;equip ITRU_LIGHTNINGFLASH;' +
           'equip ITRU_FIRESTORM;equip ITRU_ICEWAVE;equip ITRU_FIRERAIN;equip ITRU_SUMDEMON;equip ITRU_LIGHTHEAL;' +
           'give ITPO_MANA_03 20;give ITPO_HEALTH_03 10'
$hunter  = 'chapter 5;guild 6;attr str 160;attr dex 160;attr hp 650;fight 1 80;fight 3 75;' +
           'equip ITAR_DJG_H;equip ITMW_1H_SPECIAL_03;equip ITRW_BOW_H_04;give ITRW_ARROW 300;give ITPO_HEALTH_03 10'

# slot, name, waypoint, time, extra console commands
$places = @(
  @(5,  'Chapter 3: paladin, two-hander, crossbow', 'NW_CITY_UPTOWNPARADE_03', '12 0',  $paladin),
  @(6,  'Chapter 4: fire mage, runes up to circle 6', 'NW_MONASTERY_PLACE_01', '10 0', $mage),
  @(7,  'Chapter 5: dragon hunter, sword and bow',  'NW_PASS_01',              '15 0',  $hunter),
  @(8,  'Fight: fire mage vs skeletons (night)',   'NW_TAVERNE',              '22 0',  "$mage;insert SKELETON;insert SKELETON;insert SKELETONMAGE"),
  @(9,  'Fight: paladin vs orcs',                  'NW_PASS_01',              '13 0',  "$paladin;insert ORCWARRIOR_ROAM;insert ORCWARRIOR_ROAM;insert WOLF"),
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
  adb -s $questSerial push $argsFile /sdcard/OpenGothic/args.txt | Out-Null
  Remove-Item $argsFile

  $before = (adb -s $questSerial shell run-as $Package ls -l "files/save_slot_$slot.sav" 2>$null) -join ''
  adb -s $questSerial shell am force-stop $Package
  adb -s $questSerial shell input keyevent KEYCODE_WAKEUP
  adb -s $questSerial shell am broadcast -a com.oculus.vrpowermanager.prox_close | Out-Null
  adb -s $questSerial shell am start -n "$Package/android.app.NativeActivity" | Out-Null

  # Wait for the save file to change and stop growing (load ~60 s, save a few seconds later)
  $deadline = (Get-Date).AddSeconds(180)
  $last = $before; $stable = 0; $ok = $false
  while ((Get-Date) -lt $deadline) {
    Start-Sleep 3
    $now = (adb -s $questSerial shell run-as $Package ls -l "files/save_slot_$slot.sav" 2>$null) -join ''
    if ($now -and $now -ne $before) {
      if ($now -eq $last) { $stable++ } else { $stable = 0 }
      if ($stable -ge 2) { $ok = $true; break }
    }
    $last = $now
  }
  adb -s $questSerial shell am force-stop $Package
  $log = adb -s $questSerial shell run-as $Package cat files/log.txt | Select-String 'startup command|startup save'
  $failed = $log | Select-String 'failed'
  if ($ok -and -not $failed) { "slot ${slot}: $name" }
  elseif ($ok) { "slot ${slot}: $name (saved, but: $($failed -join '; '))" }
  else { "slot ${slot}: $name FAILED (no save)" }
}

adb -s $questSerial shell setprop debug.oculus.cpuLevel 1
adb -s $questSerial shell setprop debug.oculus.gpuLevel 0
try {
  foreach ($p in $places) {
    if ($Only.Count -gt 0 -and $Only -notcontains $p[0]) { continue }
    Run-Place $p
  }
}
finally {
  adb -s $questSerial shell rm -f /sdcard/OpenGothic/args.txt
  adb -s $questSerial shell am force-stop $Package
  adb -s $questSerial shell am broadcast -a com.oculus.vrpowermanager.automation_disable | Out-Null
  adb -s $questSerial shell setprop debug.oculus.cpuLevel "''"
  adb -s $questSerial shell setprop debug.oculus.gpuLevel "''"
}
