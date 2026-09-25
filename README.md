# machoman

A wine-like compatibility layer for running macOS **arm64 CLI** binaries on
Windows and Linux **x86_64** hosts. Written in C++26 + CMake.

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
  variables: `DYLD_FRAMEWORK_PATH` and `DYLD_LIBRARY_PATH` are searched
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
- **libSystem.B.dylib** (`src/dylibs/libSystem/`): only what differs from
  glibc — `open`/`fcntl` flags, `stat`/`dirent` layouts, `errno` numbers,
  `sysconf`/`clock_gettime` ids, `ioctl` requests, `mmap`/`madvise`
  flags, pthread objects (Darwin sizes), guest threads via the runtime's
  `mm_host_api`, dispatch semaphores, TLS (`__tlv_bootstrap`), dyld
  introspection (`_dyld_*`, `_dyld_find_unwind_sections`), `_NSGet*`,
  stack protector, signals, setjmp/longjmp, the printf/scanf family with
  Darwin's stack-passed variadic arguments, xlocale (`newlocale`, `*_l`,
  `nl_langinfo`, `localeconv`) and the `_DefaultRuneLocale` ctype table.
- **The system calls a Darwin program makes that Linux spells differently**:
  socket options and addresses (`SOL_SOCKET`, every `SO_*`, `sockaddr`'s
  length byte), signal masks (`SIG_BLOCK` is 1 on Darwin, 0 here), malloc
  zones (a real `malloc_zone_t` whose function pointers the guest calls),
  `posix_spawn`/`NSTask` (a Mach-O helper is re-launched through machoman
  itself), and CRC32/CRC32C, which AArch64 has instructions for.
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
  NSData, NSNumber/NSValue, NSURL, NSError, NSBundle (real `Info.plist`
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
- **The frameworks around them**: CoreMIDI/CoreAudio/AudioToolbox
  (`src/dylibs/audio/`) answer with no devices but real handles, so audio
  setup completes; Carbon/ApplicationServices/CoreServices
  (`src/dylibs/carbon/`) cover the keyboard state, text input sources and
  event-loop timers a modern Cocoa app still calls; Accelerate
  (`src/dylibs/accelerate/`) is a real vDSP: FFTs (split-complex, packed
  real), vector arithmetic and the `vv*` transcendentals;
  `/usr/lib/libiconv.2.dylib` is glibc's iconv under Apple's names.
- **Stub frameworks** (`src/dylibs/symbol-synthesis/`): one object standing
  in for every framework that is not implemented yet, installed under each
  of their paths. It defines nothing; the runtime asks it for each bind
  through `machoman_resolve_symbol`, and it answers with a real objc class
  for `_OBJC_CLASS_$_X`, an NSString for a data constant, or a function
  that reports the call once and returns 0 — so a program only fails where
  it actually depends on the missing behaviour, and the message names the
  symbol.
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

Running the sample:

```sh
./build/machoman venv-init /tmp/venv
cp third_party/samples/rg-arm64 /tmp/venv/bin/
./build/machoman run /tmp/venv rg-arm64 -n "pattern" some/dir
```

`MACHOMAN_TRACE=1` logs dylib resolution, every bind, every host call and
every static initializer; `MACHOMAN_OSLOG=1` prints the guest's own
`os_log` messages (decoded, not just the format string);
`MACHOMAN_OBJC_TRACE=1` every message send; `MACHOMAN_STUB_TRACE=1` every
synthesised framework symbol; `MACHOMAN_APPKIT_TRACE=1` only the window
system (windows, nib loading, the Wayland connection), which is
`MACHOMAN_TRACE` without the flood of host calls. A binary inside an app bundle can be symlinked
into `bin/`; `@executable_path`/`@loader_path` then find the bundle's
`Frameworks/`.

Requires a C++26 compiler (clang 22 / GCC 16 are known to work) and CMake ≥ 3.25.

## CLI

```
machoman inspect   <arm64-macho-binary>            # dump header/segments/dylibs
machoman venv-init <venv-dir>                      # create a venv
machoman translate [--stats] <arm64-macho-binary>   # run the AOT translator, report coverage
machoman run       <venv-dir> <binary-in-venv-bin>   # run until the first thing we cannot do, and say what it was
machoman disasm    <arm64-macho-binary> <va> [n]     # decode n instructions at a guest VA, with symbols
```

## Next steps

1. Live's interface: it builds its own view tree and stops at the first
   component it cannot find in it. Each such stop is one missing piece of
   AppKit or Foundation behaviour.
2. Drawing: NSOpenGLContext over the Wayland EGL surface, so what Live
   renders reaches the window.
3. `LC_DYLD_CHAINED_FIXUPS` (newer binaries), stack-passed host call
   arguments beyond eight, `posix_spawn`, guest `dlopen`/`dlsym`, a
   translation cache on disk (Live takes ~20 s to translate).
4. Performance: flags save/restore peephole (producer directly followed by
   its consumer), then a register allocator for both GPRs and xmm (for
   FMA: `fmla` maps to `vfmadd231` with no copies once vd lives in a
   register; 4-operand `fmadd d,n,m,a` picks 231/213/132 by which source
   aliases the destination, else one `vmovaps`).
