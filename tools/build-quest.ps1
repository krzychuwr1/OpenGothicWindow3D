<#
.SYNOPSIS
  Builds the Meta Quest APK (OpenXR window mode) without Gradle and optionally installs it.

.DESCRIPTION
  Needs the Android SDK (build-tools 35, platform 34+), an NDK, CMake, Ninja, Python,
  glslangValidator, Vulkan headers 1.4.341 or newer and the OpenXR loader AAR
  (org.khronos.openxr:openxr_loader_for_android), extracted into toolchain/openxr-aar.

.EXAMPLE
  ./tools/build-quest.ps1 -Install
  ./tools/build-quest.ps1 -Install -GameDir 'D:\SteamLibrary\steamapps\common\Gothic II'
#>
param(
  [string]$AndroidSdk    = $(if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }),
  [string]$Ndk           = '',
  [string]$VulkanSdk     = "$PSScriptRoot\..\toolchain\vulkan",
  [string]$Glslang       = '',
  [ValidateSet('Release','RelWithDebInfo','Debug')]
  [string]$BuildType     = 'Release',
  [switch]$Install,
  [string]$GameDir       = '',
  [switch]$Launch
)
$ErrorActionPreference = 'Stop'
$root    = (Resolve-Path "$PSScriptRoot\..").Path
$build   = Join-Path $root 'build\quest'
$package = 'org.opengothic.window3d'

if (-not $Ndk) {
  $Ndk = Get-ChildItem (Join-Path $AndroidSdk 'ndk') -Directory | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}
$buildTools = Get-ChildItem (Join-Path $AndroidSdk 'build-tools') -Directory | Where-Object Name -match '^\d+\.\d+\.\d+$' |
  Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1 -ExpandProperty FullName
$platform = Get-ChildItem (Join-Path $AndroidSdk 'platforms') -Directory | Where-Object Name -match '^android-\d+$' |
  Sort-Object { [int]($_.Name -replace 'android-','') } -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $Glslang) {
  $cmd = Get-Command glslangValidator -ErrorAction SilentlyContinue
  if ($cmd) { $Glslang = Split-Path $cmd.Source }
  elseif (Test-Path (Join-Path $AndroidSdk 'emulator\lib64\vulkan\glslangValidator.exe')) { $Glslang = Join-Path $AndroidSdk 'emulator\lib64\vulkan' }
  else { throw 'glslangValidator not found: pass -Glslang <folder>' }
}
foreach ($p in @($Ndk, $buildTools, $platform, "$VulkanSdk\include\vulkan\vulkan_core.h", "$root\toolchain\openxr-aar\jni\arm64-v8a\libopenxr_loader.so")) {
  if (-not (Test-Path $p)) { throw "Missing prerequisite: $p" }
}

$env:VULKAN_SDK = (Resolve-Path $VulkanSdk).Path
$env:PATH       = "$Glslang;$env:PATH"

Write-Host "NDK $Ndk`nBuild tools $buildTools`nPlatform $platform"
cmake -S $root -B $build -G Ninja `
  "-DCMAKE_TOOLCHAIN_FILE=$Ndk/build/cmake/android.toolchain.cmake" `
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-32 `
  "-DCMAKE_BUILD_TYPE=$BuildType" -DTEMPEST_ANDROID_GRADLE_BUILD=ON -DOPENGOTHIC_XR=ON
if ($LASTEXITCODE) { throw 'CMake configure failed' }
cmake --build $build --target Gothic2Notr
if ($LASTEXITCODE) { throw 'Native build failed' }

$stage = Join-Path $build 'apk'
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory $stage | Out-Null
$strip = Join-Path $Ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe'
& $strip --strip-unneeded -o "$stage\libGothic2Notr.so" "$build\opengothic\libGothic2Notr.so"
Copy-Item "$root\toolchain\openxr-aar\jni\arm64-v8a\libopenxr_loader.so" $stage

$unsigned = "$stage\unsigned.apk"
& "$buildTools\aapt2.exe" link -o $unsigned --manifest "$root\android\quest\AndroidManifest.xml" -I "$platform\android.jar"
if ($LASTEXITCODE) { throw 'aapt2 link failed' }
python "$root\android\quest\add-libs.py" $unsigned "$stage\libGothic2Notr.so" "$stage\libopenxr_loader.so"
if ($LASTEXITCODE) { throw 'Adding native libraries failed' }
& "$buildTools\zipalign.exe" -f -P 16 4 $unsigned "$stage\aligned.apk"
if ($LASTEXITCODE) { throw 'zipalign failed' }

$apk = Join-Path $build 'OpenGothicWindow3D.apk'
$keystore = "$env:USERPROFILE\.android\debug.keystore"
if (-not (Test-Path $keystore)) { throw "Debug keystore not found: $keystore (create one with Android Studio or keytool)" }
& "$buildTools\apksigner.bat" sign --ks $keystore --ks-pass pass:android --key-pass pass:android --out $apk "$stage\aligned.apk"
if ($LASTEXITCODE) { throw 'apksigner failed' }
Write-Host "APK: $apk"

if ($Install -or $Launch) {
  $questSerial = & (Join-Path $PSScriptRoot 'install-quest.ps1') -GetQuestSerial
}
if ($Install) {
  adb -s $questSerial install -r $apk
  if ($LASTEXITCODE) { throw 'adb -s $questSerial install failed' }
  # Read access to the shared game folder (/sdcard/OpenGothic)
  adb -s $questSerial shell appops set $package MANAGE_EXTERNAL_STORAGE allow
}
if ($GameDir) {
  & "$PSScriptRoot\push-game-data.ps1" -GameDir $GameDir
}
if ($Launch) {
  adb -s $questSerial shell am start -n "$package/android.app.NativeActivity"
}
