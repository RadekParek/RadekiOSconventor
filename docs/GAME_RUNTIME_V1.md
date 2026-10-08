# game-runtime-v1: boot-attempt APK contract

A `game-runtime-v1` APK packs a real iOS game executable and its bundle and
runs the actual guest boot on-device. The Android runner does **not** impose an
instruction-count or wall-clock cutoff: once the guest enters its own render
loop it is allowed to stay alive for gameplay. Execution stops only at a real
runtime boundary (an unimplemented import, guest exception, memory/execution
fault, or unavailable backend) or when setup fails, and the launcher remains
open with the exact reason rather than crashing.
It is still not a complete static recompilation contract: framework/input
coverage must be sufficient for the particular game before gameplay can be
claimed. The runtime does not show a preview shell in place of the guest.
The host-only `radek-gameboot --diagnostic-probe` command opts into a finite
time window so CI/CLI probes can return JSON for a guest that intentionally
runs forever; that flag is never passed by the APK.

## Native GL, not a reimplementation

Guest OpenGL ES 1.1 calls are **forwarded to the platform's own EGL/GLES driver**
(`libEGL.so`/`libGLESv1_CM.so`/`libGLESv2.so` opened at runtime); nothing is
rasterized in-process. `renderbufferStorage:fromDrawable:` creates an EGL window
surface on the launcher's Android surface (or an offscreen pbuffer when no surface
was supplied), `presentRenderbuffer:` is `eglSwapBuffers`, and every guest pointer
argument is translated through the mapped guest regions with a range check. A
surface that arrives after the first offscreen attach makes the GL layer recreate
its window surface, so late surfaces still receive frames. The report's `gles`
block states which driver was loaded, whether the drawable was handed to the
platform, `forwardedCalls`/`refusedCalls`/`framesPresented`, and every refusal as
a named diagnostic: a rendered frame is guest output, not gameplay evidence.

## Behavior contract

1. The APK embeds exactly one authorized 32-bit ARM Mach-O slice
   (`assets/gameboot/main-executable.bin`), the bundle resources
   (`assets/bundle/**`), boot metadata (`assets/gameboot.json`), and the
   tested `libcompat_runtime_v1.so` guest-CPU runtime.
2. The launcher (`dev.radek.gameruntime.GameBootActivity`) runs the boot once
   through `Java_dev_radek_gameruntime_GameBootActivity_runGameBootAttempt`,
   passing the app directory it extracts from `assets/bundle/**`. The runtime
   mounts that directory as the guest's own bundle (read-only) plus writable
   `/Documents` and `/Library` scratch directories, so the guest reads its real
   data files; refused accesses are listed in the report's `guestFileSystem`
   block instead of being invented.
3. Unimplemented imports are bound to abort-on-call traps. The guest executes
   real instructions from the Mach-O entry point without an artificial
   instruction/time budget. It continues while implemented adapters and the
   guest's own render loop are active, and stops only when it calls (or touches
   data of) a documented runtime boundary. Implemented adapters run for real
   instead of trapping: the native OpenGL ES 1.1 forwarding (below), libSystem
   memory/string/malloc and the file/stdio/math/time shims served by the virtual
   file system, the ARM EABI compiler-runtime helpers, the bounded Objective-C
   runtime, the AudioToolbox session state calls, and the bounded
   application-lifecycle chain
   (`UIApplicationMain` -> delegate instantiation -> `applicationDidFinishLaunching:`
   -> single-guest-CPU service of the queued background-thread body).
4. The launcher is **fullscreen** (`SYSTEM_UI_FLAG_IMMERSIVE_STICKY` plus
   layout through the display cutout) and runs in **sensor landscape** while the
   guest boots, showing only the game: the recovered splash frames are shown
   **once each** (~0.9 s apart) and the sequence then stays on the last frame —
   it never cycles and touches never advance it. A `SurfaceView` above the
   splash receives the guest's frames: its surface is handed to the runtime
   (`setGameSurface` → `ANativeWindow` → EGL window surface) and the guest's
   `renderbufferStorage:fromDrawable:`/`presentRenderbuffer:` pairs become
   `eglCreateWindowSurface`/`eglSwapBuffers` on the platform GLES driver, so a
   frame the guest renders covers the boot screen. The diagnostic panel stays
   hidden while the guest runs and is revealed, after the launcher switches back
   to **portrait**, when the attempt stops.
5. The launcher shows loader/trap/instruction progress in that panel. While the
   unlimited device guest is running, the panel is hidden behind the black game
   viewport. When guest execution stops or setup fails, the launcher keeps the
   fullscreen diagnostic screen open; it does not throw an Android crash or show
   a preview. The stop reason is reported as what it is: a named unimplemented
   import trap, an explicitly bounded host/legacy `TIME_LIMIT` or
   `INSTRUCTION_LIMIT` report (not used by the APK), a guest exception, a memory
   or execution fault, or an unavailable CPU backend. A JSON `null` trap name is
   never printed as an import called `null`.
