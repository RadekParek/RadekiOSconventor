# Changelog

All notable changes to RadekiOSConventor. Every entry states what was verified and
what was **not**: analysis, triage, name mappings and host static-recompilation
coverage never mean a linked game, a playable conversion or a device-tested APK.

## 2026-10-08 — Whole-game ARM translation emitted before APK gates

The Angry Birds pipeline no longer stops at the complete-game leaf prover before
emitting translated code. After the metadata/API gate correctly blocks complete
APK conversion, it now runs the fail-closed ARM lifter across the decoded game
functions and writes `bytecode-translation/game_all.c`, generated dispatch and
shim tables, and the non-zero-fill memory image. The current fixture emits 2,837
functions with zero lift failures and 100% of the measured executable text-byte
coverage. These are host portable-C translation artifacts, not a linked Android
library or playable APK.

## 2026-10-08 — Complete reviewed NDK inventory and bounded family adapters

The native runtime now registers the complete 1,229-name reviewed Bionic/NDK
inventory in addition to the exact 181-name Angry Birds fixture catalog and the
separate 73-name Darwin compatibility catalog. Existing typed bindings take
precedence; newly covered families include guest `environ`/`errno` cells,
scalar libm forwarding, zlib Adler/CRC/version/bounds, virtual-bundle AAsset
open/read/seek/close and file-length operations, bounded pthread mutex state,
and named fail-closed boundaries for remaining Android framework, driver,
dynamic-loader, and ABI signatures. Reports expose the full inventory count,
typed-versus-generic provider split, and generic-call diagnostics without
relabeling Darwin-only names as Android exports.

The native registration, full-catalog parity, host Python suite, and sanitized
native suite pass. This is provider/loader evidence only: no Android SDK/Java
build, APK install, device smoke test, first frame, menu, or playable Angry
Birds session is claimed here.

## 2026-10-08 — Unlimited device guest loop and true-black generated launchers

### The device game path no longer expires at 2,000,000 instructions

The Android `game-runtime-v1` path now treats zero instruction and zero wall-clock
limits as **unlimited**. The Unicorn backend no longer carries hidden 100-million/
60-second ceilings, and the lifecycle adapter no longer cancels the guest after
an eight-frame service window. A game loop can remain alive for rendering and
input instead of being turned into a misleading `INSTRUCTION_LIMIT` diagnostic.
The host `radek-gameboot` probe passes an explicit `--diagnostic-probe` flag so CI
can still produce finite JSON for an infinite guest; that opt-in policy is not
used by the APK/JNI entry.

### Generated launchers use black, not blue-black, presentation chrome

Both the statically converted launcher template and the forced game-runtime
launcher now set the window, viewport, letterbox, surface, and diagnostic panel
backgrounds to true black. This keeps the recovered game splash and EGL output
from being framed by the former dark-blue bars.

### The 71% same-name figure is explained at the point of use

The detail card now says that `181/254 = 71.26%` is an exact public-NDK-name
measure, not total Android triage. The other 73 Angry Birds imports require
reviewed compatibility implementations, semantic framework targets, or
compiler-runtime/unwind handling; those are deliberately separate categories.
The reviewed mapping measure can therefore be 254/254 without pretending that
all 254 names are direct Android exports.

## 2026-10-08 — Darwin-only translation layer + game-only launcher presentation

### Darwin-only imports now have translation-layer adapters

Android ships no same-name export for the Apple-spelled imports of the tracked
fixture (`__tolower`/`__toupper`/`__maskrune`, `___error`, `__stdinp`/`__stdoutp`/
`__stderrp`, `__DefaultRuneLocale`, the four `kEAGL*` keys,
`__CFConstantStringClassReference`, `_gxx_personality_sj0`, 19 OpenAL entries).
They are now bound by `darwin_compat::ShimAdapter` (34 bindings, callout window
`0xf0050000`–`0xf0080000`), registered next to the other shims in the host probe
and the device JNI, and reported in a `darwinCompat` block with per-adapter
counters. Pinned by `native/tests/darwin_compat.cpp`. None of them is added to
the Android catalogs: the **same-name NDK subset stays 181/254 = 71.26%** for
this image, and the layer's report says so explicitly.

Required loader capability: a 32-bit indirect symbol-pointer slot now accepts the
materialized guest address of a data import, not only a callout thunk address.
Measured effect on the fixture: loader `LOADED_WITH_TRAPS`, **171 resolved /
407 trapped / 0 unresolved** (was 138 resolved / 440 trapped / 0 unresolved, with
8 data imports refused); the boot still runs to the bounded 2,000,000-instruction
budget with the same 10-event startup chain.

### Game-only launcher presentation

