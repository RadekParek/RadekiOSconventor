# Support matrix and conversion contract

“Supported” refers only to the operation listed. Inspection or candidate analysis is not evidence
that an iOS app can be converted. The host emits a real standalone Android library for one proven
integer entry function; `libioscompat.so` has a host-tested C/time/POSIX and limited CoreFoundation
subset, plus a native C frame-clock service; and the bounded converter handles only IPAs whose
whole executable is proven to be exactly that one routine. No general iOS-to-Android game static recompilation backend
exists. The Android importer converts that proven subset on-device and otherwise can build a
separately identified, explicitly non-playable guest boot-attempt APK; a source-free preview shell
is available as a fallback.

Provider grades are not gameplay grades: `provided` names a reviewed Android system ABI only;
`compatibility` marks a bounded tested implementation subset; `candidate` is a possible semantic/API
target without a linked ABI adapter; `no-execution-path-yet` means no provider is identified. Reports
attach per-import evidence to dependency edges by dylib ordinal and keep callsite links, observed runtime
calls, and linked recompiled bytes explicitly at zero.

| Area | Status | Contract / limitation |
|---|---|---|
| IPA archive, plist, icon inspection | PARTIAL | Bounded, authorized, offline inspection; source retained in Android private storage until entry deletion. The IPA archive itself is never embedded; bounded conversions package the bundle's static resources verbatim with a hashed inventory |
| Icons (host inspection) | PARTIAL | Info.plist names and variants, compiled `Assets.car` raster renditions, then ranked loose images; the largest decodable source by pixel count wins within the icon family, with scale only a tie-breaker. Unsupported formats are reported, not fabricated |
| Icons (Android library) | PARTIAL | Plist/scale variants, supported compiled `Assets.car` raster renditions, then ranked PNG/JPEG resources. The largest decodable source by pixel count wins within the icon family; scale labels are not assumed to imply higher resolution. Force uses the recovered icon for the placeholder where available and records generated/fallback icon use otherwise |
| Mach-O thin/FAT/FAT64 | SUPPORTED | CPU/subtype, endian headers, bounded load-command/section/symbol parsing |
| Mach-O loader metadata | PARTIAL | Relocations, dynamic tables, binds/imports/addends, export trie, chained-fixup metadata, dependencies, LC_MAIN and signature-blob metadata. Incomplete bind tables and unsupported loader semantics block conversion |
| ObjC/Swift/unwind/init metadata | PARTIAL | Host reconstruction recovers selected Objective-C/Swift metadata and reports limitations; it does not implement the Apple runtime ABI |
| ARM64 reconstruction | BOUNDED SUBSET | A restricted closed-integer entry leaf (MOV-immediate, MOVK, register MOV, immediate ADD/SUB, RET) can be statically recompiled into `recompiled-entry.bin`, `recompiled-entry.c`, and a minimal ARM64 ET_DYN shared object. If the whole executable is exactly that import-free routine and has no unsupported metadata, the bounded complete-game builder links it into a signed APK; this is not a general game port |
| ARM64e | BLOCKED | PAC/ABI adaptation is not proven |
| ARMv6/ARMv7/v7s/Thumb/Thumb-2 | PARTIAL | Selected immediate arithmetic, register-copy and return instruction subsets can be lowered to ARMv7 and emitted in the same isolated ET_DYN format. It is not linked into a game; no 32-bit game APK is emitted |
| Compatibility registry source | PARTIAL | `ioscompat/libioscompat.cpp` gives observed imports a generated-source implementation or explicit stub resolution target; this source is not linked into the game. `compatRegistry.guestRuntimeAdapterCatalogCount` separately counts compat-runtime-v1 ARM32 callout/data-provider catalog entries (legacy alias `concreteDarwinProviderCount`); catalog membership is neither static linkage nor per-image fixup coverage |
| Runtime guest import-slot binding | PARTIAL | The game loader writes guest provider/trap addresses into supported Mach-O bind, indirect-symbol and external-relocation slots. Per-image `runtimeLinking` records actual bound/trapped/unresolved slots; this is not static Android callsite rewriting or a native game-object link. Unsupported chained fixups remain blocked |
| `libgcc_s.1.dylib` mapping | CANDIDATE / PARTIAL RUNTIME ADAPTERS | Android has no drop-in `libgcc_s.so` alias and no static compiler-rt/libunwind link is generated. A bounded set of ARM32 arithmetic helpers and unwind boundaries are registered as guest-runtime callouts; full SjLj/personality/landing-pad semantics remain incomplete |
| `libstdc++.6.dylib` mapping | CANDIDATE ONLY | GNU libstdc++ and LLVM libc++ have different C++ ABIs and mangling. Low-level symbol overlap is not a drop-in runtime, compatible exception model, or completed link |
| Darwin framework dependency grades | EVIDENCE-GRADED | `provided` is a reviewed Android system ABI target; `compatibility` marks a bounded tested implementation; `candidate` is a semantic/API target with no linked ABI adapter; `no-execution-path-yet` means no provider is identified. Dependency imports are associated by dylib ordinal; evidence counts never claim an IPA callsite link or runtime call |
| Dynamic stub hook registration | SUPPORTED (registration only) | `libioscompat.so` registry registers unmapped symbols at runtime and resolves them to counted stub trampolines. Registration is not implementation and rewrites no IPA callsites |
| Public NDK export lookup | PARTIAL (current-device check) | Uses exact `dlopen`/`dlsym`/`dladdr` checks against the reviewed public NDK library set, including native-window, neural-networks and sync libraries. Counts verify exports visible on the current device only; they do not prove ABI compatibility or link the IPA |
| Experimental shell APK | SUPPORTED (explicitly non-game) | `convert` builds `experimental-shell.apk` (aapt2/javac/d8/zipalign/apksigner) around the isolated artifacts under `experimental-shell-v1`; launcher and metadata disclose that no game code is statically recompiled; it cannot satisfy `complete-game-v1` |
| FAT ARM64 + ARM32 selection | SUPPORTED (selection only) | Automatic selection prefers ARM64. Explicit 32-bit target is accepted only when an ARM32 slice is present |
| Supported ARM32-only target | SUPPORTED (selection only) | Selects `armeabi-v7a`; no complete converter currently emits an APK |
| Conversion ceiling assessment | SUPPORTED (assessment only) | `report.json` + `reconstruction.md` record one ordered gate ledger per input, with the first unpassable gate and the prover's own reason. Later gates are `NOT_REACHED`, never estimated; the ledger states `countsAsConversionProgress: false` and is not converted code or a playability score |
| Framework/dylib attribution catalog | PARTIAL | Named attribution for the Darwin frameworks and common dylibs games link (`libz`, `libsqlite3`, `libxml2`, `libbz2`, `libiconv`, `libresolv`, `libicucore`, `libc++abi`, `libcompression`, `libarchive`, `JavaScriptCore`, …). A name is an attribution label only: each symbol is still classified and, when reachable, remains a conversion blocker until a tested implementation is linked |
| Offline source reconstruction | PARTIAL | Function discovery, CFGs, selected ARM disassembly, reference tracking and pseudocode with explicit uncertainty. Never presented as original source or executed |
| Actual iOS-to-Android API replacements | PARTIAL (tested C subset) | Host-tested C/time/POSIX functions and a limited CoreFoundation C object/collection/run-loop subset are compiled into `libioscompat.so`; these do not implement the full Apple framework ABI. Selected reachable source can be generated, but arbitrary IPA callsites are not rewritten or linked |
| QuartzCore frame-clock bridge | PARTIAL (C callback ABI only) | The converted launcher's Java Choreographer bridge dispatches frames to a native C callback scheduler with pause/FPS/timing tests. It does not implement Objective-C `CADisplayLink`, `CAAnimation`, `CALayer`, or any IPA callsite |
| Objective-C binary ABI / Swift | BLOCKED | The experimental portable runtime is not Apple's ABI/runtime and is not linked into game outputs |
| UIKit, full Foundation/CoreFoundation ABI, graphics, audio, input, lifecycle | BLOCKED | The limited tested C/CoreFoundation subset does not replace full Apple framework, Objective-C/Blocks, game lifecycle, rendering, audio or input implementations |
| Resources | PARTIAL | Icons and bundle resources can be inventoried/read for analysis. Preview shells retain app metadata, icon and static-analysis details as machine-readable metadata; the launcher shows only the shell's started/no-statically recompiled-executable state. Bounded conversions package static bundle resources verbatim under `assets/bundle/` with a hashed inventory; no gameplay assets are statically recompiled |
| Importer APK | SUPPORTED | Gradle builds the Android library/import/analyzer app for ARM64 devices |
| Complete-game APK packaging | BOUNDED SUBSET | `radek/gamepack.py` builds signed `complete-game-v1` APKs only for IPAs whose whole executable is statically proven to be one closed-integer routine with zero imports/metadata; anything else fails closed (`build_apk` still refuses the former partial launcher wrapper) |
| Android game-runtime boot-attempt APK | SOURCE-INTEGRATED; ANDROID BUILD/DEVICE RUN NOT VERIFIED | Default path packs the selected authorized 32-bit ARM Mach-O, bundle, `libcompat_runtime_v1.so`, and `libunicorn.so`. An optional portable-C handoff is accepted only after the app rechecks executable binding, ARM64 ELF class/machine, dependency policy, translated-function/JNI exports, and payload hashes, then packages the library and memory payload into the separate game-runtime APK. The translated runner is not connected to Android EGL/GLES; execution, pixels and gameplay remain unverified. The compatibility guest-CPU path retains its existing unbounded real-boundary behavior and EGL status reporting. Both paths use app-private guest `/` and `/tmp`, a read-only bundle, and an optional read-only `/Android/obb` mount. This is not the complete-game static recompilation contract |
| Android preview shell APK | SUPPORTED (explicitly non-playable) | User-triggered on-device fallback builder signs a shell with the IPA app name and available icon. No iOS executable or statically recompiled game code is included; the launcher says `Preview shell started` and that no statically recompiled executable is included, without converter branding or static-analysis details. `placeholder-info.json` keeps the full analysis summary; separate metadata/filename/provider checks prevent it from satisfying the complete-game host contract |
| Android bounded conversion APK | SUPPORTED (runtime NOT_TESTED) | For IPAs proven on-device to be one closed-integer routine with zero imports, dependencies, `__text` relocations, fixups, or runtime metadata, the builder statically recompiles the entry into `libconverted.so` (JNI), declares the packaged ARM64 `libioscompat.so` shim runtime as `DT_NEEDED`, preserves and verifies 16 KiB native-library alignment after signing, and signs a launcher APK under the same `complete-game-v1` contract. No IPA callsite rewrites are claimed |
| Host APK attachment | CONTRACT-ONLY | The app accepts only `complete-game-v1` evidence with source/ABI/API/resource/lifecycle checks; the host CLI and on-device converter produce it for the bounded subset only |
| APK static validation | SUPPORTED | The validator checks the importer APK; game APK validation additionally requires the complete-game contract, generated API implementation evidence, provenance and ABI checks |
| Android runtime/device validation | NOT TESTED | Static checks cannot prove execution or gameplay; reports say `NOT_TESTED` |

