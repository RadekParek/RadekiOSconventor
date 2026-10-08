#include "compat_runtime/cpu.hpp"

#include <unicorn/arm.h>
#include <unicorn/unicorn.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
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
// An execution limit of zero is intentionally unlimited. The Android launcher
// must be able to keep the guest's main loop alive for gameplay; the host
// diagnostic probe can still provide a finite opt-in time window through the
// runner. Do not add a backend maximum here: it would silently recreate the
// arbitrary stop that the game path is designed to avoid.
// The iOS kernel hands user code a fully enabled VFP unit: CPACR grants
// CP10/CP11 to EL0 and FPEXC.EN is set. Unicorn starts from a bare CPU, so a
// guest `vpush`/`vmov`/`vadd` would otherwise decode as an invalid instruction
// instead of executing. Enable the same state the device provides.
constexpr std::uint32_t kVfpAccessControlRegister = 0x00F00000U; // CPACR CP10/CP11 full access
constexpr std::uint32_t kVfpEnableBit = 1U << 30;                // FPEXC.EN

struct MappedRegion {
    std::size_t size = 0;
    MemoryPermission permissions = MemoryPermission::None;
};

struct HookState {
    const GuestMemoryCallbacks *memory = nullptr;
    std::uint64_t instructions = 0;
    std::uint64_t instructionLimit = 0;
    bool instructionLimitHit = false;
    bool timeLimitHit = false;
    bool calloutDispatched = false;
    bool guestExceptionRaised = false;
    GuestAddress calloutResumeAddress = 0;
    std::map<GuestAddress, MappedRegion> mappedRegions;
    bool memoryFault = false;
    bool hasFaultAddress = false;
    GuestAddress faultAddress = 0;
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
    if (readRegister(engine, UC_ARM_REG_SP, state.r[13]) != UC_ERR_OK ||
        readRegister(engine, UC_ARM_REG_LR, state.r[14]) != UC_ERR_OK ||
        readRegister(engine, UC_ARM_REG_PC, state.r[15]) != UC_ERR_OK ||
        readRegister(engine, UC_ARM_REG_CPSR, state.cpsr) != UC_ERR_OK)
        return false;
    for (std::size_t index = 0; index < state.d.size(); ++index) {
        if (uc_reg_read(engine, UC_ARM_REG_D0 + static_cast<int>(index), &state.d[index]) != UC_ERR_OK)
            return false;
    }
    return readRegister(engine, UC_ARM_REG_FPSCR, state.fpscr) == UC_ERR_OK;
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
    if (writeRegister(engine, UC_ARM_REG_SP, state.r[13]) != UC_ERR_OK ||
        writeRegister(engine, UC_ARM_REG_LR, state.r[14]) != UC_ERR_OK ||
        writeRegister(engine, UC_ARM_REG_PC, state.r[15]) != UC_ERR_OK)
        return false;
    for (std::size_t index = 0; index < state.d.size(); ++index) {
        if (uc_reg_write(engine, UC_ARM_REG_D0 + static_cast<int>(index), &state.d[index]) != UC_ERR_OK)
            return false;
    }
    return writeRegister(engine, UC_ARM_REG_FPSCR, state.fpscr) == UC_ERR_OK;
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
                            std::map<GuestAddress, MappedRegion> &mappedRegions,
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
        mappedRegions.emplace(region.base, MappedRegion{region.size, region.permissions});
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
    // The engine now owns a byte-identical copy, so nothing is pending upload.
    if (memory.clearHostWrites)
        memory.clearHostWrites();
    return true;
}

// Uploads only the pages a shim wrote into the host copy since the previous
// call. Without this the backend would re-copy the whole address space after
// every shim call, which dominates boot time (measured: 19.8 s of a 20 s boot
// in 448 full copies) and makes longer guest runs impossible.
bool uploadHostWrites(uc_engine *engine, const GuestMemoryCallbacks &memory, std::string &reason) {
    if (!memory.drainHostWrites)
        return true;
    const auto uploaded = memory.drainHostWrites(
        [engine](GuestAddress address, const std::uint8_t *bytes, std::size_t size) {
            return uc_mem_write(engine, address, bytes, size) == UC_ERR_OK;
        });
    if (!uploaded) {
        reason = "could not upload a host-written guest page into the ARM32 backend.";
        return false;
    }
    return true;
}

