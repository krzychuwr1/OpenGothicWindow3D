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

An offscreen Windows Vulkan test also ran the APK's full-screen camera-motion shader with a two-pixel depth texture and read back both its colour attachment and a D32F depth attachment. A stationary camera yielded zero vectors. A translated previous projection yielded the expected signed XYZ difference, and a point behind the previous eye yielded zero. All three cases preserved source depths of 0.2 and 0.8 to within one part per million. These checks ran on the host RTX GPU. Runtime extrapolation on Quest remains unverified.

A reference-space recenter could still extrapolate between the old window and its newly placed pose. The renderer now discards previous transforms on placement, and both projection views request the SpaceWarp frame-skip flag whenever the previous view is unusable. The next continuous frame resumes extrapolation. The host OpenXR harness checked initial placement, a future recenter before and at its effective display time, recovery on the following frame, and the flags on both eyes. The Android build linked. [Khronos SpaceWarp frame-skip flag](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerSpaceWarpInfoFlagBitsFB.html)

Moving alpha-tested meshes wrote object vectors through transparent texels. Their motion shaders now share the visible material's UV animation, alpha weight, texture and cutoff. The motion pipeline cache distinguishes opaque and cutout materials. Fifteen vertex and fragment variants passed SPIR-V validation. An offscreen Vulkan test ran the APK shaders on a Windows RTX 4080 Laptop GPU. With adjacent transparent and opaque texels, both the slot and bindless variants preserved the background motion under the transparent texel and wrote the expected XYZ vector on the opaque texel. The solid control wrote both pixels. The Quest build linked. In-game edges at the lower motion resolution still need visual review.

Physical head-motion quality, HUD over moving geometry, morph deformation and recycled instance identities remain visual risks.

The far shadow cascade baked movable props into a cache keyed only by the light matrix. Doors and items could leave old shadows until the matrix changed. Both cascades now cache static geometry and draw movable geometry at their existing refresh cadence. Per-eye renderer flags and the active camera override now end with the eye recording, including exception paths. A later standalone screenshot previously inherited the second eye's pass suppression.

A second shadow-cache issue was in indirect command batching. Static and movable props can use identical pipelines, so the first object inserted set the caster type for the entire batch. XR now batches by caster type as well. Static batches need only the camera motion pass, and desktop command creation no longer requests unused SpaceWarp pipelines. A host test compiled the production command selection with identical mock pipelines and checked both insertion orders, bindless and individual buckets, repeated reuse and desktop batching. The Quest APK linked. Visual verification of door shadows remains outstanding.

Wind animation moves mesh instances even when their world objects are marked static. On Quest these meshes now enter the movable batch so their shadows and object vectors follow their transforms. The shared cleanup path also erased the wind-update index under the opposite condition from insertion. Removing a windy object could leave an index past the end of the object vector on the next wind update. Host tests exercised removing middle and trailing entries for both wind modes, with XR and desktop caster classification. The Quest APK built. The wind cleanup defect predates the fork.

The fence, command buffer, particle, UI, light-list, instance patch and deferred-release arrays examined use `Resources::MaxFramesInFlight`. The fence precedes slot reuse. The second eye still runs shadow visibility work for cascades it does not draw. Skipping those dispatches is an unmeasured optimisation candidate.

The instance upload worker, used by the stereo frame loop, could sleep past a queued frame slot or shutdown request if the notification arrived before its first wait. Its caller then spun or hung while joining. Both sides now wait on the slot state under the same mutex, and destruction finishes a pending upload before requesting exit. A host harness ran the production synchronization methods with a gated worker and mock upload buffers. It completed 4,097 uploads across four slots, including an upload queued before the worker started and another pending at destruction. It also completed shutdown requested before the worker's first wait. The Quest APK linked successfully.

## NPC animation

The world stores NPCs in a vector which it sorts by handle and erases from during play. Its separate array of accumulated animation times stayed in the old order. A removal or sort could give one character another character's elapsed time. Quest now keeps the accumulator on the NPC itself and staggers distant updates by stable NPC handle. Non-XR builds use the original per-frame update. Save formats are unchanged.

## Focus and lifetime details

- The old visibility check could begin a frame on the transition to visible and skip its end. Events now precede the focus decision, every successful begin has an end, and hidden frames submit zero layers.
- Android's paused event loop used an infinite Android-looper wait. OpenXR session events do not wake that looper. Tempest `8e97ac98` keeps a low-frequency external presentation callback available to drain STOPPING and loss events while paused. The game checks activity resume state before simulating, rendering or submitting visible layers.
- Focus loss clears held game input, mouse motion and the active window grab. The simulation clock advances while suspended so resume avoids a large first tick. A disconnected pad now releases its virtual keys. A new grip requires release after recenter or focus loss.
- A pointed grab moved the window a fixed distance per application frame. Switching between 36 and 72 fps changed its travel speed by a factor of two, and a stationary thumbstick could snap the initial depth to the nearest limit. Distance now uses elapsed predicted display time at the original 72 Hz rate. Simulated one-second holds at 36, 72, 90 and 120 fps agreed to within 0.1 mm. A grab starting beyond the limit stayed still at rest and moved back smoothly when pulled. Physical feel is unverified.
- LOCAL reference-space changes take effect at their announced display time. A pending change used to move the window in the old space and could leave an active grab pulling it back.
- The XR singleton previously survived device destruction. Its owner now tears it down after the game resources, while Vulkan is available. Borrowed views are destroyed before the runtime frees the corresponding images.
- These paths were traced in source. Physical recenter, controller disconnect, sleep with a held grip and an orderly session exit require device or operator coverage.
- Tempest cached Bluetooth axes and mouse buttons across Android focus loss. Commit `ca58791c` resets those caches and releases outstanding touches. The pad mapper now keeps Escape held when either of its two bindings remains down, and the right bumper confirms in menus as documented.
- Tempest `2eb0e229` routes virtual controller keys through the focused widget, including modal overlays. Digital gamepad D-pad and L2/R2 input now combine with hats and analog triggers. Keyboard repeats reach Tempest's repeat handler, Caps Lock affects letter case independently of Shift, and synthetic keys with missing scan codes have distinct identities. Android focus loss discards stale key and mouse captures. The Android input code compiled and linked. Physical Bluetooth disconnect, keyboard layout handling beyond the built-in US mapping, modal navigation and uncaptured mouse motion at the screen edge need operator testing.
- A Bluetooth pad unplugged while focused could leave its last stick or button active indefinitely. Tempest `15c4f519` tracks the Android device IDs supplying the axes and keys. Fresh events prove presence, otherwise it checks the framework every half second while a device is cached. A host test of the production input code with a mock JNI device registry covered held inputs, shared and separate IDs, replacement, framework exceptions and local reference cleanup. The Android build linked. Physical disconnect and reconnect still need a controller.
- The only dmusic submodule change acquires its loader mutex with both plain and recursive flags. This agrees with the installed NDK's flag definitions and introduces no new ownership. Music playback was not separately verified.

## Optimisation trials

The first valid main-street profile showed 2.24 ms/view in the G-buffer, 1.68 ms/view in combined lighting and sky, and 5.46 ms/frame in CPU recording. A candidate replaced five scene-global compute copies per eye with fence-protected host-visible buffers. Both variants compiled. During baseline collection the runtime reported a different waypoint (`NW_TAVERNE`) and unsolicited enemy-spawn commands. The nominal 47.1 fps and 10.59 ms/view from that run are invalid for comparison. The game was force-stopped and the sleep override released, with 59% battery at 42.0 °C. Device use paused at that point. The owner subsequently confirmed the other workload and handed the headset over for exclusive tests. There is no valid paired result, and the candidate was removed from the branch.

The benchmark used to label any recent VrApi samples with the requested waypoint, overwrite existing developer arguments, and skip cleanup on interruption. It now checks battery, exclusivity among updated host runners, the live process, its reported startup commands and the argument file. Android shell noclobber protects an argument file created after preflight. Cleanup runs in `finally`, compares the entire file before removal and checks the launch PID before stopping the package. Eight host-side mock ADB cases passed (successful report, high temperature, existing arguments, wrong scene, appended arguments, partial clock setup failure, racing file creation and process replacement). Physical verification of the revised harness is pending. Other tools can still access the same device independently.

Tempest `4afd54ba` corrects the per-view denominator. The old profiler counted upload command buffers with only start and end timestamps as extra views. It also subtracted timestamps as if every queue supplied 64 valid bits. The collector now skips uploads and incomplete recordings, includes final resource barriers, and uses the graphics queue's valid timestamp width. A host test compiled the production collector and exercised mixed uploads, complete and truncated recordings, and wraparound at 32, 36 and 64 bits. A queue with zero valid bits disables timestamp recording. The Quest APK built successfully. The 7.85 ms/view smoke result above came from the old profiler. Future paired trials need a fresh baseline with the same collector in both builds.

## VR surroundings

A 2048 × 1024 opaque sRGB panorama is generated and uploaded once on the first rendered frame. Two periodic mountain ridges, a dusky horizon, a dark foreground and seeded stars use no external assets. An infinite-radius LOCAL-space compositor layer goes behind both eye quads or the SpaceWarp projection. The compositor can use either KHR equirectangular extension. This path adds zero application draw calls per frame. Compositor cost has not been measured.

The capsule beneath the handle has separate touch and pointing hit zones. A pinch or grip press changes the environment once, pauses or starts the passthrough feature and flushes the selected mode to the app's INI. The crescent enters VR and the window icon restores passthrough. The switch is exposed only after both backgrounds and its texture are available.

The exact panorama generator was compiled and run independently on Windows. Its output was repeatable and fully opaque at the intended resolution. The 2:1 host preview showed the star field above both silhouettes and a continuous horizon. In-headset appearance, compositor acceptance with warp on/off, physical gestures, setting survival after restart and compositor timing remain unverified.

A host harness compiled the production hit testing, input update, environment switch and layer assembly with simulated OpenXR calls. Eight combinations covered room/sky, quads/warp and both panorama extension paths. Assertions checked background order, eye visibility, projection chains, button texture state and hidden-frame layer count. Near and ray presses from both hands produced one change and one settings flush, holding the grip did not repeat, and an injected passthrough-switch failure preserved the mode. An injected image-release failure suppressed the game layers and requested exit. These tests exercise the application's decisions without a compositor.

## Desktop build checks

The common renderer and frame timing code included native Vulkan headers even in Metal builds. Profiler declarations now use Tempest's ordinary Vulkan API header, and the XR report is compiled only for XR. MSVC also rejected the desktop XR stub's implicit return of an explicitly constructed Size. After fixing it, MSVC 2022 passed syntax checks for the window, XR stubs, renderer, world object update and game entry point without Vulkan SDK headers. This was a Windows compilation check, not a linked desktop or Apple run.


## Branch handover and carry-over tags

The owner identified overlapping device work between 01:55 and 03:10. Treat earlier device timings as historical observations. The merged build starts its own baseline. The instance-slot history experiment was stashed before merging because its GPU readback check had failed.

These tags describe where the owner can carry each original audit commit. Commits tagged `[all]` that also touch SpaceWarp need only their shared hunks on master. The environment feature is assigned to the combined audit branch. The owner’s earlier commits keep their original subjects.

| OpenGothic commit | Tag | Subject |
| --- | --- | --- |
| `017fca32` | `[all]` | Update Tempest for the OpenXR attachment layout handoff |
| `6bd4bb76` | `[all]` | Pair XR frames across focus changes and release session resources in order |
| `1f5264f0` | `[all]` | Release stale Android input and honor shared gamepad key bindings |
| `93207acf` | `[all]` | Validate OpenXR image formats, waits and static uploads |
| `37e94807` | `[spacewarp]` | Use submitted eye projections and valid scene depth for SpaceWarp |
| `075e06b6` | `[astra]` | Add a persistent passthrough and procedural dusk environment switch |
| `986e425a` | `[all]` | Deliver Quest controller navigation to modal widgets |
| `5409f50e` | `[all]` | Verify the benchmark scene and clean up failed Quest runs |
| `0ec8d74f` | `[all]` | Keep deferred Quest animation time with its NPC across reordering |
| `62b73b63` | `[all]` | Keep moving casters out of static shadows and scope XR eye state to recording |
| `a1ee8851` | `[all]` | Service XR session transitions while Android is paused |
| `2aabfbe6` | `[all]` | Restore non-XR compilation without native Vulkan headers |
| `ff8b2d10` | `[all]` | Clean up incomplete XR targets and suppress failed image submissions |
| `42a3ad0e` | `[all]` | Protect competing launch arguments and process identity during Quest benchmarks |
| `16b46a91` | `[all]` | Use corrected Tempest GPU view timings for Quest benchmarks |
| `e8b48af1` | `[all]` | Observe pending instance uploads and shutdown before waiting |
| `eb36e314` | `[all]` | Track physical gamepad removal in the Android input backend |
| `8e984e12` | `[spacewarp]` | Skip SpaceWarp extrapolation across window placement changes |
| `85e0078b` | `[all]` | Keep pointed window travel consistent across frame rates |
| `84a1a8e9` | `[all]` | Keep static and moving shadow casters in separate XR draw commands |
| `d9a0a4e9` | `[spacewarp]` | Respect material cutouts when drawing SpaceWarp object motion |
| `260568f7` | `[spacewarp]` | Record Vulkan readback checks for SpaceWarp camera motion and depth |
| `87364e56` | `[all]` | Track wind-animated objects through shadow rendering and removal |

| Tempest commit | Tag | Subject |
| --- | --- | --- |
| `2021a6d7` | `[all]` | Honor imported attachment layouts in Vulkan barriers and check profile allocation |
| `ca58791c` | `[all]` | Reset Android controller and pointer state on focus loss |
| `2eb0e229` | `[all]` | Route Android controller keys through focused widgets and merge digital axes |
| `8e97ac98` | `[all]` | Pump external presentation lifecycle events while the Android activity is paused |
| `05f8b5c9` | `[all]` | Expose Vulkan profiler controls without requiring native Vulkan headers |
| `4afd54ba` | `[all]` | Count complete render recordings in GPU timing reports |
| `15c4f519` | `[all]` | Release cached Android gamepad input after device removal |

### Combined branch verification

