# Support matrix and conversion contract

“Supported” refers only to the operation listed. Inspection or candidate analysis is not evidence
that an iOS app can be converted. The host emits a real standalone Android library for one proven
integer entry function, contains four concrete time API shims, and converts IPAs whose whole
executable is proven to be exactly that one routine into signed `complete-game-v1` APKs (bounded
subset only — no general iOS-to-Android game translator exists). The Android importer converts the
same proven subset on-device and otherwise builds a separately identified, explicitly non-playable
preview shell.

| Area | Status | Contract / limitation |
|---|---|---|
| IPA archive, plist, icon inspection | PARTIAL | Bounded, authorized, offline inspection; source retained in Android private storage until entry deletion. The IPA archive itself is never embedded; bounded conversions package the bundle's static resources verbatim with a hashed inventory |
| Icons (host inspection) | PARTIAL | Info.plist names, scale/device variants, compiled `Assets.car` raster renditions, then ranked loose images; unsupported formats are reported, not fabricated |
| Icons (Android library) | PARTIAL | Plist/scale variants, supported compiled `Assets.car` raster renditions, then ranked PNG/JPEG resources. Force uses the recovered icon for the placeholder where available and records generated/fallback icon use otherwise |
| Mach-O thin/FAT/FAT64 | SUPPORTED | CPU/subtype, endian headers, bounded load-command/section/symbol parsing |
| Mach-O loader metadata | PARTIAL | Relocations, dynamic tables, binds/imports/addends, export trie, chained-fixup metadata, dependencies, LC_MAIN and signature-blob metadata. Incomplete bind tables and unsupported loader semantics block conversion |
| ObjC/Swift/unwind/init metadata | PARTIAL | Host reconstruction recovers selected Objective-C/Swift metadata and reports limitations; it does not implement the Apple runtime ABI |
| ARM64 reconstruction | PARTIAL | A restricted closed-integer entry leaf (MOV-immediate, MOVK, register MOV, immediate ADD/SUB, RET) can be translated into `translated-entry.bin`, `translated-entry.c`, and a minimal ARM64 ET_DYN shared object. It is statically validated but not linked into a game or APK |
| ARM64e | BLOCKED | PAC/ABI adaptation is not proven |
| ARMv6/ARMv7/v7s/Thumb/Thumb-2 | PARTIAL | Selected immediate arithmetic, register-copy and return instruction subsets can be lowered to ARMv7 and emitted in the same isolated ET_DYN format. It is not linked into a game; no 32-bit game APK is emitted |
| Compatibility registry source | PARTIAL | `ioscompat/libioscompat.cpp` gives every observed Darwin import a resolution target: verified time-shim implementations or explicitly unimplemented stub handlers. Stub counts are resolution coverage, never implementation coverage |
| Dynamic stub hook registration | SUPPORTED (registration only) | `libioscompat.so` registry registers unmapped symbols at runtime and resolves them to counted stub trampolines. Registration is not implementation and rewrites no IPA callsites |
| Experimental shell APK | SUPPORTED (explicitly non-game) | `convert` builds `experimental-shell.apk` (aapt2/javac/d8/zipalign/apksigner) around the isolated artifacts under `experimental-shell-v1`; launcher and metadata disclose that no game code is translated; it cannot satisfy `complete-game-v1` |
| FAT ARM64 + ARM32 selection | SUPPORTED (selection only) | Automatic selection prefers ARM64. Explicit 32-bit target is accepted only when an ARM32 slice is present |
| Supported ARM32-only target | SUPPORTED (selection only) | Selects `armeabi-v7a`; no complete converter currently emits an APK |
| Offline source reconstruction | PARTIAL | Function discovery, CFGs, selected ARM disassembly, reference tracking and pseudocode with explicit uncertainty. Never presented as original source or executed |
| Actual iOS-to-Android API replacements | PARTIAL (four time APIs) | Bionic-backed shims implement `CFAbsoluteTimeGetCurrent`, `CACurrentMediaTime`, `mach_absolute_time`, and `mach_timebase_info`; host tests execute their behavior. Imports reached through resolved internal calls from the selected entry can generate shim source, but no Mach-O callsite is rewritten and no shim is linked into a game/APK |
| Objective-C binary ABI / Swift | BLOCKED | The experimental portable runtime is not Apple's ABI/runtime and is not linked into game outputs |
| UIKit, Foundation, graphics, audio, input, lifecycle | BLOCKED | No complete compatibility providers or game lifecycle/input translations exist |
| Resources | PARTIAL | Icons and bundle resources can be inventoried/read for analysis. Preview shells include app metadata, icon and a static-analysis summary only; bounded conversions package static bundle resources verbatim under `assets/bundle/` with a hashed inventory; no gameplay assets are translated |
| Importer APK | SUPPORTED | Gradle builds the Android library/import/analyzer app for ARM64 devices |
| Complete-game APK packaging | BOUNDED SUBSET | `radek/gamepack.py` builds signed `complete-game-v1` APKs only for IPAs whose whole executable is statically proven to be one closed-integer routine with zero imports/metadata; anything else fails closed (`build_apk` still refuses the former partial launcher wrapper) |
| Android preview shell APK | SUPPORTED (explicitly non-playable) | User-triggered on-device builder signs a branded shell with the IPA app name, available icon and static-analysis statistics. No iOS executable or translated game code is included; the generated launcher screen shows no conversion notice, while `placeholder-info.json` keeps the full analysis summary and the report keeps `completeGameConversion`/`gameCodeIncluded` false. Separate metadata, filename and provider checks prevent it from satisfying the complete-game host contract |
| Android bounded conversion APK | SUPPORTED (runtime NOT_TESTED) | For IPAs proven on-device to be one closed-integer routine, the builder translates the entry into `libconverted.so` (JNI) and signs a launcher APK under the same `complete-game-v1` contract |
| Host APK attachment | CONTRACT-ONLY | The app accepts only `complete-game-v1` evidence with source/ABI/API/resource/lifecycle checks; the host CLI and on-device converter produce it for the bounded subset only |
| APK static validation | SUPPORTED | The validator checks the importer APK; game APK validation additionally requires the complete-game contract, generated API implementation evidence, provenance and ABI checks |
| Android runtime/device validation | NOT TESTED | Static checks cannot prove execution or gameplay; reports say `NOT_TESTED` |