`GameBootActivity` now creates in `SENSOR_LANDSCAPE`, hides the diagnostics
overlay while the game is displayed, puts a fullscreen `SurfaceView`
(`setZOrderOnTop(true)`) above the splash, and publishes the surface to the
runtime through the new JNI `setGameSurface(Surface)` ->
`gles::setDefaultNativeWindow` path (the GL layer recreates its EGL window
surface when the drawable arrives after the guest started). On a terminal state
the activity switches to `SCREEN_ORIENTATION_PORTRAIT`, hides the surface and
shows the overlay with the log. Splash frames are shown once each and the last
one is held; the earlier tap-to-cycle behavior is gone. Pinned by the
Robolectric tests in the app module.

## 2026-10-07 — Angry Birds v1.0 feedback round

Input under test: `tests/data/AngryBirds_v1.0_os30.ipa` (ARMv6, 254 imports) and the
screenshots of the on-device importer, the detail cards and the game-runtime boot screen.

### Symbol triage can now report 100% honestly (was stuck at 71%)

**Problem.** The detail card showed `direct NDK name candidates: 71% (181/254)`, which
looked like a bug next to the 100% triage figure. It was not: 181 imports are same-name
public NDK/system or shared C++ runtime exports, and the other 73 need different handling
(48 concrete `libioscompat.so` implementations, 17 Objective-C class targets, 8
compiler-rt/libunwind toolchain symbols). The single "direct NDK" denominator hid that
every import *was* already mapped, so the number could never reach 100%.

**Change.** Import triage now reports two separate, non-interchangeable figures:

- **Reviewed Android mappings: 100% (254/254)** — every observed import has exactly one
  reviewed mapping kind: same-name NDK/system export, concrete compiled compat
  implementation, reviewed semantic API target, or NDK compiler-rt/libunwind toolchain
  symbol. The per-kind counts are printed next to it
  (`AndroidApiMapper.reviewedMapping`, `reviewedMappingCount`,
  `reviewedMappingCoveragePercent`, `breakdown`, `kindCountsAreNotInterchangeable`).
- **Same-name NDK subset: 71% (181/254)** — the strict, deliberately smaller count that
  was previously the only headline; it keeps its own field (`mappedNameCandidates`,
  `candidateCoveragePercent`) and its own wording.

Neither number claims a rewritten callsite, a linked implementation or generated code;
`callsiteRewritten`, `linkedIntoGame` and `recompiledBytesLinked` are still false/0, and
the caveat text now states the distinction explicitly. Shown on the library list, the
detail card and the conversion log; pinned by a new
`AndroidApiMapperTest.reviewedAndroidMappingCoverageCountsEveryMappingKindButNeverImplementation`
test (75% mapping coverage with one explicit stub handler left out).

### Android code-byte static recompilation moves from 0%

**Problem.** `Android code-byte static recompilation progress: 0%` was uninformative:
the bounded prover only accepts an executable that is *exactly one* closed-integer
routine, so every real game reported zero, and the app never said why.

**Change (host CLI).** New bounded pass `radek/plan.py` runs the same fail-closed lifter
the differential test proves (`radek.game.lift`) over the selected 32-bit ARM slice and
records `staticRecompilationPlan` in `report.json`: discovered/recompiled/not-recompiled
functions, statically recompiled bytes, executable `__text` bytes, the metric, the time
budget, and an explicit limitation list. `portProgress` then reports the real number with
`status: PARTIAL_HOST_STATIC_RECOMPILATION`, `hostPlanOnly: true`,
`completeGameConversion: false` and a basis sentence that says it is source bytes only.

Measured on the tracked Angry Birds IPA: **2,837 / 2,838 discovered functions lifted
(1,343,056 bytes of a 1,243,432-byte `__text`, 100% after clamping), 0 functions
refused, ~20 s.** `conversionProgress` stays `NOT_BUILT` / `0%` — no APK is assembled
from those sources and nothing is linked, exactly as before. Without the optional
`capstone` package the plan reports `UNAVAILABLE` and every other report field is
unchanged; the proven-subset path never runs the plan at all.

**Change (device).** The on-device prover has no lifter, so the app keeps reporting its
own output-only figure; the port card and the basis text now explain the 0% and point at
the host plan, and the library card shows `host plan: N% of __text (not linked)` when a
host report is attached.

New tests: `tests/test_plan.py` (unreadable input, 64-bit out of scope, real Angry Birds
run, plan never completes a conversion, proven subset never reports a plan).

### Game-runtime boot APK: fullscreen, self-advancing splash, truthful stop reasons

