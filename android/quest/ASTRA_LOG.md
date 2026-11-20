# Quest audit on astra-audit

The starting revisions were OpenGothic `341775f4`, Tempest `a09928eb` and dmusic `7fe15296`. Device checks and source findings below are separate.

## Vulkan image handoff

Tempest's barrier translation independently resolved the default image layout. It transitioned borrowed eye colour, motion and depth images to shader-read layouts when finalizing the command buffer. OpenXR expects attachment layouts at release. Tempest commit `2021a6d7` uses the texture's declared default layout for these barriers and retains present layout for native presentation swapchains. It also checks timestamp query pool creation before recording a reset. Verified by tracing the barrier and texture paths; runtime validation-layer coverage is outstanding.

## Device conditions

The initial Quest 3 check found the app stopped, no benchmark arguments file, 55% battery and 41.0 °C. All benchmark scenes use low clocks.

The first installed audit APK ran the main-street benchmark at 57.9 fps and 7.85 GPU ms/view. VrApi showed CPU 1 / GPU 0 (1920/640 MHz). Cleanup completed at 58%, 40.0 °C. This is a smoke run, outside the paired optimisation measurements.

## Swapchain failure paths

The old format fallback selected an arbitrary format for an RGBA8 view. The static uploader could copy to a null mapping or wait forever after a failed submit, and the compositor could receive an uninitialized chrome layer. Image formats, upload allocation, submission, fence completion and static image release now gate use. A failed wait on a game image requests exit before exposing it to the renderer. These allocation and runtime failures have not been injected.

Resource discovery now checks both image enumeration calls. Failure while building an optional warp pair releases its partial swapchains and views. Required reference spaces and Vulkan requirements must succeed before use. At shutdown the Tempest device waits under its queue locks. A failed game-image release suppresses the associated game layers and requests exit after attempting the remaining releases.

## SpaceWarp and stereo

The fork projected the old game camera through the *current* eye. A stationary camera could therefore report zero motion as the wearer leaned. Both vector passes now use each eye's previous full projection, including portal movement. History starts afresh after a missed warp frame, loss of focus or world change. RGB uses current minus previous NDC, Vulkan depth runs from 0 to 1 and the camera's 100,000 cm far plane is submitted as 1,000 m. Both compositor inputs request sampled usage. [Khronos SpaceWarp definition](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerSpaceWarpInfoFB.html)

Object vectors fall back to the camera pass when the instance buffer grows, and avoid reading an old skeleton binding after replacement. Previously, current indices could exceed the snapshot. Points behind the previous eye no longer divide by a nonpositive clip W. At native resolution the inventory now uses a separate depth target while warp is active. HQ and epipolar fog recompute their screen-space occlusion for the second eye. The low-quality fog volume still shares the first eye's approximation.

Physical head-motion quality, HUD over moving geometry, morph deformation, recycled instance identities and alpha-tested moving silhouettes remain visual risks.

The far shadow cascade baked movable props into a cache keyed only by the light matrix. Doors and items could leave old shadows until the matrix changed. Both cascades now cache static geometry and draw movable geometry at their existing refresh cadence. Per-eye renderer flags and the active camera override now end with the eye recording, including exception paths. A later standalone screenshot previously inherited the second eye's pass suppression.

The fence, command buffer, particle, UI, light-list, instance patch and deferred-release arrays examined use `Resources::MaxFramesInFlight`. The fence precedes slot reuse. The second eye still runs shadow visibility work for cascades it does not draw. Skipping those dispatches is an unmeasured optimisation candidate.

## NPC animation

The world stores NPCs in a vector which it sorts by handle and erases from during play. Its separate array of accumulated animation times stayed in the old order. A removal or sort could give one character another character's elapsed time. Quest now keeps the accumulator on the NPC itself and staggers distant updates by stable NPC handle. Non-XR builds use the original per-frame update. Save formats are unchanged.

## Focus and lifetime details

