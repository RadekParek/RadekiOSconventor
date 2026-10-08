// Host boot-attempt probe: loads one 32-bit ARM Mach-O main executable, binds
// unimplemented imports to abort-on-call traps, forwards the OpenGL ES calls to
// the host driver, and executes real guest instructions until the first
// actually-used missing import.
//
// This is a measurement tool, not a game: the JSON report keeps status
// "not_runnable" and carries the executed-instruction count plus the stopping
// import. Exit code 0 means a report was written (even when blocked); 1 means
// the probe itself failed (usage or I/O error).
#include "compat_runtime/audio_session_shims.hpp"
#include "compat_runtime/compiler_rt_shims.hpp"
#include "compat_runtime/compat_import_catalog.hpp"
#include "compat_runtime/cxxabi_shims.hpp"
#include "compat_runtime/ndk_compat_shims.hpp"
#include "compat_runtime/ndk_full_import_catalog.hpp"
#include "compat_runtime/ndk_import_catalog.hpp"
#include "compat_runtime/cpu.hpp"
#include "compat_runtime/darwin_compat_shims.hpp"
#include "compat_runtime/gles_shims.hpp"
#include "compat_runtime/libsystem_shims.hpp"
#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/runner.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/sjlj_unwind.hpp"
#include "compat_runtime/trap_shims.hpp"
#include "compat_runtime/virtual_file_system.hpp"

#include "json.hpp"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kMaximumMainBinaryBytes = 256u * 1024u * 1024u;

