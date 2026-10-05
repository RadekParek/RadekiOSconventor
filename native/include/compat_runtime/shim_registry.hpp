#pragma once

#include "compat_runtime/cpu.hpp"
#include "compat_runtime/guest_memory.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace radek::compat_runtime {

struct ShimBinding {
    std::string darwinSymbol;
    std::string library;
    std::string adapterName;
    GuestAddress guestAddress = 0;
    std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)> invoke;
};

/** Exact-name registry: only entries with a native call adapter can resolve. */
class ShimRegistry {
    mutable std::mutex mutex_;
    std::map<std::string, ShimBinding> bindings_;
    std::map<GuestAddress, std::string> callouts_;

  public:
    void registerBinding(ShimBinding binding);
    std::optional<ShimBinding> resolve(const std::string &darwinSymbol) const;
    std::vector<ShimBinding> snapshot() const;
    GuestCalloutResult invokeCallout(GuestAddress address, CpuRegisterState &registers,
                                    GuestAddressSpace &memory, std::string &reason) const;
    std::size_t size() const;
};

} // namespace radek::compat_runtime
