#include "compat_runtime/sjlj_unwind.hpp"

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace radek::compat_runtime {
namespace {
// The adapter links contexts through the first word of the guest's
// jmp-buf (`prev` in GCC's SjLj_Function_Context); nothing beyond that
// word is read or written, so the context only has to expose it.
constexpr std::size_t kSjLjContextHeaderSize = sizeof(GuestAddress);
}

void SjLjUnwindAdapter::registerFunction(
    ShimRegistry &registry, const std::string &symbol, const std::string &adapterName,
    std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)> invoke) {
    if (nextCallout_ > 0xf001ffffU - 4U)
        throw std::overflow_error("ARM32 SjLj callout range is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libgcc_s.1.dylib";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_;
    binding.invoke = std::move(invoke);
    nextCallout_ += 4U;
    registry.registerBinding(std::move(binding));
}

void SjLjUnwindAdapter::registerExceptionFunction(
    ShimRegistry &registry, const std::string &symbol, const std::string &adapterName,
    std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)> invoke) {
    if (nextCallout_ > 0xf001ffffU - 4U)
        throw std::overflow_error("ARM32 SjLj callout range is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libgcc_s.1.dylib";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_;
    binding.invokeException = std::move(invoke);
    nextCallout_ += 4U;
    registry.registerBinding(std::move(binding));
}

bool SjLjUnwindAdapter::registerContext(CpuRegisterState &registers,
                                       GuestAddressSpace &memory,
                                       std::string &reason) {
    const auto context = registers.r[0];
    if (context == 0 || (context & 3U) != 0 ||
        !memory.contains(context, kSjLjContextHeaderSize,
                         MemoryPermission::Read | MemoryPermission::Write)) {
        reason = "__Unwind_SjLj_Register received an invalid guest function context";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto &state = states_[&memory];
    if (state.activeContexts.count(context) != 0) {
        reason = "__Unwind_SjLj_Register received an already registered context";
        return false;
    }
    const auto previous = state.top;
    if (!memory.write(context, &previous, sizeof(previous))) {
        reason = "__Unwind_SjLj_Register could not link the guest function context";
        return false;
    }
    state.activeContexts.insert(context);
    state.top = context;
    return true;
}

GuestAddress SjLjUnwindAdapter::beginThreadChain(GuestAddressSpace &memory) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &state = states_[&memory];
    const GuestAddress saved = state.top;
    state.top = 0;
    return saved;
}

void SjLjUnwindAdapter::endThreadChain(GuestAddressSpace &memory, GuestAddress savedTop) {
    // Bounded: a chain longer than this is corrupt, and walking it further
    // would not reflect any real registration order.
    constexpr std::size_t kMaxWorkerChainWalk = 1U << 16;
    std::lock_guard<std::mutex> lock(mutex_);
    auto &state = states_[&memory];
    GuestAddress cursor = state.top;
    for (std::size_t steps = 0; cursor != 0 && cursor != savedTop && steps < kMaxWorkerChainWalk;
         ++steps) {
        GuestAddress previous = 0;
        if (!memory.read(cursor, &previous, sizeof(previous)))
            break;
        state.activeContexts.erase(cursor);
        cursor = previous;
    }
    state.top = savedTop;
}

bool SjLjUnwindAdapter::unregisterContext(CpuRegisterState &registers,
                                         GuestAddressSpace &memory,
                                         std::string &reason) {
    const auto context = registers.r[0];
    if (context == 0 || (context & 3U) != 0 ||
        !memory.contains(context, sizeof(GuestAddress),
                         MemoryPermission::Read | MemoryPermission::Write)) {
        reason = "__Unwind_SjLj_Unregister received an invalid guest function context";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto stateIt = states_.find(&memory);
    if (stateIt == states_.end() || stateIt->second.top != context ||
        stateIt->second.activeContexts.count(context) == 0) {
        reason = "__Unwind_SjLj_Unregister violated the guest context LIFO chain";
        return false;
    }
    GuestAddress previous = 0;
    if (!memory.read(context, &previous, sizeof(previous)) ||
        (previous != 0 && stateIt->second.activeContexts.count(previous) == 0)) {
        reason = "__Unwind_SjLj_Unregister found a corrupt guest context link";
        return false;
    }
    stateIt->second.activeContexts.erase(context);
    stateIt->second.top = previous;
    return true;
}

void SjLjUnwindAdapter::registerBindings(ShimRegistry &registry) {
    if (registered_)
        throw std::runtime_error("ARM32 SjLj adapter is already registered");
    registerFunction(registry, "__Unwind_SjLj_Register", "sjlj-context-register",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory,
               std::string &reason) {
            try {
                return registerContext(registers, memory, reason);
            } catch (const std::exception &error) {
                reason = error.what();
                return false;
            }
        });
    registerFunction(registry, "__Unwind_SjLj_Unregister", "sjlj-context-unregister",
        [this](CpuRegisterState &registers, GuestAddressSpace &memory,
               std::string &reason) {
            try {
                return unregisterContext(registers, memory, reason);
            } catch (const std::exception &error) {
                reason = error.what();
                return false;
            }
        });
    registerExceptionFunction(registry, "__Unwind_SjLj_Resume",
        "sjlj-resume-unsupported-exception-boundary",
        [](CpuRegisterState &registers, GuestAddressSpace &, std::string &reason) {
            reason = "__Unwind_SjLj_Resume reached with guest exception object " +
                     std::to_string(registers.r[0]) +
                     "; guest SJLJ resume, personality dispatch, and landing-pad transfer "
                     "are unsupported.";
            return true;
        });
    registry.registerImageInitializer("arm32-sjlj-contexts",
        [this](GuestAddressSpace &memory, const std::vector<GuestImageSection> &,
               std::string &) {
            std::lock_guard<std::mutex> lock(mutex_);
            states_.erase(&memory);
            return true;
        });
    registered_ = true;
}

} // namespace radek::compat_runtime
