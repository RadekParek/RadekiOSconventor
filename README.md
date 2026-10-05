# RadekiOSConventor

An **offline IPA inspection and bounded native-reconstruction workbench** for authorized inputs. It includes an Android importer/analyzer, a C++ Mach-O parser, and Python reconstruction tools.

> **One bounded subset converts for real; everything else stays honestly unbuilt.** When an IPA's whole executable is statically proven to be exactly one closed-integer ARM entry routine (MOV-immediate, MOVK, register MOV, immediate ADD/SUB, RET) with no imports, dependencies, fixups or runtime metadata, both the host CLI and the on-device app convert it end to end into a signed, installable APK (`complete-game-v1`) — on Android this happens automatically during import, no extra button needed: the statically recompiled entry is packaged as `libconverted.so` and runs through JNI when the launcher opens, showing the message recovered from the IPA. The three-instruction `tests/data/hello-test.ipa` displays `hello test succesfull`; `tests/data/simple.ipa` adds a deterministic 128-operation ARM64 routine and also converts through the same path. Outside that proven subset nothing is statically recompiled: the host can still lower the entry routine into a standalone shared object and report compatibility registries, and on the `convert` path it packages those artifacts into a signed, explicitly labelled **experimental shell** APK (`experimental-shell-v1`) that states on screen that no game code is statically recompiled. The Android app can separately build an explicitly labelled, signed, installable preview shell for unconvertible IPAs; it contains no statically recompiled game code and cannot run the IPA's game. Device execution and gameplay of bounded conversions are never claimed as tested.

## Offline reconstruction

Authorized imports are analyzed before compatibility is assessed:

- Mach-O images (main executable and embedded dylibs/frameworks): headers, load commands,
  segments/sections, symbols, relocations, exports/imports, dependencies and fixups.
- Disassembly and function discovery with basic-block CFGs (ARM64/ARM64e, ARMv6/ARMv7/Thumb/Thumb-2),
  register/constant/reference tracking and reconstructed pseudocode listings.
- Objective-C classes, categories, protocols, ivars, properties, selectors and message-send
  targets; Swift type/field metadata and symbol demangling.
- Static API-call attribution: which reconstructed functions directly reference imports and, more
  conservatively, which such calls lie on resolved internal call paths from the selected entry.

Results are written as `reconstruction.json` and `reconstruction.md` beside `report.json`. Every
run also records a **conversion ceiling** (`report.json` → `conversionCeiling`, plus a
“Conversion ceiling” section in `reconstruction.md`): an ordered gate ledger over target ABI,
protection, container, slice format, loader metadata, the entry-routine proof, reachable-code
static recompilation, API linking, resources, lifecycle and APK packaging. The first gate that cannot pass
is named with the fail-closed prover's own statement as evidence, and every later gate is
`NOT_REACHED` — never estimated. For a real game this typically reports
`ENTRY_ROUTINE`/`API_LINKING` with the concrete first obstacle (for example an instruction outside
the proven subset, or *import symbols declared and not linked*), which is the honest answer to
“how far can this be converted”. The ledger carries `countsAsConversionProgress: false`: it is an
assessment, not statically recompiled code, and it never counts symbol triage or stub handlers as progress.
Analysis also writes an `ioscompat/` directory containing the generated compatibility-registry
source (`libioscompat.cpp`), copied compatibility headers and a `registry.json` classification of
every observed Darwin import as `verified` (tested implementation) or `stubbed-unimplemented`
(explicit resolution handler). Stubs record invocations and return a documented safe default;
they are resolution targets for a future linker, not API implementations, and the report counts
them separately from verified shims. `compatRegistry.symbolResolution` states this split
explicitly (verified / stubbed-unimplemented / unresolved, `linkedIntoGame: 0`,
`resolutionIsNotImplementation: true`), so a 100% resolution figure is never read as 100%
implementation, static recompilation or gameplay coverage.
When (and only when) the entry routine passes the closed-integer proof, the host writes:

- `recompiled-entry.bin`: lowered ARM64/ARMv7 function bytes;
- `librecompiled-entry.so`: a minimal, relocation-free Android ET_DYN library exporting
  `radek_recompiled_entry`, statically checked for ABI, export size/hash, and undefined symbols;
- `recompiled-entry.c`: a portable C rendering of the same proof-carrying integer operations,
  executable in host tests to compare return-value semantics;
- `leaf-experiment.ll`: supplementary textual LLVM IR for the same proven leaf.

