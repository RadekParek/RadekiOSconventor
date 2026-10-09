# Angry Birds v1.0 IPA — static findings

- **Analysis date:** 2026-10-06
- **Input:** locally held, authorized `tests/data/AngryBirds_v1.0_os30.ipa`
- **Latest machine-readable static run:** `.local/angrybirds-analysis-v11/` (ignored; not a release artifact)
- **Latest real-loader probe:** `.local/angrybirds-analysis-v10/loader-probe-latest.txt` (ignored; not a release artifact)

The IPA is 9,769,211 bytes (SHA-256 `cdea1290dcaa67ec75cf555e78caaa8d90cfaa90021ec7f05e40089fb87944cf`). This change removes the previously tracked IPA from Git while retaining the local copy at `tests/data/AngryBirds_v1.0_os30.ipa`; `.gitignore` keeps it local. It must not be committed or packaged into a build.

## Result

The analyzer returns **`BLOCKED`** (expected CLI exit status 3). The first failed conversion gate is **`LOADER_METADATA`**: Objective-C/Swift/initializer/unwind metadata is not implemented by the bounded loader. The later entry-routine and API-link gates are consequently not passed. No playable APK or complete-game artifact was produced.

Application metadata: `AngryBirds`, bundle ID `com.clickgamer.AngryBirds`, executable `AngryBirds`, build `1.0`, minimum iOS `3.0`. Only one source icon was found: `Icon.png`, 57×57, so this IPA does not exercise multi-resolution icon selection.

## Recovered slice and evidence limits

- Only the `armv6` slice was present.
- 2,281 functions were identified; 391,568 of 1,243,432 `__text` bytes were decoded (**31.49%**); 881 instructions remain unknown.
- The import inventory contains 254 symbols. 160 symbols have at least one recovered direct-call record; 94 have no recovered direct call. Summed per-symbol direct-call-site count: 1,059.
- Objective-C reconstruction recovers `AppController : NSObject` and `MyEAGLView : UIView`. The superclass values come from external relocation symbol names at the class-object superclass slots; v8 verifies the previously missing superclass fields. This is offline metadata recovery, not runtime registration of the app's classes.
- These are static records from recovered code—not runtime call counts. Rankings are subject to the 31.49% decoded-text coverage and do not infer indirect calls, Objective-C dispatch, callbacks, or loader initializers.

### Highest-frequency recovered direct-call imports

| Rank | Import | Recovered call sites |
|---:|---|---:|
| 1 | `_memcpy` | 184 |
| 2 | `__ZdaPv` | 61 |
| 3 | `__ZdlPv` | 59 |
| 4 | `_strlen` | 55 |
| 5 | `_memcmp` | 54 |
| 6 | `__Znam` | 43 |
| 7 | `_objc_msgSend` | 42 |
| 8 | `_strcmp` | 38 |
| 9 | `_memset` | 31 |
| 10 | `_free` | 26 |
| 11 | `_glDeleteBuffers` | 18 |
| 12 | `___maskrune` | 16 |
| 13 | `_sprintf` | 16 |
| 14 | `_malloc` | 15 |
| 15 | `___modsi3` | 14 |
| 16 | `_glGenBuffers` | 14 |
| 17 | `_strcasecmp` | 14 |
| 18 | `_glBindBuffer` | 12 |
| 19 | `_fprintf` | 11 |
| 20 | `_strchr` | 10 |
| 21 | `_fwrite` | 9 |
| 22 | `_strerror` | 9 |
| 23 | `__Znwm` | 8 |
| 24 | `___cxa_atexit` | 8 |
| 25 | `___error` | 8 |

## Startup and first observed application import

The image entry is `start` at `0x4320`. Its recovered direct-call graph contains one function and **zero imports**. It contains indirect transfers—`blx r12` at `0x4350` and `bx r12` at `0x4360`—whose dyld-resolved targets are not statically recovered. Therefore the handoff to `_main` and the globally earliest startup import are **unresolved**; the zero-import entry graph is not evidence that the app makes no calls.