// Reconciles the engine mapping table with the host address space. Regions the
// engine already holds keep their contents: guest stores arrive through the
// write hook and host stores through uploadHostWrites(). Only newly mapped
// regions are copied in, and permissions are only re-applied when they changed.
bool synchronizeEngineMappings(uc_engine *engine, const GuestMemoryCallbacks &memory,
                               std::map<GuestAddress, MappedRegion> &mappedRegions,
                               std::string &reason) {
    if (!memory.regions || !memory.read) {
        reason = "ARM32 backend requires guest-region and read callbacks.";
        return false;
    }
    const auto regions = memory.regions();
    std::map<GuestAddress, MappedRegion> currentRegions;
    for (const auto &region : regions) {
        if (region.base % kPageSize != 0 || region.size == 0 || region.size % kPageSize != 0) {
            reason = "ARM32 backend requires page-aligned guest regions.";
            return false;
        }
        currentRegions.emplace(region.base, MappedRegion{region.size, region.permissions});
    }

    for (auto current = mappedRegions.begin(); current != mappedRegions.end();) {
        if (currentRegions.count(current->first) != 0) {
            ++current;
            continue;
        }
        const auto unmapped = uc_mem_unmap(engine, current->first, current->second.size);
        if (unmapped != UC_ERR_OK) {
            reason = std::string("could not unmap a released guest region: ") +
                     uc_strerror(unmapped);
            return false;
        }
        current = mappedRegions.erase(current);
    }

    for (const auto &region : regions) {
        const auto knownRegion = mappedRegions.find(region.base);
        if (knownRegion == mappedRegions.end()) {
            const auto mapped = uc_mem_map(engine, region.base, region.size,
                                           unicornPermissions(region.permissions));
            if (mapped != UC_ERR_OK) {
                reason = std::string("could not map a newly allocated guest region: ") +
                         uc_strerror(mapped);
                return false;
            }
            mappedRegions.emplace(region.base, MappedRegion{region.size, region.permissions});
            if (hasPermission(region.permissions, MemoryPermission::Read)) {
                std::vector<std::uint8_t> bytes(region.size);
                if (!memory.read(region.base, bytes.data(), bytes.size())) {
                    reason = "guest memory callback refused to read a mapped region.";
                    return false;
                }
                const auto copied = uc_mem_write(engine, region.base, bytes.data(), bytes.size());
                if (copied != UC_ERR_OK) {
                    reason = std::string("could not initialize a guest region: ") +
                             uc_strerror(copied);
                    return false;
                }
            }
            continue;
        }
        if (knownRegion->second.size != region.size) {
            reason = "guest region size changed without unmapping the previous region.";
            return false;
        }
        if (knownRegion->second.permissions != region.permissions) {
            const auto protectedRegion = uc_mem_protect(engine, region.base, region.size,
                                                        unicornPermissions(region.permissions));
            if (protectedRegion != UC_ERR_OK) {
                reason = std::string("could not synchronize guest region permissions: ") +
                         uc_strerror(protectedRegion);
                return false;
            }
            knownRegion->second.permissions = region.permissions;
        }
    }
    return true;
}

