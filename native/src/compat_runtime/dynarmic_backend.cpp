// Dynarmic ARM32 execution backend.
//
// Replaces the former Unicorn backend with the actively maintained Dynarmic
// JIT (suyu-emu/dynarmic fork of MerryMage's dynarmic). The public contract is
// unchanged: the guest runner hands over page-described guest memory plus a
// register state and receives a CpuExecutionResult. Dynarmic is a better fit
// for the converted-game path because its A32 frontend is a modern, actively
// maintained ARMv8-A interpreter/JIT with precise VFP/NEON semantics, while
// Unicorn's ARM32 core is a frozen QEMU fork.
//
// Design notes:
//  * Unlike Unicorn, Dynarmic never owns a copy of guest memory: every data
//    access goes through the GuestMemoryCallbacks at execution time, so host
//    shim writes are immediately visible to the guest and no dirty-page
//    upload/synchronize step is needed.
//  * Native shim callouts (guest PCs at or above kCalloutRegionBase, plus the
//    return sentinel) are intercepted with PreCodeReadHook, which emits a
//    halt terminal into the compiled block; the host loop then performs the
//    callout dispatch and resumes. Normal guest code keeps full JIT speed.
//  * An execution limit of zero remains unlimited, exactly like before: the
//    launcher's game loop must run until the guest returns or faults, and the
//    tick callbacks only stop the CPU when a finite diagnostic budget was
//    explicitly requested.

#include "compat_runtime/cpu.hpp"

#include <dynarmic/frontend/A32/a32_ir_emitter.h>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/halt_reason.h>
#include <dynarmic/ir/terminal.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace radek::compat_runtime {
namespace {

constexpr std::uint32_t kReturnSentinel = 0xeffff000;
constexpr std::uint32_t kCalloutRegionBase = 0xf0000000;
constexpr std::uint32_t kCpsrThumbBit = 1U << 5;
constexpr std::uint32_t kCpsrModeMask = 0x1f;
constexpr std::uint32_t kCpsrUserMode = 0x10;
constexpr std::uint64_t kUnlimitedTicks = 1ULL << 62;

void appendHex(std::string &message, std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4)
        message.push_back(digits[(value >> shift) & 0xf]);
}

struct BackendState {
    const GuestMemoryCallbacks *memory = nullptr;
    Dynarmic::A32::Jit *jit = nullptr;
    std::uint64_t instructions = 0;
    std::uint64_t instructionLimit = 0;
    std::uint64_t timeLimitMicros = 0;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    bool instructionLimitHit = false;
    bool timeLimitHit = false;
    bool memoryFault = false;
    bool hasFaultAddress = false;
    GuestAddress faultAddress = 0;
    bool guestExceptionRaised = false;
    bool bypassCalloutHookOnce = false;
    std::uint64_t hintSpin = 0;
    std::string message;
};

void loadRegisters(Dynarmic::A32::Jit &jit, CpuRegisterState &state);
void storeRegisters(Dynarmic::A32::Jit &jit, const CpuRegisterState &state);

struct Callbacks final : Dynarmic::A32::UserCallbacks {
    BackendState *state = nullptr;

    void failMemory(const char *kind, Dynarmic::A32::VAddr vaddr, std::size_t size) {
        state->memoryFault = true;
        state->hasFaultAddress = true;
        state->faultAddress = static_cast<GuestAddress>(vaddr);
        state->message = "guest memory access failed at 0x";
        appendHex(state->message, vaddr);
        state->message += " (" + std::to_string(size) + " byte(s), " + kind + ").";
        if (state->jit)
            state->jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
    }

    bool readGuest(Dynarmic::A32::VAddr vaddr, std::uint8_t *bytes, std::size_t size) {
        if (!state->memory || !state->memory->read) {
            failMemory("read", vaddr, size);
            return false;
        }
        if (!state->memory->read(static_cast<GuestAddress>(vaddr), bytes, size)) {
            failMemory("read", vaddr, size);
            return false;
        }
        return true;
    }