Within `_main` at `0x74358`, the first statically observed direct import call is:

1. `_objc_msgSend` at callsite `0x74378`, through stub `0x132cf0`.
2. Static ARM32 register-flow analysis resolves `r0` to `_OBJC_CLASS_$_NSAutoreleasePool` (import slot `0x14365c`) and `r1` to selector `new` (selector slot `0x143620`). This is static argument evidence, not execution evidence; the class reference is a separate data import.
3. Next observed `_main` imports are `_UIApplicationMain` at `0x74390` through stub `0x1325c4`, then `_objc_msgSend` at `0x743a0` through stub `0x132cf0`.

Phase 2 adds bounded native Objective-C callouts and guest class-object resolvers. Host Mach-O fixtures bind `_objc_msgSend`, `_objc_msgSendSuper2`, imported class-data symbols, legacy indirect function pointers, and external-relocation data; tests cover `NSAutoreleasePool +new`/`-init`, a super message, class lookup, release, autorelease-pool push/pop, selector/class/protocol introspection, and dispatch against registered guest methods/classes. The real authorized main executable passes through the loader initializer, which registers `AppController : NSObject` and `MyEAGLView : UIView`, including their instance sizes and nine ivars apiece. A host harness allocates `MyEAGLView` and verifies `_objc_msgSend` selects the guest `createFramebuffer` IMP at `0x69610`; it does not execute that method body. This remains host-side metadata/dispatch evidence—not CPU execution of Angry Birds or an app launch. `_objc_msgSend` remains the **first statically observed application import in `_main`** (now host-bound), not a proven global first runtime-missing import.

The Foundation subset now also binds `_NSSearchPathForDirectoriesInDomains` and returns guest-backed `NSArray`/`NSString` values for a bounded set of user-domain directories. Tests inspect the guest array and strings, verify ownership/copy behavior, and cover unsupported directories. These are virtual path strings only; they are not an Android-backed sandbox filesystem or proof the IPA calls the function. The v11 static report records zero recovered direct callsites for this import.

### Runtime evidence: startup chain and execution policy

The boot attempt no longer stops at `_UIApplicationMain`. With the VFP unit
enabled for the guest, the application-lifecycle adapter instantiated the image's
own `AppController`, wired it into the `UIApplication` singleton, and delivered
`applicationDidFinishLaunching:` to the real guest implementation at `0x6871c`.
Inside that method the app created its window and EAGL view (`-[UIView layer]` ->
`CAEAGLLayer`, `numberWithBool:`, `dictionaryWithObjectsAndKeys:`,
`-[EAGLContext initWithAPI:]`, `setCurrentContext:`, `addSubview:`,
`makeKeyAndVisible`) and entered its engine's render setup. The GLES calls are
forwarded to the platform driver, compiler-runtime helpers are implemented, and
the guest filesystem is mounted.

The old host probe's 340,309 and 2,000,000-instruction stops were diagnostic
budgets, not game boundaries. The device runner now passes zero for both limits,
which means unlimited execution: it does not terminate a live game loop merely
because an instruction counter or timer expired. It still stops by name when an
unimplemented shim is touched, or on a real guest/backend fault. This is
observed startup execution, not by itself proof of a rendered frame, input, or
playable gameplay: unsupported shim families (including remaining OpenAL,
pthread, Foundation, and input surfaces) remain real conversion work.

The host `radek-gameboot --diagnostic-probe` flag retains a finite time window
only so CI can inspect this image without hanging forever in the guest loop.
Static texts below that call `_UIApplicationMain` "the next concrete startup
blocker" describe the pre-runtime state; the runtime binding now exists and the
blocker moved to the remaining framework/input coverage.

### Real-IPA loader probe with trap-mode lifecycle bindings

