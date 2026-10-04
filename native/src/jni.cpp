#include "macho.hpp"
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

constexpr ApiReplacement kImplementedApiReplacements[] = {
    {"_CFAbsoluteTimeGetCurrent", "CFAbsoluteTimeGetCurrent"},
    {"_CACurrentMediaTime", "CACurrentMediaTime"},
    {"_mach_absolute_time", "mach_absolute_time"},
    {"_mach_timebase_info", "mach_timebase_info"},
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

extern "C" JNIEXPORT jstring JNICALL Java_dev_radek_conventor_NativeBridge_analyze(JNIEnv *env, jobject,
                                                                                   jbyteArray input) {
    try {
        auto n = env->GetArrayLength(input);
        if (n > 64 * 1024 * 1024)
            throw std::runtime_error("executable exceeds on-device 64 MiB limit");
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
