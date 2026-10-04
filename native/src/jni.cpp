#include "ioscompat_registry.hpp"
#include "macho.hpp"
#include "radek_ios_shims.h"
#include "trivial.hpp"
#include <jni.h>
#include <dlfcn.h>
#include <array>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>

namespace {
// Public Android NDK libraries and the NDK shared C++ runtime are queried.
// Results are runtime name-resolution evidence, not proof that an iOS caller has a compatible ABI.
constexpr const char *kPublicNdkLibraries[] = {
    "libc.so",          "libm.so",          "libdl.so",         "liblog.so",
    "libandroid.so",    "libz.so",          "libEGL.so",        "libGLESv1_CM.so",
    "libGLESv2.so",     "libaaudio.so",     "libmediandk.so",   "libvulkan.so",
    "libOpenSLES.so",   "libOpenMAXAL.so",  "libjnigraphics.so", "libbinder_ndk.so",
    "libamidi.so",      "libcamera2ndk.so", "libc++_shared.so",
};

struct LibraryCache {
    std::mutex mutex;
    std::array<void *, std::size(kPublicNdkLibraries)> handles{};
    std::array<bool, std::size(kPublicNdkLibraries)> attempted{};
};

void *openPublicNdkLibrary(size_t index) {
    static LibraryCache cache;
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (!cache.attempted[index]) {
        cache.handles[index] = dlopen(kPublicNdkLibraries[index], RTLD_NOW | RTLD_LOCAL);
        cache.attempted[index] = true;
    }
    return cache.handles[index];
}

bool isCIdentifier(const char *name) {
    if (!name || !*name) return false;
    for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(name); *cursor; ++cursor) {
        const bool alpha = (*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z');
        const bool digit = *cursor >= '0' && *cursor <= '9';
        if (!alpha && !digit && *cursor != '_') return false;
    }
    return true;
}

const char *findPublicNdkLibrary(const char *symbol) {
    if (!isCIdentifier(symbol)) return nullptr;
    for (size_t index = 0; index < std::size(kPublicNdkLibraries); ++index) {
        void *handle = openPublicNdkLibrary(index);
        if (!handle) continue;
        void *address = dlsym(handle, symbol);
        if (!address) continue;
        Dl_info info{};
        if (dladdr(address, &info) == 0 || !info.dli_fname) continue;
        const char *basename = std::strrchr(info.dli_fname, '/');
        basename = basename ? basename + 1 : info.dli_fname;
        // dlsym(handle, name) can search dependencies too. dladdr prevents a
        // dependency's symbol from being attributed to the wrong target library.
        if (std::strcmp(basename, kPublicNdkLibraries[index]) == 0) {
            return kPublicNdkLibraries[index];
        }
    }
    return nullptr;
}

struct ApiReplacement {
    const char *darwinSymbol;
    const char *androidSymbol;
};

#define RADEK_API_REPLACEMENT_ROW(darwin, android) {darwin, #android},

constexpr ApiReplacement kImplementedApiReplacements[] = {
    {"_CFAbsoluteTimeGetCurrent", "CFAbsoluteTimeGetCurrent"},
    {"_CACurrentMediaTime", "CACurrentMediaTime"},
    {"_mach_absolute_time", "mach_absolute_time"},
    {"_mach_timebase_info", "mach_timebase_info"},
    RADEK_IOS_SHIM_TABLE(RADEK_API_REPLACEMENT_ROW)
};

const char *findImplementedApiReplacement(const char *darwinSymbol) {
    if (!isCIdentifier(darwinSymbol)) return nullptr;
    const char *target = nullptr;
    for (const auto &replacement : kImplementedApiReplacements) {
        if (std::strcmp(darwinSymbol, replacement.darwinSymbol) == 0) {
            target = replacement.androidSymbol;
            break;
        }
    }
    if (!target) return nullptr;

    static std::mutex mutex;
    static void *handle = nullptr;
    static bool attempted = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!attempted) {
            handle = dlopen("libioscompat.so", RTLD_NOW | RTLD_LOCAL);
            attempted = true;
        }
    }
    if (!handle) return nullptr;
    void *address = dlsym(handle, target);
    if (!address) return nullptr;
    Dl_info info{};
    if (dladdr(address, &info) == 0 || !info.dli_fname) return nullptr;
    const char *basename = std::strrchr(info.dli_fname, '/');
    basename = basename ? basename + 1 : info.dli_fname;
    // Do not report a same-named dependency or analyzer export as the shim.
    return std::strcmp(basename, "libioscompat.so") == 0 ? target : nullptr;
}
} // namespace

namespace {
constexpr jsize kMaxExecutableBytes = 256 * 1024 * 1024;
} // namespace

extern "C" JNIEXPORT jstring JNICALL Java_dev_radek_conventor_NativeBridge_analyze(JNIEnv *env, jobject,
                                                                                   jbyteArray input) {
    try {
        auto n = env->GetArrayLength(input);
        // Retain the device-memory guard on the executable bytes held in JNI;
        // symbol tables are bounded by their slice ranges/work budget instead.
        // The IPA archive itself has no fixed total-size policy cap.
        if (n > kMaxExecutableBytes)
            throw std::runtime_error("executable exceeds the on-device 256 MiB analysis limit");
        std::vector<uint8_t> b(n);
        env->GetByteArrayRegion(input, 0, n, reinterpret_cast<jbyte *>(b.data()));
        if (env->ExceptionCheck())
            return nullptr;
        return env->NewStringUTF(radek::analyze(b).dump().c_str());
    } catch (const std::exception &e) {
        env->ThrowNew(env->FindClass("java/io/IOException"), e.what());
        return nullptr;
    }
}

