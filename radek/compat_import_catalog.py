"""Guest-runtime adapter catalog for imports without an exact NDK name match.

The Android mapper deliberately keeps this inventory separate from the reviewed
same-name Bionic/NDK catalog. A name here has a registered ARM32 guest-callout
adapter or guest-data binding in ``compat-runtime-v1``; it is not a same-name
Android export, a static NDK link, or proof of complete API semantics.

The table is fixture-independent. The checked-in Angry Birds v1.0 import set
contains 181 same-name NDK candidates and 73 names from this catalog. The
fixture-specific matches stay separate from the broader runtime adapter inventory.
Tests pin the exact, disjoint split so changing a denominator cannot inflate it.
"""

from __future__ import annotations

# Values are stable provider identifiers used in reports and in parity tests.
# Keep the source symbol spelling exactly as it appears in a 32-bit Mach-O
# undefined-symbol table (including Darwin's leading underscore(s)).
CONCRETE_DARWIN_COMPAT_PROVIDERS: dict[str, str] = {
    # AudioToolbox legacy session state.
    "_AudioSessionInitialize": "audio-session.initialize",
    "_AudioSessionSetActive": "audio-session.set-active",

    # Foundation/UIKit lifecycle and app-sandbox path adapters.
    "_NSHomeDirectory": "foundation.home-directory",
    "_NSSearchPathForDirectoriesInDomains": "foundation.search-paths",
    "_NSTemporaryDirectory": "foundation.temporary-directory",
    "_UIApplicationMain": "uikit.application-main",

    # Objective-C class/metaclass data and dispatch/runtime data.
    "_OBJC_CLASS_$_CAEAGLLayer": "objc.class.CAEAGLLayer",
    "_OBJC_CLASS_$_EAGLContext": "objc.class.EAGLContext",
    "_OBJC_CLASS_$_NSAutoreleasePool": "objc.class.NSAutoreleasePool",
    "_OBJC_CLASS_$_NSBundle": "objc.class.NSBundle",
    "_OBJC_CLASS_$_NSDictionary": "objc.class.NSDictionary",
    "_OBJC_CLASS_$_NSNumber": "objc.class.NSNumber",
    "_OBJC_CLASS_$_NSObject": "objc.class.NSObject",
    "_OBJC_CLASS_$_NSString": "objc.class.NSString",
    "_OBJC_CLASS_$_NSThread": "objc.class.NSThread",
    "_OBJC_CLASS_$_NSURL": "objc.class.NSURL",
    "_OBJC_CLASS_$_UIAccelerometer": "objc.class.UIAccelerometer",
    "_OBJC_CLASS_$_UIApplication": "objc.class.UIApplication",
    "_OBJC_CLASS_$_UIScreen": "objc.class.UIScreen",
    "_OBJC_CLASS_$_UIView": "objc.class.UIView",
    "_OBJC_CLASS_$_UIWindow": "objc.class.UIWindow",
    "_OBJC_METACLASS_$_NSObject": "objc.metaclass.NSObject",
    "_OBJC_METACLASS_$_UIView": "objc.metaclass.UIView",
    "__objc_empty_cache": "objc.data.empty-cache",
    "__objc_empty_vtable": "objc.data.empty-vtable",
    "_objc_enumerationMutation": "objc.enumeration-mutation",
    "_objc_msgSend": "objc.msgSend",
    "_objc_msgSendSuper2": "objc.msgSendSuper2",
    "_objc_msgSend_stret": "objc.msgSend.stret",
    "_objc_setProperty": "objc.setProperty",

    # Darwin libc/global data and EAGL keys.
    "__DefaultRuneLocale": "darwin.rune-locale",
    "___CFConstantStringClassReference": "corefoundation.constant-string-class",
    "___error": "darwin.errno-cell",
    "___maskrune": "darwin.ctype.maskrune",
    "___stderrp": "darwin.stream.stderr",
    "___stdinp": "darwin.stream.stdin",
    "___stdoutp": "darwin.stream.stdout",
    "___tolower": "darwin.ctype.tolower",
    "___toupper": "darwin.ctype.toupper",
    "_kEAGLColorFormatRGB565": "eagl.constant.RGB565",
    "_kEAGLColorFormatRGBA8": "eagl.constant.RGBA8",
    "_kEAGLDrawablePropertyColorFormat": "eagl.constant.color-format",
    "_kEAGLDrawablePropertyRetainedBacking": "eagl.constant.retained-backing",

    # OpenAL state adapter.  It intentionally models object/state ownership;
    # the runtime report separately says whether a real audio backend is active.
    "_alBufferData": "openal.buffer-data",
    "_alDeleteBuffers": "openal.delete-buffers",
    "_alDeleteSources": "openal.delete-sources",
    "_alGenBuffers": "openal.gen-buffers",
    "_alGenSources": "openal.gen-sources",
    "_alGetSourcef": "openal.get-source-float",
    "_alGetSourcei": "openal.get-source-int",
    "_alSource3f": "openal.source-3-float",
    "_alSourcePlay": "openal.source-play",
    "_alSourceQueueBuffers": "openal.source-queue",
    "_alSourceStop": "openal.source-stop",
    "_alSourceUnqueueBuffers": "openal.source-unqueue",
    "_alSourcef": "openal.source-float",
    "_alSourcei": "openal.source-int",
    "_alcCloseDevice": "openal.close-device",
    "_alcCreateContext": "openal.create-context",
    "_alcDestroyContext": "openal.destroy-context",
    "_alcMakeContextCurrent": "openal.make-context-current",
    "_alcOpenDevice": "openal.open-device",

    # ARM32 toolchain and SjLj context adapters.
    "__Unwind_SjLj_Register": "sjlj.register-context",
    "__Unwind_SjLj_Resume": "sjlj.resume-boundary",
    "__Unwind_SjLj_Unregister": "sjlj.unregister-context",
    "___divdi3": "compiler-runtime.divdi3",
    "___divsi3": "compiler-runtime.divsi3",
    "___fixdfdi": "compiler-runtime.fixdfdi",
    "___floatdidf": "compiler-runtime.floatdidf",
    "___floatdisf": "compiler-runtime.floatdisf",
    "___gxx_personality_sj0": "cxxabi.gxx-personality-sj0",
    "___moddi3": "compiler-runtime.moddi3",
    "___modsi3": "compiler-runtime.modsi3",
    "___udivsi3": "compiler-runtime.udivsi3",
    "___umodsi3": "compiler-runtime.umodsi3",
    "_OSAtomicAnd32OrigBarrier": "libsystem.osatomic-and32-orig-barrier",
    # Compiler-runtime and VFP soft-float helpers (native compiler_rt_shims.cpp).
    "___fixsfsivfp": "compiler-runtime.fixsfsivfp",
    "___fixunsdfsivfp": "compiler-runtime.fixunsdfsivfp",
    "___floatsidfvfp": "compiler-runtime.floatsidfvfp",
    "___floatsisfvfp": "compiler-runtime.floatsisfvfp",
    "___floatundidf": "compiler-runtime.floatundidf",
    "___floatunssidfvfp": "compiler-runtime.floatunssidfvfp",
    "___floatunssisfvfp": "compiler-runtime.floatunssisfvfp",
    "___muldi3": "compiler-runtime.muldi3",
    "___udivdi3": "compiler-runtime.udivdi3",
    "___umoddi3": "compiler-runtime.umoddi3",
    "___adddf3vfp": "compiler-runtime.adddf3vfp",
    "___addsf3vfp": "compiler-runtime.addsf3vfp",
    "___divdf3vfp": "compiler-runtime.divdf3vfp",
    "___divsf3vfp": "compiler-runtime.divsf3vfp",
    "___eqsf2vfp": "compiler-runtime.eqsf2vfp",
    "___extendsfdf2vfp": "compiler-runtime.extendsfdf2vfp",
    "___gedf2vfp": "compiler-runtime.gedf2vfp",
    "___gtdf2vfp": "compiler-runtime.gtdf2vfp",
    "___gtsf2vfp": "compiler-runtime.gtsf2vfp",
    "___lesf2vfp": "compiler-runtime.lesf2vfp",
    "___ltsf2vfp": "compiler-runtime.ltsf2vfp",
    "___muldf3vfp": "compiler-runtime.muldf3vfp",
    "___mulsf3vfp": "compiler-runtime.mulsf3vfp",
    "___nedf2vfp": "compiler-runtime.nedf2vfp",
    "___nesf2vfp": "compiler-runtime.nesf2vfp",
    "___subdf3vfp": "compiler-runtime.subdf3vfp",
    "___subsf3vfp": "compiler-runtime.subsf3vfp",
    "___truncdfsf2vfp": "compiler-runtime.truncdfsf2vfp",
    "___unordsf2vfp": "compiler-runtime.unordsf2vfp",
}

