# OpenGothic Window3D for Meta Quest

Gothic II: Night of the Raven in a window floating in your room, with passthrough around it. The
window is a head-tracked stereo portal: each eye sees the game world through it from its own
position, at life size, so leaning and moving your head reveal depth and parallax as through a real
window. Play with a Bluetooth keyboard and mouse, an Xbox-style gamepad or the Quest controllers.

Built on [OpenGothic](https://github.com/Try/OpenGothic). The app contains no game data: you need
your own copy of Gothic II Gold Edition / Night of the Raven (Steam or GOG).

## Install (players)

1. Enable developer mode for the headset in the Meta Horizon phone app.
2. Download `OpenGothicWindow3D.apk` and `install-quest.ps1` into one folder.
3. Download [Android SDK Platform-Tools](https://developer.android.com/tools/releases/platform-tools)
   and unzip it into the same folder (so that `platform-tools\adb.exe` exists).
4. Connect the headset with a USB cable, put it on and allow USB debugging.
5. Run in PowerShell:

   ```powershell
   ./install-quest.ps1
   ```

   It installs the app, finds Gothic II in your Steam libraries or GOG installation and copies
   `Data`, `_work` and `System` (about 3 GB) to `/sdcard/OpenGothic/Gothic2` on the headset. If the
   game is elsewhere, pass `-GameDir "path\to\Gothic II"`.
6. On the headset: Library > Unknown Sources > OpenGothic Window3D.

Uninstalling the app keeps the game files; delete `/sdcard/OpenGothic` to remove them.

## The window

- **Move:** point at the handle below the window and pinch (hand tracking) or squeeze grip
  (controllers), or grab it directly. The window follows your hand and turns to face you. With a
  controller, push or pull the thumbstick while holding to move it away or closer.
- **Resize:** reach for a bottom corner until a bracket appears, then grab and drag.
- **Switch surroundings:** point at the crescent button below the handle and pinch or squeeze the controller grip to enter a quiet dusk landscape. The window icon brings back passthrough. The choice is saved in `INTERNAL/xrEnvironment` (0 for passthrough, 1 for VR).
- **Recenter:** hold the Meta button; the window comes back in front of you.
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

Grip and pinch are reserved for moving and resizing the window.

## Settings

The Quest build starts with lighter graphics than the PC version (3D at 75% resolution, no
sunshafts, screen-space reflections or SSAO, 1024 shadow maps); the in-game video options still
change them. Settings and saves stay in the app's own storage.

**Application SpaceWarp** (Video settings, experimental, Off by default): the game renders every
other display frame and the headset synthesises the rest from motion vectors and depth, so motion
in the window looks like 72 fps while the game runs at 36. Details and limitations are in
[EXPERIMENTS.md](EXPERIMENTS.md).

## Build from source

Requirements: Android SDK with build-tools and a platform of API 34 or newer, NDK 27 or newer,
CMake 3.22+, Ninja, Python 3, `glslangValidator` (the Android emulator package ships one), Vulkan
headers 1.4.341 or newer copied to `toolchain/vulkan/include`, and the OpenXR loader AAR
(`org.khronos.openxr:openxr_loader_for_android`) extracted to `toolchain/openxr-aar`.

```powershell
git clone --recursive <this repository>
./tools/build-quest.ps1 -AndroidSdk C:\Android -Install -GameDir 'D:\SteamLibrary\steamapps\common\Gothic II'
```

The script builds the native library, packages and signs the APK with the local debug key
(`%USERPROFILE%\.android\debug.keystore`) without Gradle, installs it and copies the game files.

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
