<#
.SYNOPSIS
  Downloads the two build dependencies of the Quest APK that are not in the repository:
  the Vulkan headers and the Khronos OpenXR loader for Android.

.DESCRIPTION
  - Vulkan-Headers (GitHub, KhronosGroup) -> toolchain/vulkan/include
  - org.khronos.openxr:openxr_loader_for_android (Maven Central) -> toolchain/openxr-aar

  Both folders are ignored by git. Existing folders are left alone unless -Force is given.
  Run once after cloning, then ./tools/build-quest.ps1.

.EXAMPLE
  ./tools/fetch-quest-deps.ps1
  ./tools/fetch-quest-deps.ps1 -Force
#>
param(
  [string]$VulkanHeaders = '1.4.341',
  [string]$OpenXR        = '1.1.63',
  [switch]$Force
)
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root      = (Resolve-Path "$PSScriptRoot\..").Path
$toolchain = Join-Path $root 'toolchain'
$tmp       = Join-Path ([IO.Path]::GetTempPath()) ("opengothic-deps-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $toolchain, $tmp | Out-Null

try {
  # Vulkan headers
  $vkDir = Join-Path $toolchain 'vulkan'
  if ($Force -or -not (Test-Path (Join-Path $vkDir 'include\vulkan\vulkan_core.h'))) {
    $zip = Join-Path $tmp 'vulkan-headers.zip'
    $url = "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v$VulkanHeaders.zip"
    Write-Host "Downloading Vulkan-Headers $VulkanHeaders ..."
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    $out = Join-Path $tmp 'vulkan-headers'
    [IO.Compression.ZipFile]::ExtractToDirectory($zip, $out)
    $src = Get-ChildItem $out -Directory | Select-Object -First 1
    Remove-Item $vkDir -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $vkDir | Out-Null
    Copy-Item (Join-Path $src.FullName 'include') $vkDir -Recurse
    Write-Host "  -> $vkDir\include"
  } else {
    Write-Host "Vulkan headers already in $vkDir (use -Force to replace)"
  }

  # OpenXR loader for Android (an .aar is a zip archive)
  $xrDir = Join-Path $toolchain 'openxr-aar'
  if ($Force -or -not (Test-Path (Join-Path $xrDir 'jni\arm64-v8a\libopenxr_loader.so'))) {
    $base = "https://repo1.maven.org/maven2/org/khronos/openxr/openxr_loader_for_android/$OpenXR/openxr_loader_for_android-$OpenXR.aar"
    $aar  = Join-Path $tmp 'openxr_loader.aar'
    Write-Host "Downloading OpenXR loader $OpenXR ..."
    Invoke-WebRequest -Uri $base -OutFile $aar -UseBasicParsing
    $expected = (Invoke-WebRequest -Uri "$base.sha1" -UseBasicParsing).Content
    if ($expected -is [byte[]]) { $expected = [Text.Encoding]::ASCII.GetString($expected) }
    $expected = $expected.Trim().Split(' ')[0]
    $actual   = (Get-FileHash $aar -Algorithm SHA1).Hash
    if ($actual -ne $expected) { throw "OpenXR loader checksum mismatch: $actual, expected $expected" }
    Remove-Item $xrDir -Recurse -Force -ErrorAction SilentlyContinue
    [IO.Compression.ZipFile]::ExtractToDirectory($aar, $xrDir)
    Write-Host "  -> $xrDir"
  } else {
    Write-Host "OpenXR loader already in $xrDir (use -Force to replace)"
  }
}
finally {
  Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
Write-Host 'Done. Next: ./tools/build-quest.ps1'
