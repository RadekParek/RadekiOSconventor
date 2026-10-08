// JNI entry for game-runtime boot-attempt APKs (contract "game-runtime-v1").
//
// The APK embeds one authorized IPA main executable plus the game bundle. The
// launcher reads the executable from its own assets and calls here once: the
// runtime maps the image, binds unimplemented imports to abort-on-call traps,
// and executes real guest instructions until the first actually-used missing
// import. The returned JSON report keeps status "not_runnable" (a boot
// attempt alone is never gameplay evidence); the launcher keeps diagnostics
// visible if a real runtime boundary stops a guest that has not reached play.
#include "compat_runtime/audio_session_shims.hpp"
#include "compat_runtime/compiler_rt_shims.hpp"
#include "compat_runtime/darwin_compat_shims.hpp"
#include "compat_runtime/gles_shims.hpp"
#include "compat_runtime/virtual_file_system.hpp"
#include "compat_runtime/cpu.hpp"
#include "compat_runtime/libsystem_shims.hpp"
#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/runner.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/sjlj_unwind.hpp"
#include "compat_runtime/trap_shims.hpp"
#include "compat_runtime/runtime_contract.hpp"

#include "json.hpp"

#include <jni.h>

#include <android/native_window_jni.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {
constexpr jsize kMaximumMainBinaryBytes = 256 * 1024 * 1024;

// The native window acquired from the launcher's Surface. It is kept alive for
// as long as the guest may create or swap an EGL surface; a replacement window
// releases the previous one.
ANativeWindow *gAttachedWindow = nullptr;

void attachSurface(JNIEnv *env, jobject surface) {
    ANativeWindow *window = surface != nullptr ? ANativeWindow_fromSurface(env, surface) : nullptr;
    if (window == gAttachedWindow) {
        if (window != nullptr)
            ANativeWindow_release(window);
        return;
    }
    radek::compat_runtime::gles::setDefaultNativeWindow(window);
    if (gAttachedWindow != nullptr)
        ANativeWindow_release(gAttachedWindow);
    gAttachedWindow = window;
}

// The launcher extracts assets/bundle/** to its files directory and passes the
// app directory here. Mounts mirror the host probe: the bundle payload is
// read-only, and the two writable application directories are created by the
// launcher next to it.
void mountGuestPayload(const std::string &payloadDirectory) {
    if (payloadDirectory.empty())
        return;
    auto &files = radek::compat_runtime::guestFileSystem();
    files.mount(radek::compat_runtime::bundleGuestPath(), payloadDirectory, false);
    const std::string home = payloadDirectory + "/../radek-home";
    files.mount("/Documents", home + "/Documents", true);
    files.mount("/Library", home + "/Library", true);
}

jstring jsonString(JNIEnv *env, const radek::Json &json) {
    const std::string text = json.dump();
    return env->NewStringUTF(text.c_str());
}

radek::Json blockedReport(const std::string &message, bool authorizationConfirmed) {
    radek::Json report = radek::Json::object();
    report["schemaVersion"] = std::uint64_t{1};
    report["runtimeContract"] = radek::compat_runtime::kRuntimeContract;
    report["runtimeLibrary"] = radek::compat_runtime::kRuntimeLibraryName;
    report["reportArtifactName"] = radek::compat_runtime::kRuntimeReportFileName;
    report["status"] = "not_runnable";
    report["trapMode"] = true;
    report["authorizationConfirmed"] = authorizationConfirmed;
    report["firstMissingImport"] = radek::Json();
    report["trappedImport"] = radek::Json();
    report["resolvedSymbols"] = radek::Json::array();
    report["unresolvedSymbols"] = radek::Json::array();
    report["trappedSymbols"] = radek::Json::array();
    report["unboundNlistSymbols"] = radek::Json::array();
    report["loader"] = radek::Json::object();
    report["loader"]["status"] = "NOT_ATTEMPTED";
    report["loader"]["imageMapped"] = false;
    report["cpu"] = radek::Json::object();
    report["cpu"]["status"] = "NOT_ATTEMPTED";
    report["execution"] = radek::Json::object();
    report["execution"]["status"] = "NOT_ATTEMPTED";
    report["execution"]["entryPointReached"] = false;
    report["reason"] = message;
    report["message"] = message;
    report["inputEmbeddedInRuntimeArtifact"] = true;
    return report;
}
} // namespace

