#pragma once

#include "compat_runtime/guest_memory.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace radek::compat_runtime {

struct GuestFunction {
    GuestAddress entryPoint = 0;
    bool thumb = false;
    // Zero means unlimited. The device game-runtime path deliberately does not
    // stop a running game after an arbitrary instruction count or wall-clock
    // interval; callers that need a finite diagnostic probe can opt in with
    // setEntryBudget()/setMainThreadInstructionBudget().
    std::uint64_t instructionLimit = 0;
    std::uint64_t timeLimitMicros = 0;
    std::string origin;
};

struct PreparedGuestFunction {
    GuestAddress entryPoint = 0;
    bool thumb = false;
    // Preserve the zero-is-unlimited execution policy across backend preparation.
    std::uint64_t instructionLimit = 0;
    std::uint64_t timeLimitMicros = 0;
    std::string backendName;
    std::string origin;
};

enum class CpuExecutionStatus {
    Returned,
    BackendUnavailable,
    InvalidFunction,
    MemoryFault,
    GuestExceptionRaised,
    ExecutionFault,
    InstructionLimit,
    TimeLimit,
};

struct CpuExecutionResult {
    CpuExecutionStatus status = CpuExecutionStatus::ExecutionFault;
    CpuRegisterState registers;
    std::uint64_t instructions = 0;
    bool started = false;
    std::string message;
    // Guest address of an invalid memory access, when the backend observed
    // one. Lets trap-mode callers map the fault back to an import slot.
    GuestAddress faultAddress = 0;
    bool hasFaultAddress = false;
};

/** The single CPU boundary used by the guest runner. */
class CpuBackend {
  public:
    virtual ~CpuBackend() = default;
    virtual const char *name() const noexcept = 0;
    virtual bool available() const noexcept = 0;
    virtual bool prepareGuestFunction(const GuestFunction &function,
                                      const GuestMemoryCallbacks &memory,
                                      PreparedGuestFunction &prepared,
                                      std::string &reason) const = 0;
    virtual CpuExecutionResult executeGuestFunction(const PreparedGuestFunction &function,
                                                     const GuestMemoryCallbacks &memory,
                                                     const CpuRegisterState &registers) const = 0;
};

std::unique_ptr<CpuBackend> createArm32CpuBackend();

} // namespace radek::compat_runtime
