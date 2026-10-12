# BioShock NDK needs: status

Source: `tests/data/com.2k.bioshock-ndk-needs.txt` (1,612 unresolved imports). Per-symbol
decisions: `bioshock-ndk-needs-status.tsv` in this folder.

| Decision | Count | Meaning |
|---|---|---|
| IMPLEMENTED | 53 | Real bodies in the native runtime and covered by tests: 4 compiler-rt conversions (`___fixsfdi`, `___fixunsdfdi`, `___fixunssfdi`, `___floatundisf`) and 49 constants (44 `NSString` keys and notification names with the SDK values, plus `CGRectNull`, `CGRectZero`, `CGSizeZero`, `CGAffineTransformIdentity`, `UIWindowLevelAlert`). |
| BOUNDARY: game or middleware C++ | 1,337 | Not Apple APIs. Names such as `InternalConstructor`, Havok, FMOD, ISE, PVR and FName look like the game's own code. Whether the bundle defines them could only be checked against the IPA, which is not available. Left unbound, so they trap. |
| BOUNDARY: C++ typeinfo and vtables | 158 | Game-defined types unless a bundled dylib defines them. Same check as above. |
| BOUNDARY: service classes | 38 | iAd, GameKit, GameController, AdSupport, MediaPlayer and similar online services. No offline equivalent. Unbound, so they trap. |
| BOUNDARY: host-to-guest call | 9 | AudioQueue, AudioFile and AudioServices callbacks run guest code and return to the host. The runtime has no host-to-guest call yet. |
| BOUNDARY: rasterizer | 7 | CGContext drawing, CGImage and CGDataProvider. Needs a software rasterizer. |
| BOUNDARY: object model | 6 | `NSJSONSerialization`, `UITableView`, `UITextField`, `UITextView` and the `UIWindow` metaclass need real class implementations. |
| BOUNDARY: object model | 3 | `__NSConcreteGlobalBlock`, `__NSConcreteStackBlock` (block ABI) and `kCFNumberNaN` (needs a CFNumber object). |
| BOUNDARY: loader | 1 | `dyld_stub_binder`. The loader binds imports eagerly, and any unbound import traps at boot. |

## Fixed in this pass

- `NSString *const` symbols (the EAGL keys and every string constant in `darwin_compat`) were
  bound to the string object. The loader writes the symbol's address into the import slot, so
  compiled code read the object's isa as the key. They now resolve to a cell that holds the
  pointer. Test: `testStringConstantSymbolsAreVariables`.
