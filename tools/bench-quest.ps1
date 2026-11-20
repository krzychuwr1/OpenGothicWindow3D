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
  [string]$Extra    = '',
  # Test runs ask for low clocks: less heat and battery drain (compare runs at the same level)
  [ValidateSet('low','high')]
  [string]$Perf     = 'low'
)
$ErrorActionPreference = 'Stop'
# Check native exit codes explicitly, including the expected pidof/test misses.
$PSNativeCommandUseErrorActionPreference = $false
function Invoke-BenchAdb {
  $output = & adb @args
  if ($LASTEXITCODE -ne 0) { throw "adb failed ($LASTEXITCODE): $($args -join ' ')" }
  $output
}

$remoteArgs = '/sdcard/OpenGothic/args.txt'
$launchArgs = "-nomenu -novideo -gpuprofile -perf $Perf $Extra -exec `"goto waypoint $Waypoint;set time $Time`""
$serial = Invoke-BenchAdb get-serialno
if ($serial -eq 'unknown') { throw 'No connected headset' }
# Two checkouts on this host must not run the benchmark on the same headset.
$lease = [Threading.Mutex]::new($false, "Local\OpenGothicQuestBench-$serial")
try { $owned = $lease.WaitOne(0) } catch [Threading.AbandonedMutexException] { $owned = $true }
if (-not $owned) { $lease.Dispose(); throw 'Another benchmark owns the headset' }
$argsFile = $null
$remoteStaging = "/data/local/tmp/opengothic-bench-$([Guid]::NewGuid().ToString('N'))"
$wroteArgs = $false
$forcedClocks = $false
$wakeRequested = $false
$launched = $false
$gamePid = $null
try {
  $battery = (Invoke-BenchAdb shell dumpsys battery) -join "`n"
  if ($battery -notmatch '(?m)^\s*level:\s*(\d+)' -or [int]$Matches[1] -lt 25 -or
      $battery -notmatch '(?m)^\s*temperature:\s*(\d+)' -or [int]$Matches[1] -gt 450) {
    throw 'Headset battery or temperature outside test limits. Allow at least 20 minutes to cool or charge.'
  }
  # pidof and test use exit code 1 for absence.
  & adb shell pidof $Package | Out-Null
  if ($LASTEXITCODE -ne 1) { throw 'Cannot start a benchmark while the app may already be running' }
  & adb shell test -e $remoteArgs
  if ($LASTEXITCODE -ne 1) { throw 'Existing developer arguments or device error; leaving args.txt alone' }

  $argsFile = [IO.Path]::GetTempFileName()
  [IO.File]::WriteAllText($argsFile, $launchArgs, [Text.Encoding]::ASCII)
  Invoke-BenchAdb push $argsFile $remoteStaging | Out-Null
  # A second tool may have written args.txt since the preflight check.
  # Android shell noclobber creates it only if the path is still free.
  Invoke-BenchAdb shell "set -C; cat $remoteStaging > $remoteArgs"
  $wroteArgs = $true
  if ($Perf -eq 'low') {
    $forcedClocks = $true
    Invoke-BenchAdb shell setprop debug.oculus.cpuLevel 1
    Invoke-BenchAdb shell setprop debug.oculus.gpuLevel 0
  }
  $wakeRequested = $true
  Invoke-BenchAdb shell input keyevent KEYCODE_WAKEUP
  Invoke-BenchAdb shell am broadcast -a com.oculus.vrpowermanager.prox_close | Out-Null
  $launchTime = (Invoke-BenchAdb shell date '+%s') + '.000'
  $launched = $true
  Invoke-BenchAdb shell am start -n "$Package/android.app.NativeActivity" | Out-Null
  for ($attempt = 0; $attempt -lt 25 -and -not $gamePid; ++$attempt) {
    $gamePid = & adb shell pidof $Package
    if (-not $gamePid) { Start-Sleep -Milliseconds 200 }
  }
  if (-not $gamePid) { throw 'Game process did not start' }
  Start-Sleep $LoadSeconds
  if ((Invoke-BenchAdb shell pidof $Package) -ne $gamePid) { throw 'Game process changed during loading' }
  $startup = Invoke-BenchAdb logcat -d -T $launchTime "--pid=$gamePid" -s app:*
  if (-not ($startup | Select-String -SimpleMatch "startup command `"goto waypoint $Waypoint`": ok") -or
      -not ($startup | Select-String -SimpleMatch "startup command `"set time $Time`": ok")) {
    throw 'Requested scene did not load in this process. Discarding the run.'
  }
  # Read a bounded time window without clearing the device-wide logs.
  $since = (Invoke-BenchAdb shell date '+%s') + '.000'
  Start-Sleep ($Samples + 1)
  if ((Invoke-BenchAdb shell pidof $Package) -ne $gamePid) { throw 'Game process changed during sampling' }
  if (((Invoke-BenchAdb shell cat $remoteArgs) -join "`n") -cne $launchArgs) { throw 'Launch arguments changed during sampling' }
  $vr = @(Invoke-BenchAdb logcat -d -T $since -s VrApi:* | Select-String 'App=')
  $cpu = Invoke-BenchAdb logcat -d -T $since "--pid=$gamePid" -s app:* | Select-String 'Frame CPU ms|Point lights:|Update:|Draw CPU|eye 1|Per frame|XR submit|GPU ms per view|^.*app\s+:\s+[ A-Za-z#0-9&\-]+ [\d.]+;' | Select-Object -Last 8
} finally {
  # A failing adb call must not prevent the remaining cleanup attempts.
  if ($launched) {
    $currentPid = & adb shell pidof $Package
    if (-not $gamePid -or $currentPid -eq $gamePid) { & adb shell am force-stop $Package }
  }
  if ($wakeRequested) { & adb shell am broadcast -a com.oculus.vrpowermanager.automation_disable | Out-Null }
  if ($forcedClocks) {
    & adb shell setprop debug.oculus.cpuLevel "''"
    & adb shell setprop debug.oculus.gpuLevel "''"
  }
  if ($wroteArgs -and (((& adb shell cat $remoteArgs) -join "`n") -ceq $launchArgs)) {
    & adb shell rm -f $remoteArgs
  }
  if ($argsFile) { & adb shell rm -f $remoteStaging }
  if ($argsFile) { Remove-Item -LiteralPath $argsFile }
  $lease.ReleaseMutex()
  $lease.Dispose()
}

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
