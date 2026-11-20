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

These tags describe where the owner can carry each original audit commit. Commits tagged `[all]` that also touch SpaceWarp need only their shared hunks on master. The environment feature is assigned to the combined audit branch. The owner�s earlier commits keep their original subjects.

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