## Why symbol substitutions are not API implementations

The app may report 100% **symbol classification/triage** when every observed import has been
categorized as a name candidate, guest-runtime adapter catalog entry, compiled shim export,
semantic-rewrite target, compat stub handler, or unmapped. That is separate from strict direct NDK
name matches, per-image runtime slot fixups, and statically linked implementation coverage. The
headline is **reviewed Android mapping coverage**: every import gets exactly one mapping kind
(same-name NDK/system candidate, compiler-runtime candidate, `libcompat_runtime_v1.so` guest-adapter
catalog entry, compiled `libioscompat.so` implementation export, or reviewed semantic target), so a
fully triaged IPA can reach 100%. The per-kind counts remain visible; for the Angry Birds v1.0
fixture, 181/254 are strict same-name NDK/system candidates and 73/254 are non-same-name guest
adapter catalog entries. Those 73 are not NDK exports or proof of per-image binding. The loader's
`runtimeLinking` block is the source for actual guest import-slot bind/relocation results. None of
these figures represents a static Android callsite rewrite, a linked game object, or generated game
code.
Current-device `dlopen`/`dlsym` results are reported with two explicit denominators: exact NDK exports
verified among the NDK name candidates, and verified exports among all imports. For example,
167/264 imports is 63%, not 65%; if 172 names were candidates, 167/172 would separately be 97% of
those candidates verified. Android API level in this report is the current device's runtime API,
not a guarantee for all API 36 devices or lower API levels.