**Problem.** The boot screen rendered the recovered splash in a small card and asked the
user to tap the viewport to cycle frames (`Frame 3/4 (tap viewport to cycle)`) — on top of
a boot attempt that reported a confusing `Stopped at unimplemented import: null` line even
though execution had actually ended at the bounded time budget (`TIME_LIMIT`, 338,936
instructions).

**Change (`gameruntime-template`).**

- **Fullscreen** (`SYSTEM_UI_FLAG_IMMERSIVE_STICKY`, transparent bars, layout through the
  display cutout, re-applied on window focus). The splash covers the whole display; the
  boot log sits in a translucent, scrollable, selectable panel at the bottom.
- **No touch cycling.** The tap listener is gone; the splash advances by itself (~0.9 s
  per recovered frame) while the guest boots and stops on a stable frame when the boot
  ends. The caption says `advancing automatically` / `boot finished` instead of asking for
  a tap.
- **Truthful stop reasons.** A JSON `null` trap name is never printed as an import called
  `null`; `TIME_LIMIT`/`INSTRUCTION_LIMIT` are reported as a budget stop with the executed
  instruction count and the sentence "No unimplemented import was reached during this
  window", separately from named traps, memory/execution faults, guest exceptions and an
  unavailable CPU backend. The diagnostic screen still stays open (fullscreen) on every
  stop — it never crashes and never shows a preview.

Tests: two new Robolectric cases (`budgetStopIsReportedAsABudgetAndNeverAsAnImportNamedNull`,
`splashNeverAsksForTouchCycling`) plus the existing metadata/blocked-path tests.

### Back gesture navigates instead of closing the converter

The converter app tracks the visible screen (`LIBRARY` / `DETAIL` / `SETTINGS`).
Back gesture/button now returns from a detail page or Settings to the Game Library and only
closes the app from the library itself.

### Build-time optimizations (no behavior changes)

- CI caches the pinned Android SDK components (platform 35, build-tools 35.0.0,
  NDK 27.2.12479018, CMake 3.22.1) keyed by their exact versions, so repeat runs skip the
  multi-gigabyte NDK download.
- CI uses `ccache` for the host CMake/Unicorn build and the Gradle NDK build
  (`RADEK_USE_CCACHE=1`; both Gradle modules pass the launcher arguments only when that
  variable is set, so environments without ccache behave exactly as before) and caches it
  between runs.
- `gradle.properties` enables `org.gradle.parallel` and `org.gradle.caching`.
- CI installs the optional `capstone` package (soft dependency for the host plan pass).
- No task outputs, artifacts, assertions or validation steps were removed; the workflow
  step order and every existing check are preserved.

### Documentation

README, `docs/SUPPORT.md` and `docs/GAME_RUNTIME_V1.md` now describe reviewed-mapping
coverage vs. the same-name subset, the host static-recompilation plan (and that it is host
source bytes only), and the fullscreen/automatic splash with the per-status stop wording.

## 2026-10-08 — Game-runtime progression round (native GLES forwarding, deeper boot)

Input under test: `tests/data/AngryBirds_v1.0_os30.ipa`.

### Same-name subset audited against AOSP bionic: 181/254 is the honest ceiling

The strict same-name NDK figure was re-derived from the IPA with the app's own catalog
and precedence rules and reproduces the on-device value exactly: **181/254 = 71.26%**
(libc 92, GLESv2 27, GLESv1_CM 24, libm 24, libc++_shared 14). The remaining 73 imports
were audited one by one against AOSP bionic's current `libc.map.txt`/`libm.map.txt`:
all 116 libc+libm matches are real exports, the Darwin-only spellings
(`__error`, `__maskrune`, `__stderrp`/`__stdoutp`/`__stdinp`, `__tolower`, `__toupper`),
the SJLJ unwind set and `__moddi3`/`__fixdfdi` have no Android export at all, and the
compiler-rt builtins that do exist (`__divdi3`, `__udivdi3`, `__floatdidf`,
`__floatdisf`) are 32-bit-`arm`/`x86`-only entries that the arm64 target device cannot
resolve. Two general catalog corrections came out of the audit: `ldexp` is exported by
**libc.so** (not libm.so), and the `error`/`error_at_line`/`error_message_count`/
`error_one_per_line`/`error_print_progname` family plus `environ` were missing and are
now listed. The Angry Birds-specific number is unchanged, and 100% same-name for this
binary stays unreachable; 100% applies to the reviewed-mapping figure (254/254).

### The launcher shows only the game (and the log when the attempt stops)