The 1,822,112-byte ARMv6 main executable was re-extracted from the authorized local IPA into ignored `.local/angrybirds-analysis-v10/` storage and passed to `MachOLoader` with Objective-C, AudioToolbox, SJLJ, Darwin-compatibility, and trap adapters registered. The current probe maps three segments, reads entry `0x4320` from `LC_UNIXTHREAD`, and returns **`LOADED_WITH_TRAPS`**: 171 relocation/pointer records resolve, 407 unsupported records are bound to named abort-on-call traps, and 0 records remain unbound. The loader also applies the image's 348 absolute 32-bit ARM vanilla external relocations and 517-entry indirect-symbol table. `_UIApplicationMain`, the selected Foundation path function, audio-session state adapters, Objective-C messaging/class data, compiler-runtime helpers, and the SJLJ register/unregister boundaries are now available to the boot attempt. `__Unwind_SjLj_Resume` and mutation remain explicit fail-closed exception boundaries; personality dispatch, catch search, and landing-pad transfer are not implemented.

The old loader-only probe reported 39 fixup records resolved and `_UIApplicationMain` as the first unresolved import. That is historical pre-lifecycle evidence, not the current runtime blocker: trap mode allows the runner to execute past imports that have no implementation and names the first one actually touched. Static register setup still shows `argc`, `argv`, a null principal-class name, and an `__NSConstantString` containing `AppController` at `_main`'s `_UIApplicationMain` callsite `0x74390`; those are static facts, not a global runtime call-order claim. `_AudioSessionInitialize` has no recovered direct callsite; `_AudioSessionSetActive` has two recovered callsites in `__Z16interruptHandlerPvm` at `0x6806c` and `0x6808c`. `_main`'s first statically observed direct import remains `_objc_msgSend` at `0x74378`, followed by `_UIApplicationMain` at `0x74390` and pool release at `0x743a0`.

The latest import-detail report locates `_objc_msgSend_stret` at four call sites in `MyEAGLView` touch handlers (selector `locationInView:`), `_objc_setProperty` once in `-[MyEAGLView setContext:]`, and `_objc_enumerationMutation` at four touch-handler sites. These counts are recovered static call sites—not observed runtime executions. The `context` property is statically declared with attributes `T@\"EAGLContext\",&,N,Vcontext` (retain, non-atomic), matching the shim's tested retain path; the general copy path is now tested separately with host and guest `-copy` implementations, but is not indicated by this IPA property. `_objc_enumerationMutation` now resolves to a host-tested exception boundary that stops with `GUEST_EXCEPTION_RAISED`; it does not execute guest catch/unwind, and SJLJ exception handling remains unsupported. The native image initializer also registers the actual IPA's `AppController : NSObject` (44-byte instance, 9 ivars) and `MyEAGLView : UIView` (72-byte instance, 9 ivars). The image declares three protocols and no categories; `AppController` lists `UIApplicationDelegate` and `UIAccelerometerDelegate`. The loader now registers those protocol objects and direct conformances; a host-only `objc_getProtocol`/`conformsToProtocol:` probe also verifies inherited `NSObject` conformance and returns null for an unknown protocol. A host loader probe allocates the real `MyEAGLView`; `_objc_msgSend` reports `respondsToSelector:createFramebuffer` true, an absent selector false, `isKindOfClass:UIView` true, and `isMemberOfClass:MyEAGLView` true. The same host-side metadata lets dispatch select `createFramebuffer`'s guest IMP at `0x69610`; its method body was not executed.

## Per-dylib import evidence and classifications

Import-to-dylib association now preserves the Mach-O nlist library ordinal as well as dyld bind ordinals. The table is a static provider inventory, not a current loader-status table and not a report of executed calls. “Host-tested” means the exact import's native runtime binding was exercised by a host test; for class/metaclass imports this proves only guest-data materialization, not framework class behavior. “Stub” means no tested implementation/binding exists for that import. The current trap-mode loader result is the 171 resolved / 407 trapped / 0 unbound result above.

