# RadekiOSconventor — 100% NDK / black-screen / OBB plan

**Status: IMPLEMENTED. See the implementation log below for what changed, how it
was verified, and what remains.**

Everything below was derived by reading the repository and by parsing
`tests/data/AngryBirds_v1.0_os30.ipa` directly (254 undefined-external symbols
extracted from its LC_SYMTAB).

---

## Implementation log (what was actually changed)

Verification legend: **[T]** = compiled and covered by a passing automated test
on this machine; **[R]** = code-reviewed line-by-line but only compilable/runnable
in an Android/JVM environment that is not present in this sandbox.

### A. Black-screen root cause — the ARM register bug **[T]**
`native/src/compat_runtime/app_lifecycle.cpp`,
`-[EAGLContext renderbufferStorage:fromDrawable:]`. The drawable was read from
`r[2]`, but AAPCS puts the `target` enum (`GL_RENDERBUFFER_OES`) there and the
`CAEAGLLayer*` in `r[3]`. `objectForGuest(r[2])` therefore looked up `0x8D41`
as an object address, found nothing, and silently returned success with no EGL
surface attached — exactly "drawable not ready, GL calls forwarded, refused 0,
no frame verified". Now `r[3]` is read first (with `r[2]` as a tolerant
fallback), the host viewport is used when the layer never materialized a frame,
and the method returns real failure (`0`) instead of masked success.
`presentRenderbuffer:` likewise reports `0` when nothing was presented.

### B. The frame pump (CFRunLoop) **[T]**
`objc_shims.hpp/.cpp` + `app_lifecycle.cpp`. `UIApplicationMain` no longer dead-ends
after `applicationDidFinishLaunching:` returns. `CFRunLoopRun`/`CFRunLoopRunInMode`
are registered as *transfer* callouts that service one ready source per
continuation and re-enter the guest, so a repeating 1/60 s `NSTimer` keeps
drawing frames instead of `main()` returning. Added `+[NSTimer
scheduledTimerWithTimeInterval:…]`, `performSelector:withObject:afterDelay:`,
`-[NSTimer invalidate]`, `cancelPreviousPerformRequestsWithTarget:`, and
`CFRunLoopStop`. Covered by new assertions in
`native/tests/compat_runtime_lifecycle.cpp` (loop transfers, repeated re-entry,
invalidate → clean exit to the caller). Full suite: **13/13 passing**.

### C. Host viewport + orientation **[T]**
The hardcoded 320×480 portrait default is now a *fallback only*. A configured
host viewport (the real Android surface size) feeds `-[UIScreen bounds]`,
`-applicationFrame`, and the EAGL drawable, so a landscape game gets its real
rectangle. `-applicationFrame` is served through the `objc_msgSend_stret` path
(it previously threw). The launcher publishes the surface size via a new
`setViewportSize` JNI entry, re-published after the runtime loads **[R]**.

### D. Touch input **[T for native queue / R for Java wiring]**
Host touches are queued into the guest run loop and delivered to the deepest
subview of the key window covering the point. `addSubview:` now records
children and inherits the parent frame for full-screen views. New
`postTouchEvent` JNI entry + `OnTouchListener` on the game surface **[R]**.

### E. OBB + package id named after the game **[R]**
`ArtifactNames.kt`: `androidPackageName` (bundle id → Android package id, with a
hash fallback when the id is invalid), `versionCodeOf` (CFBundleVersion →
monotonic int), `obbFileName` → `main.<versionCode>.<package>.obb`.
`GameRuntimeApkBuilder.kt` now derives the package from the IPA's
`CFBundleIdentifier` (was a source+cert hash), writes a real stored-ZIP
expansion OBB named after the bundle id, keeps the bundle embedded for small
games (fallback, no black screen), and records `expansionObb` + `versionCode` in
both `gameboot.json` and the report. `GameRuntimeArtifactContract` validates the
new package rule. Verified by Python simulation of the exact Kotlin logic
(backward-compatible with the existing hash-id test fixture).

### F. Honest triage / the unconditional BLOCKED gate **[R]**
* `Library.kt`: "has imports" no longer sets `incompatible`; only undecodable
  bindings, chained fixups, and un-decodable images do. Evidence is collected in
  `incompatibleReasons` and the final BLOCKED message names it instead of the
  old catch-all that contradicted a 100% triage line.
* `jni.cpp` `findImplementedApiReplacement`: auto-registered stubs are no longer
  reported as "implementation exports available" (they are not implementations).
* `AndroidApiMapper.kt` + `Library.kt`: `linkedImplementationCount` /
  `linkedApiReplacements` now carry the device-verified count (was hardcoded 0),
  and the `apiImplementationGeneration` status/message reflect it.

