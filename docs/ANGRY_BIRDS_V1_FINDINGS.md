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

The Python-generated implementation database still covers all 254 observed imports with 48 verified implementations and 206 explicitly unimplemented stub handlers (100% resolution-target coverage, **not** implementation coverage); its implementation-database delta is **zero** in this native runtime step. The native registry has host-tested exact callouts for narrow 8-byte `UITouch locationInView:` stret dispatch, retain/copy `_objc_setProperty`, state-only `_AudioSessionInitialize` and `_AudioSessionSetActive`, `_NSSearchPathForDirectoriesInDomains`' selected virtual paths, SJLJ context registration/unregistration plus a fail-closed `__Unwind_SjLj_Resume` exception boundary, and the fail-closed `_objc_enumerationMutation` exception boundary. The current trap-mode loader resolves 171 records and binds 407 unsupported records to traps; the old 39-record/first-unresolved-import figures are retained only as historical pre-lifecycle evidence. Mutation and SJLJ resume still cannot run guest catches or personality/landing-pad logic. These native adapters are not merged into the Python implementation database and are not evidence of a complete playable IPA conversion. No IPA callsite is rewritten; linked game calls and recompiled game bytes remain zero. The pipeline generates no entry-reachable API replacements because the recovered `start` graph does not resolve its indirect dyld handoff.

The separate `compat-runtime-v1` smoke database now has one `staticEvidence` record containing the top recovered direct-call imports, exact `_main` call/register trace, and the historical host-loader first-missing-import observation. It deliberately keeps `apps` and `gamesUnblocked` empty because no on-device smoke was performed; its validator rejects any static record that claims guest execution or a smoke status. The ranked backlog's third family is now C++ ABI/allocation/unwind, based on the observed `__ZdaPv` (61), `__ZdlPv` (59), and `__Znam` (43) direct-call counts. This is a static-evidence and priority delta, not an implementation or game-unblocking delta.

## Strict same-name subset: 181/254 (71.26%) is the honest ceiling for this binary

The question "is the strict same-name NDK subset really implemented — and can it be
100% for Angry Birds?" has a measurable answer. Re-running the app's own classification
(catalog + precedence: same-name catalog match first, then compiler-runtime, then
concrete compat implementation, then semantic target) over the IPA's 254 undefined
symbols reproduces the on-device figure exactly: **181 direct same-name matches =
71.26%** — libc 92, GLESv2 27, GLESv1_CM 24, libm 24, libc++_shared 14.

The remaining 73 imports have no same-name export in any Android system library:

- **48 concrete compatibility implementations** (`libioscompat.so`, all host-tested):
  `UIApplicationMain`, `AudioSessionInitialize`/`AudioSessionSetActive`,
  `NSSearchPathForDirectoriesInDomains`, `CFConstantStringClassReference`,
  `objc_msgSend`/`objc_msgSendSuper2`/`objc_msgSend_stret`/`objc_setProperty`/
  `objc_enumerationMutation`, the OpenAL `al*`/`alc*` entry points, the EAGL
  `kEAGL*` keys, `__DefaultRuneLocale`/`__maskrune`/`__tolower`/`__toupper`,
  `__stderrp`/`__stdoutp`/`__stdinp`, `__error`, `__gxx_personality_sj0`,
  `__divsi3`/`__udivsi3`/`__modsi3`/`__umodsi3`.
- **17 Objective-C class/metaclass targets** (8 with a reviewed semantic Android
  target, 9 resolved by the compiled compat registry).
- **8 compiler-rt/libunwind toolchain symbols** (`_Unwind_SjLj_*`, `__divdi3`,
  `__fixdfdi`, `__floatdidf`, `__floatdisf`, `__moddi3`).

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
  this converter builds for, so counting them would produce candidates no current
  device can resolve (the device export check would drop to 181/185).
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

### The 73 non-same-name imports run through the translation layer, not through fake exports

Making these imports *work* is a separate axis from the same-name metric. The
Darwin-only spellings above are bound by explicit minimal adapters in the compat
runtime (`darwin_compat::ShimAdapter`, 34 bindings: ASCII ctype sweep, real
process-stream cells for `__stdinp`/`__stdoutp`/`__stderrp`, a guest errno cell,
a zeroed `__DefaultRuneLocale` page, real `NSString` EAGL keys, a zeroed
CoreFoundation class token, a state-only OpenAL subset, and an explicit
fail-closed `__gxx_personality_sj0` boundary). The Objective-C classes go through
the ObjC adapter, the compiler-rt/libunwind symbols through the compiler-runtime
shims, and the rest through the compiled `libioscompat` registry — all registered
next to the other shims in the host probe and the device JNI.

Measured effect on the tracked fixture: the loader moved from 138 resolved /
440 trapped / 0 unresolved (with 8 data imports refused — a data symbol could not
occupy an indirect pointer slot) to **171 resolved / 407 trapped / 0 unresolved**,
and the boot now continues under the device runner's unlimited execution policy
with the same ten-event startup chain (last observed: `-[UIWindow makeKeyAndVisible]`).
The old host diagnostic still uses an explicit time window so CI can return; that
window is not shipped to the APK. What changed is that the Darwin-only names now
*resolve and execute* instead of aborting on a trap; none of them was added to the
Android catalogs, and the `darwinCompat` report block states this in the artifact.

## Next three shim families to prioritize

This ordering is based on the startup trace and static import ranking; each item still needs an ABI contract, integration tests, and real loader evidence before it can be called supported.

