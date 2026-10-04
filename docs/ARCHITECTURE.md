# Modules and extension points

- `app/`: Kotlin framework-only Android UI. `Library` owns private IPA storage, analysis reports,
  progress and architecture-independent icon recovery; `AssetCatalogIcon` reads bounded raster
  renditions from compiled `Assets.car`; `AndroidApiMapper` reports NDK name candidates, checks
  public NDK exports on the current device, dynamically registers explicit compat stub handlers for
  otherwise-unmapped symbols, and clearly separates all of those checks from implementation;
  `SafeZip` handles untrusted extraction; `Plist` handles
  XML/binary parsing; `NativeBridge` calls C++ through JNI (including the compat-registry bridge);
  `PlaceholderApkBuilder` customizes, signs and verifies a bundled source-free Android shell;
  `AndroidApiMapper` separately checks compiled time-shim exports; `ResultProvider` has strict
  paths for complete-game host APKs and explicitly non-playable placeholder APKs.
- `placeholder-template/`: minimal Android activity and fallback icon. The converter build extracts
  only its manifest, resource table, DEX and fallback icon as assets (not the signed template APK);
  per-import metadata/icon branding is applied only after the user invokes **Force convert to .apk**.
- `native/src/macho.cpp`: host/Android shared C++ Mach-O analyzer. It never executes the input.
  JSON describes source structure, not conversion success.
- `native/include/runtime.hpp`: experimental portable runtime and storage primitives with host tests.
  `native/src/apple_time_compat.cpp` is a separate, narrow ABI-shaped implementation of four time
  functions; it does not establish Foundation, Objective-C, or general CoreFoundation compatibility.
- `native/src/ioscompat_registry.cpp`: the libioscompat dynamic resolution registry. Verified time
  shims are seeded; unmapped Darwin imports are registered at runtime (`radek_compat_register_stub`,
  JNI `compatRegisterStub`) onto individually counted stub trampolines. Classification is
  `verified` vs `stubbed` only; stubs never masquerade as implementations.
- `radek/archive.py`: authorized input staging, bounded archive/plist import and metadata.
- `radek/analysis.py`: native analyzer invocation, dependency graph and fail-closed leaf eligibility.
- `radek/ir.py`: explicit register/width/flag-aware instruction records, decoding, proof and
  ARM64/ARMv7 lowering for the proven straight-line subset (MOV-immediate, MOVK, register MOV,
  immediate ADD/SUB, RET). ARM32 inputs target `armeabi-v7a`; unsupported instruction classes
  raise `Unsupported`.
- `radek/compat_layer.py`: generates the per-IPA libioscompat registry source (`ioscompat/`) in
  which every observed Darwin import is classified exactly as a verified implementation or an
  explicitly unimplemented stub handler, plus `registry.json`. It reports resolution coverage
  separately from implementation coverage and never links anything.
- `radek/c_backend.py` and `radek/elf_writer.py`: lower only that proven leaf to executable C for
  host semantic tests and a minimal ARM ET_DYN shared object exporting `radek_translated_entry`.
  The `.so` is self-contained but isolated; it is not linked into the original program or an APK.
- `radek/api_translation.py`: emits source for the four implemented time shims only when resolved
  reconstructed internal calls connect the selected entry to a matching import. The report records
  source generation and separately records zero game links.
- `radek/llvm_ir.py`: emits supplementary textual LLVM IR from the proven closed-integer leaf and
  asks `llvm-as` to verify syntax when available. `leaf-experiment.ll` is not arbitrary ARM lifting,
  recovered source, or a complete game package.
- `radek/resources.py`: icon normalization, resource inventory, executable/signature exclusion.
- `radek/dex.py`: bounded DEX integrity and class-identity inspection.
- `radek/apk.py`: importer/host APK validation. Game-APK creation is deliberately disabled until
  a complete game-code and API-replacement backend exists. The honest `experimental-shell-v1`
  builder (aapt2 → javac → d8 → zipalign → apksigner) packages isolated artifacts with a mandatory
  on-screen and metadata disclosure; its validator fails closed if the disclosure or contract is
  missing.
- `radek/pipeline.py`: guarded analysis states, durable JSON/JSONL reports and isolated temporary
  workspace cleanup. A proven leaf emits isolated code artifacts and nonzero text-byte progress;
  the convert path may build the labelled experimental shell around them. The pipeline never links
  those artifacts into a game APK or reaches `READY`.
- `tests/`, `app/src/test/`, `native/tests/`: synthetic fixtures, negative/security tests, native
  runtime tests and Robolectric importer tests. `tests/data/sample-leaf.ipa` is a committed,
  byte-reproducible sample inside the proven subset (`tools/make_sample_ipa.py` regenerates it);
  `tests/test_sample_ipa.py` asserts its full honest conversion path.

## States

The importer's analysis path is:

`IMPORTED → ANALYZING → PARTIAL | BLOCKED | FAILED`

- `PARTIAL`: inspection completed and, if the proof succeeds, an isolated translated-entry library
  and/or reachable time-shim source was emitted. Complete game conversion is not implemented.
- `BLOCKED`: input protection, unsupported executable semantics or missing conversion capabilities
  prevent a game APK. A restricted integer-entry library may still be emitted, but is not a game.
- `FAILED`: malformed input, analysis/tool failure or interrupted work.
- `READY`: reserved for a future complete native game conversion whose APK passes independent
  static validation. No current IPA-to-game path can enter `READY`.

The app's **Force convert to .apk** action does not override these states or claim game translation.
It separately builds a signed placeholder launcher carrying the app name and recovered icon where
available; its screen says that no game code was translated and that the game will not run. The
placeholder has its own filename, metadata, progress and provider validation. A host result remains
shareable/installable only after the `complete-game-v1` contract passes attachment checks; the CLI
currently emits no such result. The CLI `convert` path may additionally emit
`experimental-shell.apk` (`experimental-shell-v1`): a signed shell whose launcher and metadata
disclose that no game code is translated. It is reported in `experimentalShell` separately from
`conversionProgress`, which stays `NOT_BUILT`.

## Adding real support

Do not add bundle-ID exceptions or turn symbol matches into claimed implementations. Add parser,
decoder, IR, linker, API/runtime provider and lifecycle code with positive and negative tests, then
prove each reachable code path and API implementation is generated and linked. The four tested time
shims are real function bodies, but runtime export availability is not game integration. A dependency
can only be classified `converted`, `provided by compatibility layer`, or `Android equivalent` for
the functions actually wired into output. The closed-integer backend translates one function only; it
does not implement the app, resources, lifecycle, or arbitrary APIs.

Stub handlers are a legitimate forward step only when they stay classified as stubs: the compat
registry gives every unmapped import a stable, observable resolution target (`stubbed`) so a future
linker has something to bind, and so accidental invocation is counted instead of undefined. Counts
of stubbed handlers must never be merged into verified/implemented coverage, and removing the
placeholder/experimental disclosures from output APKs is prohibited — those strings are the
contract that keeps emitted artifacts honest.

Future graphics providers should separate API command/state capture from an Android GLES backend;
Metal requires shader/type translation plus an appropriate Android rendering backend. Neither
exists, so unsupported APIs remain blocked rather than bound to no-ops. Framework providers should
be introduced behind a versioned ABI symbol registry and tests of observable behavior, not by
accepting symbol names alone.
