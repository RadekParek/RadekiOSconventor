# game-runtime-v1: boot-attempt APK contract

A `game-runtime-v1` APK packs the selected 32-bit ARM executable and its bundle
and attempts a guest boot on-device. Its default compatibility guest-CPU path
has no instruction-count or wall-clock cutoff; it may remain alive if the guest
reaches a render loop and stops only at a real runtime boundary or setup failure.
An optional verified portable-C handoff can be imported into the APK builder; in
that case the boot activity selects the NDK-linked ARM64 library, verifies and
extracts its memory payload, and calls its JNI runner. That translated runner
currently is **not connected to Android EGL/GLES**, so no pixels, gameplay, or
playability are verified. In both paths the launcher keeps diagnostics open
rather than crashing. Neither boot-attempt path is a complete static
recompilation contract.
The host-only `radek-gameboot --diagnostic-probe` command opts into a finite
time window so CI/CLI probes can return JSON for a guest that intentionally
runs forever; that flag is never passed by the APK.

## Compatibility-path native GL, not a reimplementation

The following EGL/GLES forwarding describes the default compatibility-runtime
path only. The optional translated portable-C runner described below does not
attach an Android EGL/GLES renderer.

Guest OpenGL ES 1.1 calls are **forwarded to the platform's own EGL/GLES driver**
(`libEGL.so`/`libGLESv1_CM.so`/`libGLESv2.so` opened at runtime); nothing is
rasterized in-process. `renderbufferStorage:fromDrawable:` creates an EGL window
surface on the launcher's Android surface (or an offscreen pbuffer when no surface
was supplied), `presentRenderbuffer:` is `eglSwapBuffers`, and every guest pointer
argument is translated through the mapped guest regions with a range check. A
surface that arrives after the first offscreen attach makes the GL layer recreate
its window surface, so late surfaces still receive frames. The report's `gles`
block states which driver was loaded, whether the drawable was handed to the
platform, `guestCallsObserved` (imports entering the compat layer),
`forwardedCalls` (calls actually handed to the driver), `refusedCalls`,
`framesPresented`, and every refusal as a named diagnostic: a rendered frame is
guest output, not gameplay evidence.

### Angry Birds shader/rendering audit

The checked-in Angry Birds ARMv6 image has **51 distinct imported `_gl*` symbols**.
All 51 have explicit guest bindings in `gles_shims.cpp` (including the client-array
pointer adapters); none is a shader/program/uniform/vertex-attribute API. The
observed import surface is fixed-function OpenGL ES 1.x plus buffer, texture and
OES framebuffer calls, so the current fixture does not need GLSL shader creation
or compilation to reach its declared GL imports. The mapper's GLESv2 candidate
count alone does not mean the IPA imports shader APIs. This audit covers direct
undefined-symbol imports only: it does not prove that runtime `dlsym` or
`eglGetProcAddress` lookups never occur, nor that an Android device displayed a
non-black frame.

During the unlimited device run, a small renderer-status strip reports whether
EGL/GLES drivers and a drawable are ready, guest GL calls/refusals, and successful
EGL swaps. It remains visible over a blank viewport and never stops or time-limits
the guest. An EGL swap is only evidence that the platform accepted a present call;
it does not inspect pixel content or prove gameplay. If the screen remains black,
the live counts help separate a missing surface/driver, no guest GL call, a refused
call, and a swap with unverified image content; final diagnostics are persisted in
the runtime report and app-specific log.

## Optional translated portable-C boot path

The whole-game ARM lifter can emit portable C and, when the Android NDK is
available, link it as `libtranslated_game.so`. A successful host link is counted
only after verifying ARM64 ELF class/machine, Android `DT_NEEDED` dependencies,
every translated-function export, the sized `GameBootActivity` JNI entry, and
translation/link function and byte counts. The host then writes a separate,
hash-bound `translated-game-runtime-input.zip` containing the native library,
`rt_mem.bin` payload, and reports. It is an APK-builder input—not an APK or
proof of runtime execution.