Merged the owner's combined branch through `e5157773` and Tempest through `6b0ed3fa`. Tempest merge `2e699608` includes both parents. The merged XR frame loop continues pairing hidden frames. Focus markers use each eye's camera before the shared UI. The motion pass retains the owner's 3% depth bias. Ordinary SpaceWarp writes directly to the runtime targets. The motion overlay uses a sampleable scratch attachment and copies its values to the runtime target. The capture path reads a fenced eye image before releasing it, bounds background PNG writers and publishes each completed file by rename. The benchmark checks every requested spawn and collects new, complete stereo pairs.

The Android Release APK passed v3 signature verification and the 16 KiB alignment check. The Windows MSVC desktop syntax check passed for the window, XR stubs, layered targets, renderer, world objects and game entry point.

| Post-handover Quest 3 run, city at noon, CPU 1 / GPU 0 | VrApi fps | GPU busy | CPU & GPU | GPU per eye, last printed sample |
| --- | ---: | ---: | ---: | ---: |
| City, default settings, 14 samples | 58.4 | 97% | 23.8 ms | 8.22 ms |
| SpaceWarp, combat, stereo PNG every 3 s, 15 samples | 36.2 | 81% | 26.9 ms | 11.17 ms |

Decoded and inspected all 17 stereo pairs (1600 × 900 per eye, local capture prefix `20261120-031441-319`). Both eyes show the city gate, approaching enemies, melee and the subsequent street scene. Focus names appear over characters in both eyes and the HUD remains visible. These captures contain the application eye images. They do not measure compositor interpolation or confirm disparity quantitatively. The combat run includes readback cost. Battery after the two runs was 77% and 78%, both at 40.0 °C. The app was stopped after each run.

### Dusk compositor and motion overlay on Quest 3

Two further runs forced the dusk environment on the merged APK. The application logged `OpenXR: environment dusk panorama` in both. Meta's 1440 × 1440 screenshots show the star field above the window with SpaceWarp enabled (combat, capture prefix `20261120-031622-356`) and disabled (ordinary stereo quads, `20261120-031817-072`). The upper part of the backdrop is visible from the unattended headset pose. The screenshot framing excludes the physical toggle and the lower horizon. The two runs ended at 78% battery, 40.0 °C, with the app stopped.

For the SpaceWarp run, all 14 stereo pairs with `-show motion` decoded successfully. Both eyes show bright directional colours on moving characters against a darkened street. After combat the static scene goes dark again. The 3% object depth bias was active throughout. These images establish that the merged debug path displays motion. Wearer comfort, physical switch interaction, saved choice across restarts and compositor-only cost remain open.

### Avoiding unused second-eye shadow visibility lists

The standard second-eye render reuses both raster shadow maps, but still cleared and culled their meshlet lists. It now skips those two views during indirect setup and culling. Each eye still runs its own main/HiZ culling. VSM initialization and the separate path-tracing render path retain the full view set.

Main street at noon, default passthrough, no captures, 14 VrApi samples per run, CPU 1 / GPU 0. GPU and record times below average the final four complete profiler reports from each app log. The same baseline APK was reinstalled for the final control.

| Run | Visibility ms/eye | GPU ms/eye | Record ms/frame | VrApi fps |
| --- | ---: | ---: | ---: | ---: |
| Baseline 1 | 0.220 | 8.198 | 5.598 | 58.4 |
| Candidate 1 | 0.198 | 8.155 | 5.700 | 55.7 |
| Candidate 2 | 0.180 | 8.165 | 5.612 | 57.9 |
| Baseline 2 | 0.223 | 8.193 | 5.703 | 57.6 |

Kept for the repeatable reduction in GPU work. Mean GPU time fell about 0.035 ms/eye (0.07 ms/stereo frame), closely matching the measured visibility reduction. No CPU-recording or delivered-fps gain was established. Battery remained at 77–78% and the highest post-run temperature was 42.0 °C.

### SpaceWarp instance allocation history

A recycled instance descriptor could match the size of the previous snapshot while referring to a different mesh or skeleton. The first motion frame now leaves the camera vector in place before the vertex shader reads previous instance or bone data. The frame number advances once when the first eye snapshots instance storage. Rigid motion detection also considers the fatness field so a change in mesh thickness alone generates a vector. The owner's raster depth bias remains active.

The earlier failing GPU fixture had accidentally translated only the current camera by writing a weight into the view-projection matrix. After correcting the fixture, an RTX 4080 Laptop GPU ran six two-pixel readbacks using the merged APK's actual object/skin vertex and motion fragment SPIR-V. New rigid and skeletal allocations preserved the sentinel background despite unrelated prior data, including a reused bone palette offset. Continuous rigid translation, fatness and skeletal movement produced the expected X/Z components. A stationary rigid mesh preserved the background. This validates the shader cases on desktop Vulkan. Actual recycled identities have not been isolated in Quest captures.

### Previous morph deformation in SpaceWarp

The morph shader evaluated both vertex positions using the current morph samples and blend weight. A mesh changing shape without an object transform therefore reported zero deformation. The previous vertex now reads the matching morph descriptor from the instance snapshot, including animation index, both sample frames, interpolation and intensity. Current geometry keeps the original calculation.

On desktop Vulkan the old shader returned X motion 0.0 when a vertex moved +0.25 in clip space. The corrected APK shader returned +0.25, −0.25 for a fading layer, and the expected fractional displacement for a half-interpolated layer. A newly allocated morph retained its camera-only vector. All ten rigid, skeletal and morph readback cases passed; the Android Release build linked. In-headset perceived motion on an individually identified morph remains unverified.

### Second scene for raster shadow visibility

Repeated the measurement in `NW_TAVERNE` at noon using the same two APKs and CPU 1 / GPU 0. Baseline runs averaged 10.135 and 10.088 GPU ms/eye, 0.228 and 0.223 visibility ms/eye, and 45.8 and 45.9 fps. Candidate runs averaged 10.060 and 10.043 GPU ms/eye, both 0.178 visibility ms/eye, and 47.6 and 46.0 fps. Each had 14 VrApi samples. CPU recording was 6.535/6.398 ms on baseline and 6.476/6.492 ms on candidate. The visibility reduction repeated in the second scene; total GPU time fell about 0.12 ms per stereo frame. All four post-run checks reported 78% battery and 42.0 °C.

### Focus overlay during lost position tracking

When the runtime reported invalid eye positions, both eyes used the base game camera. The marker path depended on valid stereo and suppressed the name and melee bracket. The XR window now paints the overlay whenever it has a game camera and uses the second eye's shared render work even in the centered fallback.

Tested a temporary Android build that forced eye position validity false, with SpaceWarp requested and combat active. Inspected 18 stereo PNG pairs (prefix `20261120-033348-006`). In pair 001 the centered "Wolf" label was visible in both eyes. RGB comparison showed identical pixels at its location and across the rest of the lower image. The frame used quads during the forced loss. The test switch is absent from the release source. No physical tracking interruption or held-input melee bracket was tested.

### Failure handling in the owner's multiview measurement tool

The optional `-skip mvtest` setup ignored image binding, command-buffer, fence and queue submission errors. A failed submission could leave the render thread waiting on a fence forever. Removed the setup submission. The measurement's first render clears colour and depth, so Tempest transitions all array layers from undefined layout on that clear. A failed bind now releases the image and memory. Borrowing failures release partial views before the native images. Device loss still propagates.

A host harness injected 18 failures across three target allocations, heap selection, memory binding, view creation and device-loss exceptions. It checked release order and counts, retry, same-size reuse and shutdown. On Quest the existing experiment recorded repeated `GBuffer-multiview` GPU timestamps and completed the combat run; after cleanup the headset was 79% at 42.0 °C. The two offscreen array layers were not read back. Ordinary stereo rendering remains the default.

### Permanent OpenXR loss without a pending event

A runtime that immediately returned `XR_ERROR_INSTANCE_LOST` from event polling looked like an empty queue to the app. A formerly focused session could then keep attempting frames against the lost instance. Polling now distinguishes an empty event queue from failure. Terminal loss during polling, session begin/end or frame wait/begin/end clears focus, stops input and requests application exit. Subsequent event pumps skip the terminal instance.

A host harness compiled the production polling and result handlers. It injected immediate instance loss, a lost-session frame result, loss during session begin and a loss event. Each stopped the session, and subsequent polls made no further runtime calls. An empty queue and successful frame result preserved focus and running state. Android Release built. Permanent device loss was not induced on the unattended headset.

### Off-axis sphere bounds for terrain visibility

The analytic screen bounds for a sphere used only the projection's focal lengths. With a portal eye off centre, HiZ could test terrain meshlets against a different part of the depth image. Added the eye projection's horizontal and vertical NDC offset before converting to screen coordinates. The same camera-space calculation needed the correction in software rasterization, surfel and point-light culling shaders. Centered desktop projection has zero offset. The near-plane fallback still covers the viewport.

A desktop Vulkan compute harness executed the actual `projectSphere` function with 192 combinations of sphere position, distance, radius and eye offset. Each fully visible sphere was checked against 2,048 independent surface projections; near-plane intersections were checked against the full-viewport fallback. The old function excluded surface samples in 172 cases. The corrected function excluded none. All affected Android shaders built. No wearer lean or terrain pop-in comparison was performed.

### Re-measurement of direct scene-global uploads

Rebuilt the earlier experiment against the merged branch. Each frame slot supplied mapped storage buffers directly to the renderer, removing five GPU copies per eye. The city comparison used the same waypoint and noon, CPU 1 / GPU 0, no captures, A/B/B/A order. Both controls averaged 5.645 ms CPU recording. Candidate runs averaged 5.637 and 5.545 ms. GPU time was 8.150 and 8.173 ms/eye for controls versus 8.163 and 8.153 for the candidate. VrApi was 55.7 and 58.9 fps for controls versus 58.1 and 58.6. Battery was 79%, 41–42 °C after the runs. Reverted the candidate. The GPU timings overlapped and one candidate's CPU gain was only 0.008 ms/frame.

### Depth slices for tiled point lights

Each screen tile used to test every light overlapping its 2D rectangle, even when the surface was far outside a light's depth range. The tiled path now bins those same rectangles into four depth intervals. Slice boundaries are shared between CPU and shader; the bounds round outward across each boundary. Within a slice the light IDs remain in their original order and use the existing attenuation, normals, albedo and exposure. A host test compiled the actual list builder and checked 210,000 screen/depth samples spanning three off-axis projections, near-plane crossings and slice boundaries. All 106,553 in-range lights occurred in order.

Low clock A/B/B/A comparisons, no captures, noon, same APK pair in both scenes. Each entry is two independent runs. Times average each run's last four app reports. Battery stayed at 79–81%; temperature reached 44.0 °C after the city runs.

| Scene | Variant | GPU ms/eye | CPU record ms/frame | VrApi fps |
| --- | --- | ---: | ---: | ---: |
| Tavern | Original | 10.038 / 10.050 | 6.410 / 6.424 | 48.5 / 48.4 |
| Tavern | Depth slices | 9.595 / 9.613 | 6.553 / 6.550 | 50.7 / 47.9 |
| Main street | Original | 8.178 / 8.165 | 5.568 / 5.721 | 58.9 / 56.1 |
| Main street | Depth slices | 8.045 / 8.065 | 5.580 / 5.677 | 58.3 / 58.6 |

Kept the change for its GPU savings of about 0.88 ms per stereo frame in the tavern and 0.23 ms on the main street. The extra CPU list construction cost about 0.13 ms/frame in the tavern. Delivered fps varied across runs.

### Writable depth image layouts

Tempest's Vulkan barrier selected the depth-only attachment layout, while dynamic rendering declared the combined depth/stencil attachment layout. The device does not enable separate depth/stencil layouts. This affected the XR SpaceWarp depth target as well as ordinary depth attachments. Tempest `1b877926` uses the same combined layout for the writable transition, rendering and borrowed-image handoff.

A desktop Vulkan readback used the production layout translator, dynamic rendering and two image formats. D32 float and packed D24/S8 both cleared to 0.25, transitioned through read-only and transfer-source layouts, returned the expected depth values, and transitioned back to their borrowed attachment layout. Android Release linked. This check ran without Vulkan validation layers.

### GPU writes before CPU readback

Tempest attached its host-read barrier to the flush immediately before an image-to-buffer copy. A fence wait and cache invalidation could then precede CPU access without a memory barrier after the transfer itself. The eye PNG tool uses this path. Tempest `4d94f7ea` accumulates the writing stages until command-buffer finalization and then publishes transfer, compute and graphics writes to host readers. A buffer without a tracked hazard ID is included.

The committed command-order regression failed against the old tracker and passed against the new one in a standalone MSVC harness using the production tracker. It checks that the early flushes do not issue a host dependency, that all three writer stages appear in the final barrier and that the next empty recording inherits none of them. Android Release linked. Earlier Quest PNGs decoded correctly; they do not establish the memory ordering on all drivers.

### Environment switch and saved choice on the compositor

Tested both input paths in an instrumented APK. It supplied a synthetic near pose and pinch on the left hand, then a controller aim ray and held grip on the right hand in a separate launch. The production hit test, press/release logic, passthrough calls, INI flush, startup restore and layer submission ran on Quest. A smaller window placement kept both controls inside the screenshot. The test used a new app-private INI. The owner's INI had the same SHA-256 before and after; only the test INI was removed.

The near pinch changed passthrough to dusk once and saved `xrEnvironment=1`. Meta capture `20261120-040437-863` shows the floating window, handle and window-shaped return button above both ridgelines with ordinary stereo quads. The next launch restored dusk, the right aim ray switched back once, and the INI contained 0. Meta capture `20261120-040624-950` shows the crescent below the handle with SpaceWarp enabled. Both post-run checks were 81% and 40.0 °C with the app stopped. These were synthetic poses on the real runtime; physical targeting and comfort need a wearer.

The dusk screenshot also shows a faint, narrow dark line running through part of the panorama. The generator uses periodic mountain functions; its source has not been established from this screenshot.

### SpaceWarp history at game camera cuts

In-world teleports call `Camera::reset`, and first-person, cutscene and dialogue shot changes can reposition the camera abruptly without changing the world or portal. They previously reused the previous shot's view projection. The camera now marks these discontinuities. Both eyes consume the same decision for one warp submission. A speaker change marks a cut only once the dialogue camera actually applies it; an unchanged shot or an ordinary orbit update keeps its history.

