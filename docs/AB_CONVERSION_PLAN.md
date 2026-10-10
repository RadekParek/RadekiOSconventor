# Playable-game conversion plan — Angry Birds 1.0 (iOS 3.0, ARMv6)

Status legend: [ ] todo · [~] in progress · [x] done · [!] blocked/risk

## Product constraints (from user)

- Output must be a **playable .apk** for Angry Birds 1.0.0 (`tests/data/AngryBirds_v1.0_os30.ipa`).
- **No emulator** in the APK. Allowed: a small number of translation layers / recompilers.
- No PR / merge until fully finished.

## Architecture (native execution + API translation, zero emulation)

The original ARMv6 machine code runs **directly on the device CPU** (armeabi-v7a;
ARMv6 user-mode ISA ⊂ ARMv7-A, proven by the fact that this exact binary shipped
on iPhone 3G (ARMv6) and 3GS (ARMv7-A)). The APK ships:

1. `loader` — a minimal dyld replacement: maps the Mach-O, applies the
   convert-time rebase map (the binary is non-PIE with zero local relocs, so
   pointer discovery happens at convert time), binds the 220 lazy + 77 non-lazy
   + 348 data-reloc import slots, fills NXArgc/Argv/environ, runs `__mod_init_func`.
2. `objc` — legacy 32-bit Objective-C runtime (class connection from Mach-O
   metadata, `objc_msgSend`/`_stret`/`Super2` in ARM asm, autorelease pools,
   properties, fast enumeration).
3. `uikit/foundation/eagl` — game-driven subset (54 selectors, ~15 framework
   classes) over Android: NativeActivity, EGL, sensors, input queue.
4. `gl1` — OpenGL ES 1.1 pass-through + PVRTC→RGBA decode at upload
   (non-PVR GPUs can't consume the game's PVRTC textures).
5. `al` — OpenAL → OpenSL ES translation (buffer/source/listener subset).
6. `posix` — Darwin/BSD stdio (exact `FILE` layout), errno/ctype mapping,
   bundle-path redirection, time, threads.
7. `cxx` — Itanium C++ runtime: new/delete, `__cxa_*`, and a from-spec
   SjLj unwinder + `__gxx_personality_sj0` LSDA interpreter (the game throws
   `lang::Throwable` across frames; verified against GCC's documented ABI and
   the binary's own landing pads, reimplemented — no GPL code in the runtime).

Convert-time (host) components in `radek/game/`:

- [x] `macho.py` — exact classic-Mach-O parser (survey-proven on the real binary)
- [ ] `disasm.py` — recursive-descent ARM + literal/xref resolution
- [ ] `pointers.py` — rebase-map (pointer discovery) with zero-ambiguity proof
- [ ] `objc_meta.py` — legacy ObjC metadata model (host mirror for verification)
- [ ] `profile.py` — `angrybirds_v1` game profile (fingerprint + required API set)
- [ ] `package.py` — APK builder: AXML manifest, assets, zig-built ARM `.so`,
      v1 signing, zipalign. No DEX: the app entry is `android.app.NativeActivity`
      (`android:hasCode="false"` + `android.app.lib_name` meta-data).

Runtime components in `runtime/` (portable C99; host build for tests,
ARM build with zig for the APK):

- [ ] `loader.c` + `arm_asm.S`
- [ ] `objc.c`
- [ ] `cxx.c` (SjLj + personality + cxa)
- [ ] `darwin_posix.c`
- [ ] `ns_foundation.c` (incl. NSThread/NSTimer/runloop-lite)
- [ ] `ui_kit.c` + `eagl.c`
- [ ] `gl1.c` (incl. PVRTC decoder) + `al.c`
- [ ] `platform_android.c` + `main.c` + `rt_min.c`
- [ ] `platform_host.c` (test doubles)

Verification (no device in sandbox; Dynarmic 2.1.4 ARM+VFP harness is a
**dev-test tool only**, never shipped in the APK — same precedent as the
repo's existing pinned-Dynarmic host tests). The pinned Dynarmic 2.1.4 ARM32
backend in `native/src/compat_runtime/dynarmic_backend.cpp` executes the image's
scalar VFP only after the runtime grants CP10/CP11 access (`CPACR`) and sets
`FPEXC.EN`; without that setup the guest stops on a decode fault at its first
`vpush`. That is guest CPU-configuration state, not a Dynarmic decoder
limitation:

- [ ] Host unit tests for every runtime module (ctest + pytest).
- [~] Dynarmic guest harness: slid image + bound imports → `_main` →
      `UIApplicationMain` → delegate launch → scripted touches/accelerometer →
      N stable frames with sane GL/AL call streams, save-file writes.
      Observed today: the real image boots through `_main` →
      `UIApplicationMain` → its own `AppController`
      `applicationDidFinishLaunching:` → UIKit window/EAGL view creation and
      used to stop at the first OpenGL ES import (`_glFrontFace`, 340,309 guest
      instructions). No frame, no touch delivery, and no GL/AL call stream yet;
      the remaining items stay unchecked.
- [ ] Structural APK validation (manifest parse-back, alignment, signature,
      ARM `.so` ELF checks, import-manifest check).
- [ ] Full repo test suite green; docs updated with honest test-status notes.

## Key binary facts (surveyed, see `docs/AB_BINARY_SURVEY.md`)

- ARMv6 MH_EXECUTE, single slice, TEXT @0x1000, `start`=0x4320, `_main`=0x74358.
- Pure ARM + scalar VFP (no Thumb, no NEON, no SWP/SVC/BKPT/SETEND, no CP15).
- 254 imports; 220 lazy fn slots, 77 non-lazy slots (9 imports + 68 internal),
  348 data relocs; zero local relocs (fixed-address image 0x1000–0x1528A4).
- 2 ObjC classes (`AppController`, `MyEAGLView`), 54 selectors, 15 classrefs.
- Engine: ka3d C++ + Box2D + Lua 5.1 + libpng/zlib + libmad + WAV loader,
  all statically linked; game loop on an `NSThread`, UI/timers on main thread.
- Graphics: GLES 1.1 fixed-function + OES framebuffer; textures PNG + PVR
  (RGB565/4444-style 16bpp + PVRTC 4bpp). Audio: OpenAL + WAV/MAD-MP3.
- C++ exceptions (SjLj) genuinely used (`lang::Throwable` caught in mainloop).

## Device requirements (documented honestly in the APK + docs)

- ARMv7-A or newer CPU **with 32-bit (AArch32) support**; Android 5.0+ (API 21+);
  OpenSL ES; EGL with OpenGL ES 1.x. 64-bit-only devices (no AArch32) cannot
  run ARM32 machine code without CPU emulation, which this product forbids,
  so they are out of scope.