| Ordinal | Dependency | Grade | Observed imports | Host-tested | Stub-only | Notes |
|---:|---|---|---:|---:|---:|---|
| 1 | Foundation | `compatibility` | 7 | 7 | 0 | Six imported class objects have guest-data resolvers; `_NSSearchPathForDirectoriesInDomains` has a host-tested, guest-backed subset for selected virtual user paths. This is not full Foundation, bundle/resource lookup, or an Android-backed filesystem. |
| 2 | UIKit | `candidate` | 7 | 6 | 1 | Five UIKit classes plus `_OBJC_METACLASS_$_UIView` resolve as guest class data in this static inventory; the current runtime additionally supplies a narrow `_UIApplicationMain` lifecycle adapter, not a full UIKit/run-loop ABI. Android UI analogues are not UIKit adapters. |
| 3 | OpenGLES | `provided` | 56 | 1 | 55 | `_OBJC_CLASS_$_EAGLContext` resolves as guest class data in the static inventory; the runtime now forwards a reviewed OpenGL ES 1.1 subset to the platform driver, while unsupported calls remain traps and no device frame has been validated here. |
| 4 | QuartzCore | `compatibility` | 1 | 1 | 0 | `_OBJC_CLASS_$_CAEAGLLayer` resolves as guest class data only. This does not implement QuartzCore's Objective-C ABI, `CALayer`, or rendering. |
| 5 | CoreGraphics | `candidate` | 0 | 0 | 0 | No import associated with this ordinal. |
| 6 | OpenAL | `candidate` | 19 | 0 | 19 | AAudio is a possible output target, not an OpenAL ABI adapter. |
| 7 | AudioToolbox | `compatibility` | 2 | 2 | 0 | `_AudioSessionInitialize` and `_AudioSessionSetActive` have host-tested state-only callouts; no device activation, audio output, or interruption delivery is implemented. |
| 8 | `libstdc++.6.dylib` | `candidate` | 15 | 0 | 15 | GNU libstdc++ has no drop-in NDK provider; libc++ overlap does not establish C++ ABI/runtime compatibility. |
| 9 | `libgcc_s.1.dylib` | `compatibility` | 12 | 3 | 9 | SJLJ register/unregister are tested; `__Unwind_SjLj_Resume` resolves only to an explicit exception-stop boundary. Search, personality dispatch, landing-pad transfer, and the remaining imports are unsupported; no `libgcc_s.so` alias or completed link. |
| 10 | `libSystem.B.dylib` | `provided` | 124 | 48 | 76 | Reviewed bionic/system targets and a bounded host-tested C/POSIX subset; remaining imports are stub-only. |
| 11 | `libobjc.A.dylib` | `compatibility` | 7 | 7 | 0 | All seven observed imports have tested bindings, though several are data/class resolvers and the mutation boundary still stops without guest unwind. Guest dispatch also host-tests selector/class introspection and protocol conformance via the non-imported `_objc_getProtocol` helper. Retain/copy property storage is tested, including a guest `-copy` IMP continuation; guest catch/SJLJ unwind remains unsupported. |
| 12 | CoreFoundation | `compatibility` | 4 | 3 | 1 | `NSDictionary`, `NSObject`, and `NSObject` metaclass class-data imports resolve; `___CFConstantStringClassReference` remains stub-only. No CoreFoundation methods are supplied by this adapter. |

These grades describe provider categories, not the imported game's execution. Host tests exercise the exact bounded callouts and class-data resolvers described above. Runtime coverage remains deliberately narrow: one 8-byte touch point, retain/copy property storage (including a pinned-Unicorn guest `-copy` IMP continuation), the selected virtual search paths and their guest `NSString`/`NSArray` selectors, and `NSObject` selector/class/protocol introspection against registered guest metadata. `_objc_enumerationMutation` stops with a structured guest exception but still has no catch/unwind implementation. These tests do not establish usable Foundation/UIKit/GLES/QuartzCore/CoreFoundation behavior or a complete Objective-C ABI. `provided` likewise does not mean an IPA callsite was rewritten, linked, or verified on a device.

