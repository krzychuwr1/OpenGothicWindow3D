# Quest audit on astra-audit

The starting revisions were OpenGothic `341775f4`, Tempest `a09928eb` and dmusic `7fe15296`. Device checks and source findings below are separate.

## Vulkan image handoff

Tempest's barrier translation independently resolved the default image layout. It transitioned borrowed eye colour, motion and depth images to shader-read layouts when finalizing the command buffer. OpenXR expects attachment layouts at release. Tempest commit `2021a6d7` uses the texture's declared default layout for these barriers and retains present layout for native presentation swapchains. It also checks timestamp query pool creation before recording a reset. Verified by tracing the barrier and texture paths; runtime validation-layer coverage is outstanding.

## Device conditions

The initial Quest 3 check found the app stopped, no benchmark arguments file, 55% battery and 41.0 °C. All benchmark scenes use low clocks.

## Session, focus and teardown

- The old visibility check could begin a frame on the transition to visible and skip its end. Events now precede the focus decision, every successful begin has an end, and hidden frames submit zero layers.
- Focus loss clears held game input, mouse motion and the active window grab. The simulation clock advances while suspended so resume avoids a large first tick. A disconnected pad now releases its virtual keys. A new grip requires release after recenter or focus loss.
- LOCAL reference-space changes take effect at their announced display time. A pending change used to move the window in the old space and could leave an active grab pulling it back.
- The XR singleton previously survived device destruction. Its owner now tears it down after the game resources, while Vulkan is available. Borrowed views are destroyed before the runtime frees the corresponding images.
- These paths were traced in source. Physical recenter, controller disconnect, sleep with a held grip and an orderly session exit require device or operator coverage.
