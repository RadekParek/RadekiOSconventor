# compat-runtime-v1 architecture

`compat-runtime-v1` is an Android runtime layer for a user's own authorized 32-bit iPhone OS application packages from the iOS 2–6 era. Its intended execution model combines statically recompiled guest code with native framework implementations. An IPA and its Apple executable are selected and read at runtime; neither is packaged in the Android library.

This contract is deliberately narrow. It does not claim complete iOS behavior, a native replacement application, or working game compatibility without repeatable smoke evidence. The runtime must not draw technical messages, banners, watermarks, or other overlays while guest code is running.

## Runtime path

1. **User-selected input.** Android code opens the user's selected URI or an already-authorized stream. The caller supplies the `Payload/<App>.app/<CFBundleExecutable>` archive entry resolved from the bundle metadata. The stream helper extracts that one entry into bounded memory; it does not write the IPA or executable into the APK or extract the package tree to disk.
2. **Authorization and input policy.** The JNI boundary requires explicit authorization confirmation, rejects empty or oversized input, and reports a blocked result rather than continuing on invalid input. The Mach-O loader rejects a non-zero `cryptid`; it does not attempt to decrypt FairPlay-protected code.
3. **32-bit guest address space.** `GuestAddressSpace` provides bounded guest mappings, page permissions, heap allocations, checked guest pointers, and host/guest pointer conversion. Guest addresses remain 32-bit values and are never treated as host pointers without a checked mapping.
4. **Mach-O and dyld-lite.** `MachOLoader` selects a 32-bit ARM slice from thin or FAT input, maps segments, applies supported legacy rebase/bind streams, and resolves exact Darwin import names only through registered native call adapters. It records every observed resolved or unresolved import and selects the first unresolved symbol as the current blocker. Chained fixups and unsupported structures fail closed.
5. **Objective-C object model.** `objc_runtime` host-tests class/metaclass relationships, selector interning, inherited instance and class dispatch, retain/release, and nested autorelease pools. This is a small internal object model, not a general platform ABI implementation. Framework behavior is supplied only by explicit, tested shim adapters.
6. **ARM32 backend.** A single `CpuBackend` interface accepts a guest entry point, register state, and guest memory/callout callbacks. Android CMake fetches the pinned open-source Unicorn Engine 2.1.4 ARM backend; the project does not implement a hand-written ARM code generator. If the backend is absent or cannot prepare/execute the guest function, the runtime returns a named failure and does not claim execution.
7. **Runner and evidence.** `GuestRunner` loads the supplied main executable, stops before guest execution when imports are missing, and reports exactly the first missing import. A successful return from a guest entry point is not evidence that an app reached a menu or playable state. Those outcomes are recorded only by a smoke run in `compatibility/database.json`.

## Boundaries and current implementation state

- The current Android `ShimRegistry` starts empty. The registry and callout path are host-tested, but no production iOS framework family is registered yet. Real applications therefore remain blocked at their first unregistered import.
- The loader implements legacy 32-bit Mach-O structures needed by the narrow contract. It is not dyld, does not load arbitrary dependent Mach-O images, and rejects encrypted input and known unsupported chained-fixup metadata.
- The host-tested Objective-C object model does not by itself make an arbitrary Objective-C binary ABI-compatible. Missing class, selector, runtime, or framework behavior must stay unresolved and fail closed.
- IPA extraction requires the caller to supply the app executable's archive path. Binary plist parsing and automatic executable discovery are not yet included.
- No real title was provided or run in this session. The initial compatibility database contains no app records; its three shim-family priorities are explicitly provisional and report zero confirmed games unblocked only as an empty evidence list, not as a coverage percentage.
- The first real-game blocker is **not yet knowable**. The first exact blocker will be the `firstMissingImport` emitted when an authorized real IPA is imported. The next three provisional shim families, in order, are Objective-C runtime core, Foundation/CoreFoundation core, and UIKit/CoreGraphics/CoreAnimation rendering and input; the database records the evidence needed to reprioritize them.

## Build and test

The native host suite builds `compat_runtime_core` and exercises guest memory, heap and pointer bounds, Mach-O/FAT selection, entry points, dyld rebases/binds, exact shim lookup, first-missing-import reports, and the Objective-C object model. Without Unicorn, the CPU contract test verifies the explicit backend-unavailable result. When the pinned backend is enabled on a host, it additionally executes small ARM and Thumb return/callout fixtures.

Android builds use `:compat-runtime-v1:assembleDebug` (or `assembleRelease`) and build `libcompat_runtime_v1.so` with the pinned Unicorn ARM32 dependency. The runtime report artifact name is `compat-runtime-v1-report.json`.
