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
  [ValidatePattern('^[A-Za-z0-9_]+$')][string]$Waypoint = 'NW_CITY_MAINSTREET_02',
  [string]$Time     = '12 0',
  [string]$Package  = 'org.opengothic.window3d',
  [int]$LoadSeconds = 50,
  [int]$Samples     = 12,
  [string]$Extra    = '',
  [ValidateSet('none','combat','spells')]
  [string]$Scenario = 'none',
  [int]$Captures    = 0,
  # Seconds between the game's own per-eye PNG captures (0: none), saved to captures/
  [int]$EyeCaptures = 0,
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
$commands = @("goto waypoint $Waypoint","set time $Time")
switch ($Scenario) {
  combat { $commands += 'cheat god','insert WOLF','insert WOLF','insert WOLF','insert ORCWARRIOR_ROAM' }
  spells { $commands += 'cheat god','insert SKELETONMAGE','insert SKELETONMAGE','insert FIREWARAN' }
}
$capArg = if ($EyeCaptures -gt 0) { "-capture $EyeCaptures" } else { '' }
$exec = $commands -join ';'
$launchArgs = "-nomenu -novideo -gpuprofile -perf $Perf $capArg $Extra -exec `"$exec`""
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
$shots = @()
$eyeFiles = @()
$eyesBefore = @()
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$dir = Join-Path (Split-Path $PSScriptRoot) 'captures'
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

  if ($EyeCaptures -gt 0) { $eyesBefore = @(Invoke-BenchAdb shell run-as $Package ls files) }
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
  $executed = @(foreach ($line in $startup) {
    if ($line -match 'startup command "([^"]+)": (ok|failed)') {
      if ($Matches[2] -ne 'ok') { throw "Startup command failed: $($Matches[1])" }
      $Matches[1]
    }
  })
  if (($executed -join ';') -ine $exec) { throw "Requested scene did not load. Observed commands: $($executed -join ';')" }
  # Read a bounded time window without clearing the device-wide logs.
  $since = (Invoke-BenchAdb shell date '+%s') + '.000'
  Start-Sleep ($Samples + 1)
  if ((Invoke-BenchAdb shell pidof $Package) -ne $gamePid) { throw 'Game process changed during sampling' }
  if (((Invoke-BenchAdb shell cat $remoteArgs) -join "`n") -cne $launchArgs) { throw 'Launch arguments changed during sampling' }
  $vr = @(Invoke-BenchAdb logcat -d -T $since -s VrApi:* | Select-String 'App=')
  $cpu = Invoke-BenchAdb logcat -d -T $since "--pid=$gamePid" -s app:* | Select-String 'Frame CPU ms|Point lights:|Update:|Draw CPU|eye 1|Per frame|XR submit|GPU ms per view|^.*app\s+:\s+[ A-Za-z#0-9&\-]+ [\d.]+;' | Select-Object -Last 8
  # Record statistics before requesting compositor screenshots.
  for ($i = 0; $i -lt $Captures; ++$i) {
    $before = @(Invoke-BenchAdb shell ls /sdcard/Oculus/Screenshots)
    Invoke-BenchAdb shell am startservice -n com.oculus.metacam/.capture.CaptureService -a TAKE_SCREENSHOT | Out-Null
    Start-Sleep 4
    if ((Invoke-BenchAdb shell pidof $Package) -ne $gamePid) { throw 'Game process changed during capture' }
    $new = @(Invoke-BenchAdb shell ls /sdcard/Oculus/Screenshots | Where-Object { $_ -match '^[^/\\]+\.(jpg|jpeg|png)$' -and $before -cnotcontains $_ })
    if ($new.Count -ne 1) { throw "Could not identify screenshot $($i+1) from this run" }
    $shots += $new
  }
  if ($EyeCaptures -gt 0) {
    $new = @(Invoke-BenchAdb shell run-as $Package ls files | Where-Object { $_ -cmatch '^eye-\d+-[LR]\.png$' -and $eyesBefore -cnotcontains $_ })
    foreach ($left in $new -clike '*-L.png') {
      $right = $left -creplace '-L\.png$','-R.png'
      if ($new -ccontains $right) { $eyeFiles += $left,$right }
    }
    if ($eyeFiles.Count -eq 0) { throw 'No complete stereo PNG pair for this run' }
  }
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
if ($eyeFiles.Count -gt 0 -or $shots.Count -gt 0) {
  New-Item -ItemType Directory -Force $dir | Out-Null
}
foreach ($f in $eyeFiles) {
  $dst = Join-Path $dir "$stamp-$Waypoint-$Scenario-$f"
  $info = [Diagnostics.ProcessStartInfo]::new()
  $info.FileName = 'adb'
  $info.UseShellExecute = $false
  $info.RedirectStandardOutput = $true
  foreach ($arg in 'exec-out','run-as',$Package,'cat',"files/$f") { $info.ArgumentList.Add($arg) }
  $proc = [Diagnostics.Process]::Start($info)
  try {
    $out = [IO.File]::Create($dst)
    try { $proc.StandardOutput.BaseStream.CopyTo($out) } finally { $out.Dispose() }
    $proc.WaitForExit()
    if ($proc.ExitCode -ne 0) { throw "Could not pull $f" }
  } finally { $proc.Dispose() }
  Invoke-BenchAdb shell run-as $Package rm -f "files/$f"
  "eye capture: $dst"
}
foreach ($f in $shots) {
  $dst = Join-Path $dir "$stamp-$Waypoint-$Scenario-$f"
  Invoke-BenchAdb pull "/sdcard/Oculus/Screenshots/$f" $dst | Out-Null
  "capture: $dst"
}
