# XR experiments (branch `xr-multiview-spacewarp`)

Measurements are from Meta Quest 3 with `tools/bench-quest.ps1` (new game, hero teleported to a
waypoint at noon, `-gpuprofile`). Frame rates vary a few fps between runs; compare runs made at
the same clock level (`-Perf`).

## Application SpaceWarp

**Setting:** Options > Video settings > Application SpaceWarp (Off by default). In XR builds the
graphics-device choice, which has no use on the headset, is repurposed for it. It is stored as
`[INTERNAL] xrSpaceWarp` in the app's `Gothic.ini`.

**How it works**

- Each eye renders, besides its colour image, a motion-vector image (RGBA16F) and a depth image
  (D32F) at a quarter of the window resolution (`Renderer::drawSpaceWarp`,
  `shader/xr/spacewarp_mv.frag`).
- The window is submitted as a projection layer instead of two quad layers. Each view's pose is
  the eye position with the window's orientation, and its asymmetric field of view frames exactly
  the window rectangle, so the image lands where the quad did (`XrWindow::Impl::locateEyes`).
- `XrCompositionLayerSpaceWarpInfoFB` carries the motion vectors and depth. The portal is life
  size, so game depth in centimetres is the real distance from the eye: `nearZ` is the eye's
  distance to the window, `farZ` 1000 m.
- The runtime then runs the game at half the display rate (36 of 72 Hz) and synthesises the
  other frames from the vectors and depth (VrApi: `FPS=36/72`, `VSnc=2`, `Stale=0`).

**Cost:** the vector pass is a full-screen pass at 400x225 per eye that reconstructs each pixel's
world position from the scene depth and projects it with the previous frame's game camera seen
through the current eye: about 0.1 ms per eye.

**Limitation:** vectors describe game-camera motion only. Moving characters, weapons and other
animated objects are treated as static, so in synthesised frames they keep their previous pose
(they effectively update at 36 fps). Per-object vectors need the G-buffer pass to output each
vertex's previous position, including skinned meshes with last frame's bone matrices; expected
cost roughly 0.5-1.5 ms per eye plus keeping previous skeletons.

## Multiview

Not implemented. Tempest has no layered render targets or multiview support, the renderer is
GPU-driven (indirect draws), and the XR path gives each eye its own frame slot. Multiview would
need layered targets and per-layer views in Tempest, a shared command buffer for both eyes, per
view matrices through `gl_ViewIndex` in the material shaders, and every later pass reading its
eye's layer.

**Upper bound measured** (`-skip eye1gbuffer` skips the second eye's G-buffer entirely; the image
is wrong, only the timing matters), city at noon, SpaceWarp off:

| | GPU ms per view | fps |
| --- | --- | --- |
| both eyes draw the G-buffer | 7.82-7.86 | 58-62 |
| second eye skips it | 6.52-6.59 | 56-70 |

So a perfectly free second-eye G-buffer would save about 2.6 ms per frame. Real multiview still
runs vertex and fragment work per view; on mobile GPUs it typically saves 10-30% of the geometry
pass and much of its CPU recording, which is already small here (about 0.2 ms per eye, because
draws are indirect). Expected gain: roughly 0.3-0.8 ms per frame, for a large renderer change.
