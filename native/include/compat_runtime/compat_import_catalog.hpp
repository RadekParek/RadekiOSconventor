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
};

inline constexpr std::array<Provider, 73> kDarwinOnlyProviders{{
    {"_AudioSessionInitialize", "audio-session.initialize"},
    {"_AudioSessionSetActive", "audio-session.set-active"},
    {"_NSSearchPathForDirectoriesInDomains", "foundation.search-paths"},
    {"_OBJC_CLASS_$_CAEAGLLayer", "objc.class.CAEAGLLayer"},
    {"_OBJC_CLASS_$_EAGLContext", "objc.class.EAGLContext"},
    {"_OBJC_CLASS_$_NSAutoreleasePool", "objc.class.NSAutoreleasePool"},
    {"_OBJC_CLASS_$_NSBundle", "objc.class.NSBundle"},
    {"_OBJC_CLASS_$_NSDictionary", "objc.class.NSDictionary"},
    {"_OBJC_CLASS_$_NSNumber", "objc.class.NSNumber"},
    {"_OBJC_CLASS_$_NSObject", "objc.class.NSObject"},
    {"_OBJC_CLASS_$_NSString", "objc.class.NSString"},
    {"_OBJC_CLASS_$_NSThread", "objc.class.NSThread"},
    {"_OBJC_CLASS_$_NSURL", "objc.class.NSURL"},
    {"_OBJC_CLASS_$_UIAccelerometer", "objc.class.UIAccelerometer"},
    {"_OBJC_CLASS_$_UIApplication", "objc.class.UIApplication"},
    {"_OBJC_CLASS_$_UIScreen", "objc.class.UIScreen"},
    {"_OBJC_CLASS_$_UIView", "objc.class.UIView"},
    {"_OBJC_CLASS_$_UIWindow", "objc.class.UIWindow"},
    {"_OBJC_METACLASS_$_NSObject", "objc.metaclass.NSObject"},
    {"_OBJC_METACLASS_$_UIView", "objc.metaclass.UIView"},
    {"_UIApplicationMain", "uikit.application-main"},
    {"__DefaultRuneLocale", "darwin.rune-locale"},
    {"__Unwind_SjLj_Register", "sjlj.register-context"},
    {"__Unwind_SjLj_Resume", "sjlj.resume-boundary"},
    {"__Unwind_SjLj_Unregister", "sjlj.unregister-context"},
    {"___CFConstantStringClassReference", "corefoundation.constant-string-class"},
    {"___divdi3", "compiler-runtime.divdi3"},
    {"___divsi3", "compiler-runtime.divsi3"},
    {"___error", "darwin.errno-cell"},
    {"___fixdfdi", "compiler-runtime.fixdfdi"},
    {"___floatdidf", "compiler-runtime.floatdidf"},
    {"___floatdisf", "compiler-runtime.floatdisf"},
    {"___gxx_personality_sj0", "cxxabi.gxx-personality-sj0"},
    {"___maskrune", "darwin.ctype.maskrune"},
    {"___moddi3", "compiler-runtime.moddi3"},
    {"___modsi3", "compiler-runtime.modsi3"},
    {"___stderrp", "darwin.stream.stderr"},
    {"___stdinp", "darwin.stream.stdin"},
    {"___stdoutp", "darwin.stream.stdout"},
    {"___tolower", "darwin.ctype.tolower"},
    {"___toupper", "darwin.ctype.toupper"},
    {"___udivsi3", "compiler-runtime.udivsi3"},
    {"___umodsi3", "compiler-runtime.umodsi3"},
    {"__objc_empty_cache", "objc.data.empty-cache"},
    {"__objc_empty_vtable", "objc.data.empty-vtable"},
    {"_alBufferData", "openal.buffer-data"},
    {"_alDeleteBuffers", "openal.delete-buffers"},
    {"_alDeleteSources", "openal.delete-sources"},
    {"_alGenBuffers", "openal.gen-buffers"},
    {"_alGenSources", "openal.gen-sources"},
    {"_alGetSourcef", "openal.get-source-float"},
    {"_alGetSourcei", "openal.get-source-int"},
    {"_alSource3f", "openal.source-3-float"},
    {"_alSourcePlay", "openal.source-play"},
    {"_alSourceQueueBuffers", "openal.source-queue"},
    {"_alSourceStop", "openal.source-stop"},
    {"_alSourceUnqueueBuffers", "openal.source-unqueue"},
    {"_alSourcef", "openal.source-float"},
    {"_alSourcei", "openal.source-int"},
    {"_alcCloseDevice", "openal.close-device"},
    {"_alcCreateContext", "openal.create-context"},
    {"_alcDestroyContext", "openal.destroy-context"},
    {"_alcMakeContextCurrent", "openal.make-context-current"},
    {"_alcOpenDevice", "openal.open-device"},
    {"_kEAGLColorFormatRGB565", "eagl.constant.RGB565"},
    {"_kEAGLColorFormatRGBA8", "eagl.constant.RGBA8"},
    {"_kEAGLDrawablePropertyColorFormat", "eagl.constant.color-format"},
    {"_kEAGLDrawablePropertyRetainedBacking", "eagl.constant.retained-backing"},
    {"_objc_enumerationMutation", "objc.enumeration-mutation"},
    {"_objc_msgSend", "objc.msgSend"},
    {"_objc_msgSendSuper2", "objc.msgSendSuper2"},
    {"_objc_msgSend_stret", "objc.msgSend.stret"},
    {"_objc_setProperty", "objc.setProperty"},
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
