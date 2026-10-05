#pragma once

#include "compat_runtime/guest_memory.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace radek::compat_runtime {

struct GuestFunction {
    GuestAddress entryPoint = 0;
    bool thumb = false;
    std::uint64_t instructionLimit = 1000000;
    std::uint64_t timeLimitMicros = 1000000;
};

struct PreparedGuestFunction {
    GuestAddress entryPoint = 0;
    bool thumb = false;
    std::uint64_t instructionLimit = 1000000;
    std::uint64_t timeLimitMicros = 1000000;
    std::string backendName;
};

enum class CpuExecutionStatus {
    Returned,
    BackendUnavailable,
    InvalidFunction,
    MemoryFault,
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