// The launcher hands its SurfaceView's surface over before (or while) the boot
// attempt runs, so the guest's EAGL drawable can present through EGL on screen.
extern "C" JNIEXPORT void JNICALL
Java_dev_radek_gameruntime_GameBootActivity_setGameSurface(JNIEnv *env, jclass, jobject surface) {
    try {
        attachSurface(env, surface);
    } catch (...) {
        // A surface the runtime cannot use never fails the boot attempt; the
        // GL layer reports the missing drawable through its own diagnostics.
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_radek_gameruntime_GameBootActivity_runGameBootAttempt(JNIEnv *env, jobject,
                                                               jbyteArray mainBinary,
                                                               jstring payloadDirectory,
                                                               jboolean authorizationConfirmed) {
    if (authorizationConfirmed != JNI_TRUE)
        return jsonString(env, blockedReport("User authorization was not confirmed.", false));
    if (!mainBinary)
        return jsonString(env, blockedReport("The embedded game executable is missing.", true));
    const jsize length = env->GetArrayLength(mainBinary);
    if (length <= 0 || length > kMaximumMainBinaryBytes)
        return jsonString(env, blockedReport("The embedded game executable exceeds the runtime input limit.", true));

    try {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        env->GetByteArrayRegion(mainBinary, 0, length, reinterpret_cast<jbyte *>(bytes.data()));
        if (env->ExceptionCheck())
            return nullptr;
        radek::compat_runtime::ShimRegistry shims;
        radek::compat_runtime::objc::ShimAdapter objcShims;
        radek::compat_runtime::libsystem::ShimAdapter libsystemShims;
        radek::compat_runtime::audio::ShimAdapter audioShims;
        radek::compat_runtime::SjLjUnwindAdapter sjljUnwind;
        radek::compat_runtime::compiler_rt::ShimAdapter compilerRuntime;
        radek::compat_runtime::gles::Forwarder glesForwarder;
        radek::compat_runtime::darwin_compat::ShimAdapter darwinShims(&objcShims);
        objcShims.registerBindings(shims);
        libsystemShims.registerBindings(shims);
        audioShims.registerBindings(shims);
        sjljUnwind.registerBindings(shims);
        compilerRuntime.registerBindings(shims);
        // Guest OpenGL ES 1.1 calls go to the platform's EGL/GLES driver through
        // the launcher's Surface; nothing is rasterized in this process.
        glesForwarder.registerBindings(shims);
        darwinShims.registerBindings(shims);

        std::string payload;
        if (payloadDirectory != nullptr) {
            const char *utf = env->GetStringUTFChars(payloadDirectory, nullptr);
            if (utf != nullptr) {
                payload = utf;
                env->ReleaseStringUTFChars(payloadDirectory, utf);
            }
            if (env->ExceptionCheck())
                return nullptr;
        }
        mountGuestPayload(payload);
        auto &files = radek::compat_runtime::guestFileSystem();

        radek::compat_runtime::TrapShimAdapter traps;
        const auto cpu = radek::compat_runtime::createArm32CpuBackend();
        radek::compat_runtime::BootAttemptRunner runner(shims, *cpu, traps,
                                                       objcShims.lifecycleHooks());
        radek::Json report = runner.run(bytes, true);

        // GL observability: which driver was found, whether the drawable was
        // handed to the platform, and every call the runtime had to refuse.
        {
            radek::Json gles = radek::Json::object();
            const auto driver = glesForwarder.driver();
            gles["driverGlesLibraryLoaded"] = driver.glesLoaded;
            gles["driverEglLibraryLoaded"] = driver.eglLoaded;
            gles["driverDetail"] = driver.detail;
            gles["drawableReady"] = glesForwarder.drawableReady();
            gles["presentingToWindow"] = glesForwarder.presentingToWindow();
            gles["drawableWidth"] = static_cast<std::uint64_t>(glesForwarder.drawableWidth());
            gles["drawableHeight"] = static_cast<std::uint64_t>(glesForwarder.drawableHeight());
            gles["forwardedCalls"] = static_cast<std::uint64_t>(glesForwarder.forwardedCalls());
            gles["refusedCalls"] = static_cast<std::uint64_t>(glesForwarder.refusedCalls());
            gles["framesPresented"] = static_cast<std::uint64_t>(glesForwarder.framesPresented());
            radek::Json diagnostics = radek::Json::array();
            for (const auto &diagnostic : glesForwarder.diagnostics())
                diagnostics.push(radek::Json(diagnostic));
            gles["diagnostics"] = std::move(diagnostics);
            gles["note"] =
                "guest OpenGL ES calls are forwarded to the platform EGL/GLES driver and "
                "presented on the launcher's surface; refused calls are listed in "
                "diagnostics; a rendered frame is guest output, not gameplay evidence";
            report["gles"] = std::move(gles);
        }

        // Darwin-only translation layer: names Android does not ship get an
        // explicit, individually reported adapter instead of a trap.
        {
            radek::Json compat = radek::Json::object();
            compat["boundSymbols"] = static_cast<std::uint64_t>(darwinShims.boundSymbolCount());
            compat["ctypeCalls"] = darwinShims.ctypeCalls();
            compat["openalCalls"] = darwinShims.openalCalls();
            compat["streamCells"] = darwinShims.streamCellCount();
            compat["personalityBoundaries"] = darwinShims.personalityBoundaries();
            radek::Json diagnostics = radek::Json::array();
            for (const auto &diagnostic : darwinShims.diagnostics())
                diagnostics.push(radek::Json(diagnostic));
            compat["diagnostics"] = std::move(diagnostics);
            compat["note"] =
                "Darwin-only imports with no Android system export are served by explicit "
                "minimal adapters: real process-stream cells, ASCII C-locale ctype, real "
                "NSString EAGL keys, a guest errno cell, a state-only OpenAL subset and a "
                "fail-closed SJLJ personality boundary; none of them is a same-name NDK export";
            report["darwinCompat"] = std::move(compat);
        }

        // Filesystem observability: which directories served the guest's own file
        // reads and every access the runtime refused.
        {
            radek::Json guestFiles = radek::Json::object();
            radek::Json mounts = radek::Json::array();
            for (const auto &mount : files.mounts()) {
                radek::Json entry = radek::Json::object();
                entry["guestPath"] = mount.guestPrefix;
                entry["writable"] = mount.writable;
                mounts.push(std::move(entry));
            }
            guestFiles["mounts"] = std::move(mounts);
            guestFiles["opens"] = static_cast<std::uint64_t>(files.openCount());
            guestFiles["reads"] = static_cast<std::uint64_t>(files.readCount());
            guestFiles["bytesRead"] = static_cast<std::uint64_t>(files.bytesRead());
            guestFiles["refused"] = static_cast<std::uint64_t>(files.refusedCount());
            radek::Json diagnostics = radek::Json::array();
            for (const auto &diagnostic : files.diagnostics())
                diagnostics.push(radek::Json(diagnostic));
            guestFiles["diagnostics"] = std::move(diagnostics);
            guestFiles["note"] =
                "guest file reads are served from the launcher-extracted bundle payload; "
                "refused accesses are listed in diagnostics";
            report["guestFileSystem"] = std::move(guestFiles);
        }

        return jsonString(env, report);
    } catch (const std::exception &error) {
        const std::string detail = std::string("Runtime initialization failed closed: ") + error.what();
        return jsonString(env, blockedReport(detail, true));
    }
}
