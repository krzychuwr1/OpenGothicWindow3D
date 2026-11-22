<#
.SYNOPSIS
  Builds a single self-contained Windows GUI installer .exe for OpenGothic Window3D
  with OpenGothicWindow3D.apk and Android platform-tools (adb) embedded inside it.

.EXAMPLE
  ./tools/quest-installer/build-installer.ps1
#>
param(
  [string]$ApkPath       = '',
  [string]$PlatformTools = ''
)
$ErrorActionPreference = 'Stop'

$scriptDir = $PSScriptRoot
$repoRoot  = (Resolve-Path "$scriptDir\..\..").Path
$outDir    = Join-Path $repoRoot 'build\quest-installer'
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

if (-not $ApkPath) {
  $ApkPath = Join-Path $repoRoot 'build\quest\OpenGothicWindow3D.apk'
}
if (-not (Test-Path $ApkPath)) {
  throw "APK not found: $ApkPath (run ./tools/build-quest.ps1 first)"
}

if (-not $PlatformTools) {
  $adbCmd = Get-Command adb -ErrorAction SilentlyContinue
  if ($adbCmd) { $PlatformTools = Split-Path $adbCmd.Source }
  else {
    $sdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }
    if (Test-Path (Join-Path $sdk 'platform-tools\adb.exe')) { $PlatformTools = Join-Path $sdk 'platform-tools' }
    else { throw 'platform-tools not found: put adb on PATH, set ANDROID_HOME or pass -PlatformTools <dir>' }
  }
}

# Bundle APK + adb into a compressed resource payload embedded directly inside the .exe
$stage = Join-Path $outDir 'payload-stage'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'platform-tools') | Out-Null

Copy-Item $ApkPath (Join-Path $stage 'OpenGothicWindow3D.apk')
foreach ($f in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll') {
  $src = Join-Path $PlatformTools $f
  if (-not (Test-Path $src)) { throw "Missing platform-tools file: $src" }
  Copy-Item $src (Join-Path $stage "platform-tools\$f")
}

$payloadZip = Join-Path $outDir 'payload.zip'
Remove-Item $payloadZip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$stage\*" -DestinationPath $payloadZip -CompressionLevel Optimal

# Locate .NET Framework 4.x C# compiler (built into Windows 10/11)
$csc = Get-ChildItem 'C:\Windows\Microsoft.NET\Framework64\v4*' -Filter 'csc.exe' -Recurse -ErrorAction SilentlyContinue |
       Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $csc) { throw 'Could not find .NET Framework csc.exe.' }

$exeOut   = Join-Path $outDir 'OpenGothicWindow3D-Quest-Installer.exe'
$iconPath = Join-Path $repoRoot 'icon.ico'
$manifest = Join-Path $scriptDir 'app.manifest'
$source   = Join-Path $scriptDir 'Installer.cs'

$cscArgs = @(
  '/nologo',
  '/target:winexe',
  '/optimize+',
  "/out:$exeOut",
  "/win32manifest:$manifest",
  "/resource:$payloadZip,payload.zip",
  '/r:System.dll',
  '/r:System.Core.dll',
  '/r:System.Drawing.dll',
  '/r:System.Windows.Forms.dll',
  '/r:System.IO.Compression.dll',
  '/r:System.IO.Compression.FileSystem.dll'
)
if (Test-Path $iconPath) { $cscArgs += "/win32icon:$iconPath" }
$cscArgs += $source

Write-Host "Building single-file installer $exeOut ..."
& $csc @cscArgs
if ($LASTEXITCODE) { throw 'C# compilation failed.' }

Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item $payloadZip -Force -ErrorAction SilentlyContinue

$sizeMb = [math]::Round((Get-Item $exeOut).Length / 1MB, 2)
Write-Host "Built single-file installer: $exeOut ($sizeMb MB)"