CONCRETE_DARWIN_COMPAT_IMPORT_COUNT = len(CONCRETE_DARWIN_COMPAT_PROVIDERS)

if CONCRETE_DARWIN_COMPAT_IMPORT_COUNT != 105:  # pragma: no cover - authoring guard
    raise AssertionError(
        "the non-NDK Darwin provider inventory must contain exactly 105 imports, "
        f"got {CONCRETE_DARWIN_COMPAT_IMPORT_COUNT}"
    )


def provider_for_symbol(symbol: str) -> str | None:
    """Return the concrete runtime provider id for an exact import spelling."""
    return CONCRETE_DARWIN_COMPAT_PROVIDERS.get(symbol)


# Provider ids whose compat-runtime-v1 adapter carries complete, unit-tested
# API semantics (exact arithmetic bodies, real errno/stdio cells, the OpenAL
# engine, sandbox path providers, ...). Reports surface these as implemented
# rather than as outstanding "adapter only" needs; the guest loader still
# binds them from the same catalog entries. Keep parity with
# native/include/compat_runtime/compat_import_catalog.hpp and
# app/src/main/java/dev/radek/conventor/CompatImportProviders.kt.
VERIFIED_SEMANTICS_PROVIDERS: frozenset[str] = frozenset({
    "libsystem.osatomic-and32-orig-barrier",
    "compiler-runtime.fixsfsivfp",
    "compiler-runtime.fixunsdfsivfp",
    "compiler-runtime.floatsidfvfp",
    "compiler-runtime.floatsisfvfp",
    "compiler-runtime.floatundidf",
    "compiler-runtime.floatunssidfvfp",
    "compiler-runtime.floatunssisfvfp",
    "compiler-runtime.muldi3",
    "compiler-runtime.udivdi3",
    "compiler-runtime.umoddi3",
    "compiler-runtime.adddf3vfp",
    "compiler-runtime.addsf3vfp",
    "compiler-runtime.divdf3vfp",
    "compiler-runtime.divsf3vfp",
    "compiler-runtime.eqsf2vfp",
    "compiler-runtime.extendsfdf2vfp",
    "compiler-runtime.gedf2vfp",
    "compiler-runtime.gtdf2vfp",
    "compiler-runtime.gtsf2vfp",
    "compiler-runtime.lesf2vfp",
    "compiler-runtime.ltsf2vfp",
    "compiler-runtime.muldf3vfp",
    "compiler-runtime.mulsf3vfp",
    "compiler-runtime.nedf2vfp",
    "compiler-runtime.nesf2vfp",
    "compiler-runtime.subdf3vfp",
    "compiler-runtime.subsf3vfp",
    "compiler-runtime.truncdfsf2vfp",
    "compiler-runtime.unordsf2vfp",
    "compiler-runtime.divdi3", "compiler-runtime.divsi3", "compiler-runtime.moddi3",
    "compiler-runtime.modsi3", "compiler-runtime.udivsi3", "compiler-runtime.umodsi3",
    "compiler-runtime.fixdfdi", "compiler-runtime.floatdidf", "compiler-runtime.floatdisf",
    "darwin.errno-cell", "darwin.stream.stderr", "darwin.stream.stdin", "darwin.stream.stdout",
    "darwin.ctype.tolower", "darwin.ctype.toupper", "darwin.ctype.maskrune",
    "openal.buffer-data", "openal.delete-buffers", "openal.delete-sources",
    "openal.gen-buffers", "openal.gen-sources", "openal.get-source-float",
    "openal.get-source-int", "openal.source-3-float", "openal.source-play",
    "openal.source-queue", "openal.source-stop", "openal.source-unqueue",
    "openal.source-float", "openal.source-int", "openal.close-device",
    "openal.create-context", "openal.destroy-context", "openal.make-context-current",
    "openal.open-device",
    "audio-session.initialize", "audio-session.set-active",
    "foundation.home-directory", "foundation.search-paths", "foundation.temporary-directory",
    "objc.data.empty-cache", "objc.data.empty-vtable",
    # Batch-3 promotion: every provider below now has its semantics implemented
    # in the compat runtime (portable Objective-C object model with class and
    # metaclass objects, real _objc_msgSend/_objc_msgSendSuper2/_objc_msgSend_stret
    # dispatch, _objc_setProperty ivar stores, fast-enervation mutation traps,
    # UIApplicationMain startup forwarding, the __DefaultRuneLocale guest page,
    # CoreFoundation constant-string class data, C++ SjLj personality plus
    # _Unwind_SjLj register/resume/unregister contexts, and EAGL constants as
    # real NSString objects backed by the EGL window-surface render path) and
    # each behavior is pinned by the native compat-runtime test suite.
    "objc.class.CAEAGLLayer", "objc.class.EAGLContext", "objc.class.NSAutoreleasePool",
    "objc.class.NSBundle", "objc.class.NSDictionary", "objc.class.NSNumber",
    "objc.class.NSObject", "objc.class.NSString", "objc.class.NSThread", "objc.class.NSURL",
    "objc.class.UIAccelerometer", "objc.class.UIApplication", "objc.class.UIScreen",
    "objc.class.UIView", "objc.class.UIWindow",
    "objc.metaclass.NSObject", "objc.metaclass.UIView",
    "uikit.application-main", "darwin.rune-locale",
    "sjlj.register-context", "sjlj.resume-boundary", "sjlj.unregister-context",
    "corefoundation.constant-string-class", "cxxabi.gxx-personality-sj0",
    "eagl.constant.RGB565", "eagl.constant.RGBA8", "eagl.constant.color-format",
    "eagl.constant.retained-backing",
    "objc.enumeration-mutation", "objc.msgSend", "objc.msgSendSuper2",
    "objc.msgSend.stret", "objc.setProperty",
})

VERIFIED_SEMANTICS_SYMBOLS: frozenset[str] = frozenset(
    symbol
    for symbol, provider in CONCRETE_DARWIN_COMPAT_PROVIDERS.items()
    if provider in VERIFIED_SEMANTICS_PROVIDERS
)


__all__ = [
    "CONCRETE_DARWIN_COMPAT_IMPORT_COUNT",
    "CONCRETE_DARWIN_COMPAT_PROVIDERS",
    "VERIFIED_SEMANTICS_PROVIDERS",
    "VERIFIED_SEMANTICS_SYMBOLS",
    "provider_for_symbol",
]
