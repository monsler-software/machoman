# machoman

A wine-like compatibility layer for running macOS **arm64** and **x86_64** binaries on
Linux **x86_64** hosts. Written in C++26 + CMake.

Currently runs:
- GrandPerspective (fully)
- Roblox (Login worx; crashes on play attempt)
- Ableton Live 12 Suite (Partially; no audio)
- Some CLI Applications
nothing else were tested

Three layers, matching the three build/run stages a translated program
goes through:

| Layer | Directory | Role |
|---|---|---|
| 1. venv | `src/venv/`, `include/machoman/venv.h` | A directory tree standing in for a macOS root (like a WINEPREFIX): `bin/`, `original/`, `usr/lib/`, `System/Library/Frameworks/`, plus a manifest. |
| 2. AOT translator | `src/translator/`, `include/machoman/translator.h` | A "reverse Rosetta 2": decodes arm64 (A64) machine code and statically re-emits x86_64 machine code, once, ahead of time (not a JIT). |
| 3. runtime | `src/runtime/`, `include/machoman/runtime.h` | Loads translated code into host memory, resolves the venv's dylib stubs, and dispatches trapped Darwin syscalls to host (Win32/Linux) equivalents. |

`src/common/macho.cpp` (`include/machoman/macho.h`) is a small Mach-O 64-bit
parser shared by all three layers — it's the thing that lets `inspect`,
`translate`, and (eventually) `run` all agree on segments, sections, and
linked dylibs. Universal (fat) binaries are narrowed to their arm64 slice.

## Status

`ripgrep` (arm64 macOS build, `third_party/samples/rg-arm64`) runs end to
end: argument parsing, directory walking, multi-threaded regex search
with NEON, correct output and exit codes. C++ programs cross-compiled for
arm64 macOS run with iostreams, locales, virtual dispatch and exceptions
(unwound through translated frames by a guest-side libunwind).

- **Linux dialogs and Cocoa controls**: `NSOpenPanel` and `NSSavePanel` use
  `org.freedesktop.portal.FileChooser` over GIO, including folder selection,
  multiple files, filters, cancellation and asynchronous completion handlers.
  `NSAlert` uses GTK message dialogs. No helper command is spawned. Portal
  dialogs need a running session bus and a file chooser portal implementation.
  The optional GTK and GIO development packages enable the corresponding
  backends; headless requests cancel instead of claiming a selection.
  [`modules/evilcocoa`](modules/evilcocoa/README.md) renders standard labels and
  buttons with OpenGL, Pango text masks and an independent EGL context. Build
  with `MACHOMAN_BUILD_EVILCOCOA=OFF` to omit it; its dependencies are libepoxy
  and pangocairo. Packed Cocoa bitmap drawing, RGB/RGBA images and popup
  controls are supported; other image formats and table features remain partial.
- **Cocoa application entry point**: AppKit's `NSApplicationMain`, re-exported
  by Quartz and Cocoa, honors `NSPrincipalClass` and `NSMainNibFile`, loads
  keyed plists and compiled `NIBArchive` graphs, and enters the event loop.
  `NSWindowController` provides lazy nib loading and window ownership.
  GrandPerspective can scan directories in the background, report progress,
  cancel scans and draw its treemap with bundled color palettes. Recent scans
  are persisted through `NSDocumentController`. Cocoa coverage remains partial.
