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