6. Every report keeps `status: "not_runnable"`. Executed instructions are
   loader/CPU progress, never evidence of a working game.

## Artifact names

| Item | Value |
|---|---|
| APK file | `<SanitizedIpaStem>-game.apk` |
| Package | `dev.radek.gameruntime.p<source-sha256[0:20]><cert-sha256[0:8]>` |
| Launcher | `dev.radek.gameruntime.GameBootActivity` |
| Report key (device) | `gameRuntimeConversion` |
| Manifest (host) | `game-runtime-manifest.json` |

The host (`radek/gameruntime.py`) and the device (`ArtifactNames`,
`GameRuntimeArtifactContract`, `GameRuntimeApkBuilder`) implement the same
naming and package rules; both sides pin them with tests.

## Host tooling

```
python3 -m radek gameboot input.ipa --authorized --output job-dir
```

- Extracts the IPA with the standard archive bounds, selects the boot slice
  (thin ARM file, or the first 32-bit ARM slice of a FAT image), and probes it
  with the `radek-gameboot` host binary (built by CMake with pinned Unicorn).
- Writes `game-runtime-manifest.json` (contract inputs + `hostProbe` summary),
  `gameboot-report.json` (full native report), and `main-executable.bin` (the
  exact staged slice the device builder packs).
- The probe never fails the command: without a built `radek-gameboot` binary
  the manifest records `hostProbe.status: "NOT_PROBED"` with a reason.

## On-device builder

`GameRuntimeApkBuilder` mirrors the bounded converter's packaging mechanics
(template patch, streamed entries, 16 KiB native alignment, v1+v2+v3 signing,
signature/package/label/install audits) with game-runtime inputs:

- Template `gameruntime-template` (package/label sentinels shared with the
  other templates; DEX must define the boot launcher class).
- Executable gate: thin ARM Mach-O, or FAT with a 32-bit ARM slice; anything
  else throws and nothing is built.
- Runtime gate: `libcompat_runtime_v1.so` and its required shared backend
  `libunicorn.so` are copied from the installed converter APK (including ABI
  splits) by `CompatibilityRuntime.extractGameRuntimeInstalled`. The converter
  build explicitly packages both CMake targets into its APK. The game APK
  includes both aligned libraries beside one another, so Android's linker can
  resolve the runtime's `DT_NEEDED` Unicorn dependency. Packaging fails closed
  if either required library is absent; `libc++_shared.so` is also copied when
  the installed build includes it.
- The report records `contract: "game-runtime-v1"`,
  `bootAttemptIncluded: true`, `completeGameConversion: false`,
  `gamePlayable: false`, `gameCodeRecompiled: false`, and the executable,
  runtime, signing, and install-audit evidence.

`ResultProvider` serves `*-game.apk` files only after
`GameRuntimeArtifactContract` re-validates name, package, source hash, signer,
and digest against the report.

## Launcher presentation

- The launcher runs fullscreen in **sensor landscape** (the device can be turned
  left or right) and shows **only the game**: the recovered splash frames are
  shown fullscreen, each one exactly once, and the sequence stays on the last
  frame instead of cycling; the guest's own EGL frames take over as soon as the
  guest renders. The diagnostics panel stays hidden while the guest runs.
- When the attempt stops for any reason (unimplemented import, guest exception,
  fault, unavailable backend, or setup failure), the launcher switches back to
  **portrait** and reveals the diagnostic log, so the stop reason is readable
  without touching anything. The activity declares `configChanges` for
  orientation so rotating the device never restarts the guest.

## Angry Birds v1.0 status (tracked fixture)

`tests/data/AngryBirds_v1.0_os30.ipa` (thin ARMv6 Mach-O, 1,822,112 bytes,
267 bundle files):

- Loader: `LOADED_WITH_TRAPS`, 171 resolved / 407 trapped / 0 unresolved. The
  Darwin-only imports that Android has no same-name export for are now bound
  through the translation layer described below (stream cells, ctype sweep,
  EAGL keys, errno cell, rune locale, CoreFoundation class token, OpenAL).
- Boot: the previous host diagnostic run reached **2,000,000 guest
  instructions** only because it used the old bounded probe policy. The device
  runner now uses zero for both limits (unlimited) and therefore does not stop
  the Angry Birds guest merely because an instruction counter expired. It
  enters `_main`, performs the `NSAutoreleasePool +new` setup, enters
  `UIApplicationMain`, instantiates the image's own `AppController` delegate,
  wires it into the `UIApplication` singleton, delivers
  `applicationDidFinishLaunching:` to the real guest implementation, and keeps
  running inside the app: it builds its UIKit window/EAGL view
  (`-[UIView layer]` -> `CAEAGLLayer`, `numberWithBool:`,
  `dictionaryWithObjectsAndKeys:`, `EAGLContext initWithAPI:` /
  `setCurrentContext:`, `addSubview:`, `makeKeyAndVisible`), starts its engine
  render setup (the GLES calls are forwarded to the host driver), and asks for
  its own bundle data through the guest filesystem.
