# Quest audit on astra-audit

The starting revisions were OpenGothic `341775f4`, Tempest `a09928eb` and dmusic `7fe15296`. Device checks and source findings below are separate.

## Vulkan image handoff

Tempest's barrier translation independently resolved the default image layout. It transitioned borrowed eye colour, motion and depth images to shader-read layouts when finalizing the command buffer. OpenXR expects attachment layouts at release. Tempest commit `2021a6d7` uses the texture's declared default layout for these barriers and retains present layout for native presentation swapchains. It also checks timestamp query pool creation before recording a reset. Verified by tracing the barrier and texture paths; runtime validation-layer coverage is outstanding.

## Device conditions

The initial Quest 3 check found the app stopped, no benchmark arguments file, 55% battery and 41.0 °C. All benchmark scenes use low clocks.

The first installed audit APK ran the main-street benchmark at 57.9 fps and 7.85 GPU ms/view. VrApi showed CPU 1 / GPU 0 (1920/640 MHz). Cleanup completed at 58%, 40.0 °C. This is a smoke run, outside the paired optimisation measurements.

## Swapchain failure paths

The old format fallback selected an arbitrary format for an RGBA8 view. The static uploader could copy to a null mapping or wait forever after a failed submit, and the compositor could receive an uninitialized chrome layer. Image formats, upload allocation, submission, fence completion and static image release now gate use. A failed wait on a game image requests exit before exposing it to the renderer. These allocation and runtime failures have not been injected.

## SpaceWarp and stereo

The fork projected the old game camera through the *current* eye. A stationary camera could therefore report zero motion as the wearer leaned. Both vector passes now use each eye's previous full projection, including portal movement. History starts afresh after a missed warp frame, loss of focus or world change. RGB uses current minus previous NDC, Vulkan depth runs from 0 to 1 and the camera's 100,000 cm far plane is submitted as 1,000 m. Both compositor inputs request sampled usage. [Khronos SpaceWarp definition](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerSpaceWarpInfoFB.html)

Object vectors fall back to the camera pass when the instance buffer grows, and avoid reading an old skeleton binding after replacement. Previously, current indices could exceed the snapshot. Points behind the previous eye no longer divide by a nonpositive clip W. At native resolution the inventory now uses a separate depth target while warp is active. HQ and epipolar fog recompute their screen-space occlusion for the second eye. The low-quality fog volume still shares the first eye's approximation.

Physical head-motion quality, HUD over moving geometry, morph deformation, recycled instance identities and alpha-tested moving silhouettes remain visual risks.

## Focus and lifetime details

- The old visibility check could begin a frame on the transition to visible and skip its end. Events now precede the focus decision, every successful begin has an end, and hidden frames submit zero layers.
- Focus loss clears held game input, mouse motion and the active window grab. The simulation clock advances while suspended so resume avoids a large first tick. A disconnected pad now releases its virtual keys. A new grip requires release after recenter or focus loss.
- LOCAL reference-space changes take effect at their announced display time. A pending change used to move the window in the old space and could leave an active grab pulling it back.
- The XR singleton previously survived device destruction. Its owner now tears it down after the game resources, while Vulkan is available. Borrowed views are destroyed before the runtime frees the corresponding images.
- These paths were traced in source. Physical recenter, controller disconnect, sleep with a held grip and an orderly session exit require device or operator coverage.
- Tempest cached Bluetooth axes and mouse buttons across Android focus loss. Commit `ca58791c` resets those caches and releases outstanding touches. The pad mapper now keeps Escape held when either of its two bindings remains down, and the right bumper confirms in menus as documented.
- Tempest `2eb0e229` routes virtual controller keys through the focused widget, including modal overlays. Digital gamepad D-pad and L2/R2 input now combine with hats and analog triggers. Keyboard repeats reach Tempest's repeat handler, Caps Lock affects letter case independently of Shift, and synthetic keys with missing scan codes have distinct identities. Android focus loss discards stale key and mouse captures. The Android input code compiled and linked. Physical Bluetooth disconnect, keyboard layout handling beyond the built-in US mapping, modal navigation and uncaptured mouse motion at the screen edge need operator testing.
- The only dmusic submodule change acquires its loader mutex with both plain and recursive flags. This agrees with the installed NDK's flag definitions and introduces no new ownership. Music playback was not separately verified.

## Optimisation trials

The first valid main-street profile showed 2.24 ms/view in the G-buffer, 1.68 ms/view in combined lighting and sky, and 5.46 ms/frame in CPU recording. A candidate replaced five scene-global compute copies per eye with fence-protected host-visible buffers. Both variants compiled. During baseline collection the runtime reported a different waypoint (`NW_TAVERNE`) and unsolicited enemy-spawn commands. The nominal 47.1 fps and 10.59 ms/view from that run are invalid for comparison. The game was force-stopped and the sleep override released, with 59% battery at 42.0 °C. Further device use is paused pending exclusive access. There is no valid paired result, and the candidate was removed from the branch.

## VR surroundings

A 2048 × 1024 opaque sRGB panorama is generated and uploaded once on the first rendered frame. Two periodic mountain ridges, a dusky horizon, a dark foreground and seeded stars use no external assets. An infinite-radius LOCAL-space compositor layer goes behind both eye quads or the SpaceWarp projection. The compositor can use either KHR equirectangular extension. This path adds zero application draw calls per frame. Compositor cost has not been measured.

The capsule beneath the handle has separate touch and pointing hit zones. A pinch or grip press changes the environment once, pauses or starts the passthrough feature and flushes the selected mode to the app's INI. The crescent enters VR and the window icon restores passthrough. The switch is exposed only after both backgrounds and its texture are available.

The exact panorama generator was compiled and run independently on Windows. Its output was repeatable and fully opaque at the intended resolution. The 2:1 host preview showed the star field above both silhouettes and a continuous horizon. In-headset appearance, compositor acceptance with warp on/off, physical gestures, setting survival after restart and compositor timing remain unverified.
