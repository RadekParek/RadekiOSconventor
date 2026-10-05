#include "compat_runtime/cpu.hpp"

#include <unicorn/arm.h>
#include <unicorn/unicorn.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace radek::compat_runtime {
namespace {
constexpr std::uint32_t kPageSize = 4096;
constexpr GuestAddress kReturnSentinel = 0xeffff000;
constexpr std::uint32_t kCpsrThumbBit = 1U << 5;
constexpr std::uint32_t kCpsrModeMask = 0x1f;
constexpr std::uint32_t kCpsrUserMode = 0x10;
constexpr std::uint64_t kMaximumInstructionLimit = 100000000;
constexpr std::uint64_t kMaximumTimeLimitMicros = 60000000;

struct HookState {
    const GuestMemoryCallbacks *memory = nullptr;
    std::uint64_t instructions = 0;
    std::uint64_t instructionLimit = 0;
    bool instructionLimitHit = false;
    bool timeLimitHit = false;
    bool calloutDispatched = false;
    GuestAddress calloutResumeAddress = 0;
    bool memoryFault = false;
    std::string message;
};

uc_err writeRegister(uc_engine *engine, int identifier, std::uint32_t value) {
    return uc_reg_write(engine, identifier, &value);
}

uc_err readRegister(uc_engine *engine, int identifier, std::uint32_t &value) {
    return uc_reg_read(engine, identifier, &value);
}

bool readRegisters(uc_engine *engine, CpuRegisterState &state) {
    for (std::size_t index = 0; index < 13; ++index) {
        std::uint32_t value = 0;
        if (readRegister(engine, UC_ARM_REG_R0 + static_cast<int>(index), value) != UC_ERR_OK)
            return false;
        state.r[index] = value;
    }
    return readRegister(engine, UC_ARM_REG_SP, state.r[13]) == UC_ERR_OK &&
           readRegister(engine, UC_ARM_REG_LR, state.r[14]) == UC_ERR_OK &&
           readRegister(engine, UC_ARM_REG_PC, state.r[15]) == UC_ERR_OK &&
           readRegister(engine, UC_ARM_REG_CPSR, state.cpsr) == UC_ERR_OK;
}

bool writeRegisters(uc_engine *engine, const CpuRegisterState &state) {
    // Select the unprivileged guest register bank before writing SP/LR; otherwise
    // Unicorn would leave those values in its initial privileged bank.
    if (writeRegister(engine, UC_ARM_REG_CPSR, state.cpsr) != UC_ERR_OK)
        return false;
    for (std::size_t index = 0; index < 13; ++index) {
        if (writeRegister(engine, UC_ARM_REG_R0 + static_cast<int>(index), state.r[index]) != UC_ERR_OK)
            return false;
    }
    return writeRegister(engine, UC_ARM_REG_SP, state.r[13]) == UC_ERR_OK &&
           writeRegister(engine, UC_ARM_REG_LR, state.r[14]) == UC_ERR_OK &&
           writeRegister(engine, UC_ARM_REG_PC, state.r[15]) == UC_ERR_OK;
}

std::uint32_t unicornPermissions(MemoryPermission permissions) {
    std::uint32_t result = UC_PROT_NONE;
    if (hasPermission(permissions, MemoryPermission::Read))
        result |= UC_PROT_READ;
    if (hasPermission(permissions, MemoryPermission::Write))
        result |= UC_PROT_WRITE;
    if (hasPermission(permissions, MemoryPermission::Execute))
        result |= UC_PROT_EXEC;
    return result;
}

bool initializeEngineMemory(uc_engine *engine, const GuestMemoryCallbacks &memory,
                            std::string &reason) {
    if (!memory.regions || !memory.read) {
        reason = "ARM32 backend requires guest-region and read callbacks.";
        return false;
    }
    for (const auto &region : memory.regions()) {
        if (region.base % kPageSize != 0 || region.size == 0 || region.size % kPageSize != 0) {
            reason = "ARM32 backend requires page-aligned guest regions.";
            return false;
        }
        const auto mapped = uc_mem_map(engine, region.base, region.size,
                                       unicornPermissions(region.permissions));
        if (mapped != UC_ERR_OK) {
            reason = std::string("could not map a guest region in the ARM32 backend: ") +
                     uc_strerror(mapped);
            return false;
        }
        if (hasPermission(region.permissions, MemoryPermission::Read)) {
            std::vector<std::uint8_t> bytes(region.size);
            if (!memory.read(region.base, bytes.data(), bytes.size())) {
                reason = "guest memory callback refused to read a mapped region.";
                return false;
            }
            const auto copied = uc_mem_write(engine, region.base, bytes.data(), bytes.size());
            if (copied != UC_ERR_OK) {
                reason = std::string("could not initialize a guest region: ") + uc_strerror(copied);
                return false;
            }
        }
    }
    return true;
}

bool synchronizeEngineMemory(uc_engine *engine, const GuestMemoryCallbacks &memory,
                             std::string &reason) {
    if (!memory.regions || !memory.read)
        return false;
    for (const auto &region : memory.regions()) {
        if (!hasPermission(region.permissions, MemoryPermission::Read))
            continue;
        std::vector<std::uint8_t> bytes(region.size);
        if (!memory.read(region.base, bytes.data(), bytes.size())) {
            reason = "guest memory callback refused to synchronize a region.";
            return false;
        }
        const auto copied = uc_mem_write(engine, region.base, bytes.data(), bytes.size());
        if (copied != UC_ERR_OK) {
            reason = std::string("could not synchronize a guest region: ") + uc_strerror(copied);
            return false;
        }
    }
    return true;
}

void codeHook(uc_engine *engine, std::uint64_t address, std::uint32_t, void *userData) {
    auto &state = *static_cast<HookState *>(userData);
    if (state.instructions >= state.instructionLimit) {
        state.instructionLimitHit = true;
        state.message = "guest function reached its instruction limit.";
        (void)uc_emu_stop(engine);
        return;
    }
    ++state.instructions;
    if (address < 0xf0000000 || !state.memory || !state.memory->invokeGuestCallout)
        return;

    CpuRegisterState registers;
    if (!readRegisters(engine, registers)) {
        state.memoryFault = true;
        state.message = "ARM32 backend could not read guest registers at a shim callout.";
        (void)uc_emu_stop(engine);
        return;
    }
    std::string reason;
    const auto callout = state.memory->invokeGuestCallout(static_cast<GuestAddress>(address),
                                                           registers, reason);
    if (callout == GuestCalloutResult::NotRegistered)
        return;
    if (callout == GuestCalloutResult::Failed) {
        state.memoryFault = true;
        state.message = reason.empty() ? "native shim call adapter rejected a guest callout."
                                       : std::move(reason);
        (void)uc_emu_stop(engine);
        return;
    }

    // A successful native adapter returns through the guest LR. Preserve the ARM/Thumb
    // interworking bit and resume the backend at that return address.
    const auto returnAddress = registers.r[14];
    if ((returnAddress & 1U) != 0)
        registers.cpsr |= kCpsrThumbBit;
    else
        registers.cpsr &= ~kCpsrThumbBit;
    registers.r[15] = returnAddress & ~GuestAddress{1};
    state.calloutResumeAddress = registers.r[15] | ((registers.cpsr & kCpsrThumbBit) != 0 ? 1U : 0U);
    if (!writeRegisters(engine, registers)) {
        state.memoryFault = true;
        state.message = "ARM32 backend could not restore guest registers after a shim callout.";
        (void)uc_emu_stop(engine);
        return;
    }
    if (!synchronizeEngineMemory(engine, *state.memory, state.message)) {
        state.memoryFault = true;
        (void)uc_emu_stop(engine);
        return;
    }
    state.calloutDispatched = true;
    (void)uc_emu_stop(engine);
}

void memoryWriteHook(uc_engine *engine, uc_mem_type, std::uint64_t address, int size,
                     std::int64_t value, void *userData) {
    auto &state = *static_cast<HookState *>(userData);
    if (!state.memory || !state.memory->write || size <= 0 || size > 8) {
        state.memoryFault = true;
        state.message = "ARM32 backend produced an unsupported guest memory write.";
        (void)uc_emu_stop(engine);
        return;
    }
    std::uint64_t bits = static_cast<std::uint64_t>(value);
    std::array<std::uint8_t, 8> bytes{};
    for (int index = 0; index < size; ++index) {
        bytes[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(bits & 0xff);
        bits >>= 8;
    }
    if (!state.memory->write(static_cast<GuestAddress>(address), bytes.data(),
                             static_cast<std::size_t>(size))) {
        state.memoryFault = true;
        state.message = "guest memory callback rejected a guest write.";
        (void)uc_emu_stop(engine);
    }
}

bool invalidMemoryHook(uc_engine *, uc_mem_type, std::uint64_t address, int size,
                       std::int64_t, void *userData) {
    auto &state = *static_cast<HookState *>(userData);
    state.memoryFault = true;
    state.message = "guest memory access failed at 0x";
    constexpr char digits[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4)
        state.message.push_back(digits[(address >> shift) & 0xf]);
    state.message += " (" + std::to_string(size) + " byte(s)).";
    return false;
}

class UnicornArm32Backend final : public CpuBackend {
  public:
    const char *name() const noexcept override { return "Unicorn Engine 2.1.4 ARM32"; }
    bool available() const noexcept override { return true; }

    bool prepareGuestFunction(const GuestFunction &function, const GuestMemoryCallbacks &memory,
                              PreparedGuestFunction &prepared,
                              std::string &reason) const override {
        const auto address = function.entryPoint & ~GuestAddress{1};
        if (address == 0 || !memory.read || !memory.regions || function.instructionLimit == 0 ||
            function.instructionLimit > kMaximumInstructionLimit || function.timeLimitMicros == 0 ||
            function.timeLimitMicros > kMaximumTimeLimitMicros) {
            reason = "ARM32 guest function, memory callbacks, or execution limits are invalid.";
            return false;
        }
        std::array<std::uint8_t, 4> firstInstruction{};
        const auto requiredBytes = function.thumb ? 2U : 4U;
        if (!memory.read(address, firstInstruction.data(), requiredBytes)) {
            reason = "ARM32 guest entry is not readable in the mapped address space.";
            return false;
        }
        prepared.entryPoint = address;
        prepared.thumb = function.thumb || (function.entryPoint & 1U) != 0;
        prepared.instructionLimit = function.instructionLimit;
        prepared.timeLimitMicros = function.timeLimitMicros;
        prepared.backendName = name();
        return true;
    }

    CpuExecutionResult executeGuestFunction(const PreparedGuestFunction &function,
                                             const GuestMemoryCallbacks &memory,
                                             const CpuRegisterState &initialRegisters) const override {
        CpuExecutionResult result;
        result.registers = initialRegisters;
        uc_engine *engine = nullptr;
        const uc_mode mode = function.thumb ? UC_MODE_THUMB : UC_MODE_ARM;
        auto status = uc_open(UC_ARCH_ARM, mode, &engine);
        if (status != UC_ERR_OK) {
            result.status = CpuExecutionStatus::ExecutionFault;
            result.message = std::string("could not initialize ARM32 backend: ") + uc_strerror(status);
            return result;
        }

        std::string reason;
        if (!initializeEngineMemory(engine, memory, reason)) {
            (void)uc_close(engine);
            result.status = CpuExecutionStatus::MemoryFault;
            result.message = reason;
            return result;
        }

        CpuRegisterState registers = initialRegisters;
        registers.r[15] = function.entryPoint;
        // Return to an ARM-mode sentinel so the backend can stop uniformly even when
        // the guest entry point began in Thumb state.
        registers.r[14] = kReturnSentinel;
        registers.cpsr = (registers.cpsr & ~kCpsrModeMask) | kCpsrUserMode;
        if (function.thumb)
            registers.cpsr |= kCpsrThumbBit;
        else
            registers.cpsr &= ~kCpsrThumbBit;
        if (!writeRegisters(engine, registers)) {
            (void)uc_close(engine);
            result.status = CpuExecutionStatus::ExecutionFault;
            result.message = "could not initialize ARM32 guest registers.";
            return result;
        }

        HookState hooks;
        hooks.memory = &memory;
        hooks.instructionLimit = function.instructionLimit;
        uc_hook code = 0;
        uc_hook writes = 0;
        uc_hook invalid = 0;
        status = uc_hook_add(engine, &code, UC_HOOK_CODE,
                             reinterpret_cast<void *>(codeHook), &hooks, 1, 0);
        if (status == UC_ERR_OK)
            status = uc_hook_add(engine, &writes, UC_HOOK_MEM_WRITE,
                                 reinterpret_cast<void *>(memoryWriteHook), &hooks, 1, 0);
        if (status == UC_ERR_OK)
            status = uc_hook_add(engine, &invalid, UC_HOOK_MEM_INVALID,
                                 reinterpret_cast<void *>(invalidMemoryHook), &hooks, 1, 0);
        if (status != UC_ERR_OK) {
            (void)uc_close(engine);
            result.status = CpuExecutionStatus::ExecutionFault;
            result.message = std::string("could not install ARM32 callbacks: ") + uc_strerror(status);
            return result;
        }

        bool returned = false;
        GuestAddress resumeAddress = function.entryPoint | (function.thumb ? 1U : 0U);
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::microseconds(static_cast<std::int64_t>(function.timeLimitMicros));
        while (hooks.instructions < function.instructionLimit) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                hooks.timeLimitHit = true;
                hooks.message = "guest function reached its time limit.";
                break;
            }
            const auto remainingTime = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
            if (remainingTime <= 0) {
                hooks.timeLimitHit = true;
                hooks.message = "guest function reached its time limit.";
                break;
            }

            hooks.calloutDispatched = false;
            const auto remaining = function.instructionLimit - hooks.instructions;
            status = uc_emu_start(engine, resumeAddress, kReturnSentinel,
                                  static_cast<std::uint64_t>(remainingTime),
                                  static_cast<std::size_t>(remaining));
            if (hooks.memoryFault || hooks.instructionLimitHit)
                break;
            if (status != UC_ERR_OK) {
                hooks.message = std::string("ARM32 guest execution stopped: ") + uc_strerror(status);
                break;
            }
            std::uint32_t pc = 0;
            if (readRegister(engine, UC_ARM_REG_PC, pc) != UC_ERR_OK) {
                hooks.message = "ARM32 backend could not read the guest program counter.";
                break;
            }
            if (pc == kReturnSentinel || (pc & ~GuestAddress{1}) == kReturnSentinel) {
                returned = true;
                break;
            }
            if (!hooks.calloutDispatched) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    hooks.timeLimitHit = true;
                    hooks.message = "guest function reached its time limit.";
                } else {
                    hooks.message = "ARM32 backend stopped before the guest function returned.";
                }
                break;
            }
            if (hooks.calloutResumeAddress == 0) {
                hooks.message = "ARM32 backend lost the guest return address after a shim callout.";
                break;
            }
            resumeAddress = hooks.calloutResumeAddress;
        }

        if (!readRegisters(engine, result.registers)) {
            (void)uc_close(engine);
            result.status = CpuExecutionStatus::ExecutionFault;
            result.message = "ARM32 backend could not read the final guest register state.";
            result.started = hooks.instructions != 0;
            result.instructions = hooks.instructions;
            return result;
        }
        result.started = hooks.instructions != 0;
        result.instructions = hooks.instructions;
        if (hooks.instructionLimitHit || hooks.instructions >= function.instructionLimit) {
            result.status = CpuExecutionStatus::InstructionLimit;
            result.message = hooks.message.empty() ? "guest function reached its instruction limit." : hooks.message;
        } else if (hooks.timeLimitHit) {
            result.status = CpuExecutionStatus::TimeLimit;
            result.message = hooks.message;
        } else if (hooks.memoryFault) {
            result.status = CpuExecutionStatus::MemoryFault;
            result.message = hooks.message;
        } else if (returned) {
            result.status = CpuExecutionStatus::Returned;
        } else {
            result.status = CpuExecutionStatus::ExecutionFault;
            result.message = hooks.message.empty() ? "ARM32 backend stopped without a return." : hooks.message;
        }
        (void)uc_close(engine);
        return result;
    }
};
} // namespace

std::unique_ptr<CpuBackend> createUnicornArm32Backend() {
    return std::make_unique<UnicornArm32Backend>();
}

} // namespace radek::compat_runtime
