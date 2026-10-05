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
    report["execution"] = execution;

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
            const auto stack = addressSpace.mapAny(64 * 1024,
                MemoryPermission::Read | MemoryPermission::Write, "guest-stack");
            registers.r[13] = stack + 64 * 1024;
        }

        const auto bindings = shims_.snapshot();
        std::set<GuestAddress> thunkPages;
        for (const auto &binding : bindings) {
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
    report["cpu"]["status"] = "READY";
    report["cpu"]["backend"] = prepared.backendName;

    const auto result = cpu_.executeGuestFunction(prepared, memory, registers);
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

} // namespace radek::compat_runtime
