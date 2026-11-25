# OpenGothic Window3D for Meta Quest

Gothic II: Night of the Raven in a window floating in your room, with passthrough around it. The
window is a head-tracked stereo portal: each eye sees the game world through it from its own
position, at life size, so leaning and moving your head reveal depth and parallax as through a real
window. Play with a Bluetooth keyboard and mouse, an Xbox-style gamepad or the Quest controllers.

Built on [OpenGothic](https://github.com/Try/OpenGothic). The app contains no game data: you need
your own copy of Gothic II Gold Edition / Night of the Raven (Steam or GOG).

## Install (players)

You need a Windows PC, a USB cable for the headset and Gothic II Gold Edition / Night of the
Raven installed on the PC.

1. Enable developer mode for the headset in the Meta Horizon phone app (Devices > your headset >
   Headset settings > Developer mode). Meta asks you to register as a developer first; that is free.
2. Connect the headset to the PC with a USB cable, put it on and allow USB debugging.

3. Download `OpenGothicWindow3D-Quest-Installer.exe` from the
   [latest release](https://github.com/krzychuwr1/OpenGothicWindow3D/releases/latest) and run it.
   It is the only file you need: it contains the app and `adb`, finds Gothic II in your Steam
   libraries or GOG installation (or lets you pick the folder), installs the app and copies
   `Data`, `_work` and `System` (about 3 GB) to `/sdcard/OpenGothic/Gothic2` on the headset.
   Windows SmartScreen may warn about an unsigned program: choose More info > Run anyway.
4. On the headset: Library > Unknown Sources > OpenGothic Window3D.

Updating: run the new installer and tick *Update app only* to skip copying the game again.
Uninstalling the app keeps the game files; delete `/sdcard/OpenGothic` to remove them. Saves and
settings live in the app's own storage and are removed with the app.

If you switch between a release build and one you built yourself, uninstall the old app first:
they are signed with different keys and Android refuses to update one with the other.

## The window

- **Move:** point at the handle below the window and pinch (hand tracking) or squeeze grip
  (controllers), or grab it directly. The window follows your hand and turns to face you. With a
  controller, push or pull the thumbstick while holding to move it away or closer.
- **Resize:** reach for a bottom corner until a bracket appears, then grab and drag.
- **Recenter:** hold the Meta button; the window comes back in front of you.

A row of buttons appears below the handle when you point at it and hides again a few seconds
after you stop. Point at a button and pinch or squeeze grip to use it. From left to right:

- **Ghost hands:** shows your hands as dark outlines with pointing rays: off, automatically when
  you raise a hand after a few seconds without keyboard, mouse or pad, or always.
- **Curve:** curves the window around you like a curved monitor. The game is still seen in full
  3D through it, and SpaceWarp keeps working.
- **Edges:** square corners, rounded corners, or rounded with a soft edge.
- **Aspect lock:** keeps the window's aspect ratio while you resize it.
- **3D inventory:** the selected inventory item floats in front of the window as a 3D object.
  It has its own small handle, so you can move it where you like.
- **Surface light** (experimental): the game's colours fall on the table or floor under the
  window. The headset asks once for access to spatial data, which it needs to find the surface.
- **Surroundings:** the crescent switches to a quiet dusk landscape, the window icon brings back
  passthrough.
- **Dimming slider** (below the buttons): darkens the passthrough room, or the landscape in VR
  mode.
- The window opens at the distance where it shows the game's full field of view. Further away it
  frames less of the scene, as a real window would.

## Controls

Keyboard and mouse work as on the PC (default Gothic II keys).

| Gamepad (Xbox layout) | Quest controllers | In the world | In menus and dialogs |
| --- | --- | --- | --- |
| Left stick | Left stick | Move and strafe | Up / down / left / right |
| Right stick | Right stick | Turn the camera | |
| Right trigger / RB | Right trigger | Action / attack (Ctrl) | Confirm |
| Left trigger / LB | Left trigger | Walk | |
| A | A | Jump | Confirm |
| B | B | Draw / sheathe weapon | Back |
| X | X | Inventory | Inventory |
| Y | Y | Sneak | |
| Start | Menu | Game menu | |
| Back / View | | Log | |
| Left stick press | Left stick press | Status | |
| Right stick press | Right stick press | Map | |
| D-pad | | Move | Navigate |

Grip and pinch operate the handle, resize brackets and surroundings switch.

## Settings

The Quest build starts with lighter graphics than the PC version (3D at 75% resolution, no
sunshafts, screen-space reflections or SSAO, 1024 shadow maps); the in-game video options still
change them. Settings and saves stay in the app's own storage.

**Application SpaceWarp** (Video settings, experimental, Off by default): the game renders every
other display frame and the headset synthesises the rest from motion vectors and depth, so motion
in the window looks like 72 fps while the game runs at 36. Details and limitations are in
[EXPERIMENTS.md](EXPERIMENTS.md).

**Foveation** in Video settings is Off by default. On supported headsets it shades peripheral
sun, ambient and tiled point lighting at 2×2. A 25-degree cone around the direction your head faces
stays at full rate in each eye. Looking sideways with your eyes alone can reveal coarser lighting
at an edge. The setting works with both passthrough and the dusk environment, including SpaceWarp.
Use `-setting INTERNAL xrFoveation 1` for a single run.

## Build from source

The build runs on Windows. Requirements:

- Android SDK with build-tools and a platform of API 34 or newer, and NDK 27 or newer (Android
  Studio's SDK Manager installs them; `ANDROID_HOME` or the default `%LOCALAPPDATA%\Android\Sdk`
  is used, or pass `-AndroidSdk`)
- CMake 3.22+, Ninja and Python 3 on `PATH`
- `glslangValidator` on `PATH` (from the Vulkan SDK; the Android emulator package also ships one,
  which the script finds by itself)
- a debug keystore in `%USERPROFILE%\.android\debug.keystore` (Android Studio creates it; or
  `keytool -genkeypair -keystore %USERPROFILE%\.android\debug.keystore -alias androiddebugkey
  -storepass android -keypass android -keyalg RSA -validity 10000 -dname "CN=Android Debug"`)

```powershell
git clone --recursive -c core.longpaths=true https://github.com/krzychuwr1/OpenGothicWindow3D.git
cd OpenGothicWindow3D
./tools/fetch-quest-deps.ps1     # Vulkan headers and the OpenXR loader into toolchain/
./tools/build-quest.ps1 -Install -GameDir 'E:\Games\Gothic II'
```

Without `-Install`, `-GameDir` or `-Launch`, `build-quest.ps1` only builds the APK and needs no
headset connected.

`fetch-quest-deps.ps1` downloads the Vulkan headers (KhronosGroup on GitHub) and the
`org.khronos.openxr:openxr_loader_for_android` AAR (Maven Central) into `toolchain/`, which git
ignores. `build-quest.ps1` builds the native library, packages and signs the APK with the debug
key without Gradle (`build/quest/OpenGothicWindow3D.apk`), and with `-Install` / `-GameDir` installs
it and copies the game files. `-Variant pl` builds a second package, `org.opengothic.window3d.pl`,
that reads `/sdcard/OpenGothic/gothic2_pl`, so English and Polish game data can be installed side
by side (copy them with `tools/push-game-data.ps1 -GameDir ... -Subdir gothic2_pl -Package org.opengothic.window3d.pl`).

`tools/quest-installer/build-installer.ps1` then packs the APK and `adb` into the single-file
Windows installer (`build/quest-installer/`), compiled with the C# compiler that ships with
Windows.

Without the installer, `tools/install-quest.ps1` installs an APK and copies the game files from
the command line (`adb` on `PATH` or in a `platform-tools` folder next to the script):

```powershell
./tools/install-quest.ps1 -Apk build\quest\OpenGothicWindow3D.apk
./tools/install-quest.ps1 -Apk build\quest\OpenGothicWindow3D.apk -SkipGameFiles   # update the app only
```

### Other Android headsets

By default the scripts and the installer use exactly one connected Quest. For other Android
OpenXR headsets (untested), `install-quest.ps1`, `push-game-data.ps1` and `build-quest.ps1` take:

- `-AnyDevice` to use the only authorized adb device, or `-Serial <serial>` (as listed by
  `adb devices`) to pick one. `ANDROID_SERIAL` still has to name a Quest unless `-AnyDevice` is
  given.
- `-GameTarget Shared` (default): game files go to `/sdcard/OpenGothic/<subdir>` and the app gets
  all-files access through `appops`. `-GameTarget App`: they go to
  `/sdcard/Android/data/<package>/files/<subdir>`, which the app reads first and which needs no
  permission. Meta Quest keeps files that adb pushes there unreadable to the app, so use `Shared`
  on Quest.
- `install-quest.ps1 -SkipApp` copies only the game files.

```powershell
./tools/build-quest.ps1 -Install -AnyDevice -GameTarget App -GameDir 'E:\Games\Gothic II'
./tools/install-quest.ps1 -Serial 192.168.1.20:37045 -GameTarget App
```

The installer's **Advanced (other headsets)** option does the same in the GUI: pick any adb
device, pair and connect over wireless adb, choose where the game files go, copy game files
without installing the app, save the APK, and generate ready-to-run `.bat`/`.sh` copy scripts with
no headset connected.

## Developer options

Arguments for the Quest build are read from `/sdcard/OpenGothic/args.txt` (whitespace separated,
"quoted" for spaces):

| Option | Effect |
| --- | --- |
| `-nomenu` | Start a new game directly |
| `-novideo` | Skip videos |
| `-exec "cmd1;cmd2"` | Run console commands once the world has loaded, e.g. `goto waypoint NW_CITY_MAINSTREET_02;set time 12 0` |
| `-gpuprofile` | Log GPU time per render pass and CPU time per frame phase every 3 s |
| `-perf low` | Request the lowest CPU/GPU levels (unattended tests) |
| `-capture SECONDS` | Save both eyes' pictures as PNG files in the app's files folder |
| `-skip sun,ambient,lights,sky,tiled` | Leave lighting parts out, or use per-light volumes instead of tiles (profiling) |
| `-setting SECTION KEY VALUE` | Override a setting for this run only, e.g. `-setting INTERNAL xrSpaceWarp 1` |

`tools/bench-quest.ps1 -Waypoint <name>` runs a repeatable scene with these options and prints the
runtime's frame statistics and the game's breakdown. `-Scenario combat` or `-Scenario spells` adds
attackers (weapon swings, spell effects), `-EyeCaptures N` pulls the eyes' pictures every N seconds
into `captures/` and `-Captures N` saves headset screenshots.
