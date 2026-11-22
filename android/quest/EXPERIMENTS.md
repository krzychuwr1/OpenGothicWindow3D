# XR experiments: Application SpaceWarp and multiview

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

**Motion vectors**

1. Camera pass: a full-screen pass at 400x225 per eye reconstructs each pixel's world position
   from the scene depth and projects it with that eye's previous submitted view-projection
   (`Renderer::drawSpaceWarp`). About 0.1 ms per eye: one depth read and two matrix
   products per pixel, no geometry. It also writes the depth image.
2. Object pass: moving objects are drawn again into the same images, depth-tested against that
   depth, with a material shader variant (`MOTION_VECTORS` in `materials/main.vert`,
   `materials/motion.frag`) that skins and transforms every vertex twice: with this frame's and
   with the previous frame's instance and bone matrices. The previous ones come from a copy of
   the instance buffer taken each frame before the new data lands (`InstanceStorage::snapshotPrev`).
   - Characters and animated meshes are always drawn.
   - Movable objects are drawn only if their transform changed since the previous frame (checked
     at the start of the vertex shader), so carried weapons, shields, torches and opening doors
     get vectors while the hundreds of props in a town are skipped.
   - Cost at the low test clocks in Khorinis: about 1.1 ms per eye (0.84 ms with characters only,
     1.7 ms if every movable prop were drawn).

**Limitations**

- The object pass samples the previous frame's morph descriptors and respects the visible
  material's alpha cutoff. Newly allocated instances fall back to camera motion.
- Distant NPCs are posed every 2nd or 4th frame (animation level of detail), so their vectors are
  zero on frames without an update.
- Particles carry no vectors.

**Checking the vectors:** `-show motion` paints each eye's motion vectors over the dimmed picture
(right red, up green, left and down blue) and `bench-quest.ps1 -Scenario combat -EyeCaptures 2`
saves both eyes' pictures. Moving objects are tested against the camera pass's depth, which is
sampled at motion-vector resolution; their surfaces are pulled 3% towards the camera for that test
(without it their vectors came out in vertical stripes). Weapons swung by characters get their own
vectors.

## Multiview

Measured with an experimental multiview path (two-layer targets and a view
mask in Tempest): the Adreno 740 draws the G-buffer for both eyes in 4.3 ms against 4.6-5.0 ms for
two passes, and sharing culling needs an occlusion test valid for both eyes. A full implementation
would gain about 1-1.5 ms per frame for a change across most of the renderer, so it is not enabled.
Numbers, method and what it would take: [MULTIVIEW.md](MULTIVIEW.md).