The compat stub category means a symbol now resolves to an explicitly unimplemented handler that
records invocations; it is a resolution target for future work, not an API implementation, and its
count is never added to verified or generated-static recompilation numbers. A stub cannot safely substitute
for an arbitrary function because the missing API's signature and semantics matter. Such symbols
remain conversion blockers until a tested, ABI-compatible implementation or rewrite exists.

The tested C/time/POSIX and limited CoreFoundation shims have host behavior tests; on Android, the
mapper uses `dlopen`/`dlsym`/`dladdr` to verify compiled exports in `libioscompat.so`. This only
proves a compatibility function is available in the analyzer process: no IPA callsite is rewritten
or linked, and full framework behavior remains unimplemented. A name resolving at runtime does not
prove caller ABI compatibility, minimum-API availability on other devices, relocation, or game
integration. An `UNMAPPED` classification is a useful explicit blocker, not a conversion result.

- Some OpenGL ES 1.x/2.x/3.x C entry points have Android equivalents, but each statically called symbol,
  GLES version, context/lifecycle path, and ABI binding still has to be proven and linked. A symbol
  name match alone does not constitute a rewrite.
- AudioToolbox/CoreAudio APIs such as AudioQueue, AudioUnit, and AudioComponent do not share the
  AAudio API or lifecycle. They need typed adapters for buffers, clocks, callbacks, threading, and
  device/session behavior; replacing names with AAudio calls is not semantics-preserving.
- `UIApplicationMain` is not an `ANativeActivity_onCreate` alias. Android's native-activity entry
  has a different signature, ownership model, event loop, and lifecycle. A tested host/activity
  adapter and app-specific UI/input port are required.
