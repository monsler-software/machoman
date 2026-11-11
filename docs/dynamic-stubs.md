# Dynamic compatibility stubs

Synthesized Objective-C classes opt into stateful fallback methods. Method
and property encodings from guest protocols and properties take priority.
The first use builds a bounded ARM64 ABI descriptor and caches it with the
normal method lookup. Later calls do not parse types or repeat diagnostics.

Object properties retain assigned values, including an explicit `nil`.
Unassigned object properties on a synthesized class create a stable child
object. If the property names a class, that class's normal `new` is used
(for example, an empty NSString); otherwise the child uses the synthesized
receiver class. Scalar properties start at zero. Setters/getters preserve
integer, float, double and aggregate values, including up to four HFA
registers and known indirect results through `x8`. Initializers preserve
the receiver. Owner destruction releases property objects. Setter storage
is allocated on first use and reused on subsequent calls.

Unknown selectors on a synthesized class can use conservative name-based
inference. These results are explicitly marked as inferred; they are not
an implementation of the original API. Partial implemented host classes
retain zero fallback behavior when no type metadata is available.
Real implementations replace cached compatibility methods, including
implementations added later to a superclass.

Host frameworks can supply contracts without implementing the method:

```cpp
mm_objc_enable_dynamic_stubs(cls);
mm_objc_add_stub_method(cls, "gain", "d16@0:8");
mm_objc_add_stub_method(cls, "setGain:", "v24@0:8d16");
mm_objc_add_stub_method(cls, "extent", "{CGSize=dd}16@0:8");
```

Registration copies the type encoding. Malformed encodings, unions,
bitfields, vectors, excessive nesting and layouts larger than 4096 bytes
are rejected. Raw pointer/block results are not fabricated. Arbitrary C
imports do not contain a function signature in Mach-O bind records, so
their fallback still returns zero in the integer/FP result registers and
does not touch a guessed output buffer. A real bridge is needed to return
an API-specific structure or meaningful error from those functions.

Diagnostics are emitted once per resolved class/selector or C function:

| Marker | Meaning |
| --- | --- |
| `modeled` | Known type and a modeled default or stateful property. |
| `risk:pointer` | Raw pointer result remains null; dereferencing it can crash. |
| `risk:out-param` | A pointer argument may require an output the fallback does not produce. |
| `risk:inferred-abi` | Synthesized class method uses selector-based type inference. |
| `risk:unknown-abi` | No reliable ABI contract; a pointer/aggregate consumer may crash. |

These markers describe observable ABI risk. `modeled` does not guarantee
that missing external behavior is harmless or that an application will
launch. A known scalar result can still change control flow. Use
`MACHOMAN_STRICT_SELECTORS=1` to make automatically unresolved selectors
fatal, or `MACHOMAN_STUB_TRACE=1` to trace symbol synthesis. Search stderr
for `risk:` to prioritize bridges that can cause a later crash.

The Release host microbenchmark `test_dynamic_stubs` compares a cached
stateful getter with a real constant getter. It measures dispatch cost,
not application startup performance. C function stubs use a per-entry
relaxed atomic flag instead of a global logging mutex/hash table.

`NSURL` resource values use real host file metadata. Individual reads cache
values; bulk reads share a metadata snapshot. Explicit cache removal,
temporary values, and Cocoa main-run-loop invalidation are supported.
Unavailable properties succeed with a nil value, as documented by
Foundation; missing files and unsupported URL schemes return errors.
Birth time is returned only when the host filesystem provides it.