On-device, the game-runtime detail screen can import that ZIP. During the APK
build the selected ARM executable hash is matched to the handoff, its manifest
and file hashes are checked, the ARM64 ELF and dependency policy are checked
again, translated-function/export and JNI-entry evidence is required, and the
nested memory-image hash is verified. Only then are
`lib/arm64-v8a/libtranslated_game.so` and
`assets/translated-game-payload.zip` added to the signed game-runtime APK. The
launcher loads that library, extracts and rechecks `rt_mem.bin`, then invokes
`Java_dev_radek_gameruntime_GameBootActivity_runTranslatedGame`. The runtime
maps guest `/` and `/tmp` into dedicated writable app-private storage, serves
bundle paths from the extracted read-only bundle, and exposes `/Android/obb`
only as an optional read-only app-specific mount. There is no frame-based
execution timeout. The report distinguishes host link, APK packaging/linkage, and
runtime execution; a library in an APK is not evidence that the JNI runner
booted. This path is not connected to EGL/GLES, and pixels/gameplay remain
unverified until separately tested. The Android source/tests still require an
Android SDK/NDK build and device validation before runtime success can be
claimed.

## Behavior contract

The numbered details below describe the default compatibility guest-CPU path.
The optional translated portable-C variant follows the separate selection,
payload-verification, no-EGL and reporting contract described above; it still
retains the original authorized executable and bundle in the APK.

1. The APK embeds exactly one authorized 32-bit ARM Mach-O slice
   (`assets/gameboot/main-executable.bin`), the bundle resources
   (`assets/bundle/**`), boot metadata (`assets/gameboot.json`), and the
   tested `libcompat_runtime_v1.so` guest-CPU runtime.
2. The launcher (`dev.radek.gameruntime.GameBootActivity`) runs the boot once
   through `Java_dev_radek_gameruntime_GameBootActivity_runGameBootAttempt`,
   passing the app directory it extracts from `assets/bundle/**`. The runtime
   mounts that directory as the guest's own bundle (read-only), maps guest `/`
   to the dedicated writable internal app-private `files/game-data` root, and
   gives `/Documents`, `/Library` and `/tmp` more-specific writable mounts.
   The `/tmp` directory is created under that root before boot. When Android provides an
   app-specific OBB directory it is an optional read-only `/Android/obb` mount;
   the packaged bundle itself does not require an expansion OBB. Longest-prefix
   resolution preserves the bundle/OBB boundaries and refused accesses are
   listed in `guestFileSystem` instead of being invented.
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
   -> single-guest-CPU service of queued Objective-C background work and
   guest-callable `pthread_create` worker transfers).
4. The launcher is **fullscreen** (`SYSTEM_UI_FLAG_IMMERSIVE_STICKY` plus
   layout through the display cutout) and runs in **sensor landscape** while the
   guest boots: up to three recovered splash frames are shown **once each** for
   ~0.9 s, then the splash is hidden and the game `SurfaceView` is revealed before
   guest execution starts. The sequence never cycles and touches do not advance
   it. The surface is handed to the runtime (`setGameSurface` → `ANativeWindow` →
   EGL window surface) and the guest's
   `renderbufferStorage:fromDrawable:`/`presentRenderbuffer:` pairs become
   `eglCreateWindowSurface`/`eglSwapBuffers` on the platform GLES driver. A
   small renderer-status strip remains visible over the viewport while the
   diagnostic panel stays hidden; the panel is revealed, after the launcher
   switches back to **portrait**, when the attempt stops.
5. The launcher shows loader/trap/instruction progress in the terminal panel and
   live EGL/GLES state in the viewport strip. While the unlimited device guest is
   running, the panel is hidden behind the game viewport. When guest execution stops or setup fails, the launcher keeps the
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
  left or right); the recovered splash frames are shown fullscreen, each one
  exactly once, and the sequence stays on the last frame instead of cycling.
  The guest's own EGL frames take over after the splash, while a small live
  renderer-status strip reports swaps/refusals without claiming visible pixels
  or gameplay. The larger diagnostics panel stays hidden while the guest runs.
- When the attempt stops for any reason (unimplemented import, guest exception,
  fault, unavailable backend, or setup failure), the launcher switches back to
  **portrait** and reveals the diagnostic log, so the stop reason is readable
  without touching anything. The activity declares `configChanges` for
  orientation so rotating the device never restarts the guest.

## Angry Birds v1.0 status (tracked fixture)

`tests/data/AngryBirds_v1.0_os30.ipa` (thin ARMv6 Mach-O, 1,822,112 bytes,
267 bundle files):