The host camera test covered reset, requested versus active first-person mode, entry/exit from cutscenes, and pending, applied and repeated dialogue speakers. In a temporary Quest build, a reset was injected into an established SpaceWarp session on the city street. The app logged valid history on frame 49, invalid history and `FRAME_SKIP` on both submitted views on frame 50, and valid history again on frame 51. The injection is absent from release source. The headset finished at 81% and 41.0 °C. Physical cutscene and dialogue playback were not observed.

### Frame-slot fences and upload resource lifetime

A Vulkan slot wait checked its fence, released the timeline lock, then waited for any submission before rechecking the target. Another thread could complete and recycle the target in the gap, allowing a zero-count native wait or a wait on unrelated work. Finite waits also subtracted elapsed time repeatedly from an already reduced budget. Uploads attached their retained resources after submission without the lock used by fence completion. Allocation and submission failures could strand a native handle or leave an unsignalled reservation.

Tempest `26d0ca60` waits on the requested native fence under the pool lock, converts a finite timeout once and holds upload references until completion. A late payload is released if the GPU finished before it could be attached. Wrapper allocation preserves the old handle until it succeeds, and a rejected submission returns the reservation to the pool.

A host fixture ran 1,000 cycles with controlled completion racing late resource attachment, short and infinite waits, expired waits, held old wrappers, allocation failure, failed submission and device loss. It verified exact timeout units, resource reference counts and native-handle ownership. The Android release built. A Quest SpaceWarp spells run on the new wait and host-readback paths yielded 13 complete stereo pairs (prefix `20261120-041124-428`). Both eyes showed the spawned fire lizard and the lighting change during the fight. This normal-path run preceded the final adjustment that immediately drops already-completed payloads. It did not induce the fence race or device loss. Post-run battery was 82% at 40.0 °C, app stopped.

### CPU cost of building tiled light lists

Counted each light’s horizontal span with a start and end marker. The existing pass over the tile table accumulates the markers; the shader receives identical offsets, counts and ordered light IDs. The host fixture compared complete buffers at three projection offsets and checked all 106,553 positive contributors across 210,000 surface samples.

Four low-clock tavern runs on the same release and waypoint used control, candidate, candidate, control order. Over the last four complete reports in each run, preparing and recording lighting for both eyes cost 0.887, 0.791, 0.799 and 0.899 ms/frame respectively (elapsed phase time divided by rendered frames). Total CPU recording averaged 6.554, 6.346, 6.501 and 6.606 ms. GPU time stayed between 9.58 and 9.60 ms per eye on average; delivered fps was 51.5, 51.3, 51.2 and 51.2. Kept the change for the repeatable 0.10 ms/frame reduction in CPU lighting work. Each run used CPU/GPU levels 1/0 and stopped the app. Post-run battery ranged from 81–82% at 41–43 °C.

### Quest Home and SpaceWarp resume

On the low-clock city street with SpaceWarp enabled, Android Home put the Quest shell’s FocusPlaceholderActivity in front for five seconds. The game’s same PID (8233) reported VISIBLE → SYNCHRONIZED → STOPPING → IDLE. Bringing its NativeActivity forward produced READY → SYNCHRONIZED → VISIBLE → FOCUSED, followed by two full update/record/submit intervals at 36.15 and 35.99 app fps. VrApi averaged 36.6 fps over 14 samples at levels 1/0. The app was force-stopped afterward, battery 81% at 42 °C. This verifies a real Home/return cycle; physical headset removal and a user-triggered recenter remain untested.

### Keep the benchmark lease during capture retrieval

Two local checkouts could run back-to-back benchmarks on the same headset while the first was still copying and removing eye PNGs. The script now stops the app, allows sleep and restores the clocks before copying, and releases the per-headset mutex after capture retrieval. It copies the full set of eye files before removing any of them from the headset.

A mocked ADB run launched a separate PowerShell process to acquire the named mutex during screenshot transfer. It acquired the old script’s lease too early. With the fix the second process saw the lease held during transfer, and could acquire it after both a successful pull and an injected failed pull. Both paths stopped the mock app and released its wake request before the transfer.

### Remove the dark meridian in the dusk panorama

The Quest runtime reported clamp-to-border for both panorama texture axes. Filtering at the longitude join mixed its dark border into the sky and ground. When the optional FB Vulkan sampler-state extensions are available, the app now reads the runtime sampler, sets longitude to repeat and latitude to clamp-to-edge once, and keeps its other sampler settings. Runtimes without those extensions keep their existing sampler.

The original small-window capture (`20261120-040437-863`) had a continuous line 6.25–9.07 RGB levels below neighboring columns across five 20-row bands. Rotating the panorama or selecting equirect1 moved the line out of view. Repeating longitude with the original equirect2 orientation removed it in the same view (`20261120-042438-912`): the greatest local deficit anywhere in that longitude band was 0.18–0.80 levels. The state update returned XR_SUCCESS. The final release also ran SpaceWarp combat with dusk at 36.1 fps and low clocks. Meta capture `20261120-042812-925` showed the sky above the window and all 19 stereo PNG pairs decoded and showed the fight. The app stopped at 82% and 40 °C. Fallback sampler appearance on a different runtime is unverified.

### Refresh shadows when a gate changes caster batches

An idle mover such as a gate or lift uses a static mesh, changes to a movable mesh while travelling, then becomes static again at its destination. With the camera and sun still, the cache could preserve its old closed silhouette and omit its new shadow. Static and landscape cutout meshes with animated UVs or texture frames could also cast a frozen silhouette.

The XR draw commands now keep animated cutout masks in the dynamic shadow pass, including when otherwise-identical bindless materials share pipelines. Adding or removing a cached caster advances a geometry revision. A changed static pose or fatness also advances it. Each cascade compares its recorded revision before using either its static cache or its alternate-frame shortcut.

A host fixture compiled the production command selection, pose setters and shadow draw loop. At a fixed light matrix, the old near cascade kept both old and moving footprints and the far cascade kept only the old one. The fix produced just the moving footprint in both, then the final static footprint in both. The check also covered repeated equal poses, fatness changes, and fixed versus animated cutouts in either insertion order with both binding modes. Android Release linked and the non-XR renderer passed MSVC syntax checking. A real gate moving in-game has not been captured.

### Shared keyboard and controller keys across focus changes

A physical Up key and a controller mapped to Up had separate widget captures. Releasing either source stopped movement while the other remained held. Tempest `3161e4c9` tracks physical device/scan pairs and the virtual pad separately, while keeping one widget capture per held logical key. The first press and last release reach the widget. Physical keyboard repeats still reach its repeat handler. Character-only native key events keep their separate scan codes.

OpenXR-only focus loss used to emit ordinary button releases. An inventory or menu key held at the time could open its UI during cancellation. The Android presentation now reports XR focus changes to Tempest, which drops its captures, pending touches and modifiers. The gamepad cancels held outputs without firing their release actions; a continuously reported hold must end before a fresh press.

A host test checked both source orders, both release orders, physical repeat, matching scans on different devices, a modifier, focus cancellation and rearming after a disconnected sample. A temporary on-device input probe held X while injecting three frames of XR focus loss with the activity still foregrounded. Inventory remained closed before, during and after cancellation, including after the held button released. Fresh X presses then opened and closed it. The real city run used CPU/GPU levels 1/0, stopped at 85% and 40 °C, and included the updated shadow cache. Physical simultaneous Bluetooth/XR use and a wearer-operated gate remain unverified.

### DirectMusic startup failure and silent PCM output

The optional GothicKit music provider could keep a freed loader or performance handle after a mutex or queue allocation failed. A queue that allocated its first block and then failed to allocate its pointer table also leaked that block. dmusic `2b215e6` publishes handles only when initialization succeeds and rolls back the partial queue. The game checks loader and resolver creation, rejects unavailable or short reads in its file resolver and outputs silence when playback initialization or rendering fails. An initialized synth with zero sound banks now actually writes the silence it reports, in either PCM format.

A host test against the former library reproduced a dangling output on its second performance allocation and untouched PCM in a fresh performance. The new library passed failures at all five performance allocations and loader allocation, with zero live allocations after every case. Injected mutex failures left both creation outputs null. Guarded PCM buffers verified the precise silent byte range for mono/stereo and short/float. The Android APK linked; no audio was heard or evaluated by a wearer.

### DirectMusic loops, transitions and stopping

A queued segment kept a reference after replacement, playback or cancellation. Looking ahead to a future style or band also retained it on every audio callback. dmusic `47482a5` gives queued and composed messages matching retain/release operations and takes a temporary reference only when consuming a due message. Failed transition composition releases partial work. Playback handles a segment with no style or patterns, and reads and advances the scheduling clock under the performance mutex.

The optional provider also passed a null target to the transition API to stop music, which always returned an error. The audio callback now schedules and consumes an immediate segment stop, returns silence for that buffer, and then accepts the next theme. Disabling and immediately re-enabling music preserves a new pending theme.

The original library failed a repeated segment-replacement reference-count check. The repaired host library passed 128 replacements, four loops, 128 callbacks looking ahead to a future style, composed-transition destruction and initialization failure, no-style/no-pattern segments, and an immediate stop, ending with zero live allocations. A separate provider fixture checked disable/re-enable, an empty/failed theme, failed stop retry and PCM output. On Quest the optional GothicKit provider logged the Gamestart, XT_DayStd and KH_DayStd segments, their intermediate transitions and patterns through measure five. Low-clock city combat completed at 58.9 fps, app stopped, 87% and 40 °C. This run preceded a final guard suppressing warnings when there was no outgoing segment to release. Audio quality and physical focus-loss audibility were not evaluated.

### Music loader ownership and partial retries

A failed segment, instrument bank or style parse could leave its caller with a pointer the loader had already released. Cache insertion failure could leak the parsed resource. A segment that failed partway through resolving styles was marked downloaded, so retrying could report success with a missing style.

dmusic `0673d97` keeps each new resource local until parsing and cache insertion succeed, reuses styles already resolved by a previous partial attempt, and marks the segment downloaded only after completion. The host fixture observed the old loader return a stale bank pointer on a failed read. The repaired library passed failed reads for all three resource types, forced cache allocation failures, cached-reference reuse, two partial retries and a successful retry after the missing reference became available. Its counting allocator ended at zero. The Android release linked. These failure cases used synthetic host resources.

### Lighting shading-rate upper bound, exclusive Quest runs

Used one temporary APK, CPU level 1/GPU level 0, with normal shading (A) and uniform 2×2 fragment shading on the sun, ambient and tiled point lighting pipelines (B). Each run confirmed its active setting in the app log. The mean of the last four GPU samples per eye and VrApi FPS, in A/B/B/A order, were:

| Scene | GPU ms/eye | FPS | Mean saving per stereo frame |
|---|---|---|---|
| NW_CITY_MAINSTREET_02 | 8.075 / 7.218 / 7.235 / 8.053 | 60.3 / 67.2 / 66.7 / 60.6 | 1.675 ms |
| NW_TAVERNE | 9.608 / 7.430 / 7.475 / 9.595 | 51.2 / 65.9 / 65.7 / 51.5 | 4.298 ms |

This exceeds the 1 ms threshold for investigating a full-rate centre and coarse periphery. Uniform 2×2 was removed from the source after the experiment. Shutdown and battery checks succeeded after all eight runs. The last ended at 84%, 45.0°C.

### SpaceWarp Video choice survived its menu refresh

Opening Video or rebuilding its script VM replaced the choices with the Vulkan device name. A one-entry choice always clamped to zero, so command-line settings worked while the actual menu could never enable SpaceWarp. Configure the XR choice when refreshing Video, preserving the desktop and unsupported device label. The headset capture also showed the original label clipped to “Application”, so the final label reads “SpaceWarp”.

The host menu fixture checked unsupported and desktop rows plus both directions and mobile confirm. A temporary Quest build opened the real Video menu in the city with an isolated INI. Its selectable label went 0 → 1 (Right), back to 0 (Left after rebuilding the script VM), then to 1 (confirm). The compositor capture showed On, the app submitted SpaceWarp at 36 FPS and the private INI contained `xrSpaceWarp=1`. The owner’s INI hash matched before and after. The probe file was removed, the game stopped, and battery was 86% at 40.0°C. Input was injected; no physical controller was operated.

### Optional head-directed lighting foveation

Video → Foveation is Off by default (`INTERNAL/xrFoveation`). The XR device must support dynamic rendering, an R8_UINT shading-rate attachment, and 2×2 fragments at one sample. An optional-device-creation failure retries the original device request. Tempest `174d7420` uploads and binds the rate image for one framebuffer and keys native pipelines by attachment presence. Each fenced eye slot owns its map and staging buffer. Sun, ambient and tiled point lights use it. Sky, geometry, UI, postprocessing and motion vectors retain their normal sampling.

The map projects each eye through the current physical window against the predicted head-forward direction. Whole tiles touching the central 25° cone and partial edge tiles stay at 1×1. Other tiles use 2×2. Invalid tracking falls back to full rate. A host test of the production map loop checked 6,211,250 pixel centres across 200 poses and extents. All 5,386,112 coarse samples were outside the protected cone and outside partial tiles.

Measured at CPU 1/GPU 0 with one APK per scene, no captures during timing, A/B/B/A (Off/On/On/Off). Times are the mean of the last four profiler samples per run. Every Off run reported attachment capability and allocated no map. Each On run reported the four eye-slot maps.

| Scene | GPU ms/eye | Lighting + sky ms/eye | CPU recording ms/stereo | FPS | GPU ms saved/stereo |
|---|---|---|---|---|---|
| Main street | 8.050 / 7.345 / 7.345 / 8.103 | 1.560 / 1.038 / 1.025 / 1.595 | 5.609 / 5.603 / 5.582 / 5.676 | 60.3 / 66.0 / 66.1 / 60.3 | 1.463 |
| Tavern | 9.615 / 7.858 / 7.868 / 9.595 | 3.665 / 2.042 / 2.052 / 3.650 | 6.348 / 5.906 / 6.034 / 6.488 | 51.0 / 62.6 / 62.2 / 51.5 | 3.485 |

