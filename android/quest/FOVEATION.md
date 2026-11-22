# Foveated rendering: research notes

**Video settings → Foveation** implements the 25-degree full-rate region and 2×2 peripheral
lighting. It defaults to Off. At CPU/GPU levels 1/0 this saved 1.46 ms of GPU time per stereo
frame on the main street and 3.49 ms in the tavern. The proposals below describe the original
research.

## What the Quest 3 offers (queried on the headset, `-gpuprofile` logs it)

| Feature | Quest 3 |
| --- | --- |
| `XR_FB_foveation`, `XR_FB_foveation_vulkan`, `XR_FB_foveation_configuration`, `XR_FB_swapchain_update_state` | available: the runtime gives each swapchain image a fragment density map (fixed levels, low to high) |
| `XR_META_foveation_eye_tracked` | not available (no eye tracking on Quest 3) |
| `VK_EXT_fragment_density_map`, `..._map2`, `VK_QCOM_fragment_density_map_offset` | available |
| `VK_KHR_fragment_shading_rate` (variable rate shading) | pipeline, primitive and attachment rates; 8x8 pixel rate tiles; coarse pixels up to 4x4 |

## Why the runtime's fixed foveation would not help much here

- It only affects passes that render into the swapchain image with the density map attached. In
  this renderer that is the final tonemapping and the interface. The G-buffer, the lighting, water,
  fog and translucency render into internal targets (75% resolution by default) and would be
  unaffected.
- Its density map assumes an eye buffer: low density at the image edges. Our swapchain image is the
  window, which floats in the room; its edges are not the edges of the display. Fixed foveation
  would blur the window's border wherever the window is, including when you look straight at it.
- Density maps on our own internal targets turn them into subsampled images, which the deferred
  passes could no longer read with `texelFetch` (subsampled images need special samplers). That is
  a rework of every pass that reads the G-buffer.

## The practical route: variable rate shading from where the window appears

A shading-rate image per eye (one texel per 8x8 pixels: about 150x85 texels at the internal
resolution) says how coarsely each part of the window is shaded. It can be computed every frame
from the geometry we already have: each tile's direction from the eye against the head's forward
direction. Tiles within ~25 degrees of the view centre shade every pixel; further out 2x2, and
beyond ~40 degrees 4x2 or 4x4. The lenses and the eye resolve much less detail there, and the head
usually turns towards what you look at.

Where it pays: the full-screen lighting passes. Point lights, sun and ambient cost 1.7 ms per eye
in the city and up to 4.5 ms per eye in the countryside (`-gpuprofile`), plus about 0.85 ms of the
G-buffer pass is pixel work. If the outer half of the window shades at 2x2, those passes cost about
62% of today: roughly 1.3-3.4 ms per frame saved, more when the window is close and fills more of
the view.

Costs and risks:

- Coarse shading of deferred lighting computes light once per 2x2 block from one G-buffer sample:
  lit edges get blocky in the coarse region. Limiting it to the outer region and to the lighting
  passes (not the G-buffer, not the tonemapping) keeps edges in the centre crisp.
- Looking at the window's edge with the eyes only (head still) shows the coarser shading.
- Tempest needs: the attachment shading-rate feature at device creation, a shading-rate attachment
  in the render passes (`VkRenderingFragmentShadingRateAttachmentInfoKHR`), the combiner state in
  the lighting pipelines, and an R8 image with the shading-rate usage, written by a tiny compute
  pass or from the CPU.

Suggested order:

1. Upper bound: run the lighting passes at a pipeline rate of 2x2 for the whole window
   (`vkCmdSetFragmentShadingRateKHR`) and measure with `bench-quest.ps1` in the city and the
   countryside. If it saves less than about 1 ms per frame, stop.
2. The shading-rate image from window geometry, attached to the lighting passes only, as a video
   setting (Off / On), off by default until it has been looked at on the headset.

A different lever with no visible artefacts: the window's resolution could follow how large the
window appears (pixels per degree). At the default distance the 1600x900 window spans the game's
field of view, which is already below the display's ~25 pixels per degree; when the window is moved
further away, fewer pixels would do.
