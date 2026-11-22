# OpenGothic Window3D

**Gothic II: Night of the Raven on Meta Quest, in a 3D window floating in your room.**

The game runs on the headset itself, no PC needed while playing. It is shown through a
head-tracked stereo window with passthrough around it: each eye looks through the window from its
own position, at life size, so leaning and moving your head reveal depth and parallax as if you
were looking through a real window into Khorinis. Move, resize and curve the window, switch to a
quiet VR landscape, and play with a Bluetooth keyboard and mouse, an Xbox-style gamepad or the
Quest controllers.

This is a fork of [OpenGothic](https://github.com/Try/OpenGothic), the open-source Gothic II
engine. **No game data is included:** you need your own copy of *Gothic II Gold Edition* /
*Night of the Raven* (Steam or GOG).

## Features

- Head-tracked stereo window into the game world, in passthrough or a VR dusk landscape
- Grab, move, resize and curve the window with hands or controllers; aspect lock, edge styles,
  room dimming
- The selected inventory item floats out of the window as a real 3D object
- Optional Application SpaceWarp (72 fps motion at half the rendering cost), foveated lighting and
  surface light that spills the game's colours onto your table or floor
- Keyboard and mouse, gamepad and Quest controller support
- Settings tuned for the Quest's mobile GPU

## Requirements

- Meta Quest 3 or 3S (developed and tested on Quest 3; Quest 2 and Pro are allowed by the app but
  untested)
- Developer mode enabled on the headset (free; needed to install apps from outside the Meta store)
- A Windows PC and a USB cable to install the app and copy the game files once
- Gothic II Gold Edition / Night of the Raven installed on that PC

## Install

1. Enable developer mode for the headset in the Meta Horizon phone app, connect the headset to the
   PC with a USB cable, put it on and allow USB debugging.
2. Download `OpenGothicWindow3D-Quest-Installer.exe` from the
   [latest release](https://github.com/krzychuwr1/OpenGothicWindow3D/releases/latest) and run it.
   It finds your Gothic II installation, installs the app and copies the game files (about 3 GB).
3. On the headset: Library > Unknown Sources > OpenGothic Window3D.

A PowerShell script is available too, and the full guide covers updating, uninstalling, the window
controls and the gamepad layout: **[Quest guide](android/quest/README.md)**.

## Building from source

```powershell
git clone --recursive -c core.longpaths=true https://github.com/krzychuwr1/OpenGothicWindow3D.git
cd OpenGothicWindow3D
./tools/fetch-quest-deps.ps1
./tools/build-quest.ps1 -Install
```

Requirements (Android SDK, NDK, CMake, Ninja, Python, glslangValidator) and options are in the
[Quest guide](android/quest/README.md#build-from-source). For desktop builds (Windows, Linux, macOS)
see the [upstream README](docs/OpenGothic.md); this fork is developed and tested for the Quest.

## Known limitations

- Experimental: expect bugs. Gothic II can be played, but not everything has been tested on the
  headset.
- Mods are untested on the headset. Those that need Ikarus/LeGo, Union or other memory hacks do
  not work in OpenGothic at all.
- Release APKs are signed with a development key. To switch between a release and your own build,
  uninstall the app first.

Bugs and ideas: [issues](https://github.com/krzychuwr1/OpenGothicWindow3D/issues). Problems that
also happen in desktop OpenGothic belong in the [upstream tracker](https://github.com/Try/OpenGothic/issues).

## Credits and license

Built on [OpenGothic](https://github.com/Try/OpenGothic) by Try and its contributors, with
[Tempest](https://github.com/Try/Tempest), [ZenKit](https://github.com/GothicKit/ZenKit),
[dmusic](https://github.com/GothicKit/dmusic), [Bullet](https://github.com/bulletphysics/bullet3)
and the [OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK). Released under the
[MIT License](LICENSE), like OpenGothic.

This is an unofficial fan project. It is not affiliated with or endorsed by THQ Nordic, Piranha
Bytes or Meta. Gothic is a trademark of its respective owners.