Quest input probes traversed Resolution → Foveation → Brightness, changed Foveation independently of SpaceWarp, confirmed the values persisted in an isolated INI, and cycled Off/On while the menu was displayed. The first constructor-time probe logged the settings but its capture did not show the menu. A timed probe on the attached widget showed both choices and all three original sliders (`20261120-051316-138`). Three script-VM reloads left exactly one Foveation row and zero stale names. A simulated map failure reverted to full rate and re-enabled both eyes after 3.001 seconds, including all four slots. The dusk + SpaceWarp capture after reloads (`20261120-052224-264`) showed the selected row and its On state. The owner’s original INI hash matched after all probes. The final reload run stopped at 85%, 41.0°C. Windows non-XR syntax checks passed for the window, renderer, shaders, menu and XR stubs. Peripheral edge quality with a wearer turning their head or looking sideways remains unverified.

### Require an active release before a new XR grab

While the session stayed focused, one continuous pinch could toggle the environment again when hand input returned. The grab path converted an inactive or failed action read into a zero, which falsely rearmed the next press. Each hand now distinguishes an unavailable action from an active low value, cancels only its own grab on loss, and waits for an observed release before capturing again. The same rule covers the first tracked sample on startup.

A host harness compiled the real input update before and after the fix. It exercised an initially held right grip, inactive and failed action reads during a hold, a healthy hand while the opposite action failed, and fresh releases on both hands. Old code persisted seven switches for four genuine presses and failed five checks. New code persisted exactly four and passed all checks. Real hand occlusion and reacquisition require a wearer.

Additional audit findings under investigation include a held XR gameplay button interpreted as released when window grabbing suppresses the pad, and focus loss followed by regain within a single event drain. Neither sequence has been exercised by a wearer. Source inspection also identified cleanup hazards on Vulkan fence-wait errors and on host allocation failure after upload submission or before ending the first XR frame. These exceptional paths have not been induced on the device.

### First-frame allocations and XR image ownership

The first submit generated the chrome and panorama after clearing `frameBegun`. A `std::bad_alloc` escaped before releasing the acquired eyes or ending the frame. The compositor layer list could also allocate after image release. Optional graphics now retain their successfully uploaded parts on allocation failure. Submission uses fixed storage for the maximum seven layers.

A host fault-injection test used the production submit path with two acquired eyes. Failing chrome generation or panorama generation previously left both images acquired and ended zero frames across two cases. Both fixed cases released the eyes and each called `xrEndFrame` once. Android Release linked. Actual device memory exhaustion was not induced.

The uninstrumented foveation release also completed a low-clock city combat run with dusk and SpaceWarp at 36.0 app FPS. The compositor showed the dusk sky above the game; 19 decoded 1600×900 stereo pairs showed the fight in both eyes (`20261120-052422-304`). The package stopped at 85%, 40.0°C.

### Reserve the upload command before GPU submission

Tempest’s asynchronous upload path grew its command recycling vector after the GPU accepted the submit. Allocation failure could destroy the command pool while its copy was pending. Tempest `ca6351ed` reserves capacity under the same lock as the insertion before submitting. A host harness injected the next allocation failure into the actual UploadEngine template. Old code submitted once and prematurely destroyed one command pool. New code failed before submit, accepted the next upload and recorded no early destruction. Android Release linked. Memory pressure was simulated on the host.

### Retain a focus interruption across a single XR event drain

If FOCUSED → VISIBLE → FOCUSED arrived before the next update, the old poller cleared the XR pad but reported only its final state. The game could treat a held control as physically released. The event drain now reports an interrupted focus interval, so the normal input cancellation runs once before a fresh sample. Frame begin/end and visibility still follow the actual session state.

A host queue reproduced the lost interruption on old code and verified the corrected batched transition, ordinary loss/restore, and the next uninterrupted sample. A Quest probe injected both session events into the same poll while X was held. The game suppressed that update, inventory remained closed through release, and two fresh presses opened and closed inventory. The same run exercised the previous tracking fix with a synthetic pinch on the actual environment button: room → dusk, action/pose loss during the pinch, no change on reacquisition, then dusk → room on a fresh press. The isolated INI ended at `xrEnvironment=0`, the owner’s hash matched and the test file was removed. The game stopped at 87%, 40.0°C. No wearer or physical controller was involved.

### Preserve transfers across transient Vulkan wait failures

