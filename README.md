# RadekiOSConventor

An **offline IPA inspection and native-reconstruction workbench**, not an iOS emulator or a general game converter. It includes an Android importer/analyzer, a C++ Mach-O parser, and Python reconstruction tools.

> **No playable or complete-game APKs are currently produced.** This repository does not implement complete iOS-to-Android game translation, actual framework/API replacement, or an Android lifecycle/runtime for converted games. API-name matches and semantic mappings are analysis candidates only. The narrow closed-integer entry experiment is not a game conversion; packaging it as a game app remains disabled. The Android app can separately build an explicitly labelled, signed, installable placeholder, but it contains no translated game code and cannot run the IPA's game.

## Offline reconstruction

Authorized imports are analyzed before compatibility is assessed:

- Mach-O images (main executable and embedded dylibs/frameworks): headers, load commands,
  segments/sections, symbols, relocations, exports/imports, dependencies and fixups.
- Disassembly and function discovery with basic-block CFGs (ARM64/ARM64e, ARMv6/ARMv7/Thumb/Thumb-2),
  register/constant/reference tracking and reconstructed pseudocode listings.
- Objective-C classes, categories, protocols, ivars, properties, selectors and message-send
  targets; Swift type/field metadata and symbol demangling.
- Reachability: which imported symbols appear to be called, by which reconstructed functions,
  and which linked frameworks or APIs remain unsupported.

Results are written as `reconstruction.json` and `reconstruction.md` beside `report.json`.
When (and only when) the entry routine passes the existing closed-integer proof, the host also
writes `leaf-experiment.ll`: textual LLVM IR for that one MOV-immediate/MOVK/immediate-ADD/SUB/RET
leaf. It is explicitly not the full binary lift, recovered source, a game-code port, or an APK input.
Reconstruction is an engineering artifact, not original source; uncertain instructions and
control flow are marked, and imported code is never executed. Objective-C `__objc_msgrefs` selector
references are followed through their `__objc_selrefs` pointer slots when statically readable.

## APK output policy

- CI builds **only the RadekiOSConventor importer/analyzer APK** (`RadekiOSConventor-debug.apk`).
- Importing an IPA runs analysis only and does not automatically create an APK. The red **Force
  convert to .apk** action creates a separately named, signed and installable launcher placeholder
  branded with the IPA app name and recovered icon where available. It contains no iOS executable,
  translated game code or gameplay, and says so when launched. Placeholder creation does not count
  as code-translation or complete-game progress.
- A host APK can be attached only if its metadata declares the `complete-game-v1` contract and
  passes source-identity, complete reachable-code/API/resource, ABI, packaging and provenance
  checks. Placeholder APK metadata and provider paths are separate; a placeholder can never satisfy
  the host APK contract. The current host CLI has **no producer** for that contract.
- The original IPA is retained only in private analysis storage until the library entry is deleted;
  it is never packaged into an APK. A recovered icon is copied into the placeholder; if no original
  icon is available, a generated/fallback icon is used and reported accurately.

The host's ARM assessment prefers `arm64-v8a` when an IPA contains both ARM32 and ARM64. A
supported ARM32-only input is assessed for `armeabi-v7a`. These ABI choices describe analysis and
future conversion targeting; they do not imply that an APK was generated.

## Android API status

The Android mapper reports same-named NDK symbols and semantic rewrite targets (for example,
`UIView` → `android.view.View`) as **candidates only**. It has not generated, linked, or tested
replacement implementations. The report separately records zero generated API replacements and
zero complete runnable game code. UIKit, Foundation ABI, Swift, Objective-C dispatch, graphics,
audio, input, game lifecycle and general resource APIs remain unsupported when required.

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

For a synthetic Mach-O input, the host CLI can produce a report and explicitly refuse an
incomplete game APK:

```sh
python3 tools/make_fixture.py --arch arm64 --output .local/fixture.ipa
python3 -m radek analyze .local/fixture.ipa --authorized --output .local/analysis
# No game APK is produced.
```

To inspect an authorized IPA:

```sh
python3 -m radek analyze authorized.ipa --authorized --output workspace/analysis
```

A new output directory is required. Reports and logs persist; temporary extraction workspaces are
removed. `analyze` returns `PARTIAL` after a successful inspection. `convert` returns `BLOCKED`
when the input fits only the experimental leaf subset because it is not a complete game converter.
`READY` is reserved for a future complete conversion that passes static APK validation.

## GitHub APK builds

Open **Actions → Build and validate Android APKs → Run workflow** and select the branch containing
this implementation. Pushes and pull requests also run CI. After a successful run, download the
**RadekiOSConventor-debug.apk** importer artifact. CI runs native, Python and Android importer tests,
validates the importer APK, and includes no synthetic or placeholder game APK artifact.

Only inspect or convert IPAs you own or are authorized to process. Encrypted/FairPlay-protected
images are blocked; no protection bypass is provided.
