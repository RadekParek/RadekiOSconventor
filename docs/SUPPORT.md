# Honest support matrix

Statuses describe implemented behavior, not planned compatibility.

| Area | State | Implemented boundary |
|---|---|---|
| Android library/import/details | PARTIAL | Persistent metadata, real logs and bounded ZIP import; host-only compilation |
| ZIP extraction | SUPPORTED | Traversal, symlinks/special files, collisions, encryption, limits, CRC; Android rejects ZIP64 explicitly |
| Info.plist | SUPPORTED | XML and binary, required bundle fields, nested icon dictionaries; bounded parsing |
| Icons (host converter) | PARTIAL | Generic fallback chain: Info.plist names -> `@2x`/`@3x`/`~ipad`/`~iphone` variants -> compiled `Assets.car` renditions -> icon-like bundle images -> any remaining image. Decodes ordinary PNG (colour types, bit depths, interlace, palettes, `tRNS`) and Apple `CgBI`; records source, format, resolution, scale and every fallback attempt; emits a square transparent Android launcher icon. HEIF/HEIC renditions are reported as undecodable instead of faked |
| Icons (on-device import) | PARTIAL | Plist names/scale variants are tried first, then ranked loose bundle PNG/JPEG fallbacks. Standard images use Android decoding; bounded 8-bit non-interlaced CgBI RGB/RGBA PNGs are normalized from raw/zlib DEFLATE, BGRA and premultiplied alpha. `Assets.car` and unsupported CgBI formats remain undecoded and are reported rather than fabricated |
| Mach-O thin/FAT/FAT64 | SUPPORTED | CPU/subtype, endian headers, bounded load command/section/symbol parsing |
| Mach-O loader metadata | PARTIAL | Relocations, dynamic table fields, bind symbols/offsets/addends, export trie, chained import/page-start records, dependencies, LC_MAIN, signature blob metadata. Bind cursors are segment-bounded; invalid/unsupported bind streams are returned as explicit diagnostics so inspection can finish, and incomplete tables always block conversion. No signature trust validation or chained pointer graph rewriting |
| ObjC/Swift/unwind/init metadata | PARTIAL | Host reconstruction recovers ObjC classes/categories/protocols, ivars, properties, selectors (big and relative-small method lists), message-send selectors, class/super references and Swift type/field descriptors plus demangling. Section identification on device |
| ARM64 reconstruction | PARTIAL | Verified closed integer leaf subset; original safe instruction bytes retained in Android ELF |
| ARM64e | BLOCKED | PAC/ABI adaptation not proven |
| ARMv6/ARMv7/v7s/Thumb/Thumb-2 | PARTIAL | ARMv6 CPU subtype 6 is recognized; the verified leaf subset supports selected A32/Thumb immediate arithmetic and return instructions and lowers them to ARM64. This is not general old-game compatibility; branches, memory, calls and framework/runtime dependencies remain blocked |
| Offline source reconstruction | PARTIAL | Function discovery, basic-block CFGs, ARM64/ARMv6/ARMv7/Thumb disassembly, register/constant/reference tracking and pseudocode listings with explicit `?` markers for anything unproven. Engineering artifact only, never presented as original source; reconstructed code is never executed |
| Branching CFG / loads / calls / atomics | BLOCKED | IR vocabulary exists; no verified general lowering; rejected rather than stubbed |
| Objective-C binary ABI | BLOCKED | Portable host runtime is experimental, not an Apple runtime provider |
| Framework dependency classification | SUPPORTED | Host reachability analysis distinguishes linked-but-unused libraries from reachable calls. For the closed integer leaf only, unused dylib/import records and data-only bind metadata may be omitted because the emitted code has no call, memory or address operations; no framework symbols or no-op stubs are synthesized. Reachable/unknown code remains blocked |
| Foundation/CoreFoundation / UIKit / CoreGraphics | BLOCKED | No converted-app API providers; only reachable APIs are listed as blockers |
| Darwin C/C++ ABI / exceptions / TLS | BLOCKED | Host tests verify portable C++ mechanics only, not Darwin-to-Bionic adaptation |
| Swift | BLOCKED | Metadata/dependencies are detected, not silently dropped |
| EAGL/OpenGL ES, Metal/Vulkan | BLOCKED | No compatibility renderer implemented |
| AudioToolbox/AVFoundation/OpenAL | BLOCKED | No audio API mapping implemented |
| Touch / keyboard / gamepad / sensors / iOS lifecycle | BLOCKED | Android entry activity exists, not iOS input/lifecycle mapping |
| Resources | PARTIAL | Relative paths, localization, plist/JSON/audio/texture files preserved as opaque assets; icon-like PNGs normalized. Compiled asset catalogs are decoded for icon recovery only. No shader, texture/audio codec or lookup ABI conversion |
| APK packaging/signing | SUPPORTED | Real SDK/NDK, JNI ARM64 ELF, compiled manifest/resources/DEX/assets, reusable dev key |
| APK static validation | SUPPORTED | ZIP/manifest/package/entry/DEX/signature/ELF architecture/dependencies/resources/icon/alignment/provenance checks, DEX checksum/launcher definition and native JNI entry/code hash |
| Android runtime/device validation | BLOCKED | No device test is currently supplied; report says NOT_TESTED |

