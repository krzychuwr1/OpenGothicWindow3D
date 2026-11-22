# Multiview experiment

Multiview draws both eyes in one render pass: one draw stream, a view index in the shaders and a
two-layer render target. On the Quest it mainly saves CPU recording and shared work before the
pixels; these measurements show how much of that applies to OpenGothic's renderer.

## Measurements

Quest 3, `NW_CITY_MAINSTREET_02`, CPU level 1 / GPU level 0 (`bench-quest.ps1`), GPU time per eye
averaged over both eyes' command buffers.

| Variant (`-skip ...`) | GPU ms per eye | fps |
| --- | --- | --- |
| Baseline (two runs) | 7.96 / 7.86 | 61.1 / 60.3 |
| `eye1cull`: eye 1 reuses eye 0's culling (no HiZ, no visibility pass) | 7.62 | 63.2 |
| `eye1cull,eye1gbufps`: as above, eye 1's G-buffer without pixels (1x1 scissor) | 7.17 | 62.8 |
| `hizcull`: no HiZ occlusion in culling | 12.03 | 40.8 |
| `eye1cull,mvtest`: plus the G-buffer drawn once more with multiview (2 layers) | 9.70 | 49.6 |

What it shows:

- **Second-eye culling** (HiZ occluders, HiZ mips, visibility) costs about 0.6 ms per frame.
  Sharing it needs an occlusion test valid for both eyes: culling without HiZ makes the G-buffer
  pass three times as expensive (6.4 instead of 2.2 ms per eye), so both eyes' HiZ would have to be
  built and tested.
- **Eye 1's G-buffer** costs about 2.5 ms, of which about 1.6 ms is geometry (vertices, binning)
  and 0.85 ms pixels.
- **Multiview on the Adreno 740 shares little of that geometry work.** The multiview G-buffer pass
  (both layers, depth cleared, as in the `eye1cull` second eye) takes 2 x 2.14 = 4.3 ms per frame,
  against about 5.0 ms for two such passes, or 4.6 ms with eye 0's occluder depth. The saving is
  0.4-0.7 ms per frame: the GPU still shades vertices and bins primitives per view.

Estimated gain of a full implementation: 0.4-0.7 ms (G-buffer) + up to ~0.5 ms (culling once, after
paying for a two-eye HiZ test) + ~0.3 ms CPU recording, i.e. about 1-1.5 ms per frame (5-7% at 60
fps).

## What a full implementation needs

- Two-layer G-buffer and depth (already in the code: `LayeredTargets`, Tempest
  `vulkanBorrowLayeredAttachment/Depth` and `vulkanSetViewMask`), plus single-layer views of the
  same images for every later per-eye pass (the renderer uses the G-buffer and depth in about 150
  places), sharing Tempest's hazard tracking between the views of one image.
- Scene uniforms with the other eye's matrices and material vertex shaders that pick them by
  `gl_ViewIndex` for the occluder and G-buffer passes.
- Culling once for both eyes: HiZ per eye and a visibility shader that keeps a meshlet visible in
  either eye (frustum and HiZ of both), so the shared draw list is correct for both.
- The second eye's command buffer skipping culling, occluders and G-buffer.

Not done: for about 5% the change is large and touches most of the renderer. Application SpaceWarp
(halving the rendered frame rate) is the far bigger lever.

## Development options

`-skip eye1cull`, `-skip eye1cull,eye1gbufps`, `-skip hizcull` and `-skip mvtest` (multiview timing
pass in eye 0) reproduce the table; they draw wrong pictures and are for measurement only.