On the `convert` path only, when those artifacts exist, the host additionally assembles,
zipaligns and signs `experimental-shell.apk` using the Android toolchain (aapt2/javac/d8/
zipalign/apksigner) under the `experimental-shell-v1` contract. The shell launcher displays the
disclosure that no game code is statically recompiled, and its metadata repeats it; the validator rejects a
shell that drops the disclosure or claims game code. The shell never satisfies
`complete-game-v1`, and its build status is reported separately from `conversionProgress`, which
stays 0 / `NOT_BUILT` unless the IPA passed the bounded complete-conversion gate and a signed
`complete-game-v1` APK was actually built and statically validated.

These files contain one isolated function only. The shared object has no JNI entry, imports, game
resources, lifecycle, or callsites into the original game. The reported `portProgress.percent` is
source instruction-byte coverage of the selected slice's executable `__text` sections, not a
whole-app or playability percentage. Reconstruction is an engineering artifact, not original
source; uncertain instructions and control flow are marked, and imported code is never executed.
Objective-C `__objc_msgrefs` selector references are followed through their `__objc_selrefs`
pointer slots when statically readable.

## APK output policy

- CI builds **only the RadekiOSConventor importer/analyzer APK** (`RadekiOSConventor-debug.apk`),
  plus the hello-test and longer simple-test bounded conversions used to exercise the complete-game pipeline.
- Importing an IPA runs analysis and — when the executable passes the bounded conversion proof —
  automatically finishes the conversion into a signed, installable APK; no separate action is
  needed for proven inputs. Everything outside the proven subset creates nothing on import; there,
  the red **Force convert to .apk** action builds a separately named, signed and installable
  preview shell using the IPA app name and recovered icon where available. The preview shell
  contains no iOS executable, statically recompiled game code or gameplay; its launcher visibly says that the
  preview started and no statically recompiled executable is included. Preview-shell creation does not count
  as code-static recompilation or complete-game progress.
- A host APK can be attached only if its metadata declares the `complete-game-v1` contract and
  passes source-identity, complete reachable-code/API/resource, ABI, packaging and provenance
  checks. Preview-shell APK metadata and provider paths are separate; a preview shell can never
  satisfy the host APK contract. The host CLI **is** a producer for that contract for the proven
  bounded subset only (see `radek/gamepack.py`); IPAs outside the subset stay `NOT_BUILT`.
- The `experimental-shell-v1` APK is a third, distinct category: a signed, honestly labelled
  inspection shell for the isolated statically recompiled artifacts and compatibility-registry source. It is
  produced only on the `convert` path, is validated with `python3 -m radek validate-shell`, and can
  never be attached as a complete-game host APK.
- The original IPA archive itself is retained only in private analysis storage until the library
  entry is deleted and is never embedded whole into an APK. For bounded conversions the bundle's
  static resource files are packaged verbatim under `assets/bundle/` with a hashed inventory; for
  preview shells only the app name and a recovered icon are copied in. If no original icon is
  available, a generated/fallback icon is used and reported accurately.

The host's ARM assessment prefers `arm64-v8a` when an IPA contains both ARM32 and ARM64. A
supported ARM32-only input is assessed for `armeabi-v7a`. These ABI choices describe analysis and
future conversion targeting; they do not imply that an APK was generated.

## Android API status

The Android mapper reports same-named NDK symbols and semantic rewrite targets (for example,
`UIView` → `android.view.View`) as **candidates only**. Direct-candidate coverage is measured over all
imports; device `dlsym` export verification is shown both as a fraction of those candidates and as a
fraction of all imports (167/264 is 63%, not 65%). This is evidence for the current device/API only.
A name resolving at runtime does not prove Darwin/Android ABI compatibility or link the imported code.
`libioscompat.so` contains host-tested
C/time/POSIX compatibility functions and a limited CoreFoundation C object/collection/run-loop
subset, alongside the four Darwin time APIs (`_CFAbsoluteTimeGetCurrent`, `_CACurrentMediaTime`,
`_mach_absolute_time`, and `_mach_timebase_info`). Host source generation selects implementations
only when reconstructed call paths reach supported imports. `dlopen`/`dlsym` checks can verify
runtime exports, but do not rewrite IPA callsites or prove the imported caller ABI. The bounded
on-device APK packages `libioscompat.so` through `DT_NEEDED`; its accepted zero-import executable
still claims no per-callsite API replacement.