### Import-evidence database delta

An earlier analysis exposed only nlist `description`/`n_desc` and did not decode its high-byte library ordinal, leaving every dependency edge with zero associated imports. The native full and compact analyzers now emit the ordinal, and the host graph retains a fallback decoder for older analyzer output. In this run, all 254 import symbols are associated with the 12 dependency ordinals (association status `COMPLETE`); the per-edge counts above sum to 254. This corrects the evidence index, **not** the Python compatibility implementation database.

The import-provider catalog partitions the fixture's 254 distinct names into 181 strict same-name Android NDK/system candidates and 73 non-same-name `compat-runtime-v1` guest-adapter entries. Thus `73/254` is the count of catalogued compatibility names outside the strict NDK subset; it is not 73 direct NDK exports, 73 static links, or proof that every API is semantically complete. The broader reviewed NDK inventory contains 1,229 candidate names from the mapper; the native runtime registers every one with a specialized adapter where the ABI is known and an explicitly named bounded provider boundary for the remaining Android framework/driver APIs. The 73-entry guest-adapter catalog is pinned in the host, Kotlin and C++ catalogs; the 181-entry same-name catalog, 1,229-entry inventory, and bounded NDK adapter are pinned in C++. Runtime startup checks that each catalog name has a registry binding. The guest adapters include Objective-C class/metaclass data and dispatch, AudioSession state, Foundation search paths and UIKit lifecycle, Darwin errno/ctype/stream/data cells, EAGL keys, stateful OpenAL, ARM compiler-runtime arithmetic, and bounded C++ ABI/SjLj boundaries; the NDK wrappers include libc/POSIX/math/stdio/pthread imports, self-contained libm operations, zlib checksums, and virtual bundle assets. A catalog entry means a registered ABI adapter or guest-data materialization; it does not mean full Apple framework equivalence, a host audio device, or completed guest exception landing-pad transfer. C++ throw, SjLj resume, personality, Objective-C mutation, pthread callback creation, and process-control exits still stop at explicit runtime boundaries rather than silently returning through an invalid guest stack. Per-image import-slot fixups are reported separately in `runtimeLinking` and are not static Android callsite rewriting.

The generated `libioscompat` registry continues to report its resolution-target/stub split separately from the guest-adapter catalog. The current trap-mode loader resolves 171 records and binds 407 unsupported records to traps; the old 39-record/first-unresolved-import figures are retained only as historical pre-lifecycle evidence. The native loader installs ARM32 guest addresses into Mach-O import/fixup slots; the per-image `runtimeLinking` block records those actual binds, traps, and unresolved slots. That is runtime guest binding, not static Android game-callsite rewriting or a statically linked game object. These adapters are not evidence of a complete playable IPA conversion; statically linked game calls and recompiled game bytes remain zero. The pipeline generates no entry-reachable API replacements because the recovered `start` graph does not resolve its indirect dyld handoff.

The separate `compat-runtime-v1` smoke database now has one `staticEvidence` record containing the top recovered direct-call imports, exact `_main` call/register trace, and the historical host-loader first-missing-import observation. It deliberately keeps `apps` and `gamesUnblocked` empty because no on-device smoke was performed; its validator rejects any static record that claims guest execution or a smoke status. The remaining C++ priority is complete guest catch/landing-pad transfer; catalog registration and adapter behavior across the 181/73 inventories are host-tested, but do not constitute a game-unblocking result or a static NDK link.

## Strict same-name subset: 181/254 (71.26%) is the honest ceiling for this binary

The question "is the strict same-name NDK subset really implemented — and can it be
100% for Angry Birds?" has a measurable answer. Re-running the app's own classification
(catalog + precedence: same-name catalog match first, compiler-runtime candidates without
a guest adapter, then guest-runtime catalog entries, compiled compat exports, and semantic
targets) over the IPA's 254 undefined symbols reproduces the on-device strict figure:
**181 direct same-name matches = 71.26%** — libc 92, GLESv2 27, GLESv1_CM 24, libm 24,
libc++_shared 14.

