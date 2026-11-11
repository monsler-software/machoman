# MEVK

An experimental Metal-to-Vulkan module for machoman, also buildable as a
standalone Vulkan backend. The CPU translator and runtime are separate from
MEVK.

## Implemented

- Vulkan device selection, coherent mapped buffers, command recording, fences,
  buffer copy/fill and dependencies across submissions.
- Metal device, queues, shared/managed buffers, labels, blit encoders,
  `didModifyRange:` and buffer `synchronizeResource:`.
- `CGDirectDisplayCopyCurrentMetalDevice` returns an owned Metal device for
  AppKit's display (ID 1); unknown display IDs return nil. The CoreGraphics
  bridge is built only when MEVK is enabled. Other CoreGraphics APIs continue
  to use the existing symbol synthesis.
- Real `.metallib` loading: LLVM parses Apple AIR bitcode, MEVK translates its
  instructions and metadata to GLSL 450, and shaderc produces SPIR-V. Libraries
  are cached by content while referenced. There are no shader-name substitutes.
- All 16 functions in Ableton Live 12's `UiFrameworkMetalShaders.metallib`
  translate successfully, including rectangle, texture, line, gradient,
  playhead and waveform shaders. Pixel tests cover rectangle, texture sampling
  and polygon control flow.
- 2D R8Unorm, RGBA8 and BGRA8 textures, including sRGB color variants, strided
  uploads and staged GPU readback.
- Render pipeline/pass descriptors, one color attachment, clear/load/store,
  blending and color write masks, viewport/scissor, vertex/fragment buffers and
  textures, inline uniforms, nonindexed/indexed and instanced draws.
- `CAMetalLayer` device, pixel format, drawable size, framebuffer-only property,
  `nextDrawable` and command-buffer presentation. AppKit places Metal layers on
  separate Wayland subsurfaces, so an OpenGL parent window can coexist with a
  Metal layer. Vulkan swapchains provide direct GPU output for RGBA8/BGRA8
  UNORM drawables; three `wl_shm` buffers remain available as a CPU fallback.
  View contents placement and layer contents gravity control scaling or cropping;
  top-left placement preserves pixel coordinates in drawables with allocation
  padding, using `contentsScale` to convert pixels to logical points.
- Copied scheduled/completed guest blocks, invoked once outside state locks;
  completion callbacks also run for failed committed batches.
- Bounded Vulkan buffer and command/fence/descriptor-pool caches, framebuffers
  retained with textures, and up to eight drawable texture leases per layer.
  Resources still referenced by commands or presentation cannot be recycled.

`commit` submits without waiting for GPU completion. An ordered worker retains
commands, presents drawables and invokes completion handlers; explicit
`waitUntilCompleted` includes those handlers. Shutdown stops display links and
drains the worker before guest runtime destruction. CoreVideo uses the monitor
refresh reported by AppKit, with `MACHOMAN_DISPLAY_HZ` available as a test override.

Presentation defaults to direct Vulkan Wayland output when available. A GPU
blit crops/scales the drawable into the swapchain without frame readback or
`wl_shm` copying. MAILBOX is preferred, with FIFO as a fallback. Busy submission
slots or unavailable swapchain images skip frames rather than wait; resize and
teardown may wait for the queue. This path still uses a GPU blit and compositor
presentation, rather than rendering directly into swapchain images or scanout.
`MACHOMAN_MEVK_PRESENT=cpu` forces the readback path; `vulkan` requires direct
output and reports errors instead of falling back. Auto mode falls back to CPU
for unsupported surface/format combinations, including sRGB drawables.
AppKit serializes lazy native window creation across Cocoa and the completion
worker, so the first presented frame and `orderFront` use the same Wayland window.

Resources retain their Vulkan context. Command buffers retain GPU resources
until destruction and wait for in-flight work before freeing allocations.
Invalid ranges, incompatible resources and invalid encoder state poison the
batch: it submits nothing and preserves the first diagnostic as a Metal
`NSError`. Unsupported library/pipeline inputs return nil with an error.

## Build and test

Requires CMake 3.25+, a C++26 compiler, Vulkan headers/loader, LLVM 23+ development
files and shaderc (`shaderc_shared`). GPU tests require a working Vulkan driver.
No macOS SDK or display is needed for the standalone backend.

```sh
cmake -S modules/MEVK -B /tmp/mevk-build -G Ninja \
    -DCMAKE_CXX_COMPILER=clang++ -DMEVK_BUILD_TESTS=ON
cmake --build /tmp/mevk-build
/tmp/mevk-build/mevk-probe
ctest --test-dir /tmp/mevk-build --output-on-failure
```

Add `-DMEVK_TEST_METALLIB=/absolute/path/to/UiFrameworkMetalShaders.metallib`
when configuring to enable tests using the installed application's real shaders.
The fixture is not distributed by this module.

To build the host Metal framework and arm64 guest tests:

```sh
cmake -S . -B /tmp/machoman-mevk-build -G Ninja \
    -DCMAKE_CXX_COMPILER=clang++ -DMACHOMAN_BUILD_MEVK=ON \
    -DMEVK_BUILD_TESTS=ON \
    -DMEVK_TEST_MACOS_SDK="$PWD/third_party/sdk/MacOSX26.1.sdk" \
    -DMEVK_TEST_METALLIB=/absolute/path/to/UiFrameworkMetalShaders.metallib
cmake --build /tmp/machoman-mevk-build --target \
    test_mevk_buffers test_mevk_shaders test_mevk_render \
    mevk_guest_metal_smoke mevk_guest_metal_render
ctest --test-dir /tmp/machoman-mevk-build -R '^mevk_' --output-on-failure
```