    bool writeGuest(Dynarmic::A32::VAddr vaddr, const std::uint8_t *bytes, std::size_t size) {
        if (!state->memory) {
            failMemory("write", vaddr, size);
            return false;
        }
        const auto &relay = state->memory->writeFromGuest ? state->memory->writeFromGuest
                                                          : state->memory->write;
        if (!relay || !relay(static_cast<GuestAddress>(vaddr), bytes, size)) {
            failMemory("write", vaddr, size);
            return false;
        }
        return true;
    }

    std::optional<std::uint32_t> MemoryReadCode(Dynarmic::A32::VAddr vaddr) override {
        std::array<std::uint8_t, 4> bytes{};
        if (!readGuest(vaddr, bytes.data(), 4))
            return std::nullopt;
        return std::uint32_t(bytes[0]) | std::uint32_t(bytes[1]) << 8 |
               std::uint32_t(bytes[2]) << 16 | std::uint32_t(bytes[3]) << 24;
    }

    // Intercepts the return sentinel and the native-shim callout region by
    // emitting a runtime exception sequence instead of halting at translation
    // time: the cached block raises UndefinedInstruction on every execution,
    // ExceptionRaised dispatches the callout on the host side, and control
    // resumes through CheckHalt. Because the trap is re-raised at runtime the
    // block stays cached and re-entry-safe - no cache flushes on the callout
    // fast path. Returning false precludes translating the intercepted word
    // itself, so normal guest code translates and links at full JIT speed.
    bool PreCodeReadHook(bool, Dynarmic::A32::VAddr pc, Dynarmic::A32::IREmitter &ir) override {
        if (state->bypassCalloutHookOnce) {
            state->bypassCalloutHookOnce = false;
            return true;
        }
        if (pc == kReturnSentinel || pc >= kCalloutRegionBase) {
            ir.UpdateUpperLocationDescriptor();
            ir.ExceptionRaised(Dynarmic::A32::Exception::UndefinedInstruction);
            ir.SetTerm(Dynarmic::IR::Term::CheckHalt{Dynarmic::IR::Term::ReturnToDispatch{}});
            return false;
        }
        return true;
    }

    std::uint8_t MemoryRead8(Dynarmic::A32::VAddr vaddr) override {
        std::uint8_t value = 0;
        (void)readGuest(vaddr, &value, 1);
        return value;
    }
    std::uint16_t MemoryRead16(Dynarmic::A32::VAddr vaddr) override {
        std::array<std::uint8_t, 2> bytes{};
        (void)readGuest(vaddr, bytes.data(), 2);
        return std::uint16_t(bytes[0]) | std::uint16_t(bytes[1]) << 8;
    }
    std::uint32_t MemoryRead32(Dynarmic::A32::VAddr vaddr) override {
        std::array<std::uint8_t, 4> bytes{};
        (void)readGuest(vaddr, bytes.data(), 4);
        return std::uint32_t(bytes[0]) | std::uint32_t(bytes[1]) << 8 |
               std::uint32_t(bytes[2]) << 16 | std::uint32_t(bytes[3]) << 24;
    }
    std::uint64_t MemoryRead64(Dynarmic::A32::VAddr vaddr) override {
        std::array<std::uint8_t, 8> bytes{};
        (void)readGuest(vaddr, bytes.data(), 8);
        std::uint64_t value = 0;
        for (int index = 7; index >= 0; --index)
            value = value << 8 | bytes[static_cast<std::size_t>(index)];
        return value;
    }

