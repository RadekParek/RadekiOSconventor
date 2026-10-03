# RadekiOSConventor

An **offline native-reconstruction workbench**, not an iOS emulator. Kotlin Android importer + C++ Mach-O analyzer + Python host conversion/SDK packaging pipeline.

> **Important: this is NOT a general IPA/game converter.** Current native conversion is restricted to self-contained, straight-line integer-returning entry code with no reachable imports/framework calls, memory access, address references or unsupported runtime semantics. Linked-but-unused dylib/import records can be omitted only after that entry is proven; no no-op framework stubs are generated. UIKit, Foundation ABI, Swift, general Objective-C, graphics/audio/input and ordinary commercial apps are **BLOCKED** when required. An importer APK is not proof those apps can be converted.

## Offline reconstruction before conversion

Every import is analyzed before any conversion decision is made:

- Mach-O images (executable, embedded dylibs/frameworks): headers, load commands,
  segments/sections, symbols, relocations, exports/imports, dependencies and fixups.
- Disassembly and function discovery with basic-block CFGs (ARM64/ARM64e, ARMv6/ARMv7/Thumb/Thumb-2),
  register/constant/reference tracking and pseudocode listings.
- Objective-C classes, categories, protocols, ivars, properties, selectors, message-send
  targets; Swift type/field metadata and demangling.
- Reachability: which imported symbols are actually called, by which reconstructed functions,
  which linked frameworks are weak/optional, and which APIs are natively implementable on
  Android, need compatibility code, or are genuinely unsupported.

Results are written as `reconstruction.json` and `reconstruction.md` beside `report.json`.
A dependency that is merely linked is **never** reported as blocked: only reachable APIs are.
The reconstruction is an engineering artifact and is never claimed to be original source;
reconstructed code is never executed.

## Two distinct APKs

1. **Converter/importer app:** the Actions artifact `RadekiOSConventor-debug.apk` contains the Android library/import UI and native analyzer. The UI does not include an on-device SDK/NDK or compile IPAs on the phone.
2. **Standalone converted program:** `python3 -m radek convert ...` produces a separate `RadekiOSConventor-debug.apk` containing reconstructed **ARM64 Android ELF code**, DEX launcher, icon and bundle resources. It does not ship the IPA, Mach-O executable, an emulator, interpreter, runtime translator or converter. The currently supported entry contract returns an integer; the Android launcher displays that result. It does **not** recreate an iOS application's UI.

CI uploads the second APK separately as `standalone-native-fixture` to avoid confusing the products.

## Build, test, edit

See [Build instructions](docs/BUILD.md), [support matrix and conversion contract](docs/SUPPORT.md), [architecture](docs/ARCHITECTURE.md), and [security](docs/SECURITY.md).

```sh
python3 tools/build_native.py
python3 -m unittest discover -v
./gradlew :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
```

Java 17, Python 3.10+, C++17, Android platform 35, build-tools 35.0.0, NDK 27.2.12479018, CMake 3.22.1; Gradle wrapper included. No Python packages are required.

### Exercise actual offline conversion

With the documented Linux Android toolchain installed:

```sh
python3 tools/make_fixture.py --arch arm64 --output .local/fixture.ipa
python3 -m radek convert .local/fixture.ipa --authorized --output workspace/fixture
# workspace/fixture/RadekiOSConventor-debug.apk
```

`--arch armv6`, `armv7`, `armv7s`, `thumb`, and `thumb2` exercise **offline ARM32 → ARM64 lowering** for the proven closed integer leaf subset. This does not make general ARMv6 games (which commonly depend on UIKit, graphics, audio, input and runtime services) compatible. The fixtures are generated synthetic Mach-O programs, not installable signed iOS apps. The resulting native routine returns 42.

Icons are recovered through a generic fallback chain (Info.plist names, `@2x`/`@3x`/`~ipad`
variants, compiled `Assets.car` renditions, then other bundle images) with every attempt
recorded; the recovered icon becomes the generated APK's launcher icon.

For authorized real IPAs, `analyze` reads actual metadata/dependencies without attempting to decrypt or execute the input:

```sh
python3 -m radek analyze authorized.ipa --authorized --output workspace/analysis
```

A new output directory is required. Reports/logs persist; extraction workspaces are removed. CLI exit codes: `0 READY`, `1 FAILED`, `2 PARTIAL` (analysis-only), `3 BLOCKED`. Nonzero statuses are intentional, not silent successes.

## GitHub APK builds

Open **Actions → Build and validate Android APKs → Run workflow**, select the branch containing this implementation. Pushes and pull requests also run the workflow. Download the **RadekiOSConventor-debug.apk** artifact after a successful run. CI installs the pinned toolchain, builds C++ and Android code, runs host/Kotlin tests and real signed synthetic conversions, validates both APK types, and fails on errors.

Only convert IPAs you own or are authorized to convert. FairPlay/encrypted images are blocked; no protection bypass is provided.