1. **Finish the Objective-C runtime ABI family:** extend validation to categories and additional method/selector shapes, and complete guest exception/catch handling plus ARM32 SJLJ unwind for `_objc_enumerationMutation`. Retain/copy `_objc_setProperty` behavior is host-tested; a synthetic guest `-copy` IMP returns through a registered continuation under pinned Unicorn, while missing `-copy` still fails closed. Protocol lookup/conformance is host-tested against the real IPA's declarations. The loader registers Angry Birds' two app classes, superclass links, instance sizes, and ivar names, and host dispatch selects a real `MyEAGLView` IMP. `_objc_msgSend_stret` remains limited to an 8-byte `UITouch locationInView:` point.
2. **Foundation/CoreFoundation startup subset:** implement only exact classes/functions needed for bundle/resource lookup, initial object construction, constant strings, and a tested run loop toward the first frame. Guest class-data pointers alone do not provide Foundation or CoreFoundation behavior.
3. **C++ ABI/runtime allocation and unwind:** exact operator new/delete/array-delete plus the compiler-rt/libunwind imports (`__ZdaPv`, `__ZdlPv`, `__Znam`, `__Znwm`, `__Unwind_*`, arithmetic helpers). `libstdc++` and `libgcc_s` remain per-symbol candidates, not runtime aliases.

## Phase and verification status

- **Phase 0 — not complete.** Current checks pass: `python3 tools/build_native.py`; the Python suite runs **335 tests** and passes when the optional `capstone` package is available (the base image lacks it); and the CMake/CTest host build (**8/8 tests**) with the pinned Unicorn backend linked. The CMake host build used the temporary CMake 4.4.4 wheel and a local `pkg-config` existence shim; `CONFIG_POSIX` is now set on the fetched POSIX Unicorn targets by `native/CMakeLists.txt`, so no global `CMAKE_C_FLAGS` workaround is needed. Provider-table/runtime-export parity tests pass. App/JVM compilation, Android unit tests, Android NDK/JNI linking, app behavior, and host/app parity are not established: Java/Javac/`JAVA_HOME` remain unavailable.
- **Phase 1 — static findings recorded with the startup caveat above.** Import ranking, per-ordinal evidence, and the `_main` static trace are present. Static analysis cannot determine the globally first runtime-missing import or indirect dyld/initializer call order; the current trap-mode runner observes startup until a real boundary instead of blocking on the first unresolved record.
- **Phase 2 — in progress; not complete.** `objc_shims.cpp`, `audio_session_shims.cpp`, and `sjlj_unwind.cpp` are in the native source list; JNI registers all three adapters. Host tests cover narrow 8-byte `UITouch locationInView:` stret handling, retain/copy `_objc_setProperty` (including host `-copy` and missing-copy failure), guest metadata selector/class/protocol introspection, selected user-domain search paths returning guest-backed `NSArray`/`NSString` objects, state-only audio-session initialize/activate, LIFO SJLJ context registration/unregistration, and the explicit fail-closed boundaries for `_objc_enumerationMutation` and `__Unwind_SjLj_Resume`. A synthetic 32-bit Mach-O exercises app-class registration, allocation, IMP transfer, and guest IMP execution under pinned Unicorn (`r0 = 42`); a separate ARM guest fixture executes a guest `-copy` IMP, returns through the registered property continuation, stores the result, and returns to its caller. Another synthetic import call verifies mutation surfaces as `GUEST_EXCEPTION_RAISED` rather than returning or masquerading as a memory fault. The pinned host CTest suite passes **8/8 after the Foundation and SJLJ resume-boundary changes**. Separately, the real IPA loader initializer registers `AppController : NSObject` (44-byte instance, 9 ivars) and `MyEAGLView : UIView` (72-byte instance, 9 ivars); a host harness allocates the latter, verifies selectors and UIView ancestry, and confirms `createFramebuffer` selects IMP `0x69610` without executing its body. A separate host probe verifies real `AppController` conformance to both declared UIKit protocols and inherited `NSObject`. The state-only `_AudioSessionInitialize`/`_AudioSessionSetActive` adapters resolve both imports; neither activates a device nor produces audio or interruption callbacks. The current trap-mode real-IPA probe resolves 171 records and binds 407 unsupported records to named traps; no records remain unbound. `__Unwind_SjLj_Resume` only triggers the tested stop boundary; personality/catch/landing-pad handling remains unsupported. The historical loader-only probe blocked before guest entry; the current compatibility runner reaches the guest entry and the startup chain described above, but no device frame, input delivery, or complete gameplay session is proven. `python3 tools/build_native.py` passes; the Python API implementation-database delta remains zero. The Foundation path function is marked unused in the static call graph (zero recovered direct callsites), so its newly observed import resolution is not execution evidence.
- **Phase 4 — host proof only; not complete.** The pinned Unicorn 2.1.4 ARM32 backend at commit `8028ec436f2d9376525352dd38ed9ed6b9f6be10` now builds and links on the host. A synthetic Mach-O integration fixture enters ARM code from `LC_MAIN` (`0x1400`), follows a legacy symbol stub through a loader-patched lazy pointer into the shared `ShimRegistry` callout, returns to guest code, and finishes with `r0 = 42`. The report identifies its origin as `main-executable/LC_MAIN`, guest address `0x1400`, and Unicorn backend. This is a host test, not an Android/on-device test, and it is not execution of Angry Birds.

The source IPA is deliberately excluded from Git: the local ignored copy remains under `tests/data/AngryBirds_v1.0_os30.ipa` for authorized testing. Keep generated analysis under ignored `.local/angrybirds-analysis-v11/` and loader probes under ignored `.local/angrybirds-analysis-v10/`; regenerate them after analyzer/runtime changes rather than treating them as committed binary artifacts.