- **Process and launch semantics**: the Carbon Process Manager reports
  this one application (with the bundle's signature), and AppKit sends the
  delegate the launch messages macOS sends — `applicationDidFinishLaunching:`
  and the open-application Apple Event's
  `applicationShouldOpenUntitledFile:`/`applicationOpenUntitledFile:`.
- **Mach-O parsing**: fat/universal files, all dylib load-command kinds,
  `LC_DYLD_INFO` rebase/bind/weak-bind/lazy-bind opcode streams, export
  tries, `LC_SYMTAB`, `LC_RPATH`. Bounds-checked against truncated or
  malformed inputs.
- **Dynamic linking**: an install name resolves under the venv (then
  `$MACHOMAN_DYLIB_PATH`, then `<machoman>/dylibs`) either to a *host*
  shared object (`usr/lib/libSystem.B.dylib.so`) or to a real *arm64
  Mach-O dylib* (`usr/lib/libc++.1.dylib`, an app bundle's
  `Frameworks/`), which is loaded at a free base, rebased, bound and
  translated like the executable. `@rpath`, `@executable_path` and
  `@loader_path` work as on macOS, and so do dyld's environment
  variables. Run-path lookup follows the loading image's `LC_RPATH` entries
  and its ancestors, expanding `@loader_path` relative to the image that
  declared the entry. `DYLD_FRAMEWORK_PATH` and `DYLD_LIBRARY_PATH` are searched
  *before* the install name the binary was linked with (they override
  even an absolute one, as Apple documents), the `DYLD_FALLBACK_*` pair
  after it, `DYLD_ROOT_PATH` adds an alternate system root,
  `DYLD_INSERT_LIBRARIES` loads libraries nothing links against and
  `DYLD_PRINT_LIBRARIES` lists what was loaded. `machoman run -e
  KEY=VALUE ...` sets any of them (and anything else the guest reads)
  for one run. Guest symbols come from export tries
  (following re-exports and the dependency chain); host symbols from
  `dlsym` — first the Darwin name verbatim (`_open`: an explicit
  Darwin-ABI implementation), then without the underscore, which falls
  through to the host libc. Unresolved imports get trap pages so the
  runtime can name the missing symbol when it is hit.
- **Guest C++ runtime**: LLVM libc++/libc++abi/libunwind are cross-built
  as arm64 Mach-O dylibs (`scripts/build-guest-runtimes.sh`, needs a
  macOS SDK) and run as guest code. The C++ ABI (this-returning
  constructors, sret, vtables, exception unwinding) therefore never
  crosses into host code; only libSystem's C ABI does.
- **Swift runtime foundation**: [`src/dylibs/swiftCore`](src/dylibs/swiftCore/README.md)
  provides native Swift heap allocation, ARC, guest `swiftself` destructors,
  stack/static object lifetime, once initialization and dynamic exclusivity
  checks. Mach-O ABI probes run on arm64 and x86_64. Swift standard-library
  symbols, type metadata, weak references, Objective-C bridging and async
  tasks remain unimplemented; arbitrary Swift applications are not supported yet.
- **libSystem.B.dylib** (`src/dylibs/libSystem/`): only what differs from
  glibc — `open`/`fcntl` flags, `stat`/`dirent` layouts, `errno` numbers,
  `sysconf`/`clock_gettime` ids, `ioctl` requests, `mmap`/`madvise`
  flags, pthread objects (Darwin sizes), guest threads via the runtime's
  `mm_host_api`, dispatch semaphores, TLS (`__tlv_bootstrap`), dyld
  introspection (`_dyld_*`, `_dyld_find_unwind_sections`), `_NSGet*`,
  stack protector, signals, setjmp/longjmp, the printf/scanf family with
  Darwin's stack-passed variadic arguments, xlocale (`newlocale`, `*_l`,
  `nl_langinfo`, `localeconv`) and the `_DefaultRuneLocale` ctype table.
  Thread-local values and pthread-key destructors run with the original guest
  thread identity before TLS storage is freed; locale date functions accept
  Darwin's null/current and global locales.
  `setjmp` buffers use the SDK layouts for arm64 and x86_64, restoring
  callee-saved registers and the requested signal mask. Native recovery
  signals use a separate host stack, including when the guest uses small fibers.
- **The system calls a Darwin program makes that Linux spells differently**:
  socket options and addresses (`SOL_SOCKET`, every `SO_*`, `sockaddr`'s
  length byte), signal masks (`SIG_BLOCK` is 1 on Darwin, 0 here), malloc
  zones (a real `malloc_zone_t` whose function pointers the guest calls),
  `posix_spawn`/`NSTask` (a Mach-O helper is re-launched through machoman
  itself), and CRC32/CRC32C, which AArch64 has instructions for.
  `inet_pton`/`inet_ntop` translate IPv6 address families, including DNS-name
  classification. Mach host page-size, CPU/load and VM-statistics queries use
  Linux accounting with SDK layouts and revision counts; XNU-specific cache
  and compressor counters have no host equivalent and remain zero.
