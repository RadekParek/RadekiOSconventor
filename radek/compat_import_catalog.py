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
}

CONCRETE_DARWIN_COMPAT_IMPORT_COUNT = len(CONCRETE_DARWIN_COMPAT_PROVIDERS)

if CONCRETE_DARWIN_COMPAT_IMPORT_COUNT != 75:  # pragma: no cover - authoring guard
    raise AssertionError(
        "the non-NDK Darwin provider inventory must contain exactly 75 imports, "
        f"got {CONCRETE_DARWIN_COMPAT_IMPORT_COUNT}"
    )


def provider_for_symbol(symbol: str) -> str | None:
    """Return the concrete runtime provider id for an exact import spelling."""
    return CONCRETE_DARWIN_COMPAT_PROVIDERS.get(symbol)


__all__ = [
    "CONCRETE_DARWIN_COMPAT_IMPORT_COUNT",
    "CONCRETE_DARWIN_COMPAT_PROVIDERS",
    "provider_for_symbol",
]