Verified on NVIDIA GeForce RTX 4060:

- `mevk_buffers`: GPU blit ordering, guard bytes, invalid batches, resource
  lifetime and concurrent recording/submission.
- `mevk_resource_reuse`: bounded allocation reuse across 64 GPU submissions,
  retained resource lifetime, logical buffer bounds and GPU crop/scale geometry.
- `mevk_shaders`: AIR index support, rejection of unsupported i64 computations,
  malformed modules and metallib section/wrapper bounds and duplicates.
- `mevk_render`: pixel comparisons using actual Ableton shaders, unaligned
  Metal buffer offsets, 16-bit indices, scissor/blending, texture sampling,
  short uniforms and loops/phi transfers. Writes `mevk-render.ppm`.
- `mevk_guest_metal`: real arm64 Metal buffer API, protocol/ownership checks,
  labels, NSRange ABI, blit readback and encoder errors; display/device identity,
  sample-count queries and Live's three-permit dispatch semaphore, including
  mixed Objective-C/dispatch ownership, deadlines and a guest pthread signal.
- `mevk_guest_render`: real arm64 AIR library/pipeline/texture APIs, managed
  buffers, viewport/scissor/region/clear-color ABI, stack arguments for indexed
  draws, asynchronous/copied/reentrant callbacks, failed batches, retained
  drawable isolation and CoreVideo timestamp/refresh-return ABI at 165.002 Hz.
  The manual `--wayland` probe races the first presentation against `orderFront`
  and draws a padded texture with a distinguishable border for crop checks.

Guest tests enable strict selectors so missing APIs fail. The drawable test
uses headless AppKit by default; the same guest was also verified on Wayland.

Other projects can link `MEVK::mevk`. Public headers are in `include/mevk/`.
`mevk-shader-inspect file.metallib` reports translated shader functions and
SPIR-V sizes. Set `MEVK_BUILD_TOOLS=OFF` to build only the library.

## Enable in machoman

```sh
cmake -S . -B build -DMACHOMAN_BUILD_MEVK=ON
cmake --build build --target machoman AppKit MEVK_Metal MEVK_CoreGraphics
python3 -B scripts/run-live.py --mevk --seconds 180 --log work/live-mevk.log
```

For an interactive diagnostic run of the installed Live Trial:

```sh
python3 -B scripts/run-live.py --mevk --keep-running --gdb \
    --log work/logs/live-mevk.log
```

For direct output with aggregate timing and allocation counters:

```sh
python3 -B scripts/run-live.py --build-dir /tmp/machoman-mevk-present-build \
    --mevk --mevk-present vulkan --quiet-appkit --frame-timing --keep-running \
    --log work/logs/live-mevk-direct.log
```

`MEVK output` reports time spent submitting presentation, CPU readback counts,
drawable pool size and created/reused allocation counters. It excludes guest
UI rendering and GPU completion waits. `AppKit Metal WSI` reports actual queued
swapchain presentations and skips. Neither counter is a display FPS measurement.

The launcher checks that both Metal and CoreGraphics are built MEVK frameworks
before starting Live. `--strict-selectors` stops at the first missing
Objective-C method; `--gdb` saves native stacks at the first fault, and the
launcher saves RSS samples beside the log. `--keep-running` removes the time
deadline and keeps the RSS guard. GPU/Wayland access must be available to the
launcher. Use `--build-dir` to select a separate build.

MEVK is opt-in and defaults to OFF. Enabling it installs a real
`Metal.framework/Versions/A/Metal.so`, a CoreGraphics-to-Metal bridge and
connects AppKit to Metal.
AppKit's search path handles Cocoa/QuartzCore being loaded before Metal.
For parallel OpenGL development, keep the normal `build/` configured with
MEVK OFF and enable MEVK in a separate build directory.

An application's venv may contain a Metal stub; override it with
`DYLD_FRAMEWORK_PATH="$PWD/build/dylibs/System/Library/Frameworks"`.
`DYLD_PRINT_LIBRARIES=1` shows the loaded framework, and `MACHOMAN_MEVK_TRACE=1`
logs GPU submissions and drawable presentations.

## Current limits

This implements the AIR/render subset needed for the observed Live UI shaders,
not the full Metal API. MSL source compilation, compute, general AIR intrinsics,
vertex descriptors/stage-in, depth/stencil, MSAA, mipmaps/arrays, general sampler
states, texture blits, MetalKit and explicit Metal `enqueue` ordering are not
implemented. Unsupported operations report diagnostics rather than advertise
complete GPU feature sets.

There are 16 buffer and 16 texture slots per shader stage. Uniform byte offsets
use push constants; buffer sizes are padded internally to permit short uniforms.
General 64-bit shader arithmetic is rejected; zero-extended 32-bit buffer
indices and their phi/select transfers are supported. Texture sampling currently
supports the linear/clamp literal sampler used by the fixture.

Buffer blits still require four-byte alignment. Private/noncoherent buffers
and nondefault CPU cache modes are unsupported; managed buffers use coherent
host backing. Commands submit in commit order on a shared Vulkan queue.
Presentation supports basic layer/view translation and size conversion,
resampling to logical points. Layer transforms, clipping, fractional-scale
viewports and color management are not complete. If all three compositor buffers
are busy, the frame is skipped until releases arrive; AppKit must process events.

Next steps are broader real-application coverage, resource/pipeline cache tuning
and compute/MSAA/depth support as needed.
