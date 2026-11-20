<#
.SYNOPSIS
  Repeatable performance run on the headset in a fixed scene.

.DESCRIPTION
  Starts a new game without the menu, teleports the hero to a waypoint at noon (through
  /sdcard/OpenGothic/args.txt and the -exec console hook), then averages the runtime's
  per-second statistics (VrApi) and the game's own frame CPU breakdown.
  The unattended headset must be asleep with no foreground app. args.txt is removed after the run.

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
  [string]$Apk = '',
  [switch]$VrStats,
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
$questSerial = & (Join-Path $PSScriptRoot 'install-quest.ps1') -GetQuestSerial
function Invoke-BenchAdb {
  $output = & adb -s $questSerial @args
  if ($LASTEXITCODE -ne 0) { throw "adb failed ($LASTEXITCODE): $($args -join ' ')" }
  $output
}

function Assert-HeadsetIdle {
  $power = (Invoke-BenchAdb shell dumpsys power) -join "`n"
  $windows = (Invoke-BenchAdb shell dumpsys window displays) -join "`n"
  $activities = (Invoke-BenchAdb shell dumpsys activity -a activities) -join "`n"
  $focus = @([regex]::Matches($windows,'(?m)^\s*mFocusedApp=([^\r\n]+)') | ForEach-Object { $_.Groups[1].Value.Trim() })
  $nonNull = @($focus | Where-Object { $_ -ne 'null' })
  # The sleeping system launcher may retain an Android focus token. Its actual
  # activity, and every other activity in the stack, must be stopped and hidden.
  $stoppedHome = $nonNull.Count -eq 1 -and
    $nonNull[0] -match '^ActivityRecord\{[0-9a-f]+ u\d+ com\.oculus\.vrshell/\.HomeActivity t\d+\}$' -and
    $activities.Contains($nonNull[0]) -and
    $activities -match '(?m)^\s*state=STOPPED\b' -and
    $activities -notmatch '(?m)^\s*state=(?!STOPPED\b)\S' -and
    $activities -match 'mVisibleRequested=false mVisible=false' -and
    $activities -notmatch '\bm(?:VisibleRequested|Visible)=true\b'
  if ($power -notmatch '(?m)^\s*mWakefulness=Asleep\s*$' -or $focus.Count -eq 0 -or
      $windows -notmatch '(?m)^\s*mCurrentFocus=null\s*$' -or
      $windows -match '(?m)^\s*mCurrentFocus=(?!null\s*$)\S' -or
      ($nonNull.Count -ne 0 -and -not $stoppedHome) -or
      $activities -notmatch 'ACTIVITY MANAGER ACTIVITIES' -or
      $activities -match '(?m)^\s*(?:(?:topResumedActivity|mResumedActivity)[:=]\s*ActivityRecord|state=RESUMED\b)') {
    $state = @($power -split "`n" | Where-Object { $_ -match 'mWakefulness=' }) -join ' '
    throw "Headset busy or foreground unknown ($state $nonNull). Check again in ten minutes."
  }
}
function Copy-BenchCaptures {
  $eyeFiles = @()
  if ($EyeCaptures -gt 0) {
    $new = @(Invoke-BenchAdb shell run-as $Package ls files | Where-Object { $_ -cmatch '^eye-\d+-[LR]\.png$' -and $eyesBefore -cnotcontains $_ })
    foreach ($left in $new -clike '*-L.png') {
      $right = $left -creplace '-L\.png$','-R.png'
      if ($new -ccontains $right) { $eyeFiles += $left,$right }
    }
    if ($eyeFiles.Count -eq 0) { throw 'No complete stereo PNG pair for this run' }
  }
  if ($eyeFiles.Count -gt 0 -or $shots.Count -gt 0) {
    New-Item -ItemType Directory -Force $dir | Out-Null
  }
  foreach ($f in $eyeFiles) {
    $dst = Join-Path $dir "$stamp-$Waypoint-$Scenario-$f"
    $remoteHash = Invoke-BenchAdb shell run-as $Package sha256sum "files/$f"
    if ($remoteHash -notmatch '^([0-9a-fA-F]{64})\s') { throw "Invalid checksum for $f" }
    $expected = $Matches[1]
    $tmp = "$dst.$([Guid]::NewGuid().ToString('N')).tmp"
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = 'adb'
    $info.UseShellExecute = $false
    $info.RedirectStandardOutput = $true
    foreach ($arg in '-s',$questSerial,'exec-out','run-as',$Package,'cat',"files/$f") { $info.ArgumentList.Add($arg) }
    $proc = [Diagnostics.Process]::Start($info)
    try {
      $out = [IO.File]::Open($tmp, [IO.FileMode]::CreateNew)
      try { $proc.StandardOutput.BaseStream.CopyTo($out) } finally { $out.Dispose() }
      $proc.WaitForExit()
      if ($proc.ExitCode -ne 0) { throw "Could not pull $f" }
      if ((Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash -ne $expected) { throw "Checksum mismatch for $f" }
      [IO.File]::Move($tmp,$dst)
    } finally {
      $proc.Dispose()
      if ([IO.File]::Exists($tmp)) { [IO.File]::Delete($tmp) }
    }
    "eye capture: $dst"
  }
  # Leave complete pairs on the headset if any local eye transfer fails.
  foreach ($f in $eyeFiles) {
    Invoke-BenchAdb shell run-as $Package rm -f "files/$f"
  }
  foreach ($f in $shots) {
    $dst = Join-Path $dir "$stamp-$Waypoint-$Scenario-$f"
    Invoke-BenchAdb pull "/sdcard/Oculus/Screenshots/$f" $dst | Out-Null
    "capture: $dst"
  }
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
$capturesReady = $false
$stoppedRun = $false
$shots = @()
$eyesBefore = @()
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$dir = Join-Path (Split-Path $PSScriptRoot) 'captures'
try {
  Assert-HeadsetIdle
  $battery = (Invoke-BenchAdb shell dumpsys battery) -join "`n"
  if ($battery -notmatch '(?m)^\s*level:\s*(\d+)' -or [int]$Matches[1] -lt 25 -or
      $battery -notmatch '(?m)^\s*temperature:\s*(\d+)' -or [int]$Matches[1] -gt 450) {
    throw 'Headset battery or temperature outside test limits. Allow at least 20 minutes to cool or charge.'
  }
  # pidof and test use exit code 1 for absence.
  & adb -s $questSerial shell pidof $Package | Out-Null
  if ($LASTEXITCODE -ne 1) { throw 'Cannot start a benchmark while the app may already be running' }
  & adb -s $questSerial shell test -e $remoteArgs
  if ($LASTEXITCODE -ne 1) { throw 'Existing developer arguments or device error; leaving args.txt alone' }

  if ($Apk) {
    $file = (Resolve-Path -LiteralPath $Apk).Path
    Invoke-BenchAdb install -r $file | Out-Null
    $installed = (Invoke-BenchAdb shell pm path $Package) -replace '^package:', ''
    if ($installed -notmatch '^/data/app/[a-zA-Z0-9/_.=~-]+/base[.]apk$') { throw 'Unexpected package path' }
    $hash = ((Invoke-BenchAdb shell sha256sum $installed) -split '\s+')[0]
    if ($hash -ne (Get-FileHash -Algorithm SHA256 $file).Hash) { throw 'Installed APK hash differs' }
    Assert-HeadsetIdle
  }
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
    $gamePid = & adb -s $questSerial shell pidof $Package
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
  $capturesReady = $true
} finally {
  try {
    # A failing adb call must not prevent the remaining cleanup attempts.
    if ($launched) {
      $currentPid = & adb -s $questSerial shell pidof $Package
      if (-not $gamePid -or $currentPid -eq $gamePid) {
        & adb -s $questSerial shell am force-stop $Package
        $stoppedRun = $gamePid -and $currentPid -eq $gamePid -and $LASTEXITCODE -eq 0
      }
    }
    if ($wakeRequested) { & adb -s $questSerial shell am broadcast -a com.oculus.vrpowermanager.automation_disable | Out-Null }
    if ($forcedClocks) {
      & adb -s $questSerial shell setprop debug.oculus.cpuLevel "''"
      & adb -s $questSerial shell setprop debug.oculus.gpuLevel "''"
    }
    if ($wroteArgs -and (((& adb -s $questSerial shell cat $remoteArgs) -join "`n") -ceq $launchArgs)) {
      & adb -s $questSerial shell rm -f $remoteArgs
    }
    if ($argsFile) { & adb -s $questSerial shell rm -f $remoteStaging }
    if ($argsFile) { Remove-Item -LiteralPath $argsFile }
    if ($capturesReady) {
      if ($EyeCaptures -gt 0) {
        & adb -s $questSerial shell pidof $Package | Out-Null
        if (-not $stoppedRun -or $LASTEXITCODE -ne 1) { throw 'Cannot collect eye captures until the benchmark process has stopped' }
      }
      Copy-BenchCaptures
    }
  } finally {
    $lease.ReleaseMutex()
    $lease.Dispose()
  }
}

if ($VrStats) { $vr | ForEach-Object { $_.Line } }
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
