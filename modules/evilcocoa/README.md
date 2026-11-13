# evilcocoa

OpenGL 3.3 rendering for host Cocoa controls in machoman. This package has no
Objective-C, guest runtime, Wayland or MEVK dependency. The caller supplies a
current OpenGL context, its default framebuffer, and a list of controls in
logical points with a top-left origin.

The dark appearance uses macOS-style rounded controls, vertical bezel
gradients, soft shadows, blue default buttons and popup arrows, inset fields
and titled groups.
Focus, pressed and disabled states, clipping and text compositing run on OpenGL.
Pango shapes Unicode text and Cairo rasterizes glyph masks, cached in GL textures.
Items include labels, buttons, checkboxes, radio buttons, text fields, popup and
combo boxes, sliders, progress indicators, segmented controls, images, groups,
tables and scroll views. The AppKit adapter supplies guest `drawRect:` bitmap
snapshots through image items.

Dependencies: libepoxy and pangocairo; EGL for framebuffer tests.

```
cmake -S modules/evilcocoa -B /tmp/evilcocoa-build
cmake --build /tmp/evilcocoa-build
ctest --test-dir /tmp/evilcocoa-build --output-on-failure
```

In machoman, `MACHOMAN_BUILD_EVILCOCOA=ON` enables the AppKit adapter. Guest
OpenGL and Metal windows keep their own rendering path. Destroy a renderer
while its GL context is current.