- Loader: `LOADED_WITH_TRAPS`, 171 resolved / 407 trapped / 0 unresolved. The
  native loader has begun binding ARM32 guest import/fixup slots to registered
  guest providers or explicit traps. `runtimeLinking` reports those per-image
  slot results; this is runtime guest binding, not static Android callsite
  rewriting or a linked native game object.
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

## Non-same-name ARM32 guest-adapter catalog (`darwin_compat`)

Android has no same-name system export for the Apple-spelled Darwin imports
that motivate this layer (for example `__tolower`, `___error`, `__stdoutp`,
`__DefaultRuneLocale`, `kEAGLColorFormatRGB565`, `_gxx_personality_sj0`, and
OpenAL's `_alc*`/`_al*`). The 75-entry provider inventory also includes ARM32
compiler-runtime/unwind names and two Foundation path functions; 73 of its names
match the Angry Birds fixture. These entries stay outside the strict same-name
Android catalog and identify guest-runtime adapters/data providers instead; they
are not static NDK exports or proof of complete semantics. The source/provider
inventory is shared by Python, Kotlin, and C++; the 181 observed same-name NDK
imports also receive ARM32 guest-provider bindings (specialized GLES/libSystem/C++
providers plus bounded libc/POSIX/math/stdio/pthread/zlib/asset wrappers). The
full reviewed NDK candidate inventory contains 1,229 names and is registered
with typed adapters or explicit bounded provider boundaries, so an unimplemented
candidate is visible as a named boundary rather than an unresolved import. When
complete-game proving stops on the Angry Birds metadata/API gates, the host
pipeline still emits the whole-game ARM translation under `bytecode-translation/`
(`game_all.c`, generated dispatch tables, and `rt_mem.bin`) instead of stopping
before translation. This is portable-C host output only; it is not yet an
Android-linked game library or APK. The Darwin data/ctype/OpenAL subset lives in `native/src/compat_runtime/darwin_compat_shims.cpp`
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
- **`_gxx_personality_sj0`**: an explicit fail-closed boundary when the
  personality itself is called as an unresolved import. The generated host
  runtime nevertheless materializes the parsed ARM SjLj/LSDA call-site and
  action tables, records bounded guest registration contexts, and can transfer
  a proven catch-all or exact type match through the saved guest dispatch label.
  An uncaught throw still stops at `cxa_throw` rather than being reported as a
  successful game run.
- **C++ ABI support** (`native/src/compat_runtime/cxxabi_shims.cpp`): bounded
  exception allocation/free, `__cxa_atexit`, guard variables, guest RTTI/vtable
  data bindings, and the compiler-runtime/SjLj registration boundary. The
  adapter never jumps a host exception through an arbitrary guest ARM stack;
  LSDA selection and landing-pad transfer use generated guest-state metadata,
  with fail-closed behavior for unsupported typed matches.

The Darwin adapter is counted (`boundSymbols`, `ctypeCalls`, `openalCalls`,
`streamCells`, `personalityBoundaries`) and pinned by
`native/tests/darwin_compat.cpp` (34 bindings). The complete 75-entry provider
inventory is separately pinned by `native/tests/compat_runtime_cxxabi.cpp` and the
provider-parity tests; 73 inventory names are observed in the Angry Birds fixture.
The host probe and on-device JNI register the adapters
next to the other shims and report both `darwinCompat` and `importProviders`
blocks.

One loader capability was required for these data imports: a 32-bit Mach-O
indirect symbol-pointer slot may hold the materialized guest **address of a data
import**, not only a callout thunk address (`macho_loader.cpp`). Before this, a
data import that arrived through a pointer slot (instead of an external
relocation) was refused as unresolved.

### Same-name NDK subset after this layer

The translation layer changes what *runs*, not what *same-name* means: the
strict subset stays **181/254 = 71.26%**. The other **73/254** are non-same-name
`compat-runtime-v1` guest-adapter catalog entries (Darwin data/framework/OpenAL
adapters plus ARM32 compiler-runtime and unwind boundaries), not direct NDK
exports. The combined **254/254** reviewed-provider figure is catalog coverage,
not proof that every image's slots were fixed up or every API's semantics are
implemented. The runtime report's `runtimeLinking` block records actual per-image
bind/relocation results; those are guest-address slot fixups, not static Android
callsite rewriting. Provider addresses are guest ARM32 addresses, not raw arm64
system pointers, and none of these counts claims a playable game.