- **CommonCrypto** (`src/dylibs/libSystem/commoncrypto.cpp`): AES (128/192/256,
  ECB and CBC, PKCS#7) and SHA-256, implemented in-tree because machoman
  takes no external dependencies, and checked against the FIPS vectors.
- **libobjc.A.dylib** (`src/dylibs/libobjc/`): Apple's class/method
  metadata (relative method lists, categories, protocols, ivar sliding,
  `+load`/`+initialize`), `objc_msgSend` dispatch to guest or host
  implementations, ARC, weak references, associated objects, blocks.
- **CoreFoundation + Foundation** (`src/dylibs/Foundation/`): one object
  for both, because the types are the same — every CF type is an
  Objective-C object of a host class, which is what toll-free bridging
  means: `CFStringRef` and `NSString *` are the same pointer, `CFRetain` is
  `-retain`, and a guest subclass of `NSString` is a real class. Covers
  NSString/NSMutableString (UTF-16 storage, Apple's constant-string layout
  for `@""` literals, encodings, formatting with `%@`), the collections,
  NSData, NSNumber/NSValue, NSURL (file constructors with base URL resolution
  and UTF-8 filesystem representations), NSError, NSBundle (real `Info.plist`
  reading), property lists (XML and binary, both directions),
  CFPreferences/NSUserDefaults, NSProcessInfo, NSFileManager, NSLocale,
  NSUUID and a run loop.
- **AppKit** (`src/dylibs/AppKit/`): the object model — NSResponder and
  the responder chain, NSView with AppKit geometry (points, bottom-left
  origin, frames, bounds, hit testing, coordinate conversion), NSWindow,
  NSApplication with a real event loop, NSScreen, NSEvent, NSMenu,
  NSColor, NSCursor, NSImage, NSOpenGLContext — over a platform backend.
  The **Wayland backend** (`platform_wayland.cpp`) gives each NSWindow an
  xdg-toplevel, paints it from a `wl_shm` buffer, delivers pointer and
  keyboard events through `wl_seat` + xkbcommon, and creates an EGL
  surface only when a GL context attaches (a surface that has both is a
  protocol error). It needs `wayland-protocols` for the xdg-shell
  definitions; without it AppKit runs headless, which is enough to drive
  an app up to the point where it would draw.
- **Nib loading** (`src/dylibs/AppKit/nib.cpp`): a `.nib` is a keyed
  archive (a binary plist whose references are UIDs), so loading one needs
  no new file format — it rebuilds the objects, the menu tree, the windows
  and the connections between them. An outlet assigns through the setter
  or straight into the instance variable; an action sets target/action.
  This is what gives an app that sets no delegate in code — Ableton Live
  is one — its `NSApplicationDelegate`.
- **The frameworks around them**: CoreMIDI (`src/dylibs/audio/`) provides
  client and port handles but does not yet expose MIDI endpoints;
  Carbon/ApplicationServices/CoreServices
  (`src/dylibs/carbon/`) cover the keyboard state, text input sources and
  event-loop timers a modern Cocoa app still calls; Accelerate
  (`src/dylibs/accelerate/`) is a real vDSP: FFTs (split-complex, packed
  real), vector arithmetic and the `vv*` transcendentals;
  `/usr/lib/libiconv.2.dylib` is glibc's iconv under Apple's names.
- **CoreAudio and PipeWire** (`src/dylibs/audio/`): the HAL enumerates real
  PipeWire output nodes, their names and channel counts, exposes device/stream
  properties, and reports hotplug and default-output changes. HAL IOProcs and
  the AUHAL, Default Output and System Output AudioUnits send float32 PCM to
  PipeWire. Default units follow desktop routing; AUHAL and IOProcs use the
  selected device. Planar/interleaved conversion and buffering use preallocated
  storage, including fixed HAL blocks when the graph quantum differs.
  Host API v3 keeps a persistent guest thread/stack for ARM64 and native x86_64
  callbacks, silences failed renders, and stops callbacks before teardown.
  Isolated PipeWire integration tests capture actual samples, validate channel
  order and exercise routing, errors and shutdown without playing on hardware.
  Input/capture, MIDI transport, effects and third-party AudioUnit factories
  remain unimplemented. CMake enables the backend when `libpipewire-0.3`
  development files (>= 0.3.50) are available; `-DMACHOMAN_PIPEWIRE=OFF` omits
  it. `MACHOMAN_AUDIO_BACKEND=none` forces no-device behavior, and
  `MACHOMAN_AUDIO_TRACE=1` traces properties and stream state changes.
  Without a server or backend, no physical devices are advertised; the null
  adapter is reserved for offline engine tests.
- **Stub frameworks** (`src/dylibs/symbol-synthesis/`): one object standing
  in for every framework that is not implemented yet, installed under each
  of their paths. It defines nothing; the runtime asks it for each bind
  through `machoman_resolve_symbol`, and it answers with a real objc class
  for `_OBJC_CLASS_$_X`, an NSString for a data constant, or a function
  that reports the call once and returns 0 — so a program only fails where
  it actually depends on the missing behaviour, and the message names the
  symbol.
  Synthesized classes also support cached, typed stateful properties and
  ARM64 aggregate returns. Potential pointer, output-parameter and unknown
  ABI hazards are marked with `risk:` in stderr. See
  [dynamic compatibility stubs](docs/dynamic-stubs.md) for contracts and limits.
- **AOT translator**: the complete integer A64 ISA plus AdvSIMD integer
  and floating point (scalar and vector, FMA, conversions, compares,
  FPCR rounding mode and flush-to-zero mirrored into MXCSR). Against
  Ableton Live 12 (22M instructions) 99.8% translate; `machoman translate
  --stats` breaks the rest down by encoding class.
- **runtime**: maps images, links, translates each image's code, runs
  static initializers dependency-first, then the entry point; services
  host calls (including vtable slots of host objects, resolved with
  `dladdr`), guest threads on host threads, guest signal delivery, and
  reports precisely why a guest stopped, with a symbolised backtrace.
  Indirect branches go through a process-wide two-level code map, so
  calls between images cost the same as within one.

Ableton Live gets through all 3792 of its static initializers, `main`,
NSApplication startup, its embedded Python, licensing, screen setup, MIDI
and its menus, loads `MainMenu.nib` (which is where its application
delegate comes from), enters `-[NSApplication run]`, builds its interface,
talks to its extension host over a Unix socket, opens its databases and
creates its `NSWindow`; it is now inside `InitApplication`. Its `os_log`
output is readable with `MACHOMAN_OSLOG=1`, and `kill -QUIT` prints every
guest thread's stack, which is how each of those steps was diagnosed.

## Building

```sh
tar -C third_party/sdk -xf MacOSX*.sdk.tar.xz   # a macOS SDK, for the guest C++ runtime
scripts/build-guest-runtimes.sh                   # libc++/libc++abi/libunwind as arm64 dylibs
cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build          # also builds build/dylibs/ (libSystem, libobjc, the guest runtimes)
ctest --test-dir build --output-on-failure
```

On Linux x86_64, the runtime also has an experimental direct execution path
for thin macOS x86_64 executables and matching guest dylibs. Build their C++
runtimes separately, then rebuild machoman:

```sh
MACHOMAN_GUEST_ARCH=x86_64 scripts/build-guest-runtimes.sh
cmake --build build
```

These runtimes are installed under `build/dylibs/x86_64/usr/lib`; C++ code
stays in the guest ABI. Native imports use the same host frameworks as
translated arm64 code, with bridges for callbacks, TLS, variadic arguments
and Objective-C aggregate calls. Synchronous native faults reach guest
handlers with Darwin signal contexts; handler returns restore edited
registers, x87/SSE state and the interrupted stack. Intel-only fat binaries,
asynchronous signal delivery and alternate signal stacks still need support.

The optional DNS library uses portable wire-format routines from
[machoman-libresolv](https://github.com/monsler-software/machoman-libresolv)
and the Linux resolver for configuration and queries:

```sh
git clone https://github.com/monsler-software/machoman-libresolv modules/libresolv
cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++
cmake --build build
```

Set `MM_TEST_MACOS_SDK` to the local SDK path to enable the arm64 and Intel
guest integration tests when configuring CMake.

Guest `posix_spawn` starts each Mach-O child in a separate Linux process
through the same launcher and venv. File actions support descriptor duplication,
closing, opening and changing the working directory. Exec failures return to
the parent, and ordinary `waitpid` observes the child's exit status.
`shm_open`/`shm_unlink` use a namespace under `<venv>/.machoman/ipc/shm`;
`mmap(..., MAP_SHARED, ...)` then shares real backing pages between processes,
including children of a different guest architecture. Removing a name preserves
existing mappings.

Mach IPC uses one automatically started broker per venv. Unix sockets authenticate
Linux processes with `SO_PEERCRED`; port names stay local to each task. The broker
owns port rights, message queues, port sets, bootstrap services, notifications
and exception-port tables, and uses pidfds to clean up when a process exits.
Complex messages transfer receive/send/send-once rights, OOL memory and port
arrays between arm64 and Intel guests. Named memory entries carry memfd backing
through `SCM_RIGHTS` and `mach_vm_map` maps shared pages in each process.
Guarded receive descriptors and `mach_port_construct`/`destruct` preserve guard
checks; send-once replies can bypass an ordinary full message queue.

The SDK-built Mach tests cover all four parent/child architecture combinations,
message layouts, trailers, shared entries, notification ownership and task/thread
exception-port bookkeeping. This subsystem does not implement Mach vouchers,
scatter receives or delivery of Linux faults through Mach exception RPCs.

Running the sample:

```sh
./build/machoman venv-init /tmp/venv
cp third_party/samples/rg-arm64 /tmp/venv/bin/
./build/machoman run /tmp/venv rg-arm64 -n "pattern" some/dir
```

Installing an application from a DMG:

```sh
./build/machoman install /tmp/venv "/path/to/Application.dmg"
./build/machoman run /tmp/venv Application
```

`install` requires 7-Zip (`7zz` or `7z` on PATH, or `MACHOMAN_7ZIP` pointing
to its executable). A packaged copy can also live in `tools/` beside
machoman. It extracts the image, installs all outermost `.app` bundles in
`Applications/`, and prints their actual launch names from `Info.plist`.
Nested helper apps remain inside their parent bundle. Resources and bundle
symlinks are preserved, and portable records under `bin/` keep launch paths
working when the venv moves. Conflicting bundle/command names fail before
publication; encrypted images and `.pkg` installers are not supported.

`MACHOMAN_TRACE=1` logs dylib resolution, every bind, every host call and
every static initializer; `MACHOMAN_OSLOG=1` prints the guest's own
`os_log` messages (decoded, not just the format string);
`MACHOMAN_OBJC_TRACE=1` every message send; `MACHOMAN_STUB_TRACE=1` every
synthesised framework symbol; `MACHOMAN_APPKIT_TRACE=1` only the window
system (windows, nib loading, the Wayland connection), which is
`MACHOMAN_TRACE` without the flood of host calls. A binary inside an app bundle can be symlinked
into `bin/`; `@executable_path`/`@loader_path` then find the bundle's
`Frameworks/`.

Requires a C++26 compiler (clang 22 / GCC 16 are known to work), CMake ≥ 3.25
and Fontconfig development files for CoreText font lookup.

The experimental [MEVK module](modules/MEVK/README.md) translates precompiled
Metal AIR shaders and render/blit commands to Vulkan at runtime.
`-DMACHOMAN_BUILD_MEVK=ON` replaces the Metal framework stub with its host bridge
and connects CAMetalLayer drawables to AppKit/Wayland. It requires Vulkan,
LLVM 23+ and shaderc development files. The backend also builds standalone.
The initial renderer supports a subset of Metal; MetalKit remains a stub.

## CLI

```
machoman inspect   <arm64-macho-binary>            # dump header/segments/dylibs
machoman venv-init <venv-dir>                      # create a venv
machoman install   <venv-dir> <path-to-dmg>         # install .app bundles and register launch names
machoman translate [--stats] [--cache] <arm64-macho-binary> # translate, optionally populate/read the disk cache
machoman run       <venv-dir> <binary-in-venv-bin>   # run until the first thing we cannot do, and say what it was
machoman audit [-e KEY=VALUE]... <venv-dir> <binary-in-venv-bin> # inventory the dependency closure without executing guest code
machoman disasm    <arm64-macho-binary> <va> [n]     # decode n instructions at a guest VA, with symbols
```

`audit` uses the same search paths and symbol lookup as `run`, including
guest dylibs and reexports. It writes escaped TSV rows with resolution,
importing image, dependency, symbol, bind kind, weak flag and provider.
Exit status 2 means required imports or libraries are missing; 1 means the
audit could not load the input. Resolver results may be generated stubs or
metadata, and host resolutions still need ABI/behavior validation. Guest
initializers and main do not run; host dlopen constructors still do.
Dynamic Objective-C selectors and later dlopen requests need runtime traces.
`scripts/summarize-import-audit.py report.tsv --output inventory.md` groups
one or more reports into a readable inventory.

`run` caches each translated Mach-O in `.machoman-cache` beside its source
file, using a separate `<binary-name>.x86_64.aot` file for every executable
and library. Cached code is checked against the complete source file,
guest load address, translator build and watch instrumentation; native
helper addresses are relocated for the current process. Corrupt or
unwritable caches fall back to translation. Set `MACHOMAN_TRANSLATION_CACHE=0`
to disable caching, or `MACHOMAN_CACHE_TRACE=1` to print hits and misses.

Wayland windows use the main bundle's identifier and icon from
`CFBundleIconFile`. PNG files and PNG images embedded in ICNS are supported.
When available, the compositor receives the icon through
`xdg-toplevel-icon-v1`; a hidden desktop association and a PNG icon under
`XDG_DATA_HOME` (or `~/.local/share`) also let desktop panels resolve it.
Classic ICNS bitmap and JPEG2000 payloads are not decoded yet.

On Wayland, normal titled, resizable windows show `NSApplication.mainMenu`
in a 28-point host menu bar when pangocairo is available. Menus are painted
on the CPU into shared-memory surfaces. Application drawing has a separate
content surface below the bar; compositor sizes are reduced by the bar's
height before reaching AppKit, EGL or Metal layer placement. Menu actions
use the responder chain, with validation, disabled/hidden items, checkmarks,
submenus, keyboard navigation and Ctrl equivalents for Cmd menu shortcuts.
Interface Builder's `_NSMainMenu` archive role also installs the main menu.

Keyboard input swaps host Ctrl and Super into Darwin Command and Control,
including modifier-key codes and `flagsChanged` events. This normalization
is shared code for native shortcut modifiers; the Wayland backend applies
it to all keyboard and pointer modifier flags. Set
`MACHOMAN_NATIVE_SHORTCUTS=0` to disable the keyboard swap.

`NSPasteboard.generalPasteboard` shares the host Wayland clipboard in both
directions. UTF-8 text, HTML, RTF, PNG, TIFF, PDF and file URLs use native
MIME types; custom types retain their byte payloads. Named pasteboards are
local to the process. `NSTextView` supports copy, cut, paste and select-all,
including Ctrl shortcuts through the Command mapping above. When the
compositor exposes `ext-data-control-v1`, clipboard access also works without
a focused window. Otherwise Wayland publishes copies using a focused input
serial, deferring an earlier copy until input arrives. Headless runs keep
pasteboard contents in memory.

`NSCursor` supports standard system shapes using the host cursor theme,
current cursor, nested push/pop, hide/unhide and hiding until mouse motion.
File-backed PNG/ICNS cursor images up to 256 pixels preserve their hotspot
and ownership. Cursor rendering uses Wayland surfaces independently of the
application's graphics API.

## Next steps

1. Live's interface: it builds its own view tree and stops at the first
   component it cannot find in it. Each such stop is one missing piece of
   AppKit or Foundation behaviour.
2. Drawing: NSOpenGLContext over the Wayland EGL surface, so what Live
   renders reaches the window.
3. `LC_DYLD_CHAINED_FIXUPS` (newer binaries), stack-passed host call
   arguments beyond eight, `posix_spawn`, guest `dlopen`/`dlsym`, a
   additional guest process and dynamic loading support.
4. Performance: B.cond tests saved NZCV directly, Q loads/stores use 128-bit
   SSE moves, selected USHL/SSHL forms use AVX2 when available, and native
   FP chains retain two clean V results in XMM4/5. Next: GPR allocation
   and delayed context writes with precise side-exit/fault state (for
   FMA: `fmla` maps to `vfmadd231` with no copies once vd lives in a
   register; 4-operand `fmadd d,n,m,a` picks 231/213/132 by which source
   aliases the destination, else one `vmovaps`).