extern "C" JNIEXPORT jstring JNICALL Java_dev_radek_conventor_NativeBridge_analyzeCompact(JNIEnv *env, jobject,
                                                                                   jbyteArray input) {
    try {
        auto n = env->GetArrayLength(input);
        // Retain the device-memory guard on the executable bytes held in JNI;
        // symbol tables are bounded by their slice ranges/work budget instead.
        // The IPA archive itself has no fixed total-size policy cap.
        if (n > kMaxExecutableBytes)
            throw std::runtime_error("executable exceeds the on-device 256 MiB analysis limit");
        std::vector<uint8_t> b(n);
        env->GetByteArrayRegion(input, 0, n, reinterpret_cast<jbyte *>(b.data()));
        if (env->ExceptionCheck())
            return nullptr;
        return env->NewStringUTF(radek::analyze(b, false).dump().c_str());
    } catch (const std::exception &e) {
        env->ThrowNew(env->FindClass("java/io/IOException"), e.what());
        return nullptr;
    }
}

extern "C" JNIEXPORT jstring JNICALL Java_dev_radek_conventor_NativeBridge_translateTrivial(
    JNIEnv *env, jobject, jbyteArray input) {
    try {
        auto n = env->GetArrayLength(input);
        // Retain the device-memory guard on the executable bytes held in JNI;
        // symbol tables are bounded by their slice ranges/work budget instead.
        // The IPA archive itself has no fixed total-size policy cap.
        if (n > kMaxExecutableBytes)
            throw std::runtime_error("executable exceeds the on-device 256 MiB analysis limit");
        std::vector<uint8_t> b(n);
        env->GetByteArrayRegion(input, 0, n, reinterpret_cast<jbyte *>(b.data()));
        if (env->ExceptionCheck())
            return nullptr;
        return env->NewStringUTF(radek::translateTrivial(b).dump().c_str());
    } catch (const std::exception &e) {
        env->ThrowNew(env->FindClass("java/io/IOException"), e.what());
        return nullptr;
    }
}

extern "C" JNIEXPORT jstring JNICALL Java_dev_radek_conventor_NativeBridge_findAndroidLibrary(
    JNIEnv *env, jobject, jstring symbol) {
    if (!symbol) return nullptr;
    const char *name = env->GetStringUTFChars(symbol, nullptr);
    if (!name) return nullptr; // JNI has already raised an exception.
    const char *library = findPublicNdkLibrary(name);
    env->ReleaseStringUTFChars(symbol, name);
    return library ? env->NewStringUTF(library) : nullptr;
}

extern "C" JNIEXPORT jstring JNICALL Java_dev_radek_conventor_NativeBridge_findImplementedApiReplacement(
    JNIEnv *env, jobject, jstring symbol) {
    if (!symbol) return nullptr;
    const char *name = env->GetStringUTFChars(symbol, nullptr);
    if (!name) return nullptr; // JNI has already raised an exception.
    const char *target = findImplementedApiReplacement(name);
    env->ReleaseStringUTFChars(symbol, name);
    if (!target) return nullptr;
    std::string result = "libioscompat.so:";
    result += target;
    return env->NewStringUTF(result.c_str());
}

// --- Dynamic compatibility-registry bridge (libioscompat.so registry) -------
// Classification is honest by construction: "verified:<symbol>" identifies one
// of the tested implementation bodies; "stubbed:<trampoline>" identifies an
// explicitly unimplemented resolution handler and nothing else.

extern "C" JNIEXPORT jboolean JNICALL Java_dev_radek_conventor_NativeBridge_compatRegisterStub(
    JNIEnv *env, jobject, jstring symbol) {
    if (!symbol) return JNI_FALSE;
    const char *name = env->GetStringUTFChars(symbol, nullptr);
    if (!name) return JNI_FALSE; // JNI has already raised an exception.
    const bool registered = radek_compat::registerStub(name);
    env->ReleaseStringUTFChars(symbol, name);
    return registered ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jstring JNICALL Java_dev_radek_conventor_NativeBridge_compatClassify(JNIEnv *env, jobject,
                                                                                            jstring symbol) {
    if (!symbol) return nullptr;
    const char *name = env->GetStringUTFChars(symbol, nullptr);
    if (!name) return nullptr; // JNI has already raised an exception.
    const radek_compat::Record *record = radek_compat::lookup(name);
    std::string result;
    if (record) {
        result = record->kind == radek_compat::Kind::Verified ? "verified:" : "stubbed:";
        result += record->androidSymbol;
    }
    env->ReleaseStringUTFChars(symbol, name);
    return result.empty() ? nullptr : env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL Java_dev_radek_conventor_NativeBridge_compatSummary(JNIEnv *env, jobject) {
    std::string json = "{\"verifiedCount\":";
    json += std::to_string(radek_compat::verifiedCount());
    json += ",\"stubCount\":";
    json += std::to_string(radek_compat::stubCount());
    json += ",\"stubPoolSize\":";
    json += std::to_string(radek_compat::kStubPoolSize);
    json += ",\"stubCallTotal\":";
    json += std::to_string(radek_compat::stubCallTotal());
    json += "}";
    return env->NewStringUTF(json.c_str());
}