    void MemoryWrite8(Dynarmic::A32::VAddr vaddr, std::uint8_t value) override {
        (void)writeGuest(vaddr, &value, 1);
    }
    void MemoryWrite16(Dynarmic::A32::VAddr vaddr, std::uint16_t value) override {
        std::array<std::uint8_t, 2> bytes{std::uint8_t(value), std::uint8_t(value >> 8)};
        (void)writeGuest(vaddr, bytes.data(), 2);
    }
    void MemoryWrite32(Dynarmic::A32::VAddr vaddr, std::uint32_t value) override {
        std::array<std::uint8_t, 4> bytes{std::uint8_t(value), std::uint8_t(value >> 8),
                                          std::uint8_t(value >> 16), std::uint8_t(value >> 24)};
        (void)writeGuest(vaddr, bytes.data(), 4);
    }
    void MemoryWrite64(Dynarmic::A32::VAddr vaddr, std::uint64_t value) override {
        std::array<std::uint8_t, 8> bytes{};
        for (std::size_t index = 0; index < 8; ++index)
            bytes[index] = std::uint8_t(value >> (8 * index));
        (void)writeGuest(vaddr, bytes.data(), 8);
    }

    void InterpreterFallback(Dynarmic::A32::VAddr pc, std::size_t) override {
        state->message = "Dynarmic requested an interpreter fallback at pc=0x";
        appendHex(state->message, pc);
        state->message += "; treated as an execution fault.";
        state->jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
    }

    void CallSVC(std::uint32_t) override {}

    void ExceptionRaised(Dynarmic::A32::VAddr pc, Dynarmic::A32::Exception exception) override {
        using Dynarmic::A32::Exception;
        if (dispatchNativeTrap(pc))
            return;
        switch (exception) {
        case Exception::Yield:
        case Exception::SendEvent:
        case Exception::SendEventLocal:
        case Exception::PreloadData:
        case Exception::PreloadDataWithIntentToWrite:
        case Exception::PreloadInstruction:
            return; // hints: keep executing
        case Exception::WaitForInterrupt:
        case Exception::WaitForEvent:
            // The host cannot deliver interrupts; a guest blocked in a wait
            // state would otherwise re-raise this exception forever without
            // retiring instructions. Fail closed after enough consecutive
            // wait exceptions for AddTicks to have observed no progress.
            if (++state->hintSpin < 4096)
                return;
            state->message = "guest blocked in a wait state (WFI/WFE); the host cannot deliver interrupts.";
            state->jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
            return;
        case Exception::NoExecuteFault:
            state->memoryFault = true;
            state->hasFaultAddress = true;
            state->faultAddress = static_cast<GuestAddress>(pc);
            state->message = "guest memory access failed at 0x";
            appendHex(state->message, pc);
            state->message += " (4 byte(s), fetch).";
            break;
        default:
            state->message = "Dynarmic raised an execution exception at pc=0x";
            appendHex(state->message, pc);
            state->message += ".";
            break;
        }
        state->jit->HaltExecution(Dynarmic::HaltReason::UserDefined3);
    }

    // Marks traps raised by the runtime exception blocks emitted in
    // PreCodeReadHook. The actual dispatch happens on the host after Run()
    // returns: jit_state is only coherent at block boundaries, so registers
    // must not be read or rewritten from inside the host-call itself.
    bool dispatchNativeTrap(Dynarmic::A32::VAddr pc) {
        if (pc != kReturnSentinel && pc < kCalloutRegionBase)
            return false;
        state->jit->HaltExecution(Dynarmic::HaltReason::UserDefined1);
        return true;
    }

    void AddTicks(std::uint64_t ticks) override {
        if (ticks > 0)
            state->hintSpin = 0; // only real instruction progress clears a wait-state spin
        state->instructions += ticks;
        if (state->instructionLimit != 0 && state->instructions >= state->instructionLimit) {
            state->instructionLimitHit = true;
            state->message = "guest function reached its instruction limit.";
            state->jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
            return;
        }
        if (state->timeLimitMicros != 0 && std::chrono::steady_clock::now() >= state->deadline) {
            state->timeLimitHit = true;
            state->message = "guest function reached its time limit.";
            state->jit->HaltExecution(Dynarmic::HaltReason::UserDefined2);
        }
    }