### G. OpenAL → real audio: software mixer + AAudio output **[T for mixer / R for AAudio]**
* New files: `native/include/compat_runtime/openal_backend.hpp`,
  `native/src/compat_runtime/openal_backend.cpp` — `openal::Engine`, a
  platform-free software mixer: `alBufferData` now copies the guest's PCM into
  host memory (8-bit unsigned / 16-bit signed decode, mono→stereo duplication,
  linear resample to 44100 Hz, per-buffer normalization cache keyed by a
  generation counter so re-uploaded buffers re-decode). Sources keep a queue,
  cursor, and per-buffer position; `play`/`stop`/`queue`/`unqueue`/`AL_LOOPING`/
  `AL_GAIN`/`AL_BUFFERS_PROCESSED`/`AL_SOURCE_STATE` all delegate to the engine.
  Looping buffers auto-replay and never count as processed (they cannot be
  unqueued); deleted/missing buffers are skipped instead of wedging playback.
* Device sink: on Android (`#ifdef __ANDROID__`) `openDevice()` builds an AAudio
  output stream (44100 Hz, stereo, PCM_I16, LOW_LATENCY) whose data callback
  calls `render()` and clamps the float mix to int16; `compat_runtime_v1` now
  links `aaudio`. Off-device `openDevice()` returns false and `render()` is
  still drivable directly, which is how the tests exercise the mixer.
* `darwin_compat_shims.cpp`: the 19 OpenAL handlers are no longer state-only.
  `alcOpenDevice`/`alcMakeContextCurrent` open the output stream (idempotent),
  `alSourcePlay/Stop` start/stop mixing, `alGetSourcei` answers
  `AL_BUFFERS_PROCESSED` and `AL_SOURCE_STATE` from real playback position.
* New test `native/tests/openal_backend.cpp` (9 checks): audible stereo output
  from captured PCM, gain-zero silence, stop→silence, processed/unqueue
  bookkeeping, looping continuity, stereo channel separation, 8-bit decode,
  half-rate resample stretching, missing-buffer skip. Suite is now **14/14**.
* Angry Birds boot probe re-verified after the rewiring: loader LOADED,
  runtime linking COMPLETE, **254/254 imports resolved, 0 unresolved, 0
  trapped**, 578 fixup slots relinked (`/tmp/ab_boot.json`).