- `objc_msgSend` plus a recovered selector does not recover static receiver types, ownership rules,
  overloads, block captures, exceptions, or the complete method body. Decompiled listings remain
  review artifacts, not compiler-ready C++ ASTs.

## Reconstruction report

`python3 -m radek analyze app.ipa --authorized --output workspace/analysis` writes, next to
`report.json`:

- `reconstruction.json` — machine-readable architectures, entry points, per-image/per-slice
  metrics, recovered functions and listings, call graph, Objective-C metadata, Swift types/symbols,
  imports, linked frameworks, ranked direct-callsite counts, and an entry-rooted direct-call graph.
- `reconstruction.md` — readable evidence, reconstructed listings, import rankings and blockers
  among statically called symbols. Rankings are limited to decoded `__text` coverage; indirect calls,
  dyld handoffs, Objective-C dispatch, callbacks and loader initializers are not inferred.
- `recompiled-entry.bin`, `recompiled-entry.c`, and `librecompiled-entry.so` — emitted only if the
  entry passes the closed-integer proof. The library exports one standalone native function and has
  no imports or relocations; it is not a game binary or APK.
- `api-replacements/` — when reconstructed call paths from the selected entry reach imports in the
  compiled compatibility subset, contains selected implementation source and headers. The report
  marks these artifacts generated but not linked to the game.
- `leaf-experiment.ll` — supplementary LLVM IR for the same single leaf; syntax-checked with
  `llvm-as` when available.
- `ioscompat/` — generated compatibility-registry source, copied compatibility headers and
  `registry.json` with the per-symbol verified/stubbed classification.
- `experimental-shell.apk` — only on the `convert` path and only when the Android toolchain is
  available; labelled, signed inspection shell for the artifacts above (`experimental-shell-v1`).

`portProgress.percent` is zero until an Android ELF/APK has been built and verified. Its numerator
counts unique source instruction bytes represented by functions whose exports are verified in that
Android artifact; its denominator is the selected slice's executable `__text`. A verified standalone
`.so` is not linked into the game boot path or packaged into an APK, and the figure is not whole-app,
API/resource, or gameplay coverage. `androidLink` records the architecture/dependency and function-
export checks behind any positive value. A candidate symbol mapping is never counted as generated
or linked code; generated API source is reported separately from zero linked API replacements.

When the bounded prover refuses an input, the host CLI additionally runs the **static-recompilation
plan** (`report.json` → `staticRecompilationPlan` and `hostStaticRecompilationProgress`): the pipeline
runs the same fail-closed ARM lifter over discovered functions of the selected 32-bit ARM slice and
reports unique source instruction bytes covered by functions it can lift, with function counts. The plan
itself is host-only measurement, not emitted or linked code, and never increments
`portProgress`. Whole-game portable-C instruction coverage is separately reported as unique translated
bytes in `bytecodeTranslation`. If the Android NDK is available, the host may link a standalone Android
translation library; progress becomes positive only after the artifact exists and Android ELF
architecture/class, dependency policy, every translated function export, the JNI entry point, and
linker-to-translation function/byte counts all verify. The host can prepare a separate input ZIP, and
the on-device builder now has a fail-closed import/package/boot path for it; that Android path still
needs an SDK/NDK build and device validation. A standalone library or APK inclusion is not runtime
execution or gameplay evidence, and `conversionProgress` remains `NOT_BUILT`. The plan needs optional
`capstone` >=5.0.6 and <6.0.0 (CI pins 5.0.7); without a supported version the plan is
`UNAVAILABLE` and no host-plan coverage is claimed.

## ABI preference

- With automatic selection, a FAT IPA containing ARM32 and ARM64 prefers the ARM64 slice and the
  future target ABI `arm64-v8a`.
- Supported ARM32-only input selects an ARMv6/ARMv7-family slice for the future 32-bit ABI
  `armeabi-v7a`.
- An explicit ABI must have a matching source slice. Target selection is not evidence of successful
  static recompilation or APK output.

## Complete-game attachment contract

The `complete-game-v1` attachment gate rejects the old `closed-integer-entry-v1` output. It requires
host evidence for full reachable-function static recompilation, complete reachable API accounting and
linked implementation artifacts, complete resource/lifecycle claims, ABI/package/source identity,
original icon hash when recovered, APK signature/package parsing, and exclusion of the original IPA
and Apple executable assets. This metadata is not a substitute for behavioral testing. The host
pipeline produces this contract only for the proven bounded subset — an executable statically proven
to be exactly one closed-integer routine with no imports, dependencies, fixups or runtime metadata
(`radek/gamepack.py`, exercised in CI by `tests/data/hello-test.ipa`). Everything else stays
`NOT_BUILT`, and on-device the same contract is produced by the bounded converter for proven IPAs.