The other 73 imports have no exact same-name Android system export and each has an entry
in the `compat-runtime-v1` guest-adapter catalog:

- **24 Objective-C providers**: 15 class objects, 2 metaclass objects, the two
  empty runtime data cells, and `objc_msgSend`/`objc_msgSendSuper2`/
  `objc_msgSend_stret`/`objc_setProperty`/`objc_enumerationMutation`.
- **19 OpenAL state providers**, plus the four EAGL constant-string keys.
- **4 framework/lifecycle providers**: AudioSession initialization/activation,
  Foundation search paths, and `UIApplicationMain`.
- **9 Darwin data/ctype providers**: the rune-locale page, the CoreFoundation
  constant-string class token, errno, the three stream cells, and the ASCII
  ctype functions.
- **13 ARM toolchain providers**: the three SjLj context symbols, nine integer
  compiler-runtime helpers, and `__gxx_personality_sj0`. The full C++ ABI
  adapter additionally registers exception allocation/guards/RTTI symbols when
  present.

The exact source-symbol-to-provider table is the acceptance ledger; the category
counts above are descriptive and intentionally not added to the 181 same-name
NDK count.

Audit against AOSP bionic's current symbol maps (`aosp-mirror/platform_bionic`
`libc/libc.map.txt` with its API 36 version blocks and `libm/libm.map.txt`):

- All 92 libc and 24 libm catalog matches are real exported names (116/116).
  `ldexp` is exported by **libc.so**, not libm.so; the catalog attribution was
  corrected (no metric change — the name still matches).
- `__error`, `__maskrune`, `__stderrp`/`__stdoutp`/`__stdinp`, `__tolower`,
  `__toupper` are Darwin spellings; Android exports different names (`__errno`,
  `stderr`/`stdout`/`stdin`, `tolower`/`toupper`), so a same-name link cannot exist.
- `_Unwind_SjLj_Register`/`Resume`/`Unregister`, `__moddi3` and `__fixdfdi` are
  absent from bionic entirely.
- `__divdi3`, `__udivdi3`, `__floatdidf`, `__floatdisf` **are** in bionic libc, but
  only in the `arm x86` (32-bit) map entries; they do not exist on the arm64 target
  this converter builds for, so a same-name arm64 `dlsym` check cannot resolve them
  (the hypothetical strict export denominator would be 181/185). The current mapper
  independently counts eight compiler-runtime/unwind candidates in the IPA; all
  eight also have guest-runtime adapter catalog entries. That permits runtime guest
  slot binding to an ARM32 callout where implemented, not a static compiler-rt,
  libunwind, or `libgcc_s.so` link. `runtimeLinking` must still confirm per-image
  fixups when the game is loaded.
- OpenAL, EAGL, AudioToolbox-family and UIKit/Foundation names have no Android
  provider at all — the catalog deliberately keeps them out so they stay with the
  compiled compat implementations (asserted by `tests/test_providers.py`).