void codeHook(uc_engine *engine, std::uint64_t address, std::uint32_t, void *userData) {
    auto &state = *static_cast<HookState *>(userData);
    // Unicorn's code hook is also the instruction counter. A zero limit means
    // that it is observability-only and must never stop the guest.
    if (state.instructionLimit != 0 && state.instructions >= state.instructionLimit) {
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
    if (callout == GuestCalloutResult::ExceptionRaised) {
        state.guestExceptionRaised = true;
        state.message = reason.empty() ? "guest exception raised; guest unwinding is unsupported."
                                       : std::move(reason);
        (void)uc_emu_stop(engine);
        return;
    }

    // Native shims either return through the guest LR or request a validated
    // transfer to a guest IMP. Preserve ARM/Thumb interworking in both cases.
    if (callout == GuestCalloutResult::Transferred) {
        state.calloutResumeAddress = registers.r[15] |
            ((registers.cpsr & kCpsrThumbBit) != 0 ? 1U : 0U);
    } else {
        const auto returnAddress = registers.r[14];
        if ((returnAddress & 1U) != 0)
            registers.cpsr |= kCpsrThumbBit;
        else
            registers.cpsr &= ~kCpsrThumbBit;
        registers.r[15] = returnAddress & ~GuestAddress{1};
        state.calloutResumeAddress = registers.r[15] |
            ((registers.cpsr & kCpsrThumbBit) != 0 ? 1U : 0U);
    }
    if (!writeRegisters(engine, registers)) {
        state.memoryFault = true;
        state.message = "ARM32 backend could not restore guest registers after a shim callout.";
        (void)uc_emu_stop(engine);
        return;
    }
    if (!synchronizeEngineMappings(engine, *state.memory, state.mappedRegions, state.message) ||
        !uploadHostWrites(engine, *state.memory, state.message)) {
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
    const auto &relay = state.memory->writeFromGuest ? state.memory->writeFromGuest
                                                     : state.memory->write;
    if (!relay(static_cast<GuestAddress>(address), bytes.data(),
               static_cast<std::size_t>(size))) {
        state.memoryFault = true;
        if (address <= std::numeric_limits<GuestAddress>::max()) {
            state.hasFaultAddress = true;
            state.faultAddress = static_cast<GuestAddress>(address);
        }
        state.message = "guest memory access failed at 0x";
        constexpr char digits[] = "0123456789abcdef";
        for (int shift = 28; shift >= 0; shift -= 4)
            state.message.push_back(digits[(address >> shift) & 0xf]);
        state.message += " (" + std::to_string(size) + " byte(s)).";
        (void)uc_emu_stop(engine);
    }
}

bool invalidMemoryHook(uc_engine *engine, uc_mem_type type, std::uint64_t address, int size,
                       std::int64_t, void *userData) {
    auto &state = *static_cast<HookState *>(userData);
    state.memoryFault = true;
    if (address <= std::numeric_limits<GuestAddress>::max()) {
        state.hasFaultAddress = true;
        state.faultAddress = static_cast<GuestAddress>(address);
    }
    state.message = "guest memory access failed at 0x";
    constexpr char digits[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4)
        state.message.push_back(digits[(address >> shift) & 0xf]);
    state.message += " (" + std::to_string(size) + " byte(s), ";
    switch (type) {
    case UC_MEM_READ_UNMAPPED:
    case UC_MEM_READ_PROT:
        state.message += "read";
        break;
    case UC_MEM_WRITE_UNMAPPED:
    case UC_MEM_WRITE_PROT:
        state.message += "write";
        break;
    case UC_MEM_FETCH_UNMAPPED:
    case UC_MEM_FETCH_PROT:
        state.message += "fetch";
        break;
    default:
        state.message += "unknown access";
        break;
    }
    state.message += ").";
    std::uint32_t pc = 0;
    if (engine && readRegister(engine, UC_ARM_REG_PC, pc) == UC_ERR_OK) {
        state.message += " pc=0x";
        for (int shift = 28; shift >= 0; shift -= 4)
            state.message.push_back(digits[(pc >> shift) & 0xf]);
    }
    if (state.memory && state.memory->regions) {
        for (const auto &region : state.memory->regions()) {
            const std::uint64_t end = static_cast<std::uint64_t>(region.base) + region.size;
            if (address >= region.base && address < end) {
                state.message += " mapped=" + region.name + "[0x";
                for (int shift = 28; shift >= 0; shift -= 4)
                    state.message.push_back(digits[(region.base >> shift) & 0xf]);
                state.message += "+" + std::to_string(region.size) + "]";
                break;
            }
        }
    }
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
        if (address == 0 || !memory.read || !memory.regions) {
            reason = "ARM32 guest function or memory callbacks are invalid.";
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
        prepared.origin = function.origin;
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

        // Enable the guest VFP unit before anything else: the guest's scalar
        // float math (frame timing, geometry) is VFP and would otherwise stop
        // the boot with a decode fault that is not a guest-program fault.
        if (writeRegister(engine, UC_ARM_REG_C1_C0_2, kVfpAccessControlRegister) != UC_ERR_OK) {
            (void)uc_close(engine);
            result.status = CpuExecutionStatus::ExecutionFault;
            result.message = "ARM32 backend could not grant the guest VFP coprocessor access (CPACR).";
            return result;
        }
        std::uint32_t fpexc = 0;
        if (readRegister(engine, UC_ARM_REG_FPEXC, fpexc) != UC_ERR_OK ||
            writeRegister(engine, UC_ARM_REG_FPEXC, fpexc | kVfpEnableBit) != UC_ERR_OK) {
            (void)uc_close(engine);
            result.status = CpuExecutionStatus::ExecutionFault;
            result.message = "ARM32 backend could not enable the guest VFP unit (FPEXC.EN).";
            return result;
        }

        std::string reason;
        HookState hooks;
        hooks.memory = &memory;
        if (!initializeEngineMemory(engine, memory, hooks.mappedRegions, reason)) {
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
        const bool hasInstructionLimit = function.instructionLimit != 0;
        const bool hasTimeLimit = function.timeLimitMicros != 0;
        const auto deadline = hasTimeLimit
            ? std::chrono::steady_clock::now() +
                  std::chrono::microseconds(static_cast<std::int64_t>(function.timeLimitMicros))
            : std::chrono::steady_clock::time_point::max();
        // A zero count and zero timeout are Unicorn's no-limit values. In the
        // normal Android path this loop therefore runs until the guest returns,
        // reaches a trap, faults, or the process is otherwise stopped; there is
        // no hidden instruction or one-minute backend ceiling.
        while (!hasInstructionLimit || hooks.instructions < function.instructionLimit) {
            std::uint64_t remainingTime = 0;
            if (hasTimeLimit) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) {
                    hooks.timeLimitHit = true;
                    hooks.message = "guest function reached its time limit.";
                    break;
                }
                const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
                if (micros <= 0) {
                    hooks.timeLimitHit = true;
                    hooks.message = "guest function reached its time limit.";
                    break;
                }
                remainingTime = static_cast<std::uint64_t>(micros);
            }

            hooks.calloutDispatched = false;
            const auto remaining = hasInstructionLimit
                ? function.instructionLimit - hooks.instructions
                : std::uint64_t{0};
            status = uc_emu_start(engine, resumeAddress, kReturnSentinel,
                                  remainingTime, static_cast<std::size_t>(remaining));
            if (hooks.memoryFault || hooks.instructionLimitHit || hooks.guestExceptionRaised)
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
                if (hasInstructionLimit && hooks.instructions >= function.instructionLimit) {
                    hooks.instructionLimitHit = true;
                    hooks.message = "guest function reached its instruction limit.";
                } else if (hasTimeLimit && std::chrono::steady_clock::now() >= deadline) {
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
        result.hasFaultAddress = hooks.hasFaultAddress;
        result.faultAddress = hooks.faultAddress;
        if (hooks.instructionLimitHit ||
            (hasInstructionLimit && hooks.instructions >= function.instructionLimit)) {
            result.status = CpuExecutionStatus::InstructionLimit;
            result.message = hooks.message.empty() ? "guest function reached its instruction limit." : hooks.message;
        } else if (hooks.timeLimitHit) {
            result.status = CpuExecutionStatus::TimeLimit;
            result.message = hooks.message;
        } else if (hooks.memoryFault) {
            result.status = CpuExecutionStatus::MemoryFault;
            result.message = hooks.message;
        } else if (hooks.guestExceptionRaised) {
            result.status = CpuExecutionStatus::GuestExceptionRaised;
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