### H. Chained fixups relinker — the modern dyld format **[T]**
* `macho_loader.cpp`: `LC_DYLD_CHAINED_FIXUPS` no longer throws. A new
  `applyChainedFixups()` decodes the payload exactly per
  `include/mach-o/fixup-chains.h` (cross-checked against Apple's dyld sources):
  header, the three import-table formats (`DYLD_CHAINED_IMPORT`,
  `+ADDEND`, `+ADDEND64`), uncompressed symbol strings, per-segment starts with
  the `DYLD_CHAINED_PTR_32` (format **3**) pointer layout — including
  `DYLD_CHAINED_PTR_START_MULTI` pages where the secondary chain starts live in
  the same `page_start[]` array at absolute indices and terminate with
  `DYLD_CHAINED_PTR_START_LAST`. Bind nodes route through the same `bindAt()`
  as the classic opcode stream (shim registry, trap writer, load report all see
  them identically, tagged `chained-bind`); rebase nodes install
  `target + slide` like `applyRebases()`. Unsupported pointer formats,
  compressed symbol pools, and malformed tables fail closed with explicit
  errors. The `runtimeLinking` mechanism now lists chained fixups.
* `macho.cpp` analyzer: chained payloads get `pointerTraversal:
  implemented-ptr32` and `decodable: true` when every segment uses PTR_32 with
  a supported page size and uncompressed symbols.
* `Library.kt` gate: only *undecodable* chained fixups block at analysis time
  (matches the loader's real capability); decodable ones proceed to the boot
  attempt. The static-recompile backend (`radek/analysis.py`, `ceiling.py`,
  `gamepack.py`) still rejects chained fixups — it only emits a proven entry
  routine and genuinely cannot reconstruct them.
* New test `native/tests/macho_chained_fixups.cpp` (5 checks): synthetic
  chained Mach-O binds two imports through the registry, traps them when
  unimplemented, fails closed without traps, rejects non-PTR_32 formats, and
  applies the slide to rebases. Suite is now **15/15**.
* Angry Birds re-verified after the change: 254/254 imports, 0 unresolved,
  0 trapped (it predates chained fixups; this unblocks modern-Xcode titles).

### I. CI repairs + build speed **[T for native / R for workflow]**
* `tools/build_native.py`: the portable build's hardcoded source list now
  includes `openal_backend.cpp` and builds/runs the OpenAL and chained-fixups
  test binaries — this was the link failure that broke GitHub CI.
* `AndroidApiMapperTest`: the pinned `linkedImplementationCount == 0`
  assertion is updated to the documented contract (device-verified
  `libioscompat.so` exports count as linked implementations at the aggregate
  level; per-symbol `linkedOrRewritten` stays false because no IPA callsite is
  rewritten). The other two `== 0` assertions hold (they pass no replacement
  resolver).
* Workflow speed: the pinned Dynarmic checkout is fetched once into a shared
  directory (`.local/dynarmic-fetch`) reused by both the host CMake step and
  the Gradle NDK build through `FETCHCONTENT_SOURCE_DIR_DYNARMIC`, cached by
  actions/cache keyed on the pinned git SHA; the host build now runs at full
  runner parallelism
  (`--parallel $(nproc)` instead of 2).

### J. "NEXT BLOCKER" — one red line per import **[R]**
* `Library.kt` computes `report.nextBlocker`: the single actionable thing
  between the IPA and a fully converted APK (DRM / no ARM32 slice / the exact
  loader reason / packaging failure / "launch the game-runtime APK and read
  the NDK needs log for the first unimplemented symbol").
* `MainActivity.kt` renders it bold red (`NEXT BLOCKER: …`) at the top of the
  Compatibility report card. Cleared when a conversion reaches READY.

### What is NOT yet done / not verifiable here
* On-device confirmation that Angry Birds now presents frames (needs hardware).
* On-device audio confirmation — the AAudio sink path compiles only with the NDK
  (not present in this sandbox); the mixer itself is unit-tested host-side.
* AudioToolbox session → AAudio stream mapping beyond the existing no-error
  lifecycle handlers.
* Bioshock's 180 stub-handler bodies (needs its symbol list / IPA).
* Compiler-rt/sjlLj bodies beyond the existing adapters.
* The Dynarmic CPU backend needs the pinned fetch plus the vendored Boost
  header subset (`native/third_party/boost-headers`); host smoke tests link it
  directly and CI uses the pinned checkout.
* Android/JVM compilation of the `.kt`/`.java` edits (no JVM in sandbox) — they
  are marked **[R]** and were reviewed, not compiled.

---

## 0. About the screenshots

The three PNGs did **not** reach me — the upload directory does not exist in my
sandbox and I have no vision on them, so I could not inspect the pixels. What you
pasted as text was enough to work from, and I cross-checked every number in it
against the code that emits it.

Two consequences of that:

* Everything in §1 is verified against source, not guessed.
* If the screenshots contain anything **beyond** the text you pasted (a stack
  trace, a `logcat`, an on-screen report panel, a diagnostics list), paste that
  text too — §3.1 in particular has one diagnostic string I want you to confirm.

---

## 1. Deep analysis of the three screenshots

### 1.1 The Angry Birds binary, measured directly

`tests/data/AngryBirds_v1.0_os30.ipa` → `Payload/AngryBirds.app/AngryBirds`

| Property | Value |
|---|---|
| Mach-O | `0xfeedface`, CPU type 12 (ARM), subtype 6 → **armv6, 32-bit** |
| File type | 2 (MH_EXECUTE), 23 load commands |
| Size | 1,822,112 bytes |
| Bundle id | `com.clickgamer.AngryBirds` |
| Min OS | 3.0 · Orientation `UIInterfaceOrientationLandscapeRight` |
| Built | 2009-12-30 |

Linked images — **11**, and three of them are the interesting ones:

```
Foundation  UIKit  OpenGLES  QuartzCore  CoreGraphics
OpenAL      AudioToolbox
libstdc++.6  libgcc_s.1  libSystem.B  libobjc.A  CoreFoundation
```

Its 254 undefined-external symbols, grouped:

| Domain | Count | Note |
|---|---:|---|
| libSystem / C | 130 | incl. `pthread_*`, `setjmp/longjmp`, `objc_msgSend*` |
| OpenGL ES 1.1 | 51 | `glDrawArrays`, `glDrawElements`, `*OES` FBO/RBO set |
| C++ / compiler-rt | 33 | `___cxa_*`, `___divdi3`, `___udivsi3`, `___gxx_personality_sj0` |
| **OpenAL** | **19** | `alcOpenDevice`, `alSourceQueueBuffers`, … |
| ObjC classes/ivars | 17 | `CAEAGLLayer`, `EAGLContext`, `UIApplication`, `UIScreen`, … |
| AudioToolbox | 2 | `AudioSessionInitialize`, `AudioSessionSetActive` |
| Foundation | 1 | `NSSearchPathForDirectoriesInDomains` |
| UIKit | 1 | `UIApplicationMain` |

Two facts from this table drive everything else:

1. **Angry Birds 1.0.0 imports no `CADisplayLink`.** It animates from its main
   run loop / thread. That matters enormously in §3.
2. **Audio is OpenAL + AudioToolbox, and both are currently no-ops by design.**
   That is your "not even audio" — it is not a regression, it was never wired.

### 1.2 Screenshot 1 + 2 — Angry Birds: `100% (254/254)`

Read literally, this is already "100%". Read properly, it says something much
less flattering. Three things are wrong with the number.

**(a) `181` is not a capability — it is the size of a hardcoded table.**

```
native/include/compat_runtime/ndk_import_catalog.hpp        -> 181 entries
native/include/compat_runtime/compat_import_catalog.hpp     ->  75 entries
```

Angry Birds' `181 strict same-name NDK candidates` is *exactly* 181/181 of the
hand-maintained `ndk_import_catalog.hpp`, and its `73` guest-adapter matches are
73 of the 75 in `compat_import_catalog.hpp`. The catalogues were fitted to this
one binary. They are not a general iOS→NDK mapping, which is precisely why
Bioshock — a bigger, newer binary — only reaches 48% on the same axis.

**(b) `254 libioscompat.so implementation exports are available but not linked`
is inflated by the stub auto-registrar.**

In `Library.kt:454-457`, the resolver for *every* symbol does this first:

```kotlin
resolveCompatHandler = { symbol ->
    NativeBridge.compatRegisterStub(symbol)   // <-- registers a stub for EVERY symbol
    NativeBridge.compatClassify(symbol)
}
```

`compatRegisterStub` (`jni.cpp:277`) calls `radek_compat::registerStub(name)`,
which **creates** an export in `libioscompat.so`. `findImplementedApiReplacement`
(`jni.cpp:152`) then `dlopen`s `libioscompat.so`, `dlsym`s the symbol, and — if
`dladdr` says the symbol lives in `libioscompat.so` — counts it as an
"implemented API replacement available". So the pipeline mints a stub and then
cites the stub as evidence the API is implemented. That is a circular metric.

**(c) The "not linked" half is a literal constant.**

`AndroidApiMapper.kt:1648`

```kotlin
.put("linkedImplementationCount", 0)
```

Hardcoded zero. There is no code path that could ever make it non-zero. So
`COMPAT_EXPORTS_AVAILABLE_NOT_LINKED` is not a status the pipeline arrived at —
it is a status the pipeline is built to always report.

### 1.3 Why Angry Birds is `BLOCKED` at 100% triage — the real gate

This is the most important finding in the whole analysis.

`Library.kt:417-419`

```kotlin
val metadata = slice.optJSONArray("metadata") ?: JSONArray()
if (imports.length() > 0 || metadata.length() > 0 || slice.has("chainedFixups") || !slice.optBoolean("bindDecodingComplete", true)) {
    incompatible = true
}
```

**Any image with any import is unconditionally `incompatible`.** Then at
`Library.kt:586`:

```kotlin
val terminalState = if (encrypted || incompatible || !hasCandidate) ConversionState.BLOCKED else ...
```

So a Mach-O with 254 imports like Angry Birds — or 691 like Bioshock — can
*never* leave `BLOCKED`, no matter how complete the triage is. The triage
percentages and the state machine are not connected. That is why the log reads
"reviewed Android mappings cover 254/254 (100%)" and then, one line later,
`BLOCKED`.

The `BLOCKED` message itself is also self-contradicting:

> "…require unsupported compatibility/linker implementations. API triage found
> 254 imported symbols; **0 have only explicitly unimplemented compat
> handlers.**"

Zero unimplemented handlers, yet blocked. The message is assembled from the
generic `incompatible` branch, not from actual evidence.

Two more unconditional `incompatible = true` sites worth knowing:
`Library.kt:379` (a Mach-O slice that could not be decoded) and `Library.kt:442`
(any embedded framework/dylib at all — the loop sets it even when the embedded
image *was* decoded successfully, because the assignment sits after the
`if/else`).

### 1.4 Screenshot 3 — Bioshock: where the 44% actually is

Bioshock: 691 imports, 384/691 catalogued providers (56%), 511/691 reviewed (74%).

The arithmetic closes exactly:

```
691 - 511 = 180  ==  "compat stub handlers (unimplemented): 180"
```

So Bioshock's entire gap is **180 stub handlers**. It is not 180 *symbols that
cannot be mapped* (`unmapped: 0`); it is 180 symbols that are recognised and then
served by an explicitly-unimplemented stub. This is the single highest-leverage
number in the project: implementing those 180 bodies is what moves Bioshock from
56% toward 100%, and — because `ndk_import_catalog.hpp` is a fixed 181-entry
table while Bioshock needs 333 — the fix must be *generated*, not hand-written.

Secondary gaps:

* `333` strict-NDK candidates vs. a 181-entry catalogue. The surplus comes from
  `kReviewedNdkFallbacks` in `jni.cpp` plus the bionic table in
  `AndroidApiMapper` — a second hand-maintained table with the same ceiling
  problem.
* `14` compiler-rt candidates, only `7` have guest adapters → 7 uncatalogued
  (`___divdi3`-class helpers). Angry Birds has the same class of problem with 8,
  but all 8 happen to be covered.
* `6` semantic rewrites — the only axis that implies real behavioural work.

---

## 2. Plan: getting the NDK/compat layer to a genuine 100%

Guiding principle: **stop reporting coverage, start producing linkage.** Every
percentage today is a classification metric. The plan replaces classification
with artefacts — a generated symbol inventory, real implementation bodies, and a
real ELF link — and only *then* reports a number.

### Phase 1 — Unbreak the gate (small, unblocks everything else)

| # | Change | File | Why |
|---|---|---|---|
| 1.1 | Delete the `imports.length() > 0` disjunct from the `incompatible` test | `Library.kt:417` | It makes `BLOCKED` unconditional. Presence of imports is the normal case, never a blocker. |
| 1.2 | Replace `incompatible` with a per-reason enum (`ENCRYPTED`, `UNDECODED_IMAGE`, `UNRESOLVED_BIND`, `UNIMPLEMENTED_SYMBOL`, `OK`) | `Library.kt` | So the terminal state is derived from evidence, with each reason carrying its own count. |
| 1.3 | Move the `incompatible = true` at `Library.kt:442` inside the `else` branch | `Library.kt:440-442` | Currently fires even for embedded images that decoded fine. |
| 1.4 | Make the `BLOCKED` string print the actual reasons, not a catch-all | `Library.kt:581` | Kills the "0 unimplemented handlers … but blocked" contradiction. |

### Phase 2 — Replace the two hand-maintained tables with a generated inventory

This is what makes 100% reachable for Bioshock and for games you have never seen.

| # | Change | Detail |
|---|---|---|
| 2.1 | Add a build step that enumerates **every** export of the real device NDK/system libs (`libc`, `libm`, `libdl`, `libEGL`, `libGLESv1_CM`, `libGLESv2`, `libOpenSLES`, `libaaudio`, `libandroid`, `liblog`, `libz`, `libc++_shared`, …) by walking the linker namespace at runtime | Replaces the 181-entry `ndk_import_catalog.hpp` and the `kReviewedNdkFallbacks` table. Generated at first run, cached, versioned by API level. |
| 2.2 | Demote `ndk_import_catalog.hpp` to a *fallback* used only when the device probe is unavailable | Keeps host tests deterministic. |
| 2.3 | Generate `compat_import_catalog.hpp` from the *actual* registration sites in `darwin_compat_shims.cpp` / `objc_shims.cpp` / `gles_shims.cpp` instead of maintaining 75 rows by hand | Drift between catalogue and implementation becomes impossible. |

### Phase 3 — Implement the missing bodies (the real work)

Ordered by what Angry Birds needs first, then Bioshock's 180.

| # | Area | Work | Evidence |
|---|---|---|---|
| 3.1 | **OpenAL → AAudio/OpenSL ES** | Real device, real contexts, real PCM. Replace the state-only shims at `darwin_compat_shims.cpp:275-460`. 19 AB symbols: `alcOpenDevice/CreateContext/MakeContextCurrent/CloseDevice/DestroyContext`, `alGenBuffers/alBufferData/alGenSources/alSourceQueueBuffers/alSourcePlay/…` | `alSourcePlay` today: *"recorded and ignored: this runtime produces no audio"* |
| 3.2 | **AudioToolbox session** | `AudioSessionInitialize` / `AudioSessionSetActive` → AAudio stream + `AAudioStream_requestStart/Stop`, plus ducking/interruption policy | 2 AB symbols |
| 3.3 | **Run loop + frame pump** | See §3.3. Precondition for *any* pixels. | No `CFRunLoop`, no Choreographer, no DisplayLink wiring |
| 3.4 | **Touch input** | Deliver `touchesBegan/Moved/Ended` into the guest from `SurfaceView` `onTouchEvent` | Not implemented; AB is unplayable without it |
| 3.5 | **compiler-rt helpers** | Real implementations for `___divdi3`, `___moddi3`, `___udivsi3`, `___umodsi3`, `___fixdfdi`, `___floatdidf`, `___floatdisf` and friends; Bioshock needs 7 more | 8 AB / 14 Bioshock, 7 uncatalogued |
| 3.6 | **SjLj unwinding** | `__Unwind_SjLj_Register/Resume/Unregister` — currently boundary stubs; `___gxx_personality_sj0` needs a real personality routine | 3 AB symbols |
| 3.7 | **Bioshock's 180 stubs** | Extract the exact symbol list from the report, bucket by framework, implement per bucket | The whole 691→100% gap |
| 3.8 | **Semantic rewrites** | Bioshock's 6 — per-signature work, handled case by case | 6 symbols |

### Phase 4 — Make the numbers honest

| # | Change | File |
|---|---|---|
| 4.1 | Stop calling `compatRegisterStub(symbol)` for every symbol during analysis | `Library.kt:456` |
| 4.2 | Count auto-registered stubs as **unimplemented**, never as "implementation exports available" | `jni.cpp:152` `findImplementedApiReplacement` |
| 4.3 | Replace `.put("linkedImplementationCount", 0)` with the real count of symbols bound into the produced ELF | `AndroidApiMapper.kt:1648` |
| 4.4 | Add a `linkedImplementationPercent` axis and make it the headline number; demote "reviewed mapping coverage" to secondary | `AndroidApiMapper.kt` |
| 4.5 | Keep the existing honesty disclaimers — they are good — but attach them to the *triage* axis only | `AndroidApiMapper.kt:1592` |

**Definition of done for "100%":** `linkedImplementationCount == distinctImportSymbols`,
produced by a real ELF that really resolves those symbols, *and* the guest boots
to a verified presented frame. Not a classification percentage.

---

## 3. Plan: Angry Birds black screen

### 3.1 Root cause A — wrong ARM argument register (highest confidence)

`app_lifecycle.cpp:1191-1218`, `-[EAGLContext renderbufferStorage:fromDrawable:]`:

```cpp
if (auto *drawable = objectForGuest(memory, registers.r[2]);   // <-- WRONG SLOT
    drawable != nullptr && drawable->ivars.size() >= 4) {
```

AAPCS for `objc_msgSend(self, _cmd, ...)` is `r0 = self`, `r1 = _cmd`,
`r2 = arg0`, `r3 = arg1`. The selector is
`renderbufferStorage:(NSUInteger)target fromDrawable:(id)drawable`, so:

| Register | Should hold | Code assumes |
|---|---|---|
| `r2` | `target` = `GL_RENDERBUFFER_OES` = `0x8D41` = **36161** | the drawable ← wrong |
| `r3` | `drawable` = the `CAEAGLLayer *` | (ignored) |

So `objectForGuest(memory, 36161)` returns `nullptr`, `width`/`height` stay 0, and
the code takes the early-out:

```cpp
recordLifecycleEvent(lifecycle, "-[EAGLContext renderbufferStorage:fromDrawable:] -> drawable size unavailable");
return finish(1);          // <-- returns SUCCESS
```

**Consequences, which match your symptom exactly:**

* `attachDrawable()` is never called → `egl.surface == nullptr` →
  `drawableReady = false` → your UI prints *"EGL/GLES loaded, drawable not ready"*.
* No EGL context is ever made current, so the 51 `gl*` imports the guest calls
  are happily forwarded into the driver as no-ops: **`forwardedCalls` increments,
  `refusedCalls` stays 0.** That is your `GL calls 10 · driver calls 10 · refused 0`.
* `finish(1)` tells the guest "storage succeeded", so it proceeds to
  `presentRenderbuffer:` → `presentDrawable()` → *"no drawable storage"* →
  `framesPresented` stays 0 → *"no frame verified"*.

**Fix:** read `registers.r[3]`. Return `NO` (0) on failure and record a real
diagnostic instead of `finish(1)`.

> **Please confirm:** open the on-device report/diagnostics and search for
> `drawable size unavailable`. If it is there, root cause A is confirmed and the
> fix is a one-line change plus a behaviour change.

### 3.2 Root cause B — no run loop, so there is no second frame

Even with A fixed, Angry Birds will show at most one frame and then freeze,
because **`UIApplicationMain` is currently a one-shot**.

`app_lifecycle.cpp:280-338` wires the delegate, pushes
`kFrameDidFinishLaunching`, and jumps into `applicationDidFinishLaunching:`.
When that returns, `lifecycleContinuation` (`app_lifecycle.cpp:258-265`) pops the
frame, does:

```cpp
lifecycle.applicationMainReturned = true;
registers.r[14] = frame.callerReturnAddress;   // back into main()
```

`main()` returns → `runner.cpp:411`'s single `cpu_.executeGuestFunction(...)`
returns `Returned` → boot finished. There is:

* no `CFRunLoop` (zero hits for `RunLoop`/`runLoop` in `app_lifecycle.cpp`),
* no `CADisplayLink` wiring — `cad_display_link_compat.cpp` exists but is **not
  referenced anywhere in the guest runtime**,
* no `Choreographer` (zero hits in `native/src/compat_runtime/`),
* no re-entry into the guest after the entry function returns.

**And Angry Birds 1.0.0 has no `CADisplayLink` import** — it drives its loop from
the main run loop and its own threads (`_NSThread`, `_pthread_create`). So the
pump must be a run loop, not a display link.

**Fix:** add a host-driven pump.

1. Register a nested callout (same machinery as `lifecycleContinuation`) that
   re-enters the guest's frame callback each vsync.
2. Drive it from Android's `Choreographer` on the render thread, calling into
   JNI — `gameruntime_jni.cpp` already has the surface plumbing
   (`ANativeWindow_fromSurface` at line 49).
3. Keep the Dynarmic JIT alive between frames (guest mapped memory, register
   state, guest heap) instead of tearing down after `Returned`.
4. Add `CFRunLoopRun`/`CFRunLoopRunInMode`/`NSTimer` scheduling so a guest that
   blocks in its own run loop keeps being serviced.

### 3.3 Root cause C — audio is absent by design

`darwin_compat_shims.cpp:287`:

> "OpenAL entry points are state-only: no audio device is opened and no samples
> are produced"

`alSourcePlay` (line 331) is *"recorded and ignored"*; `alcOpenDevice` (line 437)
returns *"a state-only device token; no audio backend is opened"*.

Angry Birds imports 19 OpenAL symbols + 2 AudioToolbox symbols. **No audio today
is expected behaviour, not a bug.** Covered by §3.1/§3.2 of Phase 3 above.

### 3.4 Secondary issues found while tracing

| # | Issue | Location | Impact |
|---|---|---|---|
| C.1 | `kVirtualViewWidth = 320`, `kVirtualViewHeight = 480` — **portrait** default | `objc_shims.cpp:23-24` | Angry Birds is `UIInterfaceOrientationLandscapeRight` (480×320). Wrong default orientation for the layer frame. Should come from the real surface size. |
| C.2 | `-applicationFrame` **throws** ("requires `objc_msgSend_stret` dispatch") | `app_lifecycle.cpp:1113` | Common in 2009-era `applicationDidFinishLaunching:`. Hard failure. |
| C.3 | `-bounds`/`-frame` via non-stret dispatch **throws** | `app_lifecycle.cpp:566` | Same family of failure. |
| C.4 | `presentRenderbuffer:` returns `1` even when nothing was presented | `app_lifecycle.cpp:1189` | Masks failure, same as A. |
| C.5 | `-[UIScreen bounds]` has no UIScreen branch — falls through to generic | `app_lifecycle.cpp:1095-1130` | Only served via the `stret` path. |
| C.6 | `_UIAccelerometer` delegate is recorded but never fed | `app_lifecycle.cpp:1128+` | Accelerometer-dependent games stall. |
| C.7 | No touch delivery path at all | — | Even with pixels, unplayable. |

### 3.5 Suggested order of work for the black screen

1. **Fix `r2` → `r3`** in `renderbufferStorage:fromDrawable:` and stop returning
   success on failure. Re-test. This alone may produce a first frame.
2. **Confirm** via the `drawable size unavailable` diagnostic (§3.1 note).
3. **Add the frame pump** (§3.2) — required for anything beyond one frame.
4. **Fix orientation** to 480×320 from the real surface (C.1).
5. **Implement `-applicationFrame`** via the stret path (C.2).
6. **Wire touch input** (C.7).
7. **Implement OpenAL → AAudio** (§3 Phase 3.1) for audio.

---

## 4. Plan: OBB naming from the game's bundle id

### 4.1 What happens today

There is **no OBB file at all** right now.

* The bundle is embedded **inside the APK** as `assets/bundle/<relative>`
  (`GameRuntimeApkBuilder.kt:324`).
* `getObbDir()` is only created as an **empty, optional, read-only** mount at
  guest path `/Android/obb` (`GameBootActivity.java:321-355`,
  `GameRuntimeApkBuilder.kt:260-263`).
* The UI even says so: *"(created; optional read-only mount; no expansion OBB is
  required for this packaged bundle)"*.
* The APK package id is a **hash**, not the game's bundle id
  (`GameRuntimeApkBuilder.kt:210`):
  ```kotlin
  val packageName = "${GameRuntimeArtifactContract.PACKAGE_PREFIX}${sourceHash.take(20)}${certificateHash.take(8)}"
  ```

So for `com.lego.NinjagoSpinjitzuScavengerHunt` you currently get something like
`dev.radek.gameruntime.a1b2c3…` and no OBB.

### 4.2 Target

For `AngryBirds_v1.0_os30.ipa` (`com.clickgamer.AngryBirds`, `CFBundleVersion`
1.0):

```
APK package id : com.clickgamer.AngryBirds
OBB file name  : main.1.com.clickgamer.AngryBirds.obb
OBB location   : /sdcard/Android/obb/com.clickgamer.AngryBirds/main.1.com.clickgamer.AngryBirds.obb
```

For `com.lego.NinjagoSpinjitzuScavengerHunt`:

```
main.<versionCode>.com.lego.NinjagoSpinjitzuScavengerHunt.obb
```

This is the platform-standard expansion-file contract, so Android's own
`StorageManager`/`getObbDir()` resolution and any store-side delivery work
unchanged.

### 4.3 Work items

| # | Change | Detail |
|---|---|---|
| 4.1 | Derive the Android package id from `CFBundleIdentifier` | Sanitise to `[a-zA-Z0-9_.]`, must contain ≥2 segments, fall back to the hash id when the bundle id is missing or invalid. Keep `GameRuntimeArtifactContract.PACKAGE_PREFIX` as the fallback only. |
| 4.2 | Derive `versionCode` from `CFBundleVersion` | `1.0` → `1`; `1.4.2` → `10402`-style or a monotonic integer. Must be a positive int, stable across rebuilds of the same IPA. `versionName` = the raw `CFBundleVersion` string. |
| 4.3 | Emit a real OBB during packaging | Split `assets/bundle/**` out of the APK into a ZIP (stored/uncompressed for large media) named `main.<versionCode>.<package>.obb`. |
| 4.4 | Keep it mounted in the guest | Point `/Android/obb` at the real OBB contents instead of an empty dir; keep the existing read-only mount semantics. |
| 4.5 | Graceful fallback | If the OBB is absent or the hash mismatches, fall back to the embedded `assets/bundle/` copy and say so in the report — never a black screen. |
| 4.6 | Declare permissions | `READ_EXTERNAL_STORAGE` / storage access appropriate to the target API level for reading the OBB. |
| 4.7 | Update `ArtifactNames` | Add an `obbFileName(report)` helper alongside `apkFileName` / `gameApkFileName` (`ArtifactNames.kt`). |
| 4.8 | Tests | Extend `GameRuntimeArtifactContractTest` / `ArtifactNamesTest`: assert exact `main.<v>.<pkg>.obb` for a fixture with a known bundle id, and assert the fallback path. |

**Note on size:** Angry Birds' bundle is small (the IPA is ~9.8 MB, mostly
`data/SFX/*.wav`), so for it the OBB is mostly about matching the expected
layout. For a large game the OBB is what keeps the APK under the install limit —
worth gating on a threshold (e.g. >100 MB of bundle payload) with an override.

---

## 5. Proposed CHANGELOG entries (draft — to be committed with the work)

```markdown
## Unreleased

### Fixed
- compat-runtime: `-[EAGLContext renderbufferStorage:fromDrawable:]` read its
  drawable from ARM register r2 (the `target` enum `GL_RENDERBUFFER_OES`) instead
  of r3 (the `CAEAGLLayer*`). No EGL surface was ever created, so the EAGL
  drawable stayed unattached and no frame was presented.
  (`native/src/compat_runtime/app_lifecycle.cpp`)
- compat-runtime: `renderbufferStorage:fromDrawable:` and `presentRenderbuffer:`
  returned success when they had failed, masking the missing drawable.
- analyzer: an image with any import was unconditionally flagged `incompatible`,
  so every real IPA was reported `BLOCKED` regardless of triage coverage.
  (`app/src/main/java/dev/radek/conventor/Library.kt`)
- analyzer: an embedded image that decoded successfully still set
  `incompatible = true`.
- analyzer: auto-registered stubs were counted as "libioscompat.so implementation
  exports available", inflating the implementation-availability metric.
- analyzer: `linkedImplementationCount` was hardcoded to 0.

### Added
- Native frame pump driven from Android Choreographer, so the guest's run loop is
  serviced instead of the process returning after `applicationDidFinishLaunching:`.
- OpenAL → AAudio/OpenSL ES backend replacing the state-only shims.
- AudioToolbox `AudioSessionInitialize`/`AudioSetSessionActive` support.
- Device-derived NDK export inventory, replacing the fixed 181-entry catalogue.
- Expansion OBB emitted as `main.<versionCode>.<CFBundleIdentifier>.obb`, with the
  APK package id derived from the game's bundle identifier.
- Touch input delivery into the guest.
- `-applicationFrame` support through the `objc_msgSend_stret` path.

### Changed
- Default guest view size now comes from the real Android surface and respects
  `UIInterfaceOrientation` from `Info.plist` (was hardcoded 320x480 portrait).
- The headline coverage metric is now `linkedImplementationPercent`; reviewed
  mapping coverage is reported as the triage-only axis it always was.
```

---

## 6. What I need from you before touching code

1. **Confirm root cause A** — check the on-device diagnostics for
   `drawable size unavailable` (§3.1). This decides whether the black screen is a
   one-line fix or needs the full pump first.
2. **Paste any remaining screenshot text** not in your message (report panel,
   logcat, diagnostics list).
3. **Bioshock symbol list** — if you can attach the IPA or its
   `report.json → apiMapping.symbols`, I can bucket the 180 stubs by framework
   and size Phase 3.7 precisely instead of estimating.
4. **Confirm the OBB convention** — `main.<versionCode>.<package>.obb` (standard)
   with `versionCode` from `CFBundleVersion`, and whether you want the bundle
   split out always or only above a size threshold.

Say the word and I'll start with §3.1 (the `r[2]` → `r[3]` fix) plus its unit
test, since that is the cheapest change with the largest visible payoff.
