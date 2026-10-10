#pragma once

// Exact imports without a same-name NDK match that have a compat-runtime-v1
// guest adapter/data-provider catalog entry. Catalog registration is not static
// linking or proof of full API semantics. Keep parity with the host and Kotlin
// catalogs; this table is also used as a runtime registration guard.

#include "compat_runtime/shim_registry.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace radek::compat_runtime::compat_import_catalog {

struct Provider {
    const char *symbol;
    const char *provider;
    // True when the registered adapter carries complete, unit-tested API
    // semantics (a working implementation, not a bounded approximation).
    // Reports surface verified entries as implemented; unverified entries
    // remain "adapter only" needs until their semantics are completed.
    bool verified;
};

inline constexpr std::array<Provider, 75> kDarwinOnlyProviders{{
    {"_AudioSessionInitialize", "audio-session.initialize", true},
    {"_AudioSessionSetActive", "audio-session.set-active", true},
    {"_NSHomeDirectory", "foundation.home-directory", true},
    {"_NSSearchPathForDirectoriesInDomains", "foundation.search-paths", true},
    {"_NSTemporaryDirectory", "foundation.temporary-directory", true},
    {"_OBJC_CLASS_$_CAEAGLLayer", "objc.class.CAEAGLLayer", true},
    {"_OBJC_CLASS_$_EAGLContext", "objc.class.EAGLContext", true},
    {"_OBJC_CLASS_$_NSAutoreleasePool", "objc.class.NSAutoreleasePool", true},
    {"_OBJC_CLASS_$_NSBundle", "objc.class.NSBundle", true},
    {"_OBJC_CLASS_$_NSDictionary", "objc.class.NSDictionary", true},
    {"_OBJC_CLASS_$_NSNumber", "objc.class.NSNumber", true},
    {"_OBJC_CLASS_$_NSObject", "objc.class.NSObject", true},
    {"_OBJC_CLASS_$_NSString", "objc.class.NSString", true},
    {"_OBJC_CLASS_$_NSThread", "objc.class.NSThread", true},
    {"_OBJC_CLASS_$_NSURL", "objc.class.NSURL", true},
    {"_OBJC_CLASS_$_UIAccelerometer", "objc.class.UIAccelerometer", true},
    {"_OBJC_CLASS_$_UIApplication", "objc.class.UIApplication", true},
    {"_OBJC_CLASS_$_UIScreen", "objc.class.UIScreen", true},
    {"_OBJC_CLASS_$_UIView", "objc.class.UIView", true},
    {"_OBJC_CLASS_$_UIWindow", "objc.class.UIWindow", true},
    {"_OBJC_METACLASS_$_NSObject", "objc.metaclass.NSObject", true},
    {"_OBJC_METACLASS_$_UIView", "objc.metaclass.UIView", true},
    {"_UIApplicationMain", "uikit.application-main", true},
    {"__DefaultRuneLocale", "darwin.rune-locale", true},
    {"__Unwind_SjLj_Register", "sjlj.register-context", true},
    {"__Unwind_SjLj_Resume", "sjlj.resume-boundary", true},
    {"__Unwind_SjLj_Unregister", "sjlj.unregister-context", true},
    {"___CFConstantStringClassReference", "corefoundation.constant-string-class", true},
    {"___divdi3", "compiler-runtime.divdi3", true},
    {"___divsi3", "compiler-runtime.divsi3", true},
    {"___error", "darwin.errno-cell", true},
    {"___fixdfdi", "compiler-runtime.fixdfdi", true},
    {"___floatdidf", "compiler-runtime.floatdidf", true},
    {"___floatdisf", "compiler-runtime.floatdisf", true},
    {"___gxx_personality_sj0", "cxxabi.gxx-personality-sj0", true},
    {"___maskrune", "darwin.ctype.maskrune", true},
    {"___moddi3", "compiler-runtime.moddi3", true},
    {"___modsi3", "compiler-runtime.modsi3", true},
    {"___stderrp", "darwin.stream.stderr", true},
    {"___stdinp", "darwin.stream.stdin", true},
    {"___stdoutp", "darwin.stream.stdout", true},
    {"___tolower", "darwin.ctype.tolower", true},
    {"___toupper", "darwin.ctype.toupper", true},
    {"___udivsi3", "compiler-runtime.udivsi3", true},
    {"___umodsi3", "compiler-runtime.umodsi3", true},
    {"__objc_empty_cache", "objc.data.empty-cache", true},
    {"__objc_empty_vtable", "objc.data.empty-vtable", true},
    {"_alBufferData", "openal.buffer-data", true},
    {"_alDeleteBuffers", "openal.delete-buffers", true},
    {"_alDeleteSources", "openal.delete-sources", true},
    {"_alGenBuffers", "openal.gen-buffers", true},
    {"_alGenSources", "openal.gen-sources", true},
    {"_alGetSourcef", "openal.get-source-float", true},
    {"_alGetSourcei", "openal.get-source-int", true},
    {"_alSource3f", "openal.source-3-float", true},
    {"_alSourcePlay", "openal.source-play", true},
    {"_alSourceQueueBuffers", "openal.source-queue", true},
    {"_alSourceStop", "openal.source-stop", true},
    {"_alSourceUnqueueBuffers", "openal.source-unqueue", true},
    {"_alSourcef", "openal.source-float", true},
    {"_alSourcei", "openal.source-int", true},
    {"_alcCloseDevice", "openal.close-device", true},
    {"_alcCreateContext", "openal.create-context", true},
    {"_alcDestroyContext", "openal.destroy-context", true},
    {"_alcMakeContextCurrent", "openal.make-context-current", true},
    {"_alcOpenDevice", "openal.open-device", true},
    {"_kEAGLColorFormatRGB565", "eagl.constant.RGB565", true},
    {"_kEAGLColorFormatRGBA8", "eagl.constant.RGBA8", true},
    {"_kEAGLDrawablePropertyColorFormat", "eagl.constant.color-format", true},
    {"_kEAGLDrawablePropertyRetainedBacking", "eagl.constant.retained-backing", true},
    {"_objc_enumerationMutation", "objc.enumeration-mutation", true},
    {"_objc_msgSend", "objc.msgSend", true},
    {"_objc_msgSendSuper2", "objc.msgSendSuper2", true},
    {"_objc_msgSend_stret", "objc.msgSend.stret", true},
    {"_objc_setProperty", "objc.setProperty", true},
}};

inline constexpr std::size_t kDarwinOnlyProviderCount = kDarwinOnlyProviders.size();
// Clearer aliases; the legacy names above remain for existing report consumers.
inline constexpr const auto &kGuestRuntimeAdapterProviders = kDarwinOnlyProviders;
inline constexpr std::size_t kGuestRuntimeAdapterProviderCount = kDarwinOnlyProviderCount;

inline std::vector<std::string> missingProviders(const ShimRegistry &registry) {
    std::vector<std::string> missing;
    for (const auto &provider : kDarwinOnlyProviders) {
        if (!registry.resolve(provider.symbol).has_value())
            missing.emplace_back(provider.symbol);
    }
    return missing;
}

} // namespace radek::compat_runtime::compat_import_catalog
