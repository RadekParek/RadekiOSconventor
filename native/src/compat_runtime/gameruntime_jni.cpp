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
#include "compat_runtime/compat_import_catalog.hpp"
#include "compat_runtime/cxxabi_shims.hpp"
#include "compat_runtime/ndk_compat_shims.hpp"
#include "compat_runtime/ndk_full_import_catalog.hpp"
#include "compat_runtime/ndk_import_catalog.hpp"
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
#include <stdexcept>
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

// Bundle assets are read-only; guest user data is mounted to the app-specific
// Android/data/<package>/files tree created by the launcher. OBB storage is an
// optional read-only mount: this package carries its bundle in APK assets and
// therefore does not synthesize or require an expansion OBB.
void mountGuestPayload(const std::string &payloadDirectory,
                       const std::string &appDataDirectory,
                       const std::string &obbDirectory) {
    auto &files = radek::compat_runtime::guestFileSystem();
    if (!payloadDirectory.empty())
        files.mount(radek::compat_runtime::bundleGuestPath(), payloadDirectory, false);

    const std::string dataRoot = !appDataDirectory.empty()
        ? appDataDirectory
        : payloadDirectory.empty() ? std::string() : payloadDirectory + "/../radek-home";
    if (!dataRoot.empty()) {
        files.mount("/Documents", dataRoot + "/Documents", true);
        files.mount("/Library", dataRoot + "/Library", true);
        files.mount("/tmp", dataRoot + "/tmp", true);
    }
    if (!obbDirectory.empty())
        files.mount("/Android/obb", obbDirectory, false);
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
                                                               jstring appDataDirectory,
                                                               jstring obbDirectory,
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
        radek::compat_runtime::cxxabi::ShimAdapter cxxAbi;
        radek::compat_runtime::ndk::ShimAdapter ndkShims;
        radek::compat_runtime::gles::Forwarder glesForwarder;
        radek::compat_runtime::darwin_compat::ShimAdapter darwinShims(&objcShims);
        objcShims.registerBindings(shims);
        libsystemShims.registerBindings(shims);
        audioShims.registerBindings(shims);
        sjljUnwind.registerBindings(shims);
        compilerRuntime.registerBindings(shims);
        cxxAbi.registerBindings(shims);
        // Guest OpenGL ES 1.1 calls go to the platform's EGL/GLES driver through
        // the launcher's Surface; nothing is rasterized in this process.
        glesForwarder.registerBindings(shims);
        darwinShims.registerBindings(shims);
        ndkShims.registerBindings(shims);
        const auto missingDarwinProviders =
            radek::compat_runtime::compat_import_catalog::missingProviders(shims);
        const auto missingNdkProviders =
            radek::compat_runtime::ndk_import_catalog::missingProviders(shims);
        const auto missingFullNdkProviders =
            radek::compat_runtime::ndk_full_import_catalog::missingProviders(shims);
        if (!missingDarwinProviders.empty() || !missingNdkProviders.empty() ||
            !missingFullNdkProviders.empty()) {
            const auto &missing = !missingDarwinProviders.empty() ? missingDarwinProviders
                                  : !missingNdkProviders.empty() ? missingNdkProviders
                                                                  : missingFullNdkProviders;
            throw std::runtime_error(
                "concrete import provider catalog is not fully registered; first missing: " +
                missing.front());
        }

        const auto readJavaString = [env](jstring value) {
            std::string result;
            if (value == nullptr)
                return result;
            const char *utf = env->GetStringUTFChars(value, nullptr);
            if (utf != nullptr) {
                result = utf;
                env->ReleaseStringUTFChars(value, utf);
            }
            return result;
        };
        const std::string payload = readJavaString(payloadDirectory);
        const std::string appData = readJavaString(appDataDirectory);
        const std::string obb = readJavaString(obbDirectory);
        if (env->ExceptionCheck())
            return nullptr;
        mountGuestPayload(payload, appData, obb);
        auto &files = radek::compat_runtime::guestFileSystem();

        radek::compat_runtime::TrapShimAdapter traps;
        const auto cpu = radek::compat_runtime::createArm32CpuBackend();
        radek::compat_runtime::BootAttemptRunner runner(shims, *cpu, traps,
                                                       objcShims.lifecycleHooks());
        radek::Json report = runner.run(bytes, true);
        {
            radek::Json storage = radek::Json::object();
            storage["appDataDirectory"] = appData;
            storage["obbDirectory"] = obb.empty() ? radek::Json() : radek::Json(obb);
            storage["bundleAssetsInApk"] = true;
            storage["expansionObbRequired"] = false;
            storage["guestWritableMounts"] = radek::Json::array();
            for (const auto *path : {"/Documents", "/Library", "/tmp"})
                storage["guestWritableMounts"].push(radek::Json(path));
            storage["obbGuestMount"] = obb.empty() ? radek::Json() : radek::Json("/Android/obb (read-only)");
            report["appStorage"] = std::move(storage);
        }

        // Runtime helper registration/call counters complement the import
        // mapper's candidate count; they do not imply a static toolchain link.
        {
            radek::Json helpers = radek::Json::object();
            helpers["registeredSymbolCount"] = static_cast<std::uint64_t>(compilerRuntime.registeredSymbolCount());
            helpers["callsObserved"] = compilerRuntime.callCount();
            helpers["note"] =
                "The registered ARM32 compiler-runtime arithmetic helpers are guest callouts. "
                "This does not provide a libgcc_s.so alias or static NDK/libunwind link; C++ "
                "personality and landing-pad transfer remain separate runtime boundaries.";
            report["compilerRuntime"] = std::move(helpers);
        }

        // GL observability: which driver was found, whether the drawable was
        // handed to the platform, and every call the runtime had to refuse.
        {
            radek::Json gles = radek::Json::object();
            const auto driver = glesForwarder.driver();
            const auto *drawableContext = radek::compat_runtime::gles::lastForwarder();
            gles["driverGlesLibraryLoaded"] = driver.glesLoaded;
            gles["driverEglLibraryLoaded"] = driver.eglLoaded;
            gles["driverDetail"] = driver.detail;
            gles["drawableReady"] = drawableContext != nullptr && drawableContext->drawableReady();
            gles["presentingToWindow"] = drawableContext != nullptr && drawableContext->presentingToWindow();
            gles["drawableWidth"] = drawableContext != nullptr
                ? static_cast<std::uint64_t>(drawableContext->drawableWidth()) : 0;
            gles["drawableHeight"] = drawableContext != nullptr
                ? static_cast<std::uint64_t>(drawableContext->drawableHeight()) : 0;
            gles["guestCallsObserved"] = static_cast<std::uint64_t>(glesForwarder.guestCallsObserved());
            gles["forwardedCalls"] = static_cast<std::uint64_t>(glesForwarder.forwardedCalls());
            gles["refusedCalls"] = static_cast<std::uint64_t>(glesForwarder.refusedCalls());
            gles["framesPresented"] = drawableContext != nullptr
                ? static_cast<std::uint64_t>(drawableContext->framesPresented()) : 0;
            radek::Json diagnostics = radek::Json::array();
            for (const auto &diagnostic : glesForwarder.diagnostics())
                diagnostics.push(radek::Json(diagnostic));
            if (drawableContext != nullptr && drawableContext != &glesForwarder) {
                for (const auto &diagnostic : drawableContext->diagnostics())
                    diagnostics.push(radek::Json(diagnostic));
            }
            gles["diagnostics"] = std::move(diagnostics);
            gles["note"] =
                "guestCallsObserved counts guest imports entering the compatibility layer; "
                "forwardedCalls counts calls handed to the platform EGL/GLES driver, and "
                "refused calls are listed in diagnostics; a rendered frame is guest output, "
                "not gameplay evidence";
            report["gles"] = std::move(gles);
        }

        // Non-same-name guest imports in the compat-runtime catalog get an
        // explicit adapter/data binding instead of a trap where supported.
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
                "Non-same-name imports in the guest adapter catalog are served by explicit "
                "minimal adapters where supported: process-stream and errno cells, Darwin "
                "ctype, Objective-C/EAGL data, state-only OpenAL and compiler-runtime "
                "boundaries. Catalog registration is not proof of complete API semantics, "
                "static Android linking, or successful per-image slot fixup.";
            report["darwinCompat"] = std::move(compat);
        }

        // Same-name NDK candidates and guest-runtime adapter catalog entries
        // are separate provider inventories. Both were checked before loading.
        {
            radek::Json providers = radek::Json::object();
            providers["concreteDarwinProviderCount"] = static_cast<std::uint64_t>(
                radek::compat_runtime::compat_import_catalog::kGuestRuntimeAdapterProviderCount);
            providers["guestRuntimeAdapterCatalogCount"] = static_cast<std::uint64_t>(
                radek::compat_runtime::compat_import_catalog::kGuestRuntimeAdapterProviderCount);
            providers["guestRuntimeAdapterRegistrationStatus"] = "COMPLETE";
            providers["sameNameNdkProviderCount"] = static_cast<std::uint64_t>(
                radek::compat_runtime::ndk_import_catalog::kProviderCount);
            providers["sameNameNdkRegistrationStatus"] = "COMPLETE";
            providers["sameNameNdkRegistrationPercent"] = std::uint64_t{100};
            providers["fullNdkCandidateInventoryCount"] = static_cast<std::uint64_t>(
                radek::compat_runtime::ndk_full_import_catalog::kProviderCount);
            providers["fullNdkRegisteredProviderCount"] = static_cast<std::uint64_t>(
                radek::compat_runtime::ndk_full_import_catalog::kProviderCount);
            providers["fullNdkCatalogStatus"] = "COMPLETE";
            providers["fullNdkSemanticImplementationStatus"] = "PARTIAL_TYPED_AND_GENERIC_BOUNDARIES";
            providers["reviewedProviderCount"] = static_cast<std::uint64_t>(
                radek::compat_runtime::compat_import_catalog::kGuestRuntimeAdapterProviderCount +
                radek::compat_runtime::ndk_import_catalog::kProviderCount);
            providers["boundedNdkFallbackCalloutCount"] = static_cast<std::uint64_t>(
                ndkShims.registeredCalloutCount());
            providers["ndkFallbackCallsObserved"] = ndkShims.callCount();
            providers["guestPthreadTransfersObserved"] = ndkShims.guestThreadTransferCount();
            providers["guestPthreadCompletionsObserved"] = ndkShims.guestThreadCompletionCount();
            providers["genericNdkCallsObserved"] = ndkShims.genericCallCount();
            providers["genericNdkProviderCount"] = static_cast<std::uint64_t>(
                ndkShims.genericProviderCount());
            providers["typedNdkProviderCount"] = static_cast<std::uint64_t>(
                ndkShims.typedProviderCount());
            providers["registrationStatus"] = "COMPLETE";
            providers["sameNameNdkCandidatesAreSeparate"] = true;
            providers["note"] =
                "Registration is complete for the " +
                std::to_string(radek::compat_runtime::ndk_import_catalog::kProviderCount) +
                "-name strict same-name NDK catalog, the " +
                std::to_string(radek::compat_runtime::compat_import_catalog::kGuestRuntimeAdapterProviderCount) +
                "-name guest-runtime adapter catalog, and the " +
                std::to_string(radek::compat_runtime::ndk_full_import_catalog::kProviderCount) +
                "-name broad NDK candidate inventory. This is catalog registration, not API-semantic "
                "completeness; generic bounded adapters remain for many NDK candidates. Guest adapter "
                "catalog presence does not prove a per-image slot fixup. Actual bind/relocation results "
                "and provider names appear separately under runtimeLinking.";
            report["importProviders"] = std::move(providers);
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