## Reconstruction report

`python3 -m radek convert app.ipa --authorized --output workspace/result` writes, next to
`report.json`:

- `reconstruction.json` — machine-readable: architectures, entry points, per-image and
  per-slice metrics, recovered functions with listings, call graph, Objective-C
  classes/selectors/message sends, Swift types and symbols, imported symbols, linked
  frameworks, reachable API attribution and capability areas.
- `reconstruction.md` — the same evidence in readable form, including the reconstructed
  function listings and the blockers derived from reachable APIs.

Nothing in the reconstruction is executed, and no listing is a claim about the original
source: unproven instructions, types and control flow are marked explicitly.

## Closed integer entry contract v1

This is deliberately narrow and independent of bundle ID or game name:

- An unencrypted, little-endian MH_EXECUTE image; LC_MAIN identifies an executable `__text` entry.
- No embedded Mach-O images, chained fixups, runtime metadata or unknown loader semantics. Linked dylibs/import records, data-only dyld bind/rebase streams and relocations outside the selected entry bytes may be recorded and omitted: the accepted entry code cannot call them or access their data. An incomplete bind stream or a relocation overlapping the emitted entry is blocked.
- Code is proved linearly from the entry until an unconditional return, within 4096 instructions. No source byte is executed during conversion.
- Every read register must have been initialized within the function. No input arguments, memory references, SP/platform/callee-saved register writes, PAC, traps or syscalls are accepted.
- ARM64: 32-bit MOVZ/MOVK, unshifted immediate ADD/SUB and RET. Original instruction bytes are placed into an ELF exported JNI entry. NDK linking supplies Android load information; source addresses are unused because the accepted code has no address dependencies.
- ARMv6/ARMv7/ARMv7s: selected A32 and Thumb-1 immediate MOV/ADD/SUB and return instructions; Thumb-2 MOVW/MOVT is accepted only for ARMv7/v7s. Selected Thumb mode requires a matching symbol with `N_ARM_THUMB_DEF`. Operations are emitted as real ARM64 instructions; accepted code never reads condition flags.
- The function returns a 32-bit integer. The standalone Android launcher invokes it and displays the returned value. There is no loop dispatch/interpreter/translation runtime.
- Source bundle resources are included under `assets/bundle/` after excluding Mach-O executables and Apple signatures. This is preservation, **not** a claim that iOS filesystem/resource APIs work.

Ordinary iOS apps do not fit this contract and must remain BLOCKED. There is deliberately no force-bypass mode: skipping unresolved APIs or substituting no-op symbols would produce a misleading, commonly non-runnable APK. It would be misleading to label this implementation a completed general-purpose IPA converter.

## Experimental portable runtime

`native/include/runtime.hpp` implements class/metaclass registration, selector interning, IMP lookup, inheritance, invalidated dispatch caches, category replacement, ivar slots, property/protocol metadata, atomic retain/release, thread-local autorelease pools, data/string/array/dictionary values, notification delivery and isolated test storage. Tests exercise allocation, threading, TLS, exceptions and RTTI.

It **does not** implement Apple's object layout, metadata registration, tagged pointers, weak references, Objective-C exception ABI or variadic/aggregate `objc_msgSend`. It exports no fake Apple symbols and is never linked as a dependency provider for converted apps. Portable data types are not advertised as NSString/NSArray implementations.
