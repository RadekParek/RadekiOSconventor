# Modules and extension points

- `app/`: Kotlin framework-only Android UI. `Library` owns persistent reports/import lifecycle; `SafeZip` handles untrusted archive extraction; `Plist` handles XML/binary parsing; `NativeBridge` calls C++ through JNI; `ResultProvider` is a read-only URI-granted APK provider.
- `native/src/macho.cpp`: host/Android shared C++ analyzer. No host subprocess execution of the input. JSON describes source structure, not conversion success.
- `native/include/runtime.hpp`: experimental portable runtime and storage primitives with real host tests. Intentionally isolated from the conversion dependency resolver until a tested Apple ABI adapter exists.
- `radek/archive.py`: authorized input staging, bounded archive/plist import and metadata.
- `radek/analysis.py`: native analyzer invocation, generic dependency graph and fail-closed backend eligibility.
- `radek/ir.py`: explicit register/width/flag-aware instruction records and basic blocks, decoding, proof and ARM64 lowering. Unsupported instruction classes raise `Unsupported`.
- `radek/resources.py`: PNG normalization, resource inventory, source code/signature exclusion.
- `radek/dex.py`: bounded DEX integrity and class-identity inspection; no Dalvik interpreter.
- `radek/apk.py`: independent host SDK/NDK tool invocation, ELF/DEX/Android packaging, development signing and validation.
- `radek/pipeline.py`: guarded states, durable JSON report/JSONL logs, isolated temporary job cleanup. Only validated artifacts are published to the result directory.
- `tests/`, `app/src/test/`, `native/tests/`: synthetic fixtures, negative/security tests, native portable runtime tests, Robolectric importer tests, SDK-required end-to-end APK tests.

## States

`IMPORTED → ANALYZING → CONVERTING → PACKAGING → VALIDATING → READY`

Analysis-only ends in `PARTIAL`. Missing conversion providers/unsafe source code end in `BLOCKED`. Malformed inputs, tooling and validation failures end in `FAILED`. Terminal states cannot silently transition to READY. The Android app currently ends in PARTIAL/BLOCKED/FAILED because it has no compiler toolchain.

## Adding real support

Do not add bundle-ID exceptions. Add parser/decoder/IR/linker/provider logic with positive, negative and real APK integration tests. A dependency can only be classified `converted`, `provided by compatibility layer`, or `Android equivalent` once a real tested implementation is wired in. For the closed integer leaf only, a linked-but-unused dylib can be omitted after proving the emitted instructions have no calls, memory or address references; that does not implement the dylib ABI. Any reachable framework API still requires a real provider and remains blocked.

Future graphics providers should separate API command/state capture from an Android GLES backend; future Metal work should use a separate shader/type translation and Vulkan/SPIR-V backend. Neither backend exists in this version, so reports block their symbols instead of binding no-ops. Framework providers should similarly be introduced behind a versioned ABI symbol registry with tests of observable behavior, not by accepting symbol names alone.
