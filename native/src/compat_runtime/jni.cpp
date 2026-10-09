#include "compat_runtime/audio_session_shims.hpp"
#include "compat_runtime/compat_import_catalog.hpp"
#include "compat_runtime/compiler_rt_shims.hpp"
#include "compat_runtime/cpu.hpp"
#include "compat_runtime/cxxabi_shims.hpp"
#include "compat_runtime/ndk_compat_shims.hpp"
#include "compat_runtime/ndk_full_import_catalog.hpp"
#include "compat_runtime/ndk_import_catalog.hpp"
#include "compat_runtime/darwin_compat_shims.hpp"
#include "compat_runtime/gles_shims.hpp"
#include "compat_runtime/libsystem_shims.hpp"
#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/runner.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/sjlj_unwind.hpp"
#include "compat_runtime/runtime_contract.hpp"

#include "json.hpp"

#include <jni.h>

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr jsize kMaximumMainBinaryBytes = 256 * 1024 * 1024;

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
    report["authorizationConfirmed"] = authorizationConfirmed;
    report["firstMissingImport"] = radek::Json();
    report["resolvedSymbols"] = radek::Json::array();
    report["unresolvedSymbols"] = radek::Json::array();
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
    report["inputEmbeddedInRuntimeArtifact"] = false;
    return report;
}
} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_dev_radek_compat_runtime_RuntimeBridge_runAuthorizedMainBinary(JNIEnv *env, jobject,
                                                                     jbyteArray mainBinary,
                                                                     jboolean authorizationConfirmed) {
    if (authorizationConfirmed != JNI_TRUE)
        return jsonString(env, blockedReport("User authorization was not confirmed.", false));
    if (!mainBinary)
        return jsonString(env, blockedReport("The IPA main executable is missing.", true));
    const jsize length = env->GetArrayLength(mainBinary);
    if (length <= 0 || length > kMaximumMainBinaryBytes)
        return jsonString(env, blockedReport("The IPA main executable exceeds the runtime input limit.", true));

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
            return jsonString(env, blockedReport("Concrete import provider registration failed: " +
                                                 missing.front(), true));
        }
        const auto cpu = radek::compat_runtime::createArm32CpuBackend();
        const radek::compat_runtime::GuestRunner runner(shims, *cpu);
        radek::Json report = runner.runMainBinary(bytes, true);
        report["importProviders"] = radek::Json::object();
        report["importProviders"]["concreteDarwinProviderCount"] = static_cast<std::uint64_t>(
            radek::compat_runtime::compat_import_catalog::kGuestRuntimeAdapterProviderCount);
        report["importProviders"]["guestRuntimeAdapterCatalogCount"] = static_cast<std::uint64_t>(
            radek::compat_runtime::compat_import_catalog::kGuestRuntimeAdapterProviderCount);
        report["importProviders"]["guestRuntimeAdapterRegistrationStatus"] = "COMPLETE";
        report["importProviders"]["sameNameNdkProviderCount"] = static_cast<std::uint64_t>(
            radek::compat_runtime::ndk_import_catalog::kProviderCount);
        report["importProviders"]["sameNameNdkRegistrationStatus"] = "COMPLETE";
        report["importProviders"]["sameNameNdkRegistrationPercent"] = std::uint64_t{100};
        report["importProviders"]["fullNdkCandidateInventoryCount"] = static_cast<std::uint64_t>(
            radek::compat_runtime::ndk_full_import_catalog::kProviderCount);
        report["importProviders"]["fullNdkRegisteredProviderCount"] = static_cast<std::uint64_t>(
            radek::compat_runtime::ndk_full_import_catalog::kProviderCount);
        report["importProviders"]["fullNdkCatalogStatus"] = "COMPLETE";
        report["importProviders"]["fullNdkSemanticImplementationStatus"] = "PARTIAL_TYPED_AND_GENERIC_BOUNDARIES";
        report["importProviders"]["reviewedProviderCount"] = static_cast<std::uint64_t>(
            radek::compat_runtime::compat_import_catalog::kGuestRuntimeAdapterProviderCount +
            radek::compat_runtime::ndk_import_catalog::kProviderCount);
        report["importProviders"]["boundedNdkFallbackCalloutCount"] = static_cast<std::uint64_t>(
            ndkShims.registeredCalloutCount());
        report["importProviders"]["ndkFallbackCallsObserved"] = ndkShims.callCount();
        report["importProviders"]["guestPthreadTransfersObserved"] = ndkShims.guestThreadTransferCount();
        report["importProviders"]["guestPthreadCompletionsObserved"] = ndkShims.guestThreadCompletionCount();
        report["importProviders"]["genericNdkCallsObserved"] = ndkShims.genericCallCount();
        report["importProviders"]["genericNdkProviderCount"] = static_cast<std::uint64_t>(
            ndkShims.genericProviderCount());
        report["importProviders"]["typedNdkProviderCount"] = static_cast<std::uint64_t>(
            ndkShims.typedProviderCount());
        report["importProviders"]["registrationStatus"] = "COMPLETE";
        report["importProviders"]["sameNameNdkCandidatesAreSeparate"] = true;
        report["importProviders"]["note"] =
            "The " + std::to_string(radek::compat_runtime::compat_import_catalog::kGuestRuntimeAdapterProviderCount) +
            " guest-runtime adapter names are a separate catalog from the " +
            std::to_string(radek::compat_runtime::ndk_import_catalog::kProviderCount) +
            " strict same-name Android NDK candidates and the " +
            std::to_string(radek::compat_runtime::ndk_full_import_catalog::kProviderCount) +
            " broad NDK inventory. Registration is not API-semantic completeness, a static link, or "
            "a playability claim; actual per-image import-slot results are in runtimeLinking.";
        return jsonString(env, report);
    } catch (const std::exception &error) {
        const std::string detail = std::string("Runtime initialization failed closed: ") + error.what();
        return jsonString(env, blockedReport(detail, true));
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_dev_radek_compat_runtime_RuntimeBridge_reportImportBlocked(JNIEnv *env, jobject,
                                                                jstring message,
                                                                jboolean authorizationConfirmed) {
    if (!message)
        return jsonString(env, blockedReport("IPA import failed closed.", authorizationConfirmed == JNI_TRUE));
    const char *characters = env->GetStringUTFChars(message, nullptr);
    if (!characters)
        return nullptr;
    const std::string detail(characters);
    env->ReleaseStringUTFChars(message, characters);
    return jsonString(env, blockedReport(detail, authorizationConfirmed == JNI_TRUE));
}