A native C frame-callback service is driven by Android `Choreographer` in the bounded launcher's
lifecycle. It is not the Objective-C `CADisplayLink` class/selector ABI, and no game callsite uses
it yet. The `libgcc_s.1.dylib` mapping is only triage: Android has no drop-in `libgcc_s.so`; compiler
helpers and unwind/personality symbols need NDK compiler-rt/libunwind toolchain integration and ABI
validation before a link can be claimed.

`libioscompat.so` also carries a dynamic symbol-resolution registry: individually verified
implementation entries plus a pool of stub trampolines. Symbols that would otherwise stay unmapped
can be registered at runtime (`radek_compat_register_stub` / `NativeBridge.compatRegisterStub`) and
then resolve to an explicit stub handler instead of nothing. The on-device triage reports those as
`compat stub handler(s) registered (unimplemented)` — a resolution category that is never counted
toward verified implementations or generated API implementations. The host likewise generates a per-IPA
registry source in which every observed import is classified exactly as `verified` or
`stubbed-unimplemented`. Symbol-triage percentages describe categorization, not static recompilation
coverage. The full Foundation/CoreFoundation and Objective-C ABIs, UIKit, graphics, audio, input,
game lifecycle and general resource APIs remain unsupported when required.

## Build and test

See [Build instructions](docs/BUILD.md), [support matrix](docs/SUPPORT.md),
[architecture](docs/ARCHITECTURE.md), and [security](docs/SECURITY.md).

```sh
python3 tools/build_native.py
python3 -m unittest discover -v
./gradlew :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
```

Java 17, Python 3.10+, C++17, Android platform 35, build-tools 35.0.0, NDK 27.2.12479018,
CMake 3.22.1; the Gradle wrapper is included. No Python packages are required.

A small synthetic sample IPA (`tests/data/sample-leaf.ipa`, regenerable with
`python3 tools/make_sample_ipa.py`) is committed for end-to-end checks: its entry routine lies
entirely inside the proven subset, so analysis reaches PARTIAL with nonzero statically recompiled-byte
coverage and a complete compatibility registry. Run it through both paths:

```sh
python3 -m radek analyze tests/data/sample-leaf.ipa --authorized --output .local/analysis
python3 -m radek convert tests/data/sample-leaf.ipa --authorized --output .local/conversion
# .local/conversion holds librecompiled-entry.so, ioscompat/ registry source, and — when an
# Android toolchain is installed — experimental-shell.apk. State is BLOCKED: no game APK.
```

A longer end-to-end regression fixture is also committed. It contains a deterministic 128-operation
ARM64 entry (over eighty times the hello fixture's code bytes), with no imports or unresolved
runtime dependencies, and is inside the same narrow static recompilation subset:

```sh
python3 tools/make_simple_ipa.py  # deterministically regenerates tests/data/simple.ipa
python3 -m radek convert tests/data/simple.ipa --authorized --output .local/simple-conversion
# With an Android SDK/NDK toolchain, this reaches READY and writes simple.apk.
```

For other synthetic Mach-O inputs, `tools/make_fixture.py` produces variants:

```sh
python3 tools/make_fixture.py --arch arm64 --output .local/fixture.ipa
python3 -m radek analyze .local/fixture.ipa --authorized --output .local/analysis
# Inspect .local/analysis/librecompiled-entry.so and report.json.
# No game APK is produced.
```

To inspect an authorized IPA:

```sh
python3 -m radek analyze authorized.ipa --authorized --output workspace/analysis
```

A new output directory is required. Reports and logs persist; temporary extraction workspaces are
removed. `analyze` returns `PARTIAL` after a successful inspection and may write the isolated code
artifacts above. On `convert`, inputs outside the bounded complete-game subset remain `BLOCKED` and
may produce only a separately labelled experimental shell. Inputs inside the subset proceed through
native linking, launcher/APK packaging and static validation: with an Android toolchain they reach
`READY` and emit a signed `complete-game-v1` APK; without one they remain blocked with an explicit
missing-toolchain report. Device execution and gameplay are not claimed as tested.

## GitHub APK builds

Open **Actions → Build and validate Android APKs → Run workflow** and select the branch containing
this implementation. Pushes and pull requests also run CI. After a successful run, download the
**RadekiOSConventor-debug.apk** importer artifact. CI runs native, Python and Android importer tests,
validates the importer APK, converts `tests/data/hello-test.ipa` end to end with the host CLI, and
validates the resulting bounded complete-game APK (`complete-game-v1`) alongside the honestly
blocked sample leaf.

Only inspect or convert IPAs you own or are authorized to process. Encrypted/FairPlay-protected
images are blocked; no protection bypass is provided.
