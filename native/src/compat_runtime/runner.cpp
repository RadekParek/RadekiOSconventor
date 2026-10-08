#include "compat_runtime/runner.hpp"

#include "compat_runtime/runtime_contract.hpp"

#include <cstdint>
#include <exception>
#include <set>
#include <string>

namespace radek::compat_runtime {
namespace {
const char *executionStatusName(CpuExecutionStatus status) {
    switch (status) {
    case CpuExecutionStatus::Returned:
        return "RETURNED";
    case CpuExecutionStatus::BackendUnavailable:
        return "BACKEND_UNAVAILABLE";
    case CpuExecutionStatus::InvalidFunction:
        return "INVALID_FUNCTION";
    case CpuExecutionStatus::MemoryFault:
        return "MEMORY_FAULT";
    case CpuExecutionStatus::GuestExceptionRaised:
        return "GUEST_EXCEPTION_RAISED";
    case CpuExecutionStatus::ExecutionFault:
        return "EXECUTION_FAULT";
    case CpuExecutionStatus::InstructionLimit:
        return "INSTRUCTION_LIMIT";
    case CpuExecutionStatus::TimeLimit:
        return "TIME_LIMIT";
    }
    return "UNKNOWN";
}

radek::Json registerReport(const CpuRegisterState &registers) {
    radek::Json result = radek::Json::object();
    for (std::size_t index = 0; index < registers.r.size(); ++index)
        result["r" + std::to_string(index)] = static_cast<std::uint64_t>(registers.r[index]);
    result["cpsr"] = static_cast<std::uint64_t>(registers.cpsr);
    return result;
}

void appendImportArrays(radek::Json &report, const radek::Json &loader) {
    const auto resolved = loader.fields.find("resolvedSymbols");
    const auto unresolved = loader.fields.find("unresolvedSymbols");
    report["resolvedSymbols"] = resolved == loader.fields.end() ? radek::Json::array() : resolved->second;
    report["unresolvedSymbols"] = unresolved == loader.fields.end() ? radek::Json::array() : unresolved->second;
}

// Maps a fresh 8 MiB boot stack and lays out a minimal Darwin-style
// argv/envp frame so the Mach-O entry point can dereference SP for argc the
// way real process startup does. Returns false when the stack cannot be
// mapped or written; the caller must treat that as a failed boot setup.
bool prepareBootStack(GuestAddressSpace &addressSpace, CpuRegisterState &registers) {
    // Real iOS main-thread stacks are roughly 1 MiB, but this compatibility
    // boundary executes framework callbacks synchronously on the one guest CPU.
    // Several UIKit/Foundation startup continuations can remain nested while
    // the translated app reaches its first run-loop turn, so keep a generous
    // bounded stack rather than allowing a downward spill into the loader's
    // dynamic-address guard range.
    constexpr std::size_t stackSize = 32U * 1024U * 1024U;
    // Frame (all offsets from the 16-aligned SP, matching dyld's layout of
    // argc followed by argv pointers, envp pointers, then strings):
    //   SP+0:  argc = 1
    //   SP+4:  argv[0] -> program name string
    //   SP+8:  argv[1] = NULL
    //   SP+12: envp[0] = NULL
    //   SP+16: "radek-gameboot\0"
    constexpr GuestAddress frameSize = 32;
    constexpr char programName[] = "radek-gameboot";
    GuestAddress base = 0;
    try {
        base = addressSpace.mapAny(stackSize, MemoryPermission::Read | MemoryPermission::Write,
                                   "guest-stack");
    } catch (const std::exception &) {
        return false;
    }
    const GuestAddress top = base + static_cast<GuestAddress>(stackSize);
    const GuestAddress sp = (top - frameSize) & ~GuestAddress{15};
    const GuestAddress stringAddress = sp + 16;
    const std::uint32_t argc = 1;
    const std::uint32_t argv0 = stringAddress;
    const std::uint32_t nullWord = 0;
    if (!addressSpace.write(sp, &argc, sizeof(argc)) ||
        !addressSpace.write(sp + 4, &argv0, sizeof(argv0)) ||
        !addressSpace.write(sp + 8, &nullWord, sizeof(nullWord)) ||
        !addressSpace.write(sp + 12, &nullWord, sizeof(nullWord)) ||
        !addressSpace.write(stringAddress, programName, sizeof(programName)))
        return false;
    registers.r[13] = sp;
    return true;
}
} // namespace

radek::Json GuestRunner::runMainBinary(const std::vector<std::uint8_t> &mainBinary,
                                       bool authorizationConfirmed) const {
    radek::Json report = radek::Json::object();
    report["schemaVersion"] = std::uint64_t{1};
    report["runtimeContract"] = kRuntimeContract;
    report["runtimeLibrary"] = kRuntimeLibraryName;
    report["reportArtifactName"] = kRuntimeReportFileName;
    report["status"] = "not_runnable";
    report["authorizationConfirmed"] = authorizationConfirmed;
    report["inputEmbeddedInRuntimeArtifact"] = false;
    report["firstMissingImport"] = radek::Json();
    report["functionOrigin"] = radek::Json();
    report["resolvedSymbols"] = radek::Json::array();
    report["unresolvedSymbols"] = radek::Json::array();

    if (!authorizationConfirmed) {
        report["reason"] = "User authorization was not confirmed.";
        report["loader"] = radek::Json::object();
        report["cpu"] = radek::Json::object();
        report["execution"] = radek::Json::object();
        return report;
    }
    if (mainBinary.empty()) {
        report["reason"] = "IPA main executable is empty.";
        report["loader"] = radek::Json::object();
        report["cpu"] = radek::Json::object();
        report["execution"] = radek::Json::object();
        return report;
    }

    GuestAddressSpace addressSpace(memoryLimit_);
    MachOLoader loader;
    const auto load = loader.load(mainBinary, addressSpace, shims_);
    const auto loaderJson = load.toJson();
    report["loader"] = loaderJson;
    appendImportArrays(report, loaderJson);
    if (load.firstMissingImport)
        report["firstMissingImport"] = *load.firstMissingImport;

    radek::Json cpuReport = radek::Json::object();
    cpuReport["backend"] = cpu_.name();
    cpuReport["backendAvailable"] = cpu_.available();
    cpuReport["status"] = "NOT_ATTEMPTED";
    report["cpu"] = cpuReport;
    radek::Json execution = radek::Json::object();
    execution["status"] = "NOT_ATTEMPTED";
    execution["entryPointReached"] = false;
    execution["functionOrigin"] = radek::Json();
    report["execution"] = execution;

    radek::Json functionOrigin;
    if (load.entryPoint != 0) {
        functionOrigin = radek::Json::object();
        functionOrigin["kind"] = "macho-entrypoint";
        functionOrigin["sourceImage"] = "main-executable";
        functionOrigin["loadCommand"] = load.entryPointSource;
        functionOrigin["guestAddress"] = static_cast<std::uint64_t>(load.entryPoint);
        functionOrigin["instructionSet"] = load.thumb ? "Thumb" : "ARM";
        functionOrigin["selectedBackend"] = cpu_.name();
        functionOrigin["executionAttempted"] = false;
        report["functionOrigin"] = functionOrigin;
        report["execution"]["functionOrigin"] = functionOrigin;
    }

    if (!load.imageMapped() || (load.status != "LOADED" && load.status != "BLOCKED_UNRESOLVED_IMPORTS")) {
        report["reason"] = load.error.empty() ? "Mach-O main executable could not be loaded." : load.error;
        report["cpu"]["status"] = "BLOCKED_BY_LOADER";
        return report;
    }
    if (load.firstMissingImport) {
        report["reason"] = "First unresolved Darwin import: " + *load.firstMissingImport;
        report["cpu"]["status"] = "BLOCKED_BY_UNRESOLVED_IMPORT";
        return report;
    }

    auto registers = load.initialRegisters;
    try {
        if (registers.r[13] == 0) {
            const auto stack = addressSpace.mapAny(8U * 1024U * 1024U,
                MemoryPermission::Read | MemoryPermission::Write, "guest-stack");
            registers.r[13] = stack + 8U * 1024U * 1024U;
        }

        const auto bindings = shims_.snapshot();
        std::set<GuestAddress> thunkPages;
        for (const auto &binding : bindings) {
            if (binding.resolveGuestAddress)
                continue;
            const auto thunk = binding.guestAddress & ~GuestAddress{1};
            const auto page = thunk & ~GuestAddress{0xfff};
            if (thunkPages.insert(page).second)
                addressSpace.mapAt(page, 4096, MemoryPermission::Read | MemoryPermission::Execute,
                                   "native-shim-callouts");
        }
    } catch (const std::exception &error) {
        report["cpu"]["status"] = "PREPARATION_FAILED";
        report["reason"] = error.what();
        return report;
    }

    GuestFunction guestFunction;
    guestFunction.entryPoint = load.entryPoint;
    guestFunction.thumb = load.thumb;
    guestFunction.origin = "main-executable/" + load.entryPointSource;
    auto memory = addressSpace.callbacks();
    memory.invokeGuestCallout = [this, &addressSpace](GuestAddress address,
                                                      CpuRegisterState &state,
                                                      std::string &reason) {
        return shims_.invokeCallout(address, state, addressSpace, reason);
    };
    PreparedGuestFunction prepared;
    std::string preparationError;
    if (!cpu_.prepareGuestFunction(guestFunction, memory, prepared, preparationError)) {
        report["cpu"]["status"] = cpu_.available() ? "PREPARATION_FAILED" : "BACKEND_UNAVAILABLE";
        report["cpu"]["message"] = preparationError;
        report["reason"] = preparationError;
        return report;
    }
    if (prepared.origin.empty())
        prepared.origin = guestFunction.origin;
    report["cpu"]["status"] = "READY";
    report["cpu"]["backend"] = prepared.backendName;
    functionOrigin["function"] = prepared.origin;
    functionOrigin["backend"] = prepared.backendName;
    report["functionOrigin"] = functionOrigin;
    report["execution"]["functionOrigin"] = functionOrigin;

    const auto result = cpu_.executeGuestFunction(prepared, memory, registers);
    functionOrigin["executionAttempted"] = result.started;
    functionOrigin["executionStatus"] = executionStatusName(result.status);
    report["functionOrigin"] = functionOrigin;
    report["execution"]["functionOrigin"] = functionOrigin;
    report["cpu"]["status"] = executionStatusName(result.status);
    report["cpu"]["instructions"] = result.instructions;
    report["cpu"]["message"] = result.message;
    report["execution"]["status"] = executionStatusName(result.status);
    report["execution"]["entryPointReached"] = result.started;
    report["execution"]["instructions"] = result.instructions;
    report["execution"]["registers"] = registerReport(result.registers);
    report["reason"] = result.message;

    // A returned main function alone is not evidence of reaching an app menu or gameplay.
    report["status"] = "not_runnable";
    report["compatibilityStatus"] = "not_runnable";
    return report;
}

radek::Json BootAttemptRunner::run(const std::vector<std::uint8_t> &mainBinary,
                                   bool authorizationConfirmed) {
    radek::Json report = radek::Json::object();
    report["schemaVersion"] = std::uint64_t{1};
    report["runtimeContract"] = kRuntimeContract;
    report["runtimeLibrary"] = kRuntimeLibraryName;
    report["reportArtifactName"] = kRuntimeReportFileName;
    report["status"] = "not_runnable";
    report["trapMode"] = true;
    report["authorizationConfirmed"] = authorizationConfirmed;
    report["inputEmbeddedInRuntimeArtifact"] = false;
    report["firstMissingImport"] = radek::Json();
    report["trappedImport"] = radek::Json();
    report["functionOrigin"] = radek::Json();
    report["resolvedSymbols"] = radek::Json::array();
    report["unresolvedSymbols"] = radek::Json::array();
    report["trappedSymbols"] = radek::Json::array();
    report["unboundNlistSymbols"] = radek::Json::array();
    radek::Json executionPolicy = radek::Json::object();
    executionPolicy["instructionLimit"] = entryInstructionBudget_;
    executionPolicy["timeLimitMicros"] = entryTimeLimitMicros_;
    executionPolicy["mainThreadInstructionLimit"] = mainThreadInstructionBudget_;
    executionPolicy["mainThreadTimeLimitMicros"] = mainThreadTimeLimitMicros_;
    const bool unlimited = entryInstructionBudget_ == 0 && entryTimeLimitMicros_ == 0 &&
                           mainThreadInstructionBudget_ == 0 && mainThreadTimeLimitMicros_ == 0;
    executionPolicy["unlimited"] = unlimited;
    executionPolicy["deviceGameplayPolicy"] = unlimited;
    executionPolicy["note"] = unlimited
        ? "No artificial instruction or wall-clock cutoff; execution ends at a real runtime boundary."
        : "Explicit diagnostic limits are active; this is not the Android game APK policy.";
    report["executionPolicy"] = std::move(executionPolicy);

    if (!authorizationConfirmed) {
        report["reason"] = "User authorization was not confirmed.";
        report["loader"] = radek::Json::object();
        report["cpu"] = radek::Json::object();
        report["execution"] = radek::Json::object();
        return report;
    }
    if (mainBinary.empty()) {
        report["reason"] = "IPA main executable is empty.";
        report["loader"] = radek::Json::object();
        report["cpu"] = radek::Json::object();
        report["execution"] = radek::Json::object();
        return report;
    }

    GuestAddressSpace addressSpace(memoryLimit_);
    MachOLoader loader;
    const auto load = loader.loadWithTraps(mainBinary, addressSpace, shims_, traps_);
    const auto loaderJson = load.toJson();
    report["loader"] = loaderJson;
    appendImportArrays(report, loaderJson);
    const auto trapped = loaderJson.fields.find("trappedSymbols");
    if (trapped != loaderJson.fields.end())
        report["trappedSymbols"] = trapped->second;
    const auto nlist = loaderJson.fields.find("unboundNlistSymbols");
    if (nlist != loaderJson.fields.end())
        report["unboundNlistSymbols"] = nlist->second;
    if (load.firstMissingImport)
        report["firstMissingImport"] = *load.firstMissingImport;

    radek::Json cpuReport = radek::Json::object();
    cpuReport["backend"] = cpu_.name();
    cpuReport["backendAvailable"] = cpu_.available();
    cpuReport["status"] = "NOT_ATTEMPTED";
    report["cpu"] = cpuReport;
    radek::Json execution = radek::Json::object();
    execution["status"] = "NOT_ATTEMPTED";
    execution["entryPointReached"] = false;
    execution["functionOrigin"] = radek::Json();
    report["execution"] = execution;

    radek::Json functionOrigin;
    if (load.entryPoint != 0) {
        functionOrigin = radek::Json::object();
        functionOrigin["kind"] = "macho-entrypoint";
        functionOrigin["sourceImage"] = "main-executable";
        functionOrigin["loadCommand"] = load.entryPointSource;
        functionOrigin["guestAddress"] = static_cast<std::uint64_t>(load.entryPoint);
        functionOrigin["instructionSet"] = load.thumb ? "Thumb" : "ARM";
        functionOrigin["selectedBackend"] = cpu_.name();
        functionOrigin["executionAttempted"] = false;
        report["functionOrigin"] = functionOrigin;
        report["execution"]["functionOrigin"] = functionOrigin;
    }

    if (!load.imageMapped() || (load.status != "LOADED" && load.status != "LOADED_WITH_TRAPS")) {
        report["reason"] = load.error.empty() ? "Mach-O main executable could not be loaded."
                                              : load.error;
        report["cpu"]["status"] = "BLOCKED_BY_LOADER";
        return report;
    }
    if (!load.unresolvedSymbols.empty()) {
        report["reason"] = "Some imports could not be bound even to traps: " +
                           std::to_string(load.unresolvedSymbols.size()) +
                           " untrappable record(s).";
        report["cpu"]["status"] = "BLOCKED_BY_UNTRAPPABLE_IMPORT";
        return report;
    }
    // load.firstMissingImport may name a trapped import: that is expected in
    // trap mode (traps are not implementations) and does not block execution.

    auto registers = load.initialRegisters;
    try {
        // Boot mode never trusts the file's stack pointer: real dyld startup
        // always provides a fresh stack with argc/argv/envp laid out on it,
        // and the Mach-O entry point dereferences SP immediately.
        if (!prepareBootStack(addressSpace, registers)) {
            report["reason"] =
                "Boot stack setup failed: could not map or write the initial argv frame.";
            report["cpu"]["status"] = "BLOCKED_BY_STACK_SETUP";
            return report;
        }

        const auto bindings = shims_.snapshot();
        std::set<GuestAddress> thunkPages;
        for (const auto &binding : bindings) {
            if (binding.resolveGuestAddress)
                continue;
            const auto thunk = binding.guestAddress & ~GuestAddress{1};
            const auto page = thunk & ~GuestAddress{0xfff};
            if (thunkPages.insert(page).second)
                addressSpace.mapAt(page, 4096, MemoryPermission::Read | MemoryPermission::Execute,
                                   "native-shim-callouts");
        }
    } catch (const std::exception &error) {
        report["cpu"]["status"] = "PREPARATION_FAILED";
        report["reason"] = error.what();
        return report;
    }

    GuestFunction guestFunction;
    guestFunction.entryPoint = load.entryPoint;
    guestFunction.thumb = load.thumb;
    guestFunction.instructionLimit = entryInstructionBudget_;
    guestFunction.timeLimitMicros = entryTimeLimitMicros_;
    guestFunction.origin = "main-executable/" + load.entryPointSource;
    auto memory = addressSpace.callbacks();
    memory.invokeGuestCallout = [this, &addressSpace](GuestAddress address,
                                                      CpuRegisterState &state,
                                                      std::string &reason) {
        return shims_.invokeCallout(address, state, addressSpace, reason);
    };
    PreparedGuestFunction prepared;
    std::string preparationError;
    if (!cpu_.prepareGuestFunction(guestFunction, memory, prepared, preparationError)) {
        report["cpu"]["status"] = cpu_.available() ? "PREPARATION_FAILED" : "BACKEND_UNAVAILABLE";
        report["cpu"]["message"] = preparationError;
        report["reason"] = preparationError;
        return report;
    }
    if (prepared.origin.empty())
        prepared.origin = guestFunction.origin;
    report["cpu"]["status"] = "READY";
    report["cpu"]["backend"] = prepared.backendName;
    functionOrigin["function"] = prepared.origin;
    functionOrigin["backend"] = prepared.backendName;
    report["functionOrigin"] = functionOrigin;
    report["execution"]["functionOrigin"] = functionOrigin;

    // Zero is the unlimited value. The device path must leave the guest's
    // render/input loop alive instead of cancelling it after a handful of
    // synthetic startup frames. A host diagnostic may still set an explicit
    // lifecycle limit through the hook when it needs a finite probe.
    constexpr std::uint32_t kBootMainThreadFrames = 0;
    if (lifecycle_.setMainThreadServiceLimit)
        lifecycle_.setMainThreadServiceLimit(addressSpace, kBootMainThreadFrames);
    auto result = cpu_.executeGuestFunction(prepared, memory, registers);
    std::uint64_t totalInstructions = result.instructions;
    bool mainThreadEntryAttempted = false;
    radek::Json mainLoop = radek::Json::object();
    mainLoop["attempted"] = false;
    mainLoop["instructions"] = std::uint64_t{0};
    mainLoop["status"] = "NOT_REACHED";
    mainLoop["servicedFrames"] = std::uint64_t{0};
    mainLoop["frameLimit"] = static_cast<std::uint64_t>(kBootMainThreadFrames);
    // A completed startup chain may leave the app's background thread queued
    // (`+[NSThread detachNewThreadSelector:...]`). The runtime cannot start a
    // second host thread, so the queued body runs on the same guest CPU. It is
    // unlimited on the device, just like the entry, and has its own report
    // section when a diagnostic probe eventually stops it.
    if (result.status == CpuExecutionStatus::Returned && lifecycle_.prepareMainThreadEntry) {
        CpuRegisterState mainThreadRegisters = result.registers;
        GuestAddress mainThreadEntry = 0;
        std::string preparationError;
        if (lifecycle_.prepareMainThreadEntry(addressSpace, mainThreadRegisters,
                                              mainThreadEntry, preparationError)) {
            GuestFunction mainThreadFunction;
            mainThreadFunction.entryPoint = mainThreadEntry;
            mainThreadFunction.thumb = (mainThreadEntry & 1U) != 0;
            mainThreadFunction.instructionLimit = mainThreadInstructionBudget_;
            mainThreadFunction.timeLimitMicros = mainThreadTimeLimitMicros_;
            mainThreadFunction.origin = "lifecycle/background-thread-entry";
            PreparedGuestFunction preparedMainThread;
            std::string mainThreadPreparationError;
            if (cpu_.prepareGuestFunction(mainThreadFunction, memory, preparedMainThread,
                                          mainThreadPreparationError)) {
                mainThreadEntryAttempted = true;
                const auto mainThreadResult =
                    cpu_.executeGuestFunction(preparedMainThread, memory, mainThreadRegisters);
                totalInstructions += mainThreadResult.instructions;
                mainLoop["attempted"] = true;
                mainLoop["entryPoint"] = static_cast<std::uint64_t>(mainThreadEntry);
                mainLoop["status"] = executionStatusName(mainThreadResult.status);
                mainLoop["instructions"] = mainThreadResult.instructions;
                mainLoop["message"] = mainThreadResult.message;
                result = mainThreadResult;
            } else {
                mainLoop["status"] = "PREPARATION_FAILED";
                mainLoop["message"] = mainThreadPreparationError;
            }
        } else if (!preparationError.empty()) {
            mainLoop["status"] = "ENTRY_UNAVAILABLE";
            mainLoop["message"] = preparationError;
        }
    }
    if (lifecycle_.finishMainThreadEntry)
        lifecycle_.finishMainThreadEntry(addressSpace);
    functionOrigin["executionAttempted"] = result.started;
    functionOrigin["executionStatus"] = executionStatusName(result.status);
    report["functionOrigin"] = functionOrigin;
    report["execution"]["functionOrigin"] = functionOrigin;
    report["cpu"]["status"] = executionStatusName(result.status);
    report["cpu"]["instructions"] = totalInstructions;
    report["cpu"]["message"] = result.message;
    report["execution"]["status"] = executionStatusName(result.status);
    report["execution"]["entryPointReached"] = result.started;
    report["execution"]["instructions"] = totalInstructions;
    report["execution"]["registers"] = registerReport(result.registers);
    if (mainThreadEntryAttempted || mainLoop["status"].value != "NOT_REACHED")
        report["mainLoop"] = mainLoop;
    if (lifecycle_.describe)
        lifecycle_.describe(addressSpace, report);

    // Name the stopping import: a trap call records it directly; a data touch
    // inside the trap range maps back through the fault address.
    std::string trappedImport;
    if (const auto last = traps_.lastTrapped())
        trappedImport = *last;
    else if (result.hasFaultAddress) {
        if (const auto mapped = traps_.symbolForAddress(result.faultAddress))
            trappedImport = *mapped;
    }
    // Boot mode stops at the first trap callout (traps never return), so the
    // count is 0 when nothing was trapped and 1 when a trapped import
    // stopped the boot.
    report["trapCalls"] = static_cast<std::uint64_t>(trappedImport.empty() ? 0 : 1);
    if (!trappedImport.empty()) {
        report["trappedImport"] = trappedImport;
        if (result.message.find(trappedImport) == std::string::npos)
            report["reason"] = "touched unimplemented import '" + trappedImport +
                               "' (" + result.message + ")";
        else
            report["reason"] = result.message;
    } else {
        report["reason"] = result.message;
    }

    // Executed instructions are loader/CPU progress, never evidence of an app
    // menu, gameplay, or a working conversion.
    report["status"] = "not_runnable";
    report["compatibilityStatus"] = "not_runnable";
    return report;
}

} // namespace radek::compat_runtime
