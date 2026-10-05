#include "compat_runtime/shim_registry.hpp"

#include <stdexcept>

namespace radek::compat_runtime {

void ShimRegistry::registerBinding(ShimBinding binding) {
    if (binding.darwinSymbol.empty() || binding.library.empty() || binding.adapterName.empty() ||
        binding.guestAddress < 0xf0000000 || !binding.invoke)
        throw std::invalid_argument(
            "shim binding requires a named library, reserved guest entry and native call adapter");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto address = binding.guestAddress & ~GuestAddress{1};
    if (callouts_.find(address) != callouts_.end())
        throw std::runtime_error("duplicate native shim callout address");
    const auto symbol = binding.darwinSymbol;
    if (!bindings_.emplace(symbol, std::move(binding)).second)
        throw std::runtime_error("duplicate Darwin shim symbol");
    callouts_.emplace(address, symbol);
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
    ShimBinding binding;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto callout = callouts_.find(address & ~GuestAddress{1});
        if (callout == callouts_.end())
            return GuestCalloutResult::NotRegistered;
        const auto found = bindings_.find(callout->second);
        if (found == bindings_.end()) {
            reason = "native shim callout registry is inconsistent";
            return GuestCalloutResult::Failed;
        }
        binding = found->second;
    }
    if (!binding.invoke(registers, memory, reason)) {
        if (reason.empty())
            reason = "native adapter failed for " + binding.darwinSymbol;
        return GuestCalloutResult::Failed;
    }
    return GuestCalloutResult::Returned;
}

std::size_t ShimRegistry::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bindings_.size();
}

} // namespace radek::compat_runtime