The game-runtime launcher now runs fullscreen in **sensor landscape** and shows **only the
game**: the recovered splash frames are displayed once each and the sequence then stays on
the last frame — the previous tap-to-cycle behaviour and the modulo wrap are gone, and the
diagnostics panel starts hidden while the guest runs on screen. The `SurfaceView`'s
surface is handed to the runtime through `setGameSurface` (JNI → `ANativeWindow` →
`eglCreateWindowSurface`), and a surface that arrives after the first offscreen attach makes
the GL layer recreate its EGL window surface so the guest's frames reach the display. When
the attempt stops for any reason, the launcher switches back to **portrait** with the
diagnostic log visible; `configChanges` keeps a device rotation from restarting the guest.

### The device path actually reaches the driver and the guest's own files

The Android launcher now extracts the embedded bundle payload (`assets/bundle/**`)
to its files directory and passes the app directory to the runtime, which mounts it
as the guest's bundle plus writable `/Documents` and `/Library` scratch
directories — the guest's own data reads are served from the APK instead of being
refused. The JNI entry also registers the native GLES/EGL forwarding (it was only
registered in the host probe before, so on-device GL calls never reached the
driver) and reports the `gles` block (driver, drawable state, forwarded/refused
calls, frames presented) and `guestFileSystem` block alongside the boot report.

### The sanitizer job builds the current runtime, not an older subset

`tools/test_sanitized.sh` compiled only the pre-existing compat-runtime translation units,
so the AddressSanitizer/UndefinedBehaviorSanitizer job failed on the new GLES, compiler-rt
and virtual-filesystem sources. The three source lists now match the CMake/build-script
lists; the sanitized suites pass again.

### OpenGL ES goes to the platform driver — no reimplementation

The guest's fixed-function OpenGL ES 1.1 imports no longer stop the attempt. They are
forwarded to the real driver: `libGLESv1_CM`/`libGLESv2` and `libEGL` are opened at
runtime, the EAGL drawable is an EGL surface (`renderbufferStorage:fromDrawable:` →
`eglCreateWindowSurface` — a window surface when the Android glue supplies the
`ANativeWindow` of its `Surface`, an offscreen pbuffer otherwise), `presentRenderbuffer:`
is `eglSwapBuffers`, and every guest pointer argument is translated through the mapped
guest regions with a range check. There is **no CPU rasterizer in the tree**: on a host
without GL libraries the driver probe fails, each call is refused with a named diagnostic,
and the report says exactly that instead of faking a frame.

### Guest execution is ~70x faster, and the boot runs 6x deeper

Profiling showed 19.8 s of the 20 s budget going into a full guest-memory copy after every
shim callout. The backend now syncs mappings structurally and uploads only the pages a shim
actually wrote (page-granular dirty tracking in `GuestAddressSpace`), which took Angry
Birds from ~18k to ~1.3M guest instructions/second. Together with a realistic 8 MiB guest
stack (64 KiB overflowed into unrelated runtime pages) and bounded accesses that may span
contiguous guest regions, the host boot attempt now executes its full 2,000,000-instruction
budget — 340,309 before this round — and stops with `INSTRUCTION_LIMIT`, with **no** trap
and no runtime fault.

### The guest can read its own bundle

New `VirtualFileSystem` plus `fopen`/`fclose`/`fread`/`fwrite`/`fseek`/`ftell`/`feof`/
`ferror`/`fflush`/`fgets`/`remove`, `gettimeofday`/`time` and scalar math
(`sin`/`cos`/`pow`/`sqrt`/`floorf`/`ceilf`/`fabs`) shims. Mounts are explicit — the bundle
payload read-only, `Documents`/`Library` writable — and a path outside every mount is
refused with a named diagnostic rather than inventing file contents. The host probe takes
the extracted `.app` directory as an optional second argument.

### Compiler-runtime helpers implemented for real

`__divsi3`/`__modsi3`/`__udivsi3`/`__umodsi3`, `__divdi3`/`__moddi3`,
`__floatdidf`/`__floatdisf`/`__fixdfdi` follow the ARM EABI instead of aborting the boot;
a general `__Unwind_SjLj_Register` only requires the word it actually links (it previously
demanded 32 writable bytes and stopped the attempt at the stack top).

### Verified / not verified

- Verified: host probe on the tracked Angry Birds fixture reaches the entry point, executes
  the full bounded budget, and stops at a documented boundary (this round: the instruction
  limit); `loaderStatus` `LOADED_WITH_TRAPS` with 0 unresolved; the CI check now accepts a
  named trap **or** a bounded execution limit and still refuses anything else.
- Not verified: no device run. No rendered frame, menu, gameplay or playable conversion is
  claimed — the game-runtime APK remains a bounded boot attempt (`game-runtime-v1`), and
  `complete-game-v1` still requires the whole executable to be inside the proven subset.