    std::uint64_t GetTicksRemaining() override {
        if (state->instructionLimit != 0)
            return state->instructions >= state->instructionLimit
                       ? 0
                       : state->instructionLimit - state->instructions;
        return kUnlimitedTicks;
    }
};

void loadRegisters(Dynarmic::A32::Jit &jit, CpuRegisterState &state) {
    auto &regs = jit.Regs();
    for (std::size_t index = 0; index < 16; ++index)
        state.r[index] = regs[index];
    state.cpsr = jit.Cpsr();
    const auto &ext = jit.ExtRegs();
    for (std::size_t index = 0; index < state.d.size(); ++index)
        state.d[index] = std::uint64_t(ext[2 * index]) | std::uint64_t(ext[2 * index + 1]) << 32;
    state.fpscr = jit.Fpscr();
}

void storeRegisters(Dynarmic::A32::Jit &jit, const CpuRegisterState &state) {
    auto &regs = jit.Regs();
    for (std::size_t index = 0; index < 16; ++index)
        regs[index] = state.r[index];
    jit.SetCpsr(state.cpsr);
    auto &ext = jit.ExtRegs();
    for (std::size_t index = 0; index < state.d.size(); ++index) {
        ext[2 * index] = std::uint32_t(state.d[index]);
        ext[2 * index + 1] = std::uint32_t(state.d[index] >> 32);
    }
    jit.SetFpscr(state.fpscr);
}

class DynarmicArm32Backend final : public CpuBackend {
  public:
    const char *name() const noexcept override { return "Dynarmic A32 JIT ARM32"; }
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

        BackendState state;
        state.memory = &memory;
        state.instructionLimit = function.instructionLimit;
        state.timeLimitMicros = function.timeLimitMicros;
        if (function.timeLimitMicros != 0)
            state.deadline = std::chrono::steady_clock::now() +
                             std::chrono::microseconds(static_cast<std::int64_t>(function.timeLimitMicros));

        Callbacks callbacks;
        callbacks.state = &state;

        Dynarmic::A32::UserConfig config;
        config.callbacks = &callbacks;
        config.processor_id = 0;
        auto jit = std::make_unique<Dynarmic::A32::Jit>(config);
        state.jit = jit.get();

        CpuRegisterState registers = initialRegisters;
        registers.r[15] = function.entryPoint;
        // Return to an ARM-mode sentinel so the backend stops uniformly even
        // when the guest entry point began in Thumb state; the sentinel is
        // intercepted by PreCodeReadHook.
        registers.r[14] = kReturnSentinel;
        registers.cpsr = (registers.cpsr & ~kCpsrModeMask) | kCpsrUserMode;
        if (function.thumb)
            registers.cpsr |= kCpsrThumbBit;
        else
            registers.cpsr &= ~kCpsrThumbBit;
        storeRegisters(*jit, registers);