Catalog upgrades possible from this audit are therefore small and general, not
Angry-Birds-specific: the `error`/`error_at_line`/`error_message_count`/
`error_one_per_line`/`error_print_progname` family and `environ` were added to the
libc set (all present in bionic's map; the `error` family is API 23+). They do not
change this binary's figure. **100% same-name for this binary is not reachable**:
71.26% is the honest same-name share, while the reviewed-mapping figure (254/254)
is the one that is 100%, because every import has exactly one reviewed target kind.

### The 73 non-same-name imports and 181 same-name imports use separate provider axes

Making these imports *work* is a separate axis from the same-name metric. The
Darwin-only spellings are bound by explicit adapters in the compat runtime: the
Darwin adapter provides ctype, real process-stream cells, errno, rune/data cells,
EAGL keys and stateful OpenAL; the Objective-C adapter provides class/metaclass
data, dispatch and lifecycle; AudioSession and Foundation have typed state/path
adapters; compiler-runtime and SjLj provide ARM32 arithmetic/context behavior;
and the C++ ABI adapter provides exception storage, guards, RTTI data and
explicit guest-exception boundaries. The same-name candidates are not called
through raw arm64 `dlsym` pointers: the NDK adapter supplies ARM32 wrappers for
math, stdio, virtual-file/POSIX I/O, locale/time, strings, deterministic random
state, guest mutex state, zlib checksums, and virtual bundle assets, while
GLES/libSystem/C++ providers retain their specialized implementations. The
broader 1,229-name NDK inventory receives the same treatment: known signatures
use typed wrappers and the remaining Android framework/driver signatures use
explicit bounded provider boundaries rather than unresolved imports. Both
catalogs are validated after registration in the host gameboot and Android JNI
paths, so an omitted provider is a setup error rather than a 100% report.

A provider is not a claim that the full Apple or Android framework is equivalent:
OpenAL is still state-only, variadic stdio formatting is literal-only, pthread
creation is a bounded scheduler boundary, and non-local C++/Objective-C
exception transfer still stops because a host callback cannot jump through guest
ARM frames. The runtime loader can bind guest import/fixup slots, as reported in
`runtimeLinking`; it does not rewrite static Android game-code callsites or link
recompiled game objects. The tracked fixture's loader and startup-chain counts
remain host/runtime evidence only; they do not establish a playable game session.

## Next three shim families to prioritize

This ordering is based on the startup trace and static import ranking; each item still needs an ABI contract, integration tests, and real loader evidence before it can be called supported.

1. **Finish the Objective-C runtime ABI family:** extend validation to categories and additional method/selector shapes, and complete guest exception/catch handling plus ARM32 SJLJ unwind for `_objc_enumerationMutation`. Retain/copy `_objc_setProperty` behavior is host-tested; a synthetic guest `-copy` IMP returns through a registered continuation under pinned Unicorn, while missing `-copy` still fails closed. Protocol lookup/conformance is host-tested against the real IPA's declarations. The loader registers Angry Birds' two app classes, superclass links, instance sizes, and ivar names, and host dispatch selects a real `MyEAGLView` IMP. `_objc_msgSend_stret` remains limited to an 8-byte `UITouch locationInView:` point.
2. **Foundation/CoreFoundation startup subset:** implement only exact classes/functions needed for bundle/resource lookup, initial object construction, constant strings, and a tested run loop toward the first frame. Guest class-data pointers alone do not provide Foundation or CoreFoundation behavior.
3. **C++ ABI/runtime allocation and unwind:** operator new/delete, compiler-rt arithmetic, C++ exception allocation/guards/RTTI and SJLJ context registration now have typed host-tested providers. Full guest catch/landing-pad transfer remains the next boundary; `libstdc++` and `libgcc_s` remain per-symbol candidates, not runtime aliases.

## Phase and verification status

- **Phase 0 — not complete.** Current checks pass: `python3 tools/build_native.py`; the Python suite runs **337 tests** and passes with the optional `capstone` package (9 tests are skipped by environment/optional-backend gates); and the native host/provider tests with the pinned Unicorn backend pass. The CMake/Gradle Android build remains unverified because Java/Javac/`JAVA_HOME` are unavailable. The CMake host build used the temporary CMake 4.4.4 wheel and a local `pkg-config` existence shim; `CONFIG_POSIX` is now set on the fetched POSIX Unicorn targets by `native/CMakeLists.txt`, so no global `CMAKE_C_FLAGS` workaround is needed. Provider-table/runtime-export parity tests pass. App/JVM compilation, Android unit tests, Android NDK/JNI linking, app behavior, and host/app parity are not established: Java/Javac/`JAVA_HOME` remain unavailable.
- **Phase 1 — static findings recorded with the startup caveat above.** Import ranking, per-ordinal evidence, and the `_main` static trace are present. Static analysis cannot determine the globally first runtime-missing import or indirect dyld/initializer call order; the current trap-mode runner observes startup until a real boundary instead of blocking on the first unresolved record.
- **Phase 2 — in progress; not complete.** `objc_shims.cpp`, `audio_session_shims.cpp`, `sjlj_unwind.cpp`, `compiler_rt_shims.cpp`, `cxxabi_shims.cpp`, `ndk_compat_shims.cpp`, `gles_shims.cpp`, `libsystem_shims.cpp`, and `darwin_compat_shims.cpp` are in the native source list; host gameboot and Android JNI register the full typed provider set. Host tests cover narrow 8-byte `UITouch locationInView:` stret handling, retain/copy `_objc_setProperty` (including host `-copy` and missing-copy failure), guest metadata selector/class/protocol introspection, selected user-domain search paths returning guest-backed `NSArray`/`NSString` objects, state-only audio-session initialize/activate, LIFO SJLJ context registration/unregistration, and the explicit fail-closed boundaries for `_objc_enumerationMutation` and `__Unwind_SjLj_Resume`. A synthetic 32-bit Mach-O exercises app-class registration, allocation, IMP transfer, and guest IMP execution under pinned Unicorn (`r0 = 42`); a separate ARM guest fixture executes a guest `-copy` IMP, returns through the registered property continuation, stores the result, and returns to its caller. Another synthetic import call verifies mutation surfaces as `GUEST_EXCEPTION_RAISED` rather than returning or masquerading as a memory fault. The pinned host tests and the dedicated C++ ABI/181-entry NDK provider test pass after the Foundation and SJLJ resume-boundary changes. Separately, the real IPA loader initializer registers `AppController : NSObject` (44-byte instance, 9 ivars) and `MyEAGLView : UIView` (72-byte instance, 9 ivars); a host harness allocates the latter, verifies selectors and UIView ancestry, and confirms `createFramebuffer` selects IMP `0x69610` without executing its body. A separate host probe verifies real `AppController` conformance to both declared UIKit protocols and inherited `NSObject`. The state-only `_AudioSessionInitialize`/`_AudioSessionSetActive` adapters resolve both imports; neither activates a device nor produces audio or interruption callbacks. The current trap-mode real-IPA probe resolves 171 records and binds 407 unsupported records to named traps; no records remain unbound. `__Unwind_SjLj_Resume` only triggers the tested stop boundary; personality/catch/landing-pad handling remains unsupported. The historical loader-only probe blocked before guest entry; the current compatibility runner reaches the guest entry and the startup chain described above, but no device frame, input delivery, or complete gameplay session is proven. `python3 tools/build_native.py` passes; the Python API implementation-database delta remains zero. The Foundation path function is marked unused in the static call graph (zero recovered direct callsites), so its newly observed import resolution is not execution evidence.
- **Phase 4 — host proof only; not complete.** The pinned Unicorn 2.1.4 ARM32 backend at commit `8028ec436f2d9376525352dd38ed9ed6b9f6be10` now builds and links on the host. A synthetic Mach-O integration fixture enters ARM code from `LC_MAIN` (`0x1400`), follows a legacy symbol stub through a loader-patched lazy pointer into the shared `ShimRegistry` callout, returns to guest code, and finishes with `r0 = 42`. The report identifies its origin as `main-executable/LC_MAIN`, guest address `0x1400`, and Unicorn backend. This is a host test, not an Android/on-device test, and it is not execution of Angry Birds.

The source IPA is deliberately excluded from Git: the local ignored copy remains under `tests/data/AngryBirds_v1.0_os30.ipa` for authorized testing. Keep generated analysis under ignored `.local/angrybirds-analysis-v11/` and loader probes under ignored `.local/angrybirds-analysis-v10/`; regenerate them after analyzer/runtime changes rather than treating them as committed binary artifacts.
