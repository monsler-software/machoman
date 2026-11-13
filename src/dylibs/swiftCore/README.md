# Swift runtime foundation

`libswiftCore.dylib.so` supplies a first subset of Swift's stable 64-bit
runtime ABI. CMake builds it under `build/dylibs/usr/lib/swift/`, matching
the SDK's `/usr/lib/swift/libswiftCore.dylib` install name. The existing
loader finds it without a special symbol table or venv changes.

Implemented:

- Aligned raw allocations and the 16-byte native Swift object header.
- Atomic and nonatomic retain/release, batch operations, try-retain,
  retain-count queries and native uniqueness checks. Nonatomic entry points
  currently use the same atomic implementation.
- Deallocating guest destructors from metadata word `-2`, with `swiftself`
  in arm64 `x20` or Darwin x86_64 `r13`. Destructors own deallocation;
  the host preserves the caller's `swiftself` across the nested guest call.
- Stack objects, lifetime verification, immortal static objects and
  uninitialized-object deallocation.
- `swift_once` with guest callbacks, publication across threads and nested
  initializers for different tokens.
- Thread-local dynamic exclusivity checks, including read/read access,
  untracked conflict checks, non-LIFO removal and the debugger disable flag.

The object-header and metadata layouts follow Swift's published ABI:
[RefCount.h](https://github.com/swiftlang/swift/blob/swift-6.2-RELEASE/stdlib/public/SwiftShims/swift/shims/RefCount.h),
[Metadata.h](https://github.com/swiftlang/swift/blob/swift-6.2-RELEASE/include/swift/ABI/Metadata.h),
[MetadataValues.h](https://github.com/swiftlang/swift/blob/swift-6.2-RELEASE/include/swift/ABI/MetadataValues.h).
Runtime boolean results use host `int` so the complete guest `w0` is defined.

This does **not yet run arbitrary Swift applications**. Standard-library
Swift symbols (`String`, `Array`, `print`, etc.), type metadata construction,
value witnesses and boxes, protocol conformance, dynamic casts, weak/unowned
references, Objective-C bridging, error objects and concurrency tasks are
not implemented. The library does not synthesise successful stubs for these
APIs. Unknown imports stay unresolved; unsupported lifetime representations
and exclusivity conflicts abort with a diagnostic and guest backtrace.
Only native objects with inline reference counts and the standard address
point are supported. Full language support will need guest Swift standard
library code as well as further runtime and interoperability work; ordinary
Swift calling conventions cannot be passed through a generic C host call.

Validation:

```sh
cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++ \
  -DMM_TEST_MACOS_SDK="$PWD/third_party/sdk/MacOSX26.1.sdk"
cmake --build build --target machoman libSystem swiftCore test_swift_runtime \
  guest_swift_runtime_arm64_binary guest_swift_runtime_x86_64_binary
ctest --test-dir build -R swift_runtime --output-on-failure
```

The host test covers concurrent ARC, callback registers, nested once,
exclusivity, stack lifetime and failure diagnostics. The guest probes link
against the SDK's real Swift core import library and execute as Mach-O
arm64 and x86_64 binaries. They are written in C with Clang's
`swiftcall`/`swift_context` attributes, which emit the actual Swift destructor
ABI. A Swift compiler is not required for these probes; they validate the
runtime ABI, not Swift-language or standard-library coverage.
