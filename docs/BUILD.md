# Build, run, and test

## Clean Linux machine

Install Python 3.10+, a C++17 compiler (`g++`), Java 17 JDK, and the Android SDK command-line tools.
Set `ANDROID_HOME` or `ANDROID_SDK_ROOT` to the SDK directory.

```sh
sdkmanager 'platform-tools' 'platforms;android-35' 'build-tools;35.0.0' 'ndk;27.2.12479018' 'cmake;3.22.1'
sdkmanager --licenses
```

Run the native, host and Android importer checks (including the compiled `libioscompat.so` time
shims and their host behavior tests):

```sh
python3 tools/build_native.py
python3 -m unittest discover -v
bash tools/test_sanitized.sh
./gradlew --no-daemon :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
```

The Android importer APK is `app/build/outputs/apk/debug/app-debug.apk`; CI copies it to
`RadekiOSConventor-debug.apk`. There are no developer-local files to check in.

The host's `radek validate` command needs Android build-tools 35.0.0 (`aapt2`, `zipalign`, and
`apksigner`) to validate an already-built importer or future complete-game APK. It does not build a
game APK. With the same toolchain installed, `radek convert` additionally assembles the labelled
`experimental-shell.apk` around isolated statically recompiled artifacts, and `radek validate-shell` checks
its `experimental-shell-v1` contract (disclosure, DEX, metadata, signature).

## Import and inspect on Android

Install the importer APK, choose **Choose IPA**, confirm authorization, and select a document. The
source IPA is stored in private app storage until the library entry is deleted. Import performs
bounded extraction and inspection, then removes its temporary extracted tree. It does not create a
game APK. Details include bundle name/version/identifier, declared `MinimumOSVersion` (not inferred),
recovered icon when decodable (including supported compiled `Assets.car` renditions), architectures,
API candidates, current-device compatibility-export checks, blockers, the raw JSON report and analysis logs.

The analysis progress bar measures input copying and extraction; when the bounded conversion proof
applies, the same import job continues into the packaging stages and builds the signed APK
automatically. Runnable game code remains at zero unless that proof applies or a complete host
conversion is attached under the strict contract. Proven IPAs become a signed APK whose statically recompiled
entry routine runs through JNI and displays the message recovered from the IPA, with no extra user
action. Everything else can be turned, via the red **Force convert to .apk** action, into a
separately named, installable preview shell from the bundled Android template that uses the IPA app
name and recovered icon where available but contains no statically recompiled game code or gameplay. Its
launcher visibly states that the preview started and no statically recompiled executable is included, without
converter branding or static-analysis details. The detailed record stays in machine-readable
metadata and the app's library entry. The runtime
packager reads either UTF-8 or UTF-16 Android binary-XML string pools, patches or inserts
`<uses-sdk>` so the manifest can never default to SDK 1, then checks its exact ZIP entry set,
uncompressed/aligned manifest, DEX, resource table and icon payloads before signing and verifying
the APK. It finishes with `InstallAudit`, which re-runs the installer's own structural checks
(parse, signing, SDK levels, stored and page-aligned native libraries, already-installed signature
conflicts) so a problem is reported instead of collapsing into Android's "app not installed".

A host APK attachment must match the IPA's SHA-256/package identity and safe IPA-derived basename,
carry `complete-game-v1` metadata, account for every reachable function and API implementation,
claim complete resources/lifecycle support, contain no original IPA or Apple executable, and pass
Android package/signature checks. The host CLI produces this contract only for the bounded subset
(whole executable equals one proven closed-integer routine with no imports, dependencies, fixups or
runtime metadata); `tests/data/hello-test.ipa` and the longer `tests/data/simple.ipa` are regression
fixtures for that path. Restricted native-entry experiment APKs are rejected.

## Host IPA inspection

Build the native analyzer, then inspect an authorized IPA:

```sh
python3 tools/build_native.py
python3 -m radek analyze authorized.ipa --authorized --output workspace/analysis
```

