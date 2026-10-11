#include "compat_runtime/shim_registry.hpp"

#include <atomic>
#include <stdexcept>

namespace radek::compat_runtime {

namespace {
std::atomic<std::uint64_t> gLiveCalloutCount{0};
// Points at the darwinSymbol of the most recent callout's binding (stable storage).
std::atomic<const std::string *> gLiveLastCallout{nullptr};
} // namespace

std::uint64_t liveCalloutCount() { return gLiveCalloutCount.load(std::memory_order_relaxed); }

std::string liveLastCalloutSymbol() {
    const auto *symbol = gLiveLastCallout.load(std::memory_order_acquire);
    return symbol != nullptr ? *symbol : std::string();
}

void ShimRegistry::registerBinding(ShimBinding binding) {
    const bool dataBinding = static_cast<bool>(binding.resolveGuestAddress);
    const auto callbackCount = static_cast<unsigned>(static_cast<bool>(binding.invoke)) +
                               static_cast<unsigned>(static_cast<bool>(binding.invokeTransfer)) +
                               static_cast<unsigned>(static_cast<bool>(binding.invokeException));
    const bool invalidCallout = !dataBinding &&
                                (binding.guestAddress < 0xf0000000 || callbackCount != 1);
    const bool invalidDataBinding = dataBinding &&
                                    (binding.guestAddress != 0 || binding.invoke ||
                                     binding.invokeTransfer || binding.invokeException);
    if (binding.darwinSymbol.empty() || binding.library.empty() || binding.adapterName.empty() ||
        invalidCallout || invalidDataBinding)
        throw std::invalid_argument(
            "shim binding requires a named library and either a reserved native callout or guest-data resolver");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dataBinding) {
        const auto address = binding.guestAddress & ~GuestAddress{1};
        if (callouts_.find(address) != callouts_.end())
            throw std::runtime_error("duplicate native shim callout address");
    }
    const auto symbol = binding.darwinSymbol;
    if (!bindings_.emplace(symbol, std::move(binding)).second)
        throw std::runtime_error("duplicate Darwin shim symbol");
    const auto &stored = bindings_.at(symbol);
    if (!stored.resolveGuestAddress)
        callouts_.emplace(stored.guestAddress & ~GuestAddress{1}, &stored);
}

void ShimRegistry::registerImageInitializer(const std::string &name,
                                            GuestImageInitializer initializer) {
    if (name.empty() || !initializer)
        throw std::invalid_argument("guest image initializer requires a name and callback");
    std::lock_guard<std::mutex> lock(mutex_);
    if (!imageInitializers_.emplace(name, std::move(initializer)).second)
        throw std::runtime_error("duplicate guest image initializer: " + name);
}

bool ShimRegistry::initializeImage(GuestAddressSpace &memory,
                                  const std::vector<GuestImageSection> &sections,
                                  std::string &reason) const {
    std::vector<std::pair<std::string, GuestImageInitializer>> initializers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        initializers.reserve(imageInitializers_.size());
        for (const auto &initializer : imageInitializers_)
            initializers.push_back(initializer);
    }
    for (const auto &initializer : initializers) {
        std::string detail;
        if (!initializer.second(memory, sections, detail)) {
            reason = detail.empty() ? "guest image initializer failed: " + initializer.first
                                    : initializer.first + ": " + detail;
            return false;
        }
    }
    return true;
}

std::optional<ShimBinding> ShimRegistry::resolve(const std::string &darwinSymbol) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = bindings_.find(darwinSymbol);
    if (found == bindings_.end())
        return std::nullopt;
    return found->second;
}

std::vector<ShimBinding> ShimRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ShimBinding> result;
    result.reserve(bindings_.size());
    for (const auto &entry : bindings_)
        result.push_back(entry.second);
    return result;
}

GuestCalloutResult ShimRegistry::invokeCallout(GuestAddress address,
                                                   CpuRegisterState &registers,
                                                   GuestAddressSpace &memory,
                                                   std::string &reason) const {
    const ShimBinding *binding = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto callout = callouts_.find(address & ~GuestAddress{1});
        if (callout == callouts_.end())
            return GuestCalloutResult::NotRegistered;
        binding = callout->second;
    }
    gLiveCalloutCount.fetch_add(1, std::memory_order_relaxed);
    gLiveLastCallout.store(&binding->darwinSymbol, std::memory_order_release);
    if (binding->invokeException) {
        if (!binding->invokeException(registers, memory, reason)) {
            if (reason.empty())
                reason = "native exception adapter failed for " + binding->darwinSymbol;
            return GuestCalloutResult::Failed;
        }
        if (reason.empty())
            reason = "guest exception raised by " + binding->darwinSymbol;
        return GuestCalloutResult::ExceptionRaised;
    }
    if (binding->invokeTransfer) {
        GuestAddress target = 0;
        if (!binding->invokeTransfer(registers, memory, target, reason)) {
            if (reason.empty())
                reason = "native adapter failed for " + binding->darwinSymbol;
            return GuestCalloutResult::Failed;
        }
        if (target == 0)
            return GuestCalloutResult::Returned;
        const auto codeAddress = target & ~GuestAddress{1};
        if (!memory.contains(codeAddress, 2, MemoryPermission::Execute)) {
            reason = "native adapter selected a guest IMP outside executable guest memory";
            return GuestCalloutResult::Failed;
        }
        registers.r[15] = codeAddress;
        constexpr std::uint32_t thumbStateBit = 1U << 5;
        if ((target & 1U) != 0)
            registers.cpsr |= thumbStateBit;
        else
            registers.cpsr &= ~thumbStateBit;
        return GuestCalloutResult::Transferred;
    }
    if (!binding->invoke || !binding->invoke(registers, memory, reason)) {
        if (reason.empty())
            reason = "native adapter failed for " + binding->darwinSymbol;
        return GuestCalloutResult::Failed;
    }
    return GuestCalloutResult::Returned;
}

std::size_t ShimRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bindings_.size();
}

} // namespace radek::compat_runtime
