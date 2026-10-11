#pragma once

#include "compat_runtime/cpu.hpp"
#include "compat_runtime/guest_memory.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace radek::compat_runtime {

/** Live count of guest imports entering the shim layer, and the last one called. */
std::uint64_t liveCalloutCount();
std::string liveLastCalloutSymbol();
// Steady-clock span from the first to the most recent guest import, in nanoseconds (0 when none).
std::int64_t liveCalloutWindowNanos();

struct ShimBinding {
    std::string darwinSymbol;
    std::string library;
    std::string adapterName;
    GuestAddress guestAddress = 0;
    std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)> invoke;
    // A bounded adapter may hand control to a validated guest IMP. The target
    // carries the ARM/Thumb bit in bit zero; the CPU backend performs the transfer.
    std::function<bool(CpuRegisterState &, GuestAddressSpace &, GuestAddress &,
                       std::string &)> invokeTransfer;
    // Data imports (for example, Objective-C class objects) are materialized in
    // the guest address space when the loader binds them; they are not callouts.
    std::function<bool(GuestAddressSpace &, GuestAddress &, std::string &)> resolveGuestAddress;
    // Guest-language exceptions stop execution with a distinct status when the
    // bounded runtime cannot perform the guest ABI's full unwind/catch protocol.
    std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)> invokeException;
};

struct GuestImageSection {
    std::string segmentName;
    std::string name;
    GuestAddress address = 0;
    std::uint32_t size = 0;
};

using GuestImageInitializer = std::function<bool(
    GuestAddressSpace &, const std::vector<GuestImageSection> &, std::string &)>;

/** Exact-name registry of native callouts and guest-address data resolvers. */
class ShimRegistry {
    mutable std::mutex mutex_;
    std::map<std::string, ShimBinding> bindings_;
    // Stable pointers into bindings_ (std::map nodes never move and nothing erases),
    // so a callout dispatches without copying the binding or its callbacks.
    std::map<GuestAddress, const ShimBinding *> callouts_;
    std::map<std::string, GuestImageInitializer> imageInitializers_;

  public:
    void registerBinding(ShimBinding binding);
    void registerImageInitializer(const std::string &name, GuestImageInitializer initializer);
    bool initializeImage(GuestAddressSpace &memory,
                         const std::vector<GuestImageSection> &sections,
                         std::string &reason) const;
    std::optional<ShimBinding> resolve(const std::string &darwinSymbol) const;
    std::vector<ShimBinding> snapshot() const;
    GuestCalloutResult invokeCallout(GuestAddress address, CpuRegisterState &registers,
                                    GuestAddressSpace &memory, std::string &reason) const;
    std::size_t size() const;
};

} // namespace radek::compat_runtime