## Why symbol substitutions are not API implementations

The app may report 100% **symbol classification/triage** when every observed import has been
categorized as a name candidate, semantic-rewrite candidate, implemented-shim export, compat stub
handler, or unmapped. That is deliberately separate from direct NDK candidates and actual
linked-implementation coverage. The compat stub category means a symbol now resolves to an
explicitly unimplemented handler that records invocations; it is a resolution target for future
work, not an API implementation, and its count is never added to verified or generated-translation
numbers.
The four concrete time shims have host behavior tests; on Android, the mapper uses `dlopen`/`dlsym`/
`dladdr` to verify their exports in `libioscompat.so`. This only proves the replacement function is
available in the analyzer process: no IPA callsite is rewritten or linked, and other API behavior
remains unimplemented. A name resolving at runtime does not prove caller ABI compatibility,
minimum-API availability on other devices, relocation, or game integration. An `UNMAPPED`
classification is a useful explicit blocker, not a conversion result.

- Some OpenGL ES 1.x/2.x/3.x C entry points have Android equivalents, but each reachable symbol,
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
  imports, linked frameworks and reachable API attribution.
- `reconstruction.md` — readable evidence, reconstructed listings and reachable-API blockers.
- `translated-entry.bin`, `translated-entry.c`, and `libtranslated-entry.so` — emitted only if the
  entry passes the closed-integer proof. The library exports one standalone native function and has
  no imports or relocations; it is not a game binary or APK.
- `api-replacements/` — when a reconstructed call path from the selected entry reaches one of four
  supported time imports, contains the selected implementation source and header. The report marks
  these artifacts generated but not linked to the game.
- `leaf-experiment.ll` — supplementary LLVM IR for the same single leaf; syntax-checked with
  `llvm-as` when available.
- `ioscompat/` — generated compatibility-registry source, copied time-shim header and
  `registry.json` with the per-symbol verified/stubbed classification.
- `experimental-shell.apk` — only on the `convert` path and only when the Android toolchain is
  available; labelled, signed inspection shell for the artifacts above (`experimental-shell-v1`).

For a proven entry, `portProgress.percent` is the translated source-byte count divided by executable
`__text` bytes in the selected Mach-O slice. It can be nonzero (or even 100% for a tiny synthetic
binary) while the overall game remains incomplete; it is not a function/API/resource or gameplay
score. `conversionProgress.status` remains `NOT_BUILT`. A candidate symbol mapping is never counted
as generated code, and generated API source is reported separately from zero linked API replacements.

## ABI preference

- With automatic selection, a FAT IPA containing ARM32 and ARM64 prefers the ARM64 slice and the
  future target ABI `arm64-v8a`.
- Supported ARM32-only input selects an ARMv6/ARMv7-family slice for the future 32-bit ABI
  `armeabi-v7a`.
- An explicit ABI must have a matching source slice. Target selection is not evidence of successful
  translation or APK output.

## Complete-game attachment contract

The `complete-game-v1` attachment gate rejects the old `closed-integer-entry-v1` output. It requires
host evidence for full reachable-function translation, complete reachable API accounting and
linked implementation artifacts, complete resource/lifecycle claims, ABI/package/source identity,
original icon hash when recovered, APK signature/package parsing, and exclusion of the original IPA
and Apple executable assets. This metadata is not a substitute for behavioral testing. The host
pipeline produces this contract only for the proven bounded subset — an executable statically proven
to be exactly one closed-integer routine with no imports, dependencies, fixups or runtime metadata
(`radek/gamepack.py`, exercised in CI by `tests/data/hello-test.ipa`). Everything else stays
`NOT_BUILT`, and on-device the same contract is produced by the bounded converter for proven IPAs.