        bool returned = false;
        for (;;) {
            const auto halt = jit->Run();
            const auto halted = [&halt](Dynarmic::HaltReason bit) {
                return (static_cast<unsigned>(halt) & static_cast<unsigned>(bit)) != 0;
            };
            if (state.memoryFault)
                break;
            if (state.instructionLimitHit || state.timeLimitHit)
                break;
            if (halted(Dynarmic::HaltReason::UserDefined3))
                break;
            if (halted(Dynarmic::HaltReason::CacheInvalidation))
                continue; // a cache flush was requested; re-dispatch with fresh blocks
            if (halted(Dynarmic::HaltReason::UserDefined1)) {
                // A runtime exception block fired for the return sentinel or a
                // native-shim callout. jit_state is coherent here (block
                // boundary), so dispatch on the host and resume.
                const auto pendingPc = jit->Regs()[15];
                if (pendingPc == kReturnSentinel) {
                    returned = true;
                    break;
                }
                if (pendingPc < kCalloutRegionBase) {
                    state.message = "Dynarmic halted for an unrecognized reason.";
                    break;
                }
                if (state.timeLimitMicros != 0 &&
                    std::chrono::steady_clock::now() >= state.deadline) {
                    state.timeLimitHit = true;
                    state.message = "guest function reached its time limit.";
                    break;
                }
                CpuRegisterState calloutRegisters;
                loadRegisters(*jit, calloutRegisters);
                calloutRegisters.r[15] = pendingPc;
                std::string reason;
                const auto callout = memory.invokeGuestCallout
                                         ? memory.invokeGuestCallout(static_cast<GuestAddress>(pendingPc),
                                                                     calloutRegisters, reason)
                                         : GuestCalloutResult::NotRegistered;
                if (callout == GuestCalloutResult::NotRegistered) {
                    // Parity with the previous backend: an unregistered address
                    // in the callout region executes its guest bytes. Rebuild
                    // the block once without the intercept terminal.
                    state.bypassCalloutHookOnce = true;
                    jit->ClearCache();
                    continue;
                }
                if (callout == GuestCalloutResult::Failed) {
                    state.memoryFault = true;
                    state.message = reason.empty() ? "native shim call adapter rejected a guest callout."
                                                   : std::move(reason);
                    break;
                }
                if (callout == GuestCalloutResult::ExceptionRaised) {
                    state.guestExceptionRaised = true;
                    state.message = reason.empty() ? "guest exception raised; guest unwinding is unsupported."
                                                   : std::move(reason);
                    break;
                }
                // Native shims either return through the guest LR or request a
                // validated transfer to a guest IMP. Preserve ARM/Thumb
                // interworking in both cases.
                CpuRegisterState after = calloutRegisters;
                if (callout == GuestCalloutResult::Returned) {
                    const auto returnAddress = after.r[14];
                    if ((returnAddress & 1U) != 0)
                        after.cpsr |= kCpsrThumbBit;
                    else
                        after.cpsr &= ~kCpsrThumbBit;
                    after.r[15] = returnAddress & ~GuestAddress{1};
                }
                storeRegisters(*jit, after);
                continue;
            }
            if (static_cast<unsigned>(halt) == 0u || halt == Dynarmic::HaltReason::Step) {
                // The cycle budget ran out without any explicit halt; treat it
                // as the instruction-limit boundary.
                state.instructionLimitHit = true;
                state.message = "guest function reached its instruction limit.";
                break;
            }
            state.message = state.message.empty() ? "Dynarmic stopped before the guest function returned."
                                                  : state.message;
            break;
        }

        loadRegisters(*jit, result.registers);
        result.started = state.instructions != 0;
        result.instructions = state.instructions;
        result.hasFaultAddress = state.hasFaultAddress;
        result.faultAddress = state.faultAddress;
        if (state.instructionLimitHit) {
            result.status = CpuExecutionStatus::InstructionLimit;
            result.message = state.message.empty() ? "guest function reached its instruction limit." : state.message;
        } else if (state.timeLimitHit) {
            result.status = CpuExecutionStatus::TimeLimit;
            result.message = state.message;
        } else if (state.memoryFault) {
            result.status = CpuExecutionStatus::MemoryFault;
            result.message = state.message.empty() ? "guest memory access failed." : state.message;
        } else if (state.guestExceptionRaised) {
            result.status = CpuExecutionStatus::GuestExceptionRaised;
            result.message = state.message;
        } else if (returned) {
            result.status = CpuExecutionStatus::Returned;
        } else {
            result.status = CpuExecutionStatus::ExecutionFault;
            result.message = state.message.empty() ? "Dynarmic stopped without a return." : state.message;
        }
        return result;
    }
};

} // namespace

std::unique_ptr<CpuBackend> createDynarmicArm32Backend() {
    return std::make_unique<DynarmicArm32Backend>();
}

} // namespace radek::compat_runtime