The workspace must not already exist. Reports and logs persist; extracted workspaces are removed.
The host reconstructs code metadata and may prove one narrow closed-integer entry leaf. When that
proof succeeds, it writes a raw Android instruction blob, portable C, a minimal loadable ARM shared
object exporting `radek_recompiled_entry`, and supplementary `leaf-experiment.ll`. It statically
checks the ELF architecture, symbol size/hash, and lack of undefined symbols; C is compiled and run
against representative IR cases in the unit tests. No Android device load test is performed.

If resolved reconstructed internal calls establish a path from the selected entry to a supported
compiled compatibility import, the host also emits its selected implementation source under
`api-replacements/`. These sources are behavior-tested on the host, but are not linked to the
statically recompiled host entry. The on-device bounded
APK builder packages its prebuilt ARM64 `libioscompat.so` and declares `DT_NEEDED`; it still rewrites
no individual imports because the accepted executable has none. Reports distinguish partial
source instruction-byte progress from complete-game progress. Only when the whole executable
`__text` equals the proven routine, and imports, dependencies, fixups, relocations and runtime
metadata are absent, does `convert` attempt complete-game packaging. A successful signed and
statically validated APK reaches `READY`; unsupported inputs or missing build tools never receive
that status. This bounded path does not statically recompile arbitrary game code, APIs, resources or lifecycle.

ARM selection for the analysis is deterministic: automatic selection prefers ARM64 in a FAT IPA
containing both ARM32 and ARM64; supported ARM32-only inputs target 32-bit Android ARMv7 (`armeabi-v7a`).
This target selection is not evidence that an APK was built. Explicit `--target-abi` overrides must
have a matching input slice.

The API mapper treats general symbol/semantic matches as candidates. `libioscompat.so` contains
host-tested C/time/POSIX functions and a limited CoreFoundation object/collection/run-loop subset;
source generation selects these only when a resolved reconstructed call path connects the selected
entry to a supported import. The mapper does not rewrite Mach-O bindings or link the shim to the
standalone entry. The native C frame-clock service is driven by the converted launcher's Android
Choreographer bridge, but it is not the Objective-C `CADisplayLink` ABI and has no IPA callsite.
`libgcc_s.1.dylib` is only triaged to compiler-rt/libunwind candidates, not a library alias or link;
unsupported entry-reachable APIs remain blockers, and no general playable conversion is claimed.

## Validation and status codes

Validate the Android importer after building it:

```sh
python3 -m radek validate app/build/outputs/apk/debug/app-debug.apk \
  --package dev.radek.conventor \
  --entry dev.radek.conventor.MainActivity \
  --converter-app
```

For future converted results, validation requires the complete-game evidence contract; the old
`closed-integer-entry-v1` wrapper no longer qualifies. Runtime execution remains `NOT_TESTED` unless
separate device tests are supplied.

- `0 READY`: the bounded complete-game conversion produced a signed APK and passed static checks;
  this does not mean device execution or gameplay was tested.
- `1 FAILED`: malformed input, tool failure or interrupted work.
- `2 PARTIAL`: inspection completed without a complete conversion.
- `3 BLOCKED`: no complete-game APK was built; a separately labelled experimental shell may still
  exist and is never reported as a game conversion.

## Development signing and CI

Gradle signs the **importer app** using its normal Android debug build configuration. No signing
key is embedded in the repository. When Force is used, the app creates and retains an installation-
local signer in no-backup private storage and signs the generated preview shell and, for proven
IPAs, the bounded conversion APK; signing is an install requirement, not a gameplay endorsement.
CI publishes no imported-IPA preview APK.

`.github/workflows/build.yml` runs native/Python/Android tests, builds and validates the importer,
converts both `tests/data/hello-test.ipa` and the longer `tests/data/simple.ipa` end to end, validates
the resulting bounded conversion APKs, and uploads `RadekiOSConventor-debug.apk` plus validation,
test and diagnostic conversion artifacts. CI does not publish a synthetic or unproven game APK. No
iOS executable is run by tests, and there is no device runtime smoke test at present.

## Optional source formatting

C++ uses `.clang-format` (LLVM style, four spaces, 110 columns). Python uses Black with
`pyproject.toml` (110 columns); formatters are developer-only, not build/runtime dependencies.
