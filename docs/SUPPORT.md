# Support matrix and conversion contract

“Supported” refers only to the operation listed. Inspection or candidate analysis is not evidence
that an iOS app can be converted. The repository currently has no complete iOS-to-Android game
translator. The Android importer can build a separately identified installable placeholder, but not
a playable or complete-game APK.

| Area | Status | Contract / limitation |
|---|---|---|
| IPA archive, plist, icon inspection | PARTIAL | Bounded, authorized, offline inspection; source retained in Android private storage until entry deletion. No original IPA is embedded in any APK |
| Icons (host inspection) | PARTIAL | Info.plist names, scale/device variants, compiled `Assets.car` raster renditions, then ranked loose images; unsupported formats are reported, not fabricated |
| Icons (Android library) | PARTIAL | Plist/scale variants, supported compiled `Assets.car` raster renditions, then ranked PNG/JPEG resources. Force uses the recovered icon for the placeholder where available and records generated/fallback icon use otherwise |
| Mach-O thin/FAT/FAT64 | SUPPORTED | CPU/subtype, endian headers, bounded load-command/section/symbol parsing |
| Mach-O loader metadata | PARTIAL | Relocations, dynamic tables, binds/imports/addends, export trie, chained-fixup metadata, dependencies, LC_MAIN and signature-blob metadata. Incomplete bind tables and unsupported loader semantics block conversion |
| ObjC/Swift/unwind/init metadata | PARTIAL | Host reconstruction recovers selected Objective-C/Swift metadata and reports limitations; it does not implement the Apple runtime ABI |
| ARM64 reconstruction | PARTIAL | A restricted closed integer leaf can be assessed/lowered in memory. No resulting game code or APK is emitted |
| ARM64e | BLOCKED | PAC/ABI adaptation is not proven |
| ARMv6/ARMv7/v7s/Thumb/Thumb-2 | PARTIAL | Selected immediate arithmetic/return instruction subsets can be lowered in memory to ARMv7 form. This does not make a game executable; no 32-bit APK is emitted |
| FAT ARM64 + ARM32 selection | SUPPORTED (selection only) | Automatic selection prefers ARM64. Explicit 32-bit target is accepted only when an ARM32 slice is present |
| Supported ARM32-only target | SUPPORTED (selection only) | Selects `armeabi-v7a`; no complete converter currently emits an APK |
| Offline source reconstruction | PARTIAL | Function discovery, CFGs, selected ARM disassembly, reference tracking and pseudocode with explicit uncertainty. Never presented as original source or executed |
| Actual iOS-to-Android API replacements | NOT IMPLEMENTED | Same-name NDK symbols and semantic targets are candidates only. No replacement implementation is generated, linked or tested |
| Objective-C binary ABI / Swift | BLOCKED | The experimental portable runtime is not Apple's ABI/runtime and is not linked into game outputs |
| UIKit, Foundation, graphics, audio, input, lifecycle | BLOCKED | No complete compatibility providers or game lifecycle/input translations exist |
| Resources | PARTIAL | Icons and bundle resources can be inventoried/read for analysis. The placeholder includes app metadata, icon and a static-analysis summary only; no game assets are translated or packaged |
| Importer APK | SUPPORTED | Gradle builds the Android library/import/analyzer app for ARM64 devices |
| Complete-game APK packaging | DISABLED | The former closed-integer launcher wrapper was a partial test artifact, not a complete game. `build_apk` refuses to emit it; the host CLI has no complete-game producer |
| Android placeholder APK | SUPPORTED (explicitly non-playable) | User-triggered on-device builder signs a branded shell with the IPA app name and available icon. No iOS executable or translated game code is included; separate metadata, filename and provider checks prevent it from satisfying the complete-game host contract |
| Host APK attachment | CONTRACT-ONLY | The app accepts only `complete-game-v1` evidence with source/ABI/API/resource/lifecycle checks. No current repository converter produces this contract |
| APK static validation | SUPPORTED | The validator checks the importer APK; game APK validation additionally requires the complete-game contract, generated API implementation evidence, provenance and ABI checks |
| Android runtime/device validation | NOT TESTED | Static checks cannot prove execution or gameplay; reports say `NOT_TESTED` |

## Why symbol substitutions are not API implementations

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
- `leaf-experiment.ll` — emitted only if the entry passes the closed-integer proof; one minimal LLVM
  leaf, never full-game IR or APK input, and syntax-checked with `llvm-as` when it is available.

The report may record an in-memory experimental leaf lowering, but marks it as an assessment only;
`portProgress` remains zero and `conversionProgress.status` remains `NOT_BUILT`. A candidate symbol
mapping is never counted as generated code.

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
and Apple executable assets. This metadata is not a substitute for behavioral testing. The current
host pipeline has no producer for this contract, so no game APK can be attached from repository
outputs today.