- Stop: on the device, only a named unimplemented import, guest exception, memory
  or execution fault, unavailable backend, or setup failure ends the attempt.
  The host `--diagnostic-probe` flag may still return `TIME_LIMIT` for CI and
  reports that as a host-only diagnostic, never as an APK gameplay cutoff.
- Report: `lifecycle.applicationMainEntered: true`,
  `delegateClassName: "AppController"`, startup-chain events, and the guest
  filesystem's refusals are named when no bundle mount is
  configured.
- The VFP unit is enabled for the guest (`CPACR` CP10/CP11 access and
  `FPEXC.EN`), because the ARMv6 image uses scalar VFP from its first delegate
  frame on; without it the attempt stopped on a decode fault at `vpush`.
- The host suite (`tests/test_gameruntime.py`) and CI pin the *shape* of this
  behavior (entry point reached, a documented host-probe boundary — a named
  trapped import, guest/backend fault, or explicit diagnostic timeout — and a
  non-empty startup chain); the manifest from the CI run is uploaded as
  `angrybirds-gameboot-artifacts`. The APK/JNI path is separately asserted to
  leave all execution budgets at zero.
- Remaining honest gap: on the host the OpenGL ES calls have no driver to
  forward to, so nothing is rendered there; on Android the same calls are
  forwarded to the platform GLES/EGL driver (see the GL forwarding section).

## Darwin-only translation layer (`darwin_compat`)

Android ships no system library that exports the Apple-spelled names the old
Mach-O images import (`__tolower`, `___error`, `__stdoutp`, `__DefaultRuneLocale`,
`kEAGLColorFormatRGB565`, `_gxx_personality_sj0`, OpenAL's `_alc*`/`_al*`, ...).
Those imports are **not** added to the Android catalogs — inventing same-name
NDK exports would be lying about the platform. Instead they get explicit minimal
adapters in `native/src/compat_runtime/darwin_compat_shims.cpp`
(`darwin_compat::ShimAdapter`, callout window `0xf0050000`–`0xf0080000`):

- **ctype sweep** (`__tolower`, `__toupper`, `__maskrune`): ASCII/C-locale
  behavior; bytes above `0x7f` are returned unchanged (no locale tables are
  reproduced).
- **stream cells** (`__stdinp`, `__stdoutp`, `__stderrp`): guest cells holding
  real process-stream handles served by the compat filesystem; guest `fclose`
  on them is a recorded no-op and never closes the host stream.
- **errno cell** (`__error`): a single guest cell (per-thread errno is not
  reproduced — stated in the reported diagnostics).
- **rune locale** (`__DefaultRuneLocale`): a zeroed guest page; the table layout
  is not reproduced and `__maskrune` does not read it.
- **EAGL keys** (`kEAGLColorFormatRGB565`/`RGBA8`,
  `kEAGLDrawablePropertyColorFormat`/`RetainedBacking`): real
  `NSString` constant objects created through the Objective-C adapter.
- **CoreFoundation token** (`__CFConstantStringClassReference`): a zeroed class
  token; CoreFoundation string classes are not implemented.
- **OpenAL** (14 `_al*` + 5 `_alc*` entries): state-only bookkeeping (generated
  buffer/source ids, and per-source int/float/queue state round-tripped through
  `_alGetSourcei`/`_alGetSourcef`). No audio is produced and the report says so.
- **`_gxx_personality_sj0`**: an explicit fail-closed boundary — calling it
  raises the guest exception path instead of pretending to unwind; the C++
  exception runtime stays trapped.

Each adapter is counted (`boundSymbols`, `ctypeCalls`, `openalCalls`,
`streamCells`, `personalityBoundaries`) and the set is pinned by
`native/tests/darwin_compat.cpp` (34 bindings) so a removal fails the suite. The
host probe and the on-device JNI register the adapter next to the other shims and
report a `darwinCompat` block.

One loader capability was required for these data imports: a 32-bit Mach-O
indirect symbol-pointer slot may hold the materialized guest **address of a data
import**, not only a callout thunk address (`macho_loader.cpp`). Before this, a
data import that arrived through a pointer slot (instead of an external
relocation) was refused as unresolved.

### Same-name NDK subset after this layer

The translation layer changes what *runs*, not what *same-name* means: the
honest same-name subset for this image stays **181/254 = 71.26%**, because these
imports are served by compat implementations, not by same-name NDK exports. The
reviewed-mapping figure (254/254) already counts every one of them.