std::vector<std::uint8_t> readMainBinary(const char *path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        throw std::runtime_error(std::string("cannot open Mach-O input: ") + path);
    const auto size = input.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > kMaximumMainBinaryBytes)
        throw std::runtime_error("Mach-O input is empty or exceeds the 256 MiB limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char *>(bytes.data()),
                                       static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot read Mach-O input");
    return bytes;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2 || argc > 4) {
        std::cerr << "usage: radek-gameboot <macho-main-executable> [bundle-payload-directory] [--diagnostic-probe]\n"
                     "  The optional payload directory is the extracted .app directory the guest's\n"
                     "  own file reads are served from; without it every guest file access is\n"
                     "  refused with a named diagnostic instead of inventing file contents.\n"
                     "  The optional --diagnostic-probe flag adds a host-only time window so a\n"
                     "  command-line probe returns even when a real game enters its main loop.\n";
        return 1;
    }
    try {
        const char *payloadDirectory = nullptr;
        bool diagnosticProbe = false;
        for (int index = 2; index < argc; ++index) {
            if (std::string(argv[index]) == "--diagnostic-probe") {
                diagnosticProbe = true;
            } else if (payloadDirectory == nullptr) {
                payloadDirectory = argv[index];
            } else {
                std::cerr << "unknown or duplicate radek-gameboot argument: " << argv[index] << "\n";
                return 1;
            }
        }
        const auto bytes = readMainBinary(argv[1]);
        // The guest's own bundle reads are served from directories this front end
        // chose. The bundle mount is read-only; the fabricated NSHomeDirectory
        // results (Documents/Library/"~") get a writable scratch directory.
        auto &files = radek::compat_runtime::guestFileSystem();
        if (payloadDirectory != nullptr) {
            files.mount(radek::compat_runtime::bundleGuestPath(), payloadDirectory, false);
            std::string scratch = std::string(payloadDirectory) + "/../radek-home";
            files.mount("/Documents", scratch + "/Documents", true);
            files.mount("/Library", scratch + "/Library", true);
        }
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
            throw std::runtime_error(
                "concrete import provider catalog is not fully registered; first missing: " +
                missing.front());
        }
        radek::compat_runtime::TrapShimAdapter traps;
        const auto cpu = radek::compat_runtime::createArm32CpuBackend();
        radek::compat_runtime::BootAttemptRunner runner(shims, *cpu, traps,
                                                       objcShims.lifecycleHooks());
        if (diagnosticProbe) {
            // This limit belongs only to the host's JSON probe. The Android JNI
            // entry leaves both values at zero, which means unlimited execution
            // for the installed game APK.
            runner.setEntryBudget(0, 20'000'000);
            runner.setMainThreadInstructionBudget(0, 20'000'000);
        }
        radek::Json report = runner.run(bytes, true);

        // Compiler-runtime observability: how much of the guest's integer
        // division/modulo and 64-bit conversion arithmetic ran through the
        // implemented helpers.
        {
            radek::Json helpers = radek::Json::object();
            helpers["implementedSymbols"] = static_cast<std::uint64_t>(9);
            helpers["calls"] = compilerRuntime.callCount();
            helpers["basis"] =
                "ARM EABI compiler-runtime helpers (__divsi3/__modsi3/__udivsi3/__umodsi3, "
                "__divdi3/__moddi3, __floatdidf/__floatdisf/__fixdfdi); the C++ exception "
                "runtime stays fail-closed through traps";
            report["compilerRuntime"] = std::move(helpers);
        }

        // Provider observability: the strict Darwin-only catalog is checked at
        // startup, independently of the loader's later import resolution.
        {
            radek::Json providers = radek::Json::object();
            providers["concreteDarwinProviderCount"] = static_cast<std::uint64_t>(
                radek::compat_runtime::compat_import_catalog::kDarwinOnlyProviderCount);
            providers["sameNameNdkProviderCount"] = static_cast<std::uint64_t>(
                radek::compat_runtime::ndk_import_catalog::kProviderCount);
            providers["fullNdkCandidateInventoryCount"] = static_cast<std::uint64_t>(
                radek::compat_runtime::ndk_full_import_catalog::kProviderCount);
            providers["fullNdkCatalogStatus"] = "COMPLETE";
            providers["reviewedProviderCount"] = static_cast<std::uint64_t>(
                radek::compat_runtime::compat_import_catalog::kDarwinOnlyProviderCount +
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
                "Darwin-only providers are typed compatibility adapters or guest-data bindings; "
                "they are not relabelled Android NDK exports and do not prove game linkage.";
            report["importProviders"] = std::move(providers);
        }

        // Filesystem observability: which directories the guest's own file
        // reads were served from, and every access the runtime refused.
        {
            radek::Json guestFiles = radek::Json::object();
            radek::Json mounts = radek::Json::array();
            for (const auto &mount : files.mounts()) {
                radek::Json entry = radek::Json::object();
                entry["guestPath"] = mount.guestPrefix;
                entry["hostDirectory"] = mount.hostDirectory;
                entry["writable"] = mount.writable;
                mounts.push(std::move(entry));
            }
            guestFiles["mounts"] = std::move(mounts);
            guestFiles["opens"] = files.openCount();
            guestFiles["reads"] = files.readCount();
            guestFiles["bytesRead"] = files.bytesRead();
            guestFiles["writes"] = files.writeCount();
            guestFiles["bytesWritten"] = files.bytesWritten();
            guestFiles["refused"] = files.refusedCount();
            radek::Json opened = radek::Json::array();
            for (const auto &path : files.openPaths())
                opened.push(radek::Json(path));
            guestFiles["openPaths"] = std::move(opened);
            radek::Json diagnostics = radek::Json::array();
            for (const auto &diagnostic : files.diagnostics())
                diagnostics.push(radek::Json(diagnostic));
            guestFiles["diagnostics"] = std::move(diagnostics);
            guestFiles["note"] =
                "guest file reads are served from the mounted bundle payload; refused "
                "accesses are listed in diagnostics";
            report["guestFileSystem"] = std::move(guestFiles);
        }

        // GL observability: which driver was found, whether its draws are handed to
        // the real GLES implementation, and every call the runtime had to refuse.
        {
            // GL imports are bound to this per-attempt forwarder, while the
            // EAGL compatibility object owns the drawable forwarder keyed by
            // the guest address space. Keep those observations separate: a
            // guest call entering a no-driver host must not be mistaken for a
            // driver-forwarded call or a presented frame.
            const auto *drawableContext = radek::compat_runtime::gles::lastForwarder();
            const auto driver = glesForwarder.driver();
            radek::Json gles = radek::Json::object();
            gles["hostGlesDefines"] = static_cast<std::uint64_t>(1);
            gles["driverGlesLibraryLoaded"] = driver.glesLoaded;
            gles["driverEglLibraryLoaded"] = driver.eglLoaded;
            gles["driverDetail"] = driver.detail;
            gles["drawableReady"] = drawableContext != nullptr && drawableContext->drawableReady();
            gles["presentingToWindow"] = drawableContext != nullptr && drawableContext->presentingToWindow();
            gles["drawableWidth"] =
                drawableContext != nullptr ? static_cast<std::uint64_t>(drawableContext->drawableWidth()) : 0;
            gles["drawableHeight"] =
                drawableContext != nullptr ? static_cast<std::uint64_t>(drawableContext->drawableHeight()) : 0;
            gles["guestCallsObserved"] = glesForwarder.guestCallsObserved();
            gles["forwardedCalls"] = glesForwarder.forwardedCalls();
            gles["refusedCalls"] = glesForwarder.refusedCalls();
            gles["framesPresented"] =
                drawableContext != nullptr ? static_cast<std::uint64_t>(drawableContext->framesPresented()) : 0;
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
                "forwardedCalls counts calls actually handed to a host driver, and refused "
                "calls are listed in diagnostics; a rendered frame is guest output, not "
                "gameplay evidence and not a playable conversion";
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

        std::cout << report.dump() << "\n";

        const auto &fields = report.fields;
        const auto status = fields.find("status");
        const auto reason = fields.find("reason");
        const auto execution = fields.find("execution");
        std::string instructions = "?";
        if (execution != fields.end()) {
            const auto count = execution->second.fields.find("instructions");
            if (count != execution->second.fields.end())
                instructions = count->second.value;
        }
        std::cerr << "gameboot: status="
                  << (status != fields.end() ? status->second.value : "?")
                  << " instructions=" << instructions << " reason="
                  << (reason != fields.end() ? reason->second.value : "?") << "\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "gameboot failed: " << error.what() << "\n";
        return 1;
    }
}