- The old visibility check could begin a frame on the transition to visible and skip its end. Events now precede the focus decision, every successful begin has an end, and hidden frames submit zero layers.
- Android's paused event loop used an infinite Android-looper wait. OpenXR session events do not wake that looper. Tempest `8e97ac98` keeps a low-frequency external presentation callback available to drain STOPPING and loss events while paused. The game checks activity resume state before simulating, rendering or submitting visible layers.
- Focus loss clears held game input, mouse motion and the active window grab. The simulation clock advances while suspended so resume avoids a large first tick. A disconnected pad now releases its virtual keys. A new grip requires release after recenter or focus loss.
- LOCAL reference-space changes take effect at their announced display time. A pending change used to move the window in the old space and could leave an active grab pulling it back.
- The XR singleton previously survived device destruction. Its owner now tears it down after the game resources, while Vulkan is available. Borrowed views are destroyed before the runtime frees the corresponding images.
- These paths were traced in source. Physical recenter, controller disconnect, sleep with a held grip and an orderly session exit require device or operator coverage.
- Tempest cached Bluetooth axes and mouse buttons across Android focus loss. Commit `ca58791c` resets those caches and releases outstanding touches. The pad mapper now keeps Escape held when either of its two bindings remains down, and the right bumper confirms in menus as documented.
- Tempest `2eb0e229` routes virtual controller keys through the focused widget, including modal overlays. Digital gamepad D-pad and L2/R2 input now combine with hats and analog triggers. Keyboard repeats reach Tempest's repeat handler, Caps Lock affects letter case independently of Shift, and synthetic keys with missing scan codes have distinct identities. Android focus loss discards stale key and mouse captures. The Android input code compiled and linked. Physical Bluetooth disconnect, keyboard layout handling beyond the built-in US mapping, modal navigation and uncaptured mouse motion at the screen edge need operator testing.
- The only dmusic submodule change acquires its loader mutex with both plain and recursive flags. This agrees with the installed NDK's flag definitions and introduces no new ownership. Music playback was not separately verified.

## Optimisation trials

The first valid main-street profile showed 2.24 ms/view in the G-buffer, 1.68 ms/view in combined lighting and sky, and 5.46 ms/frame in CPU recording. A candidate replaced five scene-global compute copies per eye with fence-protected host-visible buffers. Both variants compiled. During baseline collection the runtime reported a different waypoint (`NW_TAVERNE`) and unsolicited enemy-spawn commands. The nominal 47.1 fps and 10.59 ms/view from that run are invalid for comparison. The game was force-stopped and the sleep override released, with 59% battery at 42.0 °C. Further device use is paused pending exclusive access. There is no valid paired result, and the candidate was removed from the branch.

The benchmark used to label any recent VrApi samples with the requested waypoint, overwrite existing developer arguments, and skip cleanup on interruption. It now checks battery, exclusivity among updated host runners, the live process, its reported startup commands and the argument file. Cleanup runs in `finally` and preserves a replaced argument file. Six host-side mock ADB cases passed (successful report, high temperature, existing arguments, wrong scene, replaced arguments and partial clock setup failure). Physical verification of the revised harness is pending. Other tools can still access the same device independently.

## VR surroundings

A 2048 × 1024 opaque sRGB panorama is generated and uploaded once on the first rendered frame. Two periodic mountain ridges, a dusky horizon, a dark foreground and seeded stars use no external assets. An infinite-radius LOCAL-space compositor layer goes behind both eye quads or the SpaceWarp projection. The compositor can use either KHR equirectangular extension. This path adds zero application draw calls per frame. Compositor cost has not been measured.

The capsule beneath the handle has separate touch and pointing hit zones. A pinch or grip press changes the environment once, pauses or starts the passthrough feature and flushes the selected mode to the app's INI. The crescent enters VR and the window icon restores passthrough. The switch is exposed only after both backgrounds and its texture are available.

The exact panorama generator was compiled and run independently on Windows. Its output was repeatable and fully opaque at the intended resolution. The 2:1 host preview showed the star field above both silhouettes and a continuous horizon. In-headset appearance, compositor acceptance with warp on/off, physical gestures, setting survival after restart and compositor timing remain unverified.

A host harness compiled the production hit testing, input update, environment switch and layer assembly with simulated OpenXR calls. Eight combinations covered room/sky, quads/warp and both panorama extension paths. Assertions checked background order, eye visibility, projection chains, button texture state and hidden-frame layer count. Near and ray presses from both hands produced one change and one settings flush, holding the grip did not repeat, and an injected passthrough-switch failure preserved the mode. An injected image-release failure suppressed the game layers and requested exit. These tests exercise the application's decisions without a compositor.

## Desktop build checks

The common renderer and frame timing code included native Vulkan headers even in Metal builds. Profiler declarations now use Tempest's ordinary Vulkan API header, and the XR report is compiled only for XR. MSVC also rejected the desktop XR stub's implicit return of an explicitly constructed Size. After fixing it, MSVC 2022 passed syntax checks for the window, XR stubs, renderer, world object update and game entry point without Vulkan SDK headers. This was a Windows compilation check, not a linked desktop or Apple run.