An accepted upload can still be pending when a fence wait reports host or device allocation failure. Tempest `1657abb7` keeps the fence payload and pending state, propagates errors from finite waits, and retries blocking waits outside the timeline lock. The Quest static uploader now polls its accepted copy to completion after a transient wait failure and retains the staging buffer throughout. A submit reporting device loss is also drained before cleanup. [Vulkan device-loss and resource lifetime rules](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html#devsandqueues-lost-device)

Six host assertions failed on the old fence path and passed on the fix, covering both allocation errors, pool refresh, retained references and successful retry. The existing 1,000-cycle concurrent fence test passed. A separate host fixture for the production static submit/wait path failed three of five cases before the fix and passed all five after, including repeated transient errors, terminal loss and rejected submit. Android Release linked. Under persistent driver memory errors the blocking paths await confirmed completion; the tests did not exhaust actual headset memory.

### Destruction after Vulkan device loss

The public fence destructor called the same throwing wait as an explicit user wait, and the upload pool destructor could stop at the first lost fence. Both could terminate the process during unwinding. A submit reporting device loss also returned its reserved fence to the healthy pool before establishing that the command buffer was retired. Tempest `a15c980e` drains that submission, keeps its fence terminal, waits all upload commands during teardown and preserves the device-loss exception for explicit waits. Device idle waits retry transient memory errors before freeing resources.

Isolated host processes ran the actual fence destructor and upload pool with a lost mock device. Both old processes entered `std::terminate`. The corrected explicit wait still threw, the public destructor returned, and the upload pool waited for all three commands before destroying them. A separate submit test returned two transient allocation errors before loss. The fixed path called the wait three times and retained terminal fence status; the old path made zero waits and marked it reusable. Android Release linked. Actual device loss was not induced on Quest. The preceding transfer-wait release completed real dusk/spell rendering and capture at low clocks and stopped at 89%, 40.0°C.

### Reuse contiguous freed instance storage

Freeing an instance range between two free neighbours merged only one side. Three contiguous regions could remain in two pieces, forcing the next large mesh or palette to grow CPU storage and potentially reallocate the GPU buffer. Freeing a zero-size ID could also insert a useless free range. The allocator now merges both sides of a released span and ignores empty ranges.

A host test used the production allocation and free functions. It allocated four 128-byte regions, tried all 24 release orders and then requested 512 bytes. Old code grew the backing store in 16 orders (up to 1,088 bytes including the reserved first matrix) and retained an empty range in all 24. The fix reused the original 576 bytes in every order and left no empty ranges. Android Release linked. This is a measured storage bound for the fixture; live scene memory savings were not measured.

### Blend translucent window controls in linear light

The handle, resize brackets and surroundings button multiplied sRGB colour bytes by opacity before OpenXR decoded them. At the centre of the idle handle this yielded 0.136 linear red over black instead of 0.357. The generated textures now store straight RGB and all three quad types request source alpha with the unpremultiplied-alpha flag. The compositor applies coverage in linear light. [OpenXR swapchain colour space](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSwapchain.html)

A host harness compiled the pixel function and layer flags from each revision. Across sRGB and UNORM formats, four colours, three opacities and four coverage values for all three controls, 81 of 288 old results exceeded a 0.006 linear tolerance; all corrected results passed. Android Release linked. The numeric test models compositor blending. Headset appearance of this change awaits a compositor capture.

### Animation palette batching trial, reverted

Tried copying a complete changed palette once, skipping identical content and publishing one dirty mask per 32 blocks. A host fixture checked 6,580 mask ranges and concurrent writers. The same instrumented Quest APK ran both implementations in A/B/B/A order in each scene, with CPU/GPU levels 1/0 and no captures. For each run the setter duration is the mean of its final eight 128-commit reports. Update is the last four frame reports. These are real gameplay writes, about 34,800 per report in the city and 32,800 in the tavern.

| Scene | Metric | Original | Batched | Batched | Original |
| --- | --- | ---: | ---: | ---: | ---: |
| Main street | ns per palette write | 841 | 781 | 784 | 841 |
| Main street | Update, ms/frame | 5.185 | 5.136 | 5.267 | 5.135 |
| Main street | App FPS | 60.6 | 60.3 | 60.2 | 60.2 |
| Tavern | ns per palette write | 922 | 843 | 828 | 932 |
| Tavern | Update, ms/frame | 5.385 | 5.466 | 5.513 | 5.433 |
| Tavern | App FPS | 51.4 | 51.8 | 51.4 | 51.6 |

The measured work saved about 16 µs per storage commit on the main street and 23 µs in the tavern, summed over workers. The CPU update and app FPS showed no repeatable improvement, so the release keeps the existing write strategy. Every run stopped the game. The series peaked at 45.0°C, rested between runs and ended at 87%, 43.0°C.

### Recreate passthrough when OpenXR requests it

The event loop ignored passthrough state changes and kept submitting the old room layer after a reinitialization request. It now destroys the layer before its parent and creates a replacement in the selected mode. If the room is unavailable, the compositor shows the uploaded dusk sky and hides the switch. Recovery brings back the selected surroundings. Runtime errors leave the saved preference intact. A failed layer creation now also releases its parent. [OpenXR passthrough state flags](https://registry.khronos.org/OpenXR/specs/1.0/man/html/XrPassthroughStateChangedFlagBitsFB.html)

A host queue exercised eight event and creation sequences using the production event handler. The old path failed seven; the new path passed all eight, including combined flags, partial creation, destroy order, camera-paused VR and selection recovery. On Quest, injected notifications recreated the real room handles in passthrough, hid the unavailable room, restored it, then recreated the handles in dusk. Subsequent calls switched to room and back to dusk successfully. The compositor capture showed the city, dusk and the idle handle after recovery (`20261120-055315-021`). SpaceWarp stayed active, with 36.5 app FPS at low clocks. The app stopped at 87%, 42.0°C. This simulated the notification; a spontaneous headset camera failure was not induced.

### Initialize new morph and instance ranges in GPU page padding

The first instance upload copies only live CPU bytes into a GPU allocation rounded up to 4 KiB. When a new morph descriptor fit in that padding, its initial all-zero write matched fresh CPU memory. It could therefore leave uninitialized intensity and sample offsets on the GPU. The allocator now marks appended blocks for upload when they fit in the existing GPU allocation. The dirty-bit helper uses a C++20 atomic reference to the existing integer so concurrent setters no longer access it as a different object type.

A host fixture ran the production allocation, byte setter, commit and patch packing code against a device buffer prefilled with 0xA5. All six initial zero descriptors contained stale bytes on the old path. The fix initialized all six and preserved the untouched guard after each range. Android Release linked. Actual allocator contents depend on the driver; no specific visible morph glitch was attributed to this defect on Quest.

### Retain ownership of OpenXR through Vulkan startup

Failure during XR instance creation leaked the temporary window wrapper. After successful XR preinitialization, an exception in Vulkan API creation, GPU selection or device construction could also leave the singleton and its creation hooks pointing at the old instance. An initial owner now covers that interval. Once the Vulkan device exists it transfers ownership to the normal scope so XR image views and the session are still destroyed first.

A host fault-injection fixture compiled the production ownership sequence and ran six startups in one process. Old code failed four cleanup checks. Fixed code released the instance and hooks at every failure point, successfully started again, and destroyed attached XR resources before the mock Vulkan device. Android Release linked. Startup failure was simulated on the host.

### Sample controllers during focused frames with zero layers

A focused game continues ticking when OpenXR sets `shouldRender` to false. The old frame loop ended those frames before sampling actions, so the last stick deflection could move the character indefinitely after release. Visible sessions now sample input before deciding whether to render. A grab can start or move the window only with a visible image and valid head pose. A held grip still consumes the stick and must be released before a new grab.

A host fixture ran ten frames through the production begin/input paths, including two zero-layer requests, a hidden session and loss of head tracking. Both versions matched ten begins with ten ends. The old path failed six checks and triggered three environment switches for one deliberate tap. The fix passed every check and triggered one switch. A Quest probe supplied synthetic stick values during two zero-layer frames: the log showed −1 before, 0 then +0.5 during, and 0 on the next rendered frame. A centered test pose kept both corrected controls in view. The compositor capture showed the idle handle and window icon below the city against the dusk sky (`20261120-060014-930`). The app stopped at 89%, 40.0°C. These input and compositor checks used CPU/GPU levels 1/0; wearer-operated grips remain untested.

### Release partially allocated Vulkan images under memory pressure

Both Vulkan texture allocation overloads detached the image from its allocator before throwing on missing memory or a failed bind. This lost the image handle and, after a failed bind, its allocated memory range. Tempest `599f48f4` leaves the allocator attached so the existing texture destructor releases both.

A host fixture used the production image allocation, destruction and free paths for a texture upload and a render target. Across normal completion and injected allocation, bind and view-creation failures, old code leaked four images and two memory ranges. The corrected eight cases leaked zero. Android Release linked. Real GPU memory exhaustion was not induced.

### Release Vulkan command buffers without allocating on teardown

Tempest constructed a temporary array while destroying a command buffer split into 64 or more native commands. Under host memory pressure that allocation could terminate the process during unwinding. Tempest `71a15ca3` lets the owning Vulkan command pool release its commands and also removes an unused array allocation during pool reset. A mock driver and failing allocator reproduced termination at 64 commands on the old destructor. The fix released all resources for 0, 1, 32, 63, 64 and 97 recorded commands, including the current command and the profiling query pool. Android Release linked. [Vulkan command pool destruction](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyCommandPool.html)

A binary check also caught an incremental test APK that still contained the zero-layer probe after its source was restored with an older timestamp. The earlier labeled probe captures remain diagnostic. The replacement was built offline in a fresh directory from `ae4d16fc` (1,055 build steps), checked for the test markers and installed. Its SHA-256 is `ca34dcf315c4776f2a53cd79772d53906b11c6035d381cfea7f6be9e2e8a0fac`. Subsequent release builds use that independent output directory.

### Reject stale instance patches after failed mapped transfers

Tempest’s Vulkan buffer update silently accepted a failed map or flush. On a reused upload slot this allowed a new dispatch to execute old patch headers and payloads. Failed readback invalidation could likewise copy stale bytes to a caller. Tempest `3258bffd` propagates map, flush and invalidate errors for updates, fills, initial writes and reads, and unmaps successfully mapped memory before propagating a cache error.

The instance upload worker now reports its exception at the render thread’s pre-submit join and marks the live data for retry. A failed full upload keeps the previous buffer. A failed patch allocation or command recording leaves its dirty blocks pending. Worker destruction drains pending work, including failures.

A mock Vulkan test covered twelve map/cache/success cases: the old code failed eight checks and issued two misaligned initial maps. The fix passed all twelve. A threaded test used the production patch packer and worker with a reused slot. The old silent-failure path failed four checks; an injected thrown update also ended the old test process. Fixed code rejected the frame, restored both ranges on retry, preserved the prior full buffer on failed growth, and shut down with a failed upload pending. The 4,097-upload slot/shutdown regression and Android Release build also passed. No Vulkan memory fault was induced on Quest.

### Distinguish the experiment branches from the audited build

The experiment notes now identify which branches still use current-frame morph offsets and solid cutout silhouettes. The foveation notes link the implemented setting and the measured main-street and tavern savings. The controls reference includes the surroundings switch among the grip and pinch targets. The original experiment measurements remain available for the owner’s branch layout.

### Reuse reset Vulkan command buffers on Android

Tempest `901ba68d` retains up to 64 extra command-buffer handles per pool after reset. The first command stays in place and excess handles are freed. A driver fixture recorded 16 frames with 1–97 commands. Allocations fell from 602 to 98 and individual frees from 601 to 33. Both versions peaked at 97 live handles with zero state or ownership violations.

On Quest 3, both waypoints used CPU level 1 / GPU level 0, the same scene arguments and control/candidate/candidate/control order. Installed APK hashes were checked on each switch. Each recording and GPU number below is the mean of the final four app reports in that run. The control was the clean `ae4d16fc` APK. The candidate also included `71a15ca3`’s separate pool teardown and unused reset allocation cleanup.

| Scene | Stereo recording CPU ms, control | Stereo recording CPU ms, reuse | GPU ms/eye, control → reuse | VrApi FPS, control → reuse |
| --- | --- | --- | --- | --- |
| City main street | 5.623, 5.631 | 5.371, 5.345 | 8.043, 8.047 → 8.040, 8.067 | 60.4, 60.5 → 60.4, 60.5 |
| Tavern | 6.429, 6.464 | 6.137, 6.137 | 9.590, 9.570 → 9.568, 9.590 | 51.1, 51.6 → 51.6, 51.4 |

Kept the change for 0.269 ms less CPU recording per stereo frame in the city and 0.310 ms in the tavern. FPS and GPU timings overlapped across variants. The benchmark stopped the game and released the wake override between runs. Final battery was 88% at 42.0 °C.

### Cancel gameplay keys when the window captures the XR controls

A held XR X button used to produce a Tab release when a handle, bracket or environment button captured the controller. That release could open inventory. Other affected release handlers included modal navigation and key binding. Losing a single runtime action could produce the same false release. Full focus loss could also leave a repeating inventory transfer running after its keyboard or mouse capture disappeared.

The XR input sampler now waits for an active neutral sample before readmitting each interrupted button, trigger or stick. It continues reading the grab stick for window positioning. The game keeps the previous native gamepad contributions and the surviving XR controls so that an overlapping Bluetooth press or a legitimate release on the same frame keeps its normal effect. An interrupted key reaches its original widget through Tempest `44ef8e94`. Android routes simultaneous keys to their individual UI recipients. Cancelling an unrelated key preserves an active melee parry. Losing full focus stops inventory repeat timers.

Host tests exercised grip and drop with X, a displaced stick and a trigger, Return and Up in menu mode, an inactive action alongside a genuine release, a failed sync and recovery, shared Bluetooth and keyboard keys, and an Escape alias handoff. Separate dispatch tests covered modal capture, an expired widget, independent parry and inventory timer ownership.

A diagnostic APK injected controller samples into the focused Quest city scene. The game logged Tab down with inventory closed, then a cancellation when the right hand acquired the handle. Inventory stayed closed through the drop. A fresh X press and release opened inventory, and the next press closed it. The isolated INI run ended at 91% and 39.0 °C. The owner's settings hash matched before and after. The signed release contained neither diagnostic marker and its installed APK hash matched `950723bd542b26c60f1468f9da7c09c87481471efdb1ec152245d70cd679292b`. Physical hand pinches, Bluetooth overlap and held chest transfers still need wearer testing.

### Expire finite single-frame facial and prop morphs

The morph scheduler removed an object after sampling a one-frame pose even when the pose had a future end time. The last nonzero deformation then stayed in its GPU descriptor. A finite pose now stays scheduled through the end tick and clears on the first later tick. Its two sample offsets are identical, so its interpolation field stays zero while the pose is held. An unlimited one-frame pose still leaves the scheduler after its first update.

A host test used the production animation start and update functions with a recording instance buffer. It checked the deadline boundary, an unlimited pose, an extension and a shorter multi-frame companion. The previous implementation failed four checks and the fix passed all of them. The descriptor stayed byte-identical until the finite pose expired. Android Release linked. No specific facial expression was isolated on the headset.

### Sample cloud motion at the same tick in both eyes

The sky uniforms took their cloud offsets from the previous global tick. The first eye advanced that tick after sampling, so the next eye saw a different sky phase. Both eyes now sample the sky immediately after setting the frame time and before uploading their uniforms.

A host check ran the production cloud calculation in the old and new uniform update orders at six stereo ticks, including a repeated tick, both layer wraps and a tick beyond 32 bits. The old order produced ten stale layer/time pairs. The corrected order produced zero. Android Release linked. The test measured uniform values directly. The small temporal offset has not been judged by a wearer.

### Fill complete cutscene audio buffers and advance the clock by decoded samples

Tempest registered mono clips with OpenAL as mono but divided every callback byte count by a stereo frame size. A mono callback consequently consumed half the audio it reported. The Bink sound producer also returned an untouched buffer whenever fewer samples than requested were available. Tempest `00aa0595` uses the producer's channel count. The video callback consumes the available complete frames, fills the rest with silence and advances a thread-safe playback clock only for samples it consumed.

A host fixture exercised the production callback and clock for mono and stereo with full, empty, short and replenished buffers. Sentinel samples guarded both ends. The old code failed 14 output, clock or queue checks. The corrected code passed all checks. Android Release linked. Headset audio quality has not been evaluated by a listener.

The audio and cutscene audit also identified pending work. A movie can silence the current GothicKit theme after exit, and an unrelated settings notification can restart music during playback. A pending theme can lose its reload flag after a duplicate request. A queued video can retain the previous context's frame pointer, and a visible, unfocused session can continue advancing the decoder. Replacing the time of a nearby event in the dmusic queue may also disturb its ordering. These findings are from source inspection.

### Preserve the current music across movies and environment changes

Movies now pause the music's OpenAL source. Resuming the same source preserves the current segment when the player stays in the same music zone. A settings notification from the surroundings button keeps the stream paused, and switching providers during a movie also waits for movie exit before starting the replacement. A terminal video playback error follows the normal movie-exit path. Duplicate requests for a pending theme preserve the request until the audio callback consumes it.

A host fixture used the production provider queue, video start/stop decisions, music settings callback and an emulated sound source. The old code failed six continuity/settings checks. The new code passed same-zone resume, repeated and replacement themes, unrelated settings during a movie, switching providers during playback, music disabled in settings and a failed replacement allocation. Android Release linked. The fixture verified source state and theme selection, without evaluating sound by ear.

### Hold cutscene video and audio together on XR focus loss

The visible XR render loop could keep decoding and queuing movie audio while gameplay and video completion checks were paused. After EOF it could even requeue an old frame. The focus transition now pauses the movie's audio tracks and decoder, retains the displayed image and offsets the playback clock by the pause duration. A second clip paints only its own nonempty decoded attachment. This removes the cached pointer into a previous clip when a new clip has no decoded frame yet. Each submitted UI mesh retains a native texture reference through its frame-slot fence. The playback context uses the existing fence destructors to drain conversion work.

A host fixture compared visible unfocused frames, repeated pause/resume, EOF and a recoverable first-frame decode error in a queued clip. Five checks failed before and zero after. Android Release linked.

On Quest a diagnostic build played the installed game's normal logo through the video decoder while using the dusk environment, SpaceWarp enabled and the GothicKit music provider. The decoder reached frame 5 and paused its one audio track. After six simulated unfocused polls it resumed on frame 5, rebased the clock by 88 ms, and decoded frames 6–10. The OpenAL track reported PAUSED and PLAYING at those transitions. Skipping the movie resumed the same `xt_daystd.sgt` music source. An unrelated settings notification during the movie left it paused. A separate pause/notification/resume sequence gave the same result. A wearer did not evaluate audibility or physically open the system menu. The run finished at 94% and 37.0 °C. The owner's INI hash was unchanged. The subsequent release APK was checked for diagnostic markers and its installed SHA-256 matched `d7b4aad497acd8d352b467564009e36f8121c6a7f6e1e8c05bbddc964057b7e0`.

### Keep rescheduled DirectMusic events in playback order

dmusic replaces same-type events within nine ticks of a requested time. It changed the timestamp in place, which could leave an event ahead of an earlier tempo change or behind a later one. The queue now restores heap order in the affected direction. Removing an event also checks for a right child before comparing the two children. Payload retention and the existing same-time message-type ordering are preserved in dmusic `0cf1ed5`.

The native queue test covered moves from tick 100 to 108 and back across a tempo change at 104, KEEP semantics and segment reference counts. It then drained queues of 1–255 initial entries with additional deterministic replacements. Old code reported 421 ordering/ownership check failures. The fix drained 33,690 events with zero failures. A release Quest city spells run loaded the GothicKit provider, logged transitions into XT_DayStd and KH_DayStd, then played patterns through measure five. All requested spell-scenario spawns succeeded. Dusk and SpaceWarp were enabled at CPU 1 / GPU 0. The game stopped at 95% and 37.0 °C. No audible assessment was made.

### Retry interrupted GPU light updates

Growing the scene light buffer released the old buffer before its replacement had allocated. Incremental updates discarded their dirty bits before staging and recording the patch. A failed allocation, staging write or dispatch could consequently leave existing bindings without the original buffer or leave unchanged lights stale on retry. The old buffers now survive replacement allocation, and dirty bits survive until the patch dispatch is recorded.

A host fixture ran the production update methods against two disjoint dirty ranges with failures at scene-buffer growth, staging-buffer growth, header upload, payload upload and dispatch. It compared the retained data and the byte-for-byte contents after retry. The original failed 14 checks and the fix passed all five paths. Android Release linked. Failures after successful command recording and driver allocation failures on Quest were outside this test.

### Build the light tree from the current eye's light data

Path tracing and irradiance caching could build their acceleration tree before pending changes to the light buffer were recorded. The second eye then saw updates the first eye had missed. The tree build now follows the light patch in the same command buffer and still precedes both optional consumers. A host command-order check modeled a two-light update across two eyes. Originally only the second eye saw both new values. Both eyes now see them, and the default renderer records no tree build. Android Release linked. These optional ray-query paths were checked from code and have not been run on Quest.

### Release partially initialized OpenAL effects

The sampled and streaming sound constructors could abandon an OpenAL buffer when its data or callback registration failed. They also returned a source after an unsuccessful attachment. Tempest `5e2c4c20` gives both constructors a common cleanup path, deletes a source before its attached buffer, and checks setup before playback. A null producer yields an empty effect.

A host fixture ran the production constructors and destructor against a counting OpenAL adapter. It injected errors at buffer creation, sample/callback setup, source creation, attachment and reference-distance setup, and checked success and empty sample destruction. The old code failed six checks, the change passed them all and Android Release linked. No native audio initialization failure was induced on Quest.

### Restore brush state and release stale UI resources

A reused UI mesh block could keep and bind its former texture after switching to a solid brush, or retain a sprite allocation after switching to a direct texture. Painting consecutive blocks with the same texture but a different clamp mode or channel format also reused the wrong state. Tempest `1fe44360` releases the obsolete references when the frame slot is reused, includes the sampling properties in state comparisons, and derives textured pipeline selection from the actual brush. Restoring an unchanged saved state now also pops it.

A host fixture ran the production brush state and mesh update methods with reference-counted texture/sprite handles. It covered texture and clamp changes, nested state restoration, an unchanged push/pop, and a sprite → direct texture → solid transition in one slot. The old code failed six checks and the new code passed all six. Android Release linked. No claim about headset memory consumption follows from this fixture.

An independent source review also found an inherited issue in Tempest's rectangle allocator. Releasing a full-page CPU atlas allocation can dereference its null parent, and smaller freed rectangles may disappear from the searchable tree. The regular Gothic HUD and focus labels use direct game font textures. We found no full-page sprite allocation in their normal Quest paths. This allocator issue remains open.

### Bound point lights separately in each view-depth slice

The tiled lighting pass projected the eight corners of each light's enclosing box and used the same screen rectangle for every intersecting depth slice. The new CPU bounds use the largest sphere cross-section in each occupied slice. Signed projection extents handle both off-axis eyes, with overlapping depth boundaries and a half-pixel screen margin. The shader, shading rate, light model and summation order are unchanged.

The first version clipped at the nominal far plane. A host regression using the default portal dimensions found a valid D32 depth that reconstructed beyond that plane in both fused and split arithmetic. The camera frustum admits the test light. The final slice now extends to the light's own far edge. A separate host run checked 630,000 sampled pixels, including offset and flipped projections and slice boundaries. It preserved all 257,821 in-range contributions in order and reduced synthetic list entries from 12,132 to 10,413.

On Quest, a diagnostic build read back each eye's pre-water scene depth after waiting for its own submission. A stratified one-in-sixteen pixel sample checked every visible light against the unprojected pixel and the exact list used by that eye. The tavern returned 372,263 and 359,672 in-range contributions, the city 32,540 and 29,034. Both eyes in both scenes returned zero missing contributions and zero unsorted tile lists. These diagnostic runs are excluded from timing.

Exclusive low-clock tests used CPU 1 / GPU 0, fixed noon, ordinary stereo and no captures. Each scene ran control/first/first/control, followed by final/final/control after the separate readbacks. Device and local APK hashes matched at every switch. GPU and recording figures average each run's last four reports, then average across runs.

| Scene | Build | VrApi fps per run | GPU ms/eye | Lighting and sky ms/eye | Stereo CPU recording ms/frame |
| --- | --- | --- | ---: | ---: | ---: |
| Tavern | Control | 51.6, 51.5, 51.9 | 9.569 | 3.660 | 6.173 |
| Tavern | First bounds | 55.9, 55.7 | 8.841 | 2.900 | 5.779 |
| Tavern | Final bounds | 55.8, 55.6 | 8.851 | 2.941 | 5.831 |
| City | Control | 60.6, 60.6, 60.6 | 8.034 | 1.554 | 5.388 |
| City | First bounds | 60.9, 60.9 | 7.998 | 1.502 | 5.298 |
| City | Final bounds | 60.6, 60.7 | 8.009 | 1.513 | 5.262 |

Kept the corrected bounds. The tavern saved 0.718 GPU ms per eye (1.436 ms across both eyes) and 0.342 ms of stereo CPU recording. City GPU time changed by 0.025 ms per eye and fps was effectively flat. Final runs ended at 92% battery and 40–41 °C with the app stopped. These checks do not cover every camera position or light in the game.

### Keep opaque HUD pixels on the SpaceWarp window plane

The game draws its shared HUD over both eyes, but previously submitted the underlying scene depth and motion at those pixels. Tempest `88ceb0c7` replays the shared UI mesh into a small transmittance mask using each brush's texture, alpha and blend mode. Opaque HUD samples now submit zero motion and window-plane depth after moving objects. Visible inventory models use their UI depth buffer for coverage. Translucent HUD pixels retain scene motion, and the per-eye world focus label stays outside the mask. Ordinary stereo and video skip the pass. Pre-recording allocation failure skips the correction and schedules a retry.

A low-clock Quest city combat run in the dusk environment injected two allocation failures and observed recovery. GPU readback found the same 400×225 mask in both eyes (411 opaque and 476 partially covered samples, hash 1336189657). All nine controlled blend, inventory depth and motion cases passed in each eye. All 19 stereo PNG pairs decoded at 1600×900. The motion overlay followed moving combatants, and the compositor screenshot showed the game window against the dusk sky. These captures precede or sample the compositor and do not establish wearer-perceived interpolation quality. Thin edges and partial transparency remain approximations at motion-vector resolution. An unrelated diagnostic found that Tempest's generic RGBA16F-to-RGBA32F CPU conversion interprets half words as normalized integers. The motion check therefore compared native half-float bits. The RGBA8 stereo capture path does not use that conversion.

The uninstrumented city SpaceWarp builds ran control/change/change/control at CPU 1 / GPU 0 and fixed noon. Controls returned 36.7 and 36.7 fps, 11.045 and 11.300 GPU ms/eye, and 6.258 and 6.260 ms of stereo CPU recording. Changed builds returned 36.4 and 36.4 fps, 11.230 and 11.160 GPU ms/eye, and 6.501 and 6.328 ms of recording. The mean differences were +0.023 GPU ms/eye and +0.156 recording ms/frame. Kept for correctness. Android Release linked, the shader passed SPIR-V validation, and the affected desktop path passed MSVC syntax checks. Runs ended at 92–93% and 38–41 °C with the app stopped.

### Publish and collect complete stereo PNGs

Tempest's PNG encoder ignored short writes and the output device's final flush. An exhausted file system could therefore publish a truncated eye image under its final name. Tempest `01f1446f` checks each write, flushes before reporting success and propagates output failure past the codec fallback. The capture worker removes its failed temporary file. The benchmark now stops its own process before listing new complete L/R pairs. Each transfer goes to a unique local temporary file, checks the remote SHA-256 and publishes without overwriting an existing file. A transfer failure leaves the complete pairs on the headset.

A native MSVC fixture linked the production codec with the vendored libpng and zlib. It exercised successful RGBA8 output, short and throwing writes, and rejected and throwing final flushes. The old encoder missed all five result/flush checks. The new encoder passed, and an independent decoder recovered the exact 16 RGBA channel bytes. A PowerShell fixture using the production capture routine passed clean transfer, interrupted transfer, same-length corruption and existing destination cases. It preserved incomplete and pre-existing pairs.

On Quest, the uninstrumented Android Release build ran city combat in dusk with SpaceWarp, motion visualization and a 15-second eye capture interval. The stopped process left four complete pairs. All eight transferred hashes matched, and each PNG decoded at 1600×900. The headset ended at 93% and 39.0 °C. Disk exhaustion and USB failure were exercised only by host fixtures.

### Honor each announced LOCAL-space change at its effective time

If the runtime announced two recenters before the next frame, the second timestamp replaced the first. A frame between their effective times could reuse the previous window placement, held grab and SpaceWarp history after the coordinate system had changed. The XR session now keeps pending times, consumes all changes due at the predicted display time and performs one placement/input reset for that frame.

A host fixture ran the production event filter and placement gate. It covered two future changes, out-of-order announcements, duplicate times, several changes due in one frame, another session and another space type. The old code failed four checks. The new code reset all four required frames and preserved held input on intervening frames. Android Release linked. Multiple physical recenter events were not generated on the unattended headset.

Source review also identified three remaining input cases. Android can report a canceled hardware key-up, physical gamepad removal can look like a normal button release, and a quick native gamepad tap can be consumed entirely between two game updates. Android's mouse translation also omits the two side buttons supported by the game's bindings. These cases have not been reproduced with external peripherals on Quest.

### Rejected inline Vulkan updates of the five view-global buffers

An XR-only trial recorded each view's globals with `vkCmdUpdateBuffer`, replacing the mapped staging uploads and per-view compute dispatches. A host comparison found the same CPU-staged bytes in 480 buffer snapshots across 48 stereo frames and all four slots. The production trial and control were built from the same game and HUD code. The trial source has been removed.

Exclusive main-street runs at noon, ordinary stereo, CPU 1 / GPU 0, used control/trial/trial/control with installed APK hashes checked at each switch. The controls returned 61.1 and 60.9 fps, 7.968 and 7.995 GPU ms/eye, and 5.258 and 5.365 stereo recording ms/frame. The trial returned 60.6 and 60.9 fps, 8.055 and 8.010 GPU ms/eye, and 5.951 and 5.821 recording ms/frame. Mean recording time increased by 0.575 ms/frame and mean GPU time by 0.051 ms/eye. The unchanged release path is faster in this scene. The extra diagnostic GPU readback APK was built but was unnecessary for this rejection. Runs ended at 92% and 40–42 °C with the app stopped.

### Present XR frames while waiting for the next cutscene image

The decoder slept on the render thread between OpenXR frame begin and end. A short or empty audio track left its consumed-sample clock behind, so the requested sleep grew on each remaining movie frame. XR playback now keeps displaying the current image until the next frame is due. Audio can delay a frame by up to one movie frame beyond its wall-clock deadline. The decoder keeps the last image until its display interval ends, and empty audio packets require no copy. The ordinary window still uses its existing sleep-based pacing.

A host fixture ran the production decoding, clock and focus decisions for a six-second 25 fps clip presented at 72 Hz. With only one second of audio the old code consumed 316 simulated seconds and asked for a five-second sleep on the last frame. With the change, all 150 images finished by 6.041 seconds with zero sleep requests. A silent track, no track, a one-second pause, an early skip and the ordinary window pacing all passed. Android Release linked and MSVC passed the desktop syntax check.

A diagnostic Quest build played the installed game's 25 fps logo and stopped enqueuing decoded audio packets after the sixth picture. The audio clock reached 960 ms and stayed there while the decoder reached frame 60 at 2,414 ms of active playback. The runtime accepted 200 movie submissions. A simulated focus interruption kept frame 15 for 280 ms across 20 submissions. A normal Escape event then closed playback. Between submissions 20 and 200 the headset accepted 180 frames in 2.530 seconds (71.1 per second). The diagnostic changed only the process's audio queue. A wearer did not evaluate lip-sync or sound. The headset ended at 92% and 39.0 °C, stopped.

### Keep available window controls usable after a partial XR failure

The handle and both corner brackets shared a readiness flag. Losing one upload hid the others, while their unconditionally active hit zones could still take gameplay input. Each part now draws and responds once its own static texture is available. A host allocation failure during one upload leaves independent controls and the surroundings upload usable. If passthrough disappears while its toggle is held, that capture ends and rearming still requires a release. A saved dusk preference whose startup pause failed is retried when the runtime reports passthrough recovery.

A host fixture exercised the production upload decisions, chrome layer list, near/ray hit zones and passthrough recovery with missing swapchains, rejected uploads and allocation exceptions at each part. It checked 102 conditions. The old code failed 18, the change passed all 102. A failed interactive switch retained its previous selection.

A Quest diagnostic injected a corner allocation failure and rejected the first startup switch to dusk. At frame 10 the runtime accepted five layers, the handle and button were ready, and a corner hit returned no target. After an injected passthrough restoration event, frame 20 used dusk and cleared the pending selection. The same targets and background persisted at frame 80. The compositor screenshot shows the city window against the dusk sky. This run also scheduled two synthetic LOCAL changes in distinct future frames: the first reset left one pending time, the second left zero. Physical pinches, actual memory exhaustion and physical recenter actions were not performed. The run finished at 92% and 39.0 °C with the game stopped.

### Honor canceled keyboard releases and Android mouse side buttons

Tempest `1cf85340` forwards a canceled Android keyboard up through the existing key-cancel dispatch. A canceled Tab can now clear its capture without opening inventory. Source ownership still preserves a held key contributed by another device. Android also translates the mouse Back and Forward bits into the existing Gothic side-button bindings.

A host fixture executed the production keyboard translation and capture routing with a canceled Tab, two-device release orders and a character-only key. It also exercised overlapping left/back/forward mouse buttons and a repeated unchanged sample. The old translation failed five checks. The corrected version produced just the one deliberate Tab release and all six expected mouse edges. Android Release linked. The uninstrumented APK's installed SHA-256 matched `f3ecc46d965d171210f1c5576c6353bf8bd1c808feae7f049395cf8f0235e998`. The app was stopped, sleep automation released, and the owner's INI checksum matched its original value at 93% and 38.0 °C. No physical keyboard cancellation or five-button mouse press was observed on Quest.

### Keep a modal dialog from reviving its opening key

A key-down handler can enter a synchronous dialog that pumps events. If Return was released while the handler was open, the outer dispatcher could recreate its capture after the dialog closed. The next genuine Return became a repeat. With Tempest `dae80282`, only the original pending press can publish its target. Nested release, focus cancellation or a newer press on the same scan code wins. Other keys continue to dispatch inside the modal loop.

A host fixture exercised 23 checks for nested release, same-scan replacement, focus clear, repeat before and after capture publication, another scan, shortcut consumption and overlay fallback. Nine checks failed on the old dispatcher and all passed on the new one. A second executable linked the actual Windows Release Tempest DLL and dispatched through a real widget. It received two fresh downs, one repeat and one up in the nested-release case. The Android Release APK also linked. No settings dialog or save operation was invoked on the headset for this test.

The full Windows x64 Release build linked `Tempest.dll` and `Gothic2Notr.exe` with DirectX 12 and Vulkan enabled. The offline build used the installed Windows SDK's DXC, an import library generated from the installed Vulkan loader, the worktree's Vulkan headers and the Android SDK's host shader compiler. The DLL was exercised by the input fixture. No desktop game session was launched.

### Cancel gameplay actions when a physical controller disappears

A physical X/Tab held during disconnect used to open inventory when its missing state was interpreted as an ordinary release. Tempest `6b40e656` keeps the last polled axis and key contributions separately and identifies which still qualify for normal release. Disconnect, device replacement, canceled joystick motion and flagged button/trigger ups invalidate only the affected source. An obsolete device's up or an OS repeat cannot claim the current key device. The game combines surviving native and XR sources before canceling a mapped key.

A host fixture ran the production native routing and gamepad mapper with a mock JNI device registry. It covered unplug, a canceled button alongside an ordinary release, simultaneous XR ownership, digital and analog trigger overlap, motion cancellation, replacement IDs, stale ups, repeats and rearming. The old code failed eight of fourteen checks. The new code passed all fourteen. Android and Windows Release both linked. This tests disconnect semantics in the app. An actual Bluetooth unplug was not performed on the unattended Quest.

### Deliver short Android controller taps in order

Android's native queue can deliver both edges of a physical button before the game polls. A complete X tap then disappeared. Immediate keyboard and mouse delivery also allowed those sources to overtake gamepad events. Tempest `c79c6c33` queues normalized input in native order for the game's frame loop. A modal dialog can consume the next event after Android acknowledges the native batch. Closing the dialog yields any remaining transitions to its parent. Stick look integrates once per update. Activity window focus and OpenXR session focus now both govern XR input, including the handle and resize controls.

The previous native poll produced zero mapped edges for a complete X tap in one batch. The new decoder, queue and mapper passed 40 host checks, including both Tab ownership orders, a newly sampled XR hold, a canceled button, hot unplug, focus loss, look integration and a hat transition that closed a key-binding dialog. Android and Windows Release linked.

Two low-clock city runs used Android shell gamepad X taps. On each, three Quest compositor captures showed ordinary play, open inventory, then ordinary play after the second tap. The final signed APK matched installed SHA-256 `d6f9f90b502a4958cf5c2d090d9247b74492aec1f0dfea4b8745ac36a4cd5f58`. Its run ended at 100% and 35.0 °C, with the game stopped, sleep released and the owner's INI hash unchanged. Same-batch delivery was controlled in the host fixture. A physical Bluetooth controller, a native focus transition during a real system dialog and simultaneous hand/keyboard input still need wearer testing.

### Classify each tracked eye at the waterline

The renderer used the base camera's underwater flag after applying the tracked eye offset and camera shake. An eye that moved across the water could use the wrong refraction and full-screen fog pass. Each active eye now counts distinct water-surface crossings along its offset and retains the base classification when no surface intervenes. The eye query accepts triangles from either side and counts a shared triangle edge or a double-sided polygon once. A half-millimetre vertical bias towards the base medium makes endpoint contact deterministic on the level and sloped surfaces used for swimming. Clearing the eye restores the original game-camera state. A perfectly vertical water polygon has no height-defined interior at exact contact.

A host executable linked the project's Bullet collision library and exercised the production eye override against level and sloped meshes, reversed winding, shared edges, doubled faces, stacked surfaces, exact contact, shake, missing water, repeat frames and restoration. The old flag failed 176 of 608 checks. The new code passed all 608. Windows and Android Release linked.

On Quest at CPU 1 / GPU 0 with SpaceWarp, a diagnostic camera put one eye 7 cm above a loaded city water surface and the other 7 cm below. The base alternated between dry and submerged. Logs recorded the expected zero or one crossing in all four combinations, with fog selected for the upper eye and underwater rendering for the lower one. Four inspected 1600 × 900 stereo pairs showed the distinct shading. The diagnostic used synthetic positions and an earlier build without the final endpoint bias. Exact waterline contact was verified on the host. The uninstrumented release installed with SHA-256 `e76f5704c43f90f1969e5995757cb2896d3f8e8f23654ad99272718e0bafd515`. The game was stopped, sleep released and the owner's INI hash matched at 100% and 37.0 °C. Natural head movement across a shoreline still needs a wearer.

### Rejected selective scene uniform uploads

The default stereo path uploaded and copied all five 1,204-byte view uniforms for each eye. The trial omitted unused virtual-shadow data and the second eye's two raster-shadow views, and removed a duplicate main-view upload. Path tracing, VSM and ray-traced shadows kept the views they read. A host fixture compared the actual uniform bytes, frame-slot writes and dispatch counts across all view selections. All 1,216 checks passed.

City main street at noon, ordinary stereo, no captures, CPU 1 / GPU 0. Each result uses 14 VrApi samples and the last four complete GPU and CPU reports.

| Run | FPS | GPU ms/eye | Record ms/frame | Update globals ms/eye |
| --- | ---: | ---: | ---: | ---: |
| Control 1 | 61.0 | 7.975 | 5.319 | 0.080 |
| Trial 1 | 61.0 | 7.978 | 5.336 | 0.083 |
| Trial 2 | 60.9 | 7.990 | 5.285 | 0.083 |
| Control 2 | 60.9 | 8.003 | 5.374 | 0.080 |

The mean recording difference was 0.036 ms/frame, smaller than the 0.055 ms spread between controls. Frame rate was identical and the GPU copy pass remained around 0.08 ms/eye. The trial was reverted. The final control restored the release APK, stopped the game and released sleep at 100% battery and 41.0 °C.

### Retain settled one-frame poses on interactive objects

A one-frame prop animation marked itself dirty after every successful update. Both city and tavern counters observed 884 model-prop updates per frame, including 237 settled one-frame props which resampled 651 animation tracks and rebuilt 1,074 skeleton bones. Rebuilding also marked their attachments and collision poses for publication. XR now holds a single fully blended one-frame object pose once its sampled bones are valid and the existing quaternion interpolation is independent of frame fraction. Forced updates still rebuild, and matrix, layer, body-state and head changes invalidate the pose. The game continues to advance event time and process animation effects.

A host fixture using the production interpolation and skeleton code compared every sampled pose, previous sample, published bone matrix, simulated physics copy and last-update time. Tests included ordered and unordered hierarchies, combination sequences, blends, reverse transitions, object moves, head rotation, forced updates and non-unit quaternions. All 12,000 checks passed. Both release targets linked.

City at noon, CPU 1 / GPU 0, ordinary stereo. Four complete CPU/GPU reports per run, 14 VrApi samples, no captures.

| Run | Game tick ms | Animation ms | Total update ms | Stereo record ms | GPU ms/eye | FPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Control 1 | 3.314 | 1.754 | 5.141 | 5.344 | 7.988 | 61.0 |
| Trial 1 | 3.084 | 1.572 | 4.724 | 5.511 | 7.968 | 60.7 |
| Trial 2 | 3.160 | 1.578 | 4.809 | 5.566 | 8.038 | 60.8 |
| Control 2 | 3.272 | 1.775 | 5.119 | 5.367 | 7.985 | 61.2 |

Both trials improved animation and game tick. Mean update fell by 0.364 ms/frame, including 0.189 ms in animation. Recording rose by 0.183 ms/frame, leaving a 0.181 ms reduction across update and recording. This GPU-bound city run showed no FPS gain. The final control ended at 98% and 42.0 °C, stopped and released to sleep. A wearer did not operate a chest or door during this comparison.

### Preserve usable XR controls when an optional action fails

A failed optional action creation left its handle in both binding lists. An invalid suggested action could reject the Touch and hand profiles together. Failed pose spaces were still passed to the loader each frame, and a failed action-set attachment still triggered synchronization attempts. The game now publishes only successfully created handles, submits the surviving bindings separately for each profile, locates only valid spaces and keeps native input available if XR action attachment fails. A rejected attachment also releases its unattached action set.

A host runtime injected errors into action creation, path conversion, attachment and one hand's pose space, including failed calls that modified their output. The previous code failed 27 of 75 checks. The new code passed all 75. Android and Windows Release linked.

On Quest a diagnostic build rejected only the near-touch action at startup. The real runtime accepted 15 Touch bindings and four hand bindings, created both aim spaces and completed all five sampled action syncs. The SpaceWarp city run reported 36.7 fps at CPU 1 / GPU 0. A separate ordinary stereo run rejected action-set attachment in the process. The log recorded zero sync calls and three compositor captures showed native gamepad X opening and closing inventory. It ended at 97% and 42.0 °C, stopped and released to sleep. The failure injection did not simulate a physical user's aim or pinch.

### Match the Quest worker pool to its existing three-worker limit

Quest reported a three-worker limit but still created sixteen threads and woke the full pool when dispatching jobs. The Android XR pool now starts three. Its job partitioning and the desktop pool size are unchanged. A host fixture running the production worker code counted sixteen versus three OS threads and passed all 656 job and shutdown checks on each build, including explicit tasks outnumbering the pool.

City main street, ordinary stereo, CPU 1 / GPU 0. The first three runs recorded 14 VrApi samples and the final control recorded 15. CPU/GPU means use the last four complete reports per run.

| Run | Update ms | Animation ms | Game tick ms | Stereo record ms | GPU ms/eye | FPS |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Control 1 | 4.787 | 1.590 | 3.127 | 5.476 | 7.980 | 60.7 |
| Trial 1 | 4.567 | 1.409 | 3.096 | 5.485 | 7.975 | 61.1 |
| Trial 2 | 4.580 | 1.433 | 3.084 | 5.521 | 7.983 | 61.0 |
| Control 2 | 4.761 | 1.576 | 3.114 | 5.520 | 8.003 | 60.7 |

Mean CPU update fell by 0.201 ms/frame, including 0.162 ms in animation. Mean recording rose by 0.005 ms. Both trials beat both controls on update time. This comparison covers the city at low clocks. The final control ended stopped and released to sleep at 95% and 42.0 °C.

### Recover ordinary stereo when optional SpaceWarp targets run out of memory

Session startup prepared the optional motion and depth swapchains even when the player had SpaceWarp off. A host allocation failure or a Vulkan image-view allocation failure could escape startup after the required color targets were already ready. Session setup now releases borrowed views before destroying their swapchains, disables SpaceWarp for that session, and proceeds with ordinary stereo. It also checks both format enumeration calls and retains only successfully created handles. Device loss and unrelated errors still propagate.

A host runtime injected failures in enumeration, swapchain creation and all twelve motion/depth view positions. It also wrote unusable handles on failed creation. The original failed 132 of 332 checks and the fix passed all 332. The uninstrumented Android Release linked.

A Quest diagnostic injected a GPU allocation error after wrapping the first right-eye depth image, with SpaceWarp and the VR environment requested. Six sampled frames successfully submitted five compositor layers in ordinary stereo with zero retained motion handles or depth views. The 1440 × 1440 compositor capture showed the city under the dusk sky. VrApi reported 67.5 fps over six low-clock samples. This exercised recovery from an injected allocation error. Actual system memory exhaustion and a wearer's stereo perception were not tested. The uninstrumented APK was restored and its installed SHA-256 matched `cd65aef42168d7796ed937c7e49ebfc6c384374b3d9efa7d24a84e149af2b29b`. The game was stopped, sleep released and owner INI hash unchanged at 96% and 39.0 °C.

### Release instrument-bank conversion storage

Loading a DLS instrument bank in the GothicKit music provider allocated its sample headers twice and lost the first array on every successful conversion. The font and its decoded PCM also crossed allocator boundaries on destruction, and several allocation failures leaked partial state or produced an incomplete preset. dmusic `888d540` creates the headers once, rejects incomplete generator and modulator lists, frees scratch storage on every return, and gives TinySoundFont ownership of the buffers it releases.

A synthetic two-preset bank used the real converter, decoder, preset loader, renderer and destructor with tracked allocation callbacks. The original retained one block and used the wrong deallocator twice on a successful load. The fixed converter passed all 113 checks over nine successful render/close cycles and individual failure at all 17 creation allocations. Every successful run produced the same stereo sample hash as the original. Android and Windows Release linked.

The uninstrumented Quest APK ran a low-clock city spell scenario with GothicKit, the dusk environment and SpaceWarp enabled. The app log recorded Gamestart, XT_DayStd, KH_DayStd, the intervening transitions and playback through measure five. VrApi reported 36.6 fps over eight samples. The game stopped and released sleep at 96% and 38.0 °C. Allocation accounting and sample equality were verified on the host. No listener assessed the physical audio output.

### Keep newer music requests across an interrupted theme load

A failed or empty theme cleared the shared pending theme, including a replacement queued while the audio callback was loading. The GothicKit stream could also consume a new theme then flush it when disable and re-enable landed between the separate stop and play checks. Both providers now leave replacements queued when a load fails and allow a same-file retry on the next request. GothicKit claims stop or play under the pending-theme lock, releases the lock before loading resources, and flushes a stop that arrived during the load. Failed stream stops are retried before consuming new music.

A two-thread host fixture ran the production provider code with controlled loader and stream responses. It covered same-file and different-file replacements, disable/re-enable before and during a load, an empty theme, duplicate requests, and failed stop and PCM callbacks. The original failed 116 of 772 sample and queue checks. Both fixed providers passed all 772 checks. Windows and uninstrumented Android Release linked.

A diagnostic Quest build queued a second `xt_daystd.sgt` request from another thread while substituting a missing segment for the first load. The app log showed the missing segment, the immediate same-file retry, actual XT_DayStd playback and a later successful transition to KH_DayStd. The SpaceWarp city spells run reported 36.6 fps at CPU 1 / GPU 0 and ended stopped and released to sleep at 96% and 37.0 °C. A listener did not assess the audio.

### Rejected keeping the final eye render pass open for the HUD

Tone mapping and the final CMAA2 pass leave the XR color attachment active. A trial removed the following preserve/rebind before the per-eye labels and HUD. It retained the inventory depth pass and the video/no-world clear. The player-facing change was reverted after timing.

| Run | GPU ms/eye | Stereo record ms | Update ms | FPS |
| --- | ---: | ---: | ---: | ---: |
| Control 1 | 7.990 | 5.509 | 4.596 | 61.0 |
| Trial 1 | 8.020 | 5.454 | 4.577 | 60.7 |
| Trial 2 | 8.020 | 5.402 | 4.677 | 60.6 |
| Control 2 | 7.980 | 5.584 | 4.584 | 60.7 |

Ordinary stereo city at CPU 1 / GPU 0, 14–15 VrApi samples, four complete CPU/GPU reports per run. Recording improved by 0.118 ms/frame on average, but both trials took longer per eye on the GPU than either control. GPU busy was 98% throughout. Mean frame rate went from 60.85 to 60.65 fps. The final control ended stopped and released to sleep at 95% and 41.0 °C. No visual-equivalence claim was needed to discard the trial.

### Allocate Vulkan upload commands only while filling the window artwork

Required XR startup created a ring of three command buffers and three fences before creating the game eyes. The static uploads only ever used the first pair. A failed allocation, even for an unused fence, could prevent stereo and passthrough from starting. The first rendered frame now uses one command buffer and fence for its synchronous artwork uploads, then releases both and the pool. Host or device allocation exhaustion leaves the independent game, passthrough, and SpaceWarp layers available. Other Vulkan setup failures request an application exit.

A host Vulkan fixture injected failures at each creation step, wrote unusable handles on failure, and threw from each independent artwork fill. It checked eye availability, creation and release counts, device loss, and sessions without artwork. The original failed 265 of 433 checks. The new code passed all 433. The uninstrumented Android Release linked.

On Quest, a diagnostic substituted a GPU allocation failure for the artwork fence after allocating the command buffer. Six SpaceWarp combat frames reported room passthrough available and all four artwork readiness flags clear. I inspected native 1600 × 900 stereo pairs 005 and 015, which showed the city, characters, focus text and health bar. The run reported 35.9 fps at CPU 1 / GPU 0 and ended stopped and released to sleep at 95% and 40.0 °C. The allocation error was injected.

### Retry an interrupted OpenXR session transition

Beginning or ending a session was attempted only on receipt of a state-change event. A runtime failure could leave the app indefinitely idle in READY or incorrectly mark a still-running STOPPING session as ended. The frame loop now retries on the next paced poll while that transition remains pending. It suppresses frames during STOPPING, applies the requested performance level after a successful begin, exits immediately on session or instance loss, and exits cleanly after three consecutive transition failures. Events from other sessions are ignored.

A host runtime covered zero through four consecutive failures to begin or end, terminal loss, state changes between attempts, foreign-session events and both performance settings. Old code failed 62 of 844 checks. The fixed code passed all 844. The uninstrumented Android Release linked.

The artwork-failure combat run also rejected the first begin with a simulated XR runtime error. The next poll successfully called the real `xrBeginSession` without another READY event. The runtime then reported SYNCHRONIZED, VISIBLE and FOCUSED and rendered both captured eyes. STOPPING error retries were exercised on the host. No physical headset removal or OS-driven stop failure was observed.

### Keep an off-center window grab under the hand

On the first held frame the window moved its handle center to the hand or aim ray, even when the player had selected the end of the handle. Direct touch also snapped the window forward if the pinch was inside the touch depth tolerance. Capture now remembers the actual contact point in both hand and window coordinates. The window still turns toward the viewer and follows controller depth adjustment while keeping that contact point fixed to the grip.

The host test exercised 270 near/ray combinations across three window widths, off-center positions and contact depths, followed by head and hand motion, rotation, stick movement and release. The previous implementation failed 1,548 of 2,160 checks, with a maximum stationary jump of 20.94 cm. The fix passed all 2,160. The uninstrumented Android Release linked.

On Quest a diagnostic supplied an off-center left pinch 4 cm in front of the handle, then a right controller ray on its other end. The real input path captured and released the correct hand both times. During stationary holds and 8 cm lateral moves the maximum logged anchor error was under 0.001 mm. A compositor capture showed the displaced game window beneath the dusk sky. Both gestures used synthetic poses, so a wearer's targeting and comfort remain unverified. The uninstrumented APK was restored and its installed SHA-256 matched `a8faeb0c8689dbf4ecb85ee1a9c9df761f1f393e17a3f2e83fa4db1274de04d9`. The game was stopped, sleep released and owner INI hash unchanged at 95% and 39.0 °C.

### Publish only successful OpenXR startup outputs

Instance, session and window swapchain creation previously wrote directly to persistent handles. A failed call that modified its output could leave an invalid optional swapchain available for upload or destruction. Optional sampler-function lookup had the same problem. Startup also read extension and color-format lists after failed enumeration, or beyond a shortened count. OpenXR handles and function pointers are now published only on success, and both required and motion/depth swapchains use checked format lists.

A host runtime returned unusable handles and non-null function pointers on error, shortened lists after the sizing call, and rejected each startup resource separately. The old code failed 38 of 173 checks. The fix passed all 173. The uninstrumented Android release linked.

On Quest a diagnostic rejected the grab-handle swapchain with a non-null output, and reported failure for sampler lookup after the runtime supplied a real function pointer. Five sampled frames reported both failed outputs null while the corners, panorama and environment button were ready. The compositor capture showed the city below the dusk sky with SpaceWarp enabled. VrApi reported 36.5 fps at CPU 1 / GPU 0. The injected failure cases for required instance/session/eye creation were exercised on the host. The installed uninstrumented release hash matched `2e0b4c6f440c4fe6d9d8d3425c1ea1efbf1dd4ef00779c2bbd85af335d94a63c`. The game was stopped, sleep released and owner INI hash unchanged at 95% and 38.0 °C.

### Can water passes be omitted for either eye?

A temporary Quest counter tested each active water and ghost cluster against the current eye frustum, with 1 cm added to its bounding sphere. After the waypoint and time commands, the city had 51–53 of 187 water clusters intersecting the frustum over 80 eye samples, the tavern 40–66 over 76, and the merchant path 47–51 over 108. Ghost counts were zero. All 264 eye samples required water under this conservative test. Occluded water can still intersect the frustum, including from inside the tavern.

Dropped the diagnostic and the proposed per-eye CPU scan. It would skip zero passes at these viewpoints. These were feasibility runs at CPU 1 / GPU 0, with 60.8, 55.8 and 72.7 fps respectively. No before/after frame-time gain was measured. The game stopped and sleep was released after each run, at 95–96% battery and 39.0–41.0 °C.

### Keep music channels bound as instrument banks are loaded

Updated dmusic to `b5d1e6a`. The synthesizer cached pointers into a growable font array and subtracted pointers from different allocations to repair them after a resize. Channels now point directly to the stable TinySoundFont objects. The cache retains the source DLS bank for as long as it uses its address as a lookup key. If the cache cannot add a new font it closes that font and preserves the existing playable channels.

A host test used 23 generated banks and the real conversion, cache and playback code. It grew the table twice, made one growth fail and retried, checked each channel and source bank reference, then played a note through every channel. The old implementation failed 309 of 674 checks, dropped ten established channels on the failed growth and left five allocations live. The fix passed all 674 and freed all allocations. Windows and Quest Release linked. The controlled cache relocation and allocation failure were verified on the host.

### Quest material vertices read one packed index word

Merged the owner's location-save command from `quest-test` at `3aa52f16`. No saves were made or changed during this handover. On Quest, bit 31 of a drawable packed triangle identifies active lanes, avoiding a second storage-buffer fetch of the count record per material vertex. All three producers preserve the legacy count record and XYZ indices. Windows keeps its existing shader and packing.

At CPU 1 / GPU 0, matching city controls with 92 lights measured 8.005 and 7.973 ms GPU per eye at 60.7 and 61.0 fps. The two trials measured 7.363 and 7.360 ms at 66.4 and 66.3 fps. G-buffer time fell from 2.300 to 1.905–1.907 ms. A separate control changed to 67 lights during sampling (7.700 ms, 63.6 fps) and was excluded from the matched comparison. In the tavern, one control measured 8.862 ms / 55.7 fps, and two trials 8.420 and 8.408 ms / 58.3 and 58.4 fps. The handover interrupted the second tavern control. The city comparison supports retaining the change independently. Each completed run stopped the game and released sleep at 94–95% and 39–42 °C.

The host test preserved 2,452,657 drawable triples across 83,200 meshlets. Vulkan motion readbacks matched in ten object, skin and morph cases. The production landscape vertex shader produced byte-identical coverage and interpolated depth in 68 cases (2,228,224 bytes). Android and Windows Release linked. A separate single-bone experiment was removed from the worktree before handover and has no headset timing or production change.

### Separate Android physical mouse selection from touchscreen targeting

Physical mice report relative movement and clicks at the window center. The mobile menu interpreted those clicks as taps, selecting the center row regardless of wheel or keyboard selection. Tempest now gives physical mice an independent capture ID (`8e8f52ee`). Left click activates the highlighted row once, right click goes back, and touchscreen slot zero keeps positional targeting and dragging. The host input/menu fixture passed 77/77 checks versus 22/77 before. The uninstrumented Quest build linked. An actual Bluetooth mouse remains untested.

### Restore signed instrument tuning in GothicKit playback

DLS region tuning stores signed cents. Shifting the 16-bit field right by 16 discarded every correction. Reading the signed field and passing cents directly to the SoundFont generator restored the expected playback rates. Actual synthesized audio at seven tunings between −99 and +99 cents passed all 49 host checks (15 failed before). At −50 and +50 cents, a 44.1 kHz voice advanced 0.971532 and 1.029302 source samples per output sample, respectively. The previous converter advanced 1.0 in both cases. Soundtrack prevalence and listening on Quest remain unverified.

### Reproduce continuous camera motion and check Quest occupancy

Use `-xrturn 18` to feed a constant horizontal turn in degrees/second into the same camera and hero rotation path as the mouse. `-xrforward` holds the gamepad forward action during gameplay. `-captureburst 4` captures four adjacent submitted stereo pairs after the scene settles and encodes the PNGs after the last pair. Use `-EyeCaptures 2` to pull them with the benchmark. Pixel readback waits for the GPU, so timing samples exclude the burst. The benchmark checks display wakefulness and focused/resumed apps before using the headset. Optional `-Apk` verifies an installation under the benchmark lease. `-VrStats` preserves runtime sample fields.

Before changing camera timing, the city at 18°/s with SpaceWarp on showed 0.506° mean yaw per app frame and 0.058° standard deviation across 53 adjacent steps before readback (10th–90th percentile 0.439–0.596°). Simulation intervals ranged from 25.2 to 31.8 ms at the same percentiles. The four left/right `-show motion` pairs at `20261120-111044-649` showed blue horizontal field intensity changing between frames across static buildings. The capture frames themselves had longer steps from readback. VrApi recorded 36–37/72 at VSnc 2 with zero stale frames in the sampled steady period. This is application-frame evidence. The runtime does not export its synthesized eye images through the app capture. Game stopped, sleep released, INI unchanged at 92%, 39 °C. The occupancy guard declined the next plain-stereo run before any install or wake.

### Preserve small camera turns at the input source

At 20% right-stick deflection the response curve produces 4.84 mouse pixels per second. Whole-pixel conversion held the camera still for roughly 0.2 seconds and then rotated it in one step. The game now applies the fractional stick displacement each simulation update. Tempest `a8ee2c1c` also accumulates relative mouse samples across Android's batched history and carries fractional physical mouse displacement across events. The host input/menu fixture passed all 85 checks, including 100 repeated positive and negative batched movements on each axis. A paired Bluetooth mouse and wearer comfort still need a physical test.

In the baseline SpaceWarp captures, 152 bidirectionally tracked patches on buildings and paving moved left by 5.9–22.0 pixels (median 11.87). All matched the direction shown by the motion overlay. In 17 patches with both vector components readable without clipping, the median signed vector-minus-image error was −0.07 horizontal and +0.14 vertical pixels. The 10th–90th percentiles were −0.34 to +0.63 and −0.44 to +0.43 pixels respectively. This supports retaining the sign and scale for opaque geometry. The current eye projection and the previous submitted projection of that eye produce the XY NDC difference. The fixed LOCAL application reference space uses identity `appSpaceDeltaPose`. The sampled D32 depth uses [0,1], near at the eye-to-portal distance in metres and far at 1000 m. [Khronos extension](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#XR_FB_space_warp) · [Meta Application SpaceWarp guide](https://developers.meta.com/vr/documentation/native/android/mobile-asw/)

### Recognize a stopped Quest launcher during the occupancy check

At 11:43 CET the display was asleep. Android retained a window-manager focus token for the system HomeActivity while its own activity record and all ten other activities were STOPPED and hidden. Every current-focus window was null, and the game process was absent. The benchmark now recognizes that standby state before installation and again before wake. An awake display, active window, another focused application, resumed activity or unreadable foreground still blocks the run. A replay of the actual standby dump passed, and six separate occupied or unknown cases were rejected.

### Remove the touchscreen walk pad from the Quest HUD

The touch-to-walk widget painted a 100 × 100 rectangle with 20% white at (10,790) in a 1600 × 900 eye. Across the four borders, the inward RGB jump in the stationary right eye at `20261120-084233-586` was 51 on each side. The mobile widget now stays hidden for an OpenXR window. Tempest also excludes hidden children from pointer targeting. Flat Android retains the walk pad.

### Generate SpaceWarp vectors at the visible surface

Sky pixels now rotate with the game camera and exclude translation. Solid and cutout movers keep their previous same-eye projection and the existing depth bias. Cutout textures select the same footprint as the scene pass. Moving ghosts and sufficiently opaque or bright translucent mesh and particle cores can contribute their own motion and geometric depth. Faint smoke, multiplicative effects and soft additive edges retain the background motion. Particle history matches lifetime identifiers, includes billboard shape and orientation, and drops a reused slot. A single submitted depth/vector can represent only one surface per sample. Animated water shading and reflections follow the water surface's geometry.

The host Vulkan sky fixture passed five camera/depth cases. An RTX 4080 ran the packaged Quest particle shaders and matched all eleven readbacks covering solids, cutouts, smoke, glow, slot reuse, occlusion, previous-view translation, growing particles and ribbons. The 3% bias for opaque movers remains. Meta's native sample also caches previous transforms and supplies the change of application reference frame. The portal's LOCAL frame stays fixed when the hero turns. [Meta native sample](https://github.com/meta-quest/Meta-OpenXR-SDK/blob/main/Samples/XrSamples/XrSpaceWarp/Src/XrSpaceWarp.c) · [Meta alpha-clip example](https://github.com/oculus-samples/Unity-AppSpaceWarp/blob/main/Assets/AppSpaceWarp/Shaders/URP_UnlitAlphaClip_WithMotionVectors.shader)

### Pace camera and movement against the display cadence

Direct use of consecutive runtime predictions made turn steps less even. In the city at 18°/s their standard deviation rose from 0.058° to 0.083° across 53 updates. The trial ran at 36.5/72 fps with VSnc 2, ten zero-stale samples, 100% battery and 35 °C after cleanup. The firmware did not expose a separate ASW counter. Moving the update after the wait also raised the runtime CPU & GPU metric from 27.2 to 32.9 ms. That trial was discarded.

The game update again overlaps prior GPU work. The XR simulation follows one display interval in plain stereo and two with SpaceWarp, spreading prediction corrections across updates. A runtime without refresh-rate reporting uses the measured interval. Display-clock discontinuities reset the filter. A host replay of the same Quest timestamps at 18°/s produced a mean 0.506° turn step and 0.017° standard deviation. Ten refresh/SpaceWarp combinations preserved elapsed time through 2,000 frames each. Replay measures the simulation input. The final combined smoke and wearer test cover the rest of the camera chain.

### Resize the aperture while preserving life-size projection

The game camera's reference plane stays at the distance calibrated for a 1.35 m-high window. The corner brackets independently change the width and height of the opening. Stereo baseline, 100 game units per metre and room-to-world parallax remain fixed. Recentring places the window at the calibrated viewing distance. A host geometry sweep of 6,804 combinations compared projected intersections and stereo disparity across seven apertures, eye offsets, viewing distances and scene depths. Both matched the ray-plane calculation to floating-point precision.

Eye density follows the angular size of the window with hysteresis, up to 2048 × 1536 per eye subject to runtime limits. The same rectangles drive the projection and quad paths, with quarter-resolution motion and depth. The compositor swapchains are allocated once. Scene targets can grow on a worker and switch at a frame boundary. The previous set retires after the last contributing eye fence. The UI has a minimum logical canvas and density scaling, including inventory model viewports. Use `-xrwindow W H` to reproduce an initial aperture in metres. Window-size cost on the Quest has not been measured in a controlled pair. The owner requested a single combined smoke in place of additional benchmarks.

A subsequent clock review found an overload corner case: limiting positive corrections to twice the nominal period slowed simulation at a sustained 25 fps on a 72 Hz display. Removing that upper bound passed a 1,200-frame replay with sustained 40 ms simulation steps. The existing 50 ms cap on an individual gameplay update remains. The 18°/s trace result was unchanged.
