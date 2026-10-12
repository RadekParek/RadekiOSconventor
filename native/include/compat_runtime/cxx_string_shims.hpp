#pragma once

#include "compat_runtime/libsystem_shims.hpp"
#include "compat_runtime/shim_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace radek::compat_runtime::cxx_string {

/**
 * Narrow `std::basic_string<char>` in the libstdc++ copy-on-write ABI that iOS
 * game binaries import as `__ZNSs...`.
 *
 * Each string object is one guest word: a pointer to its characters. The
 * representation header sits in the twelve bytes before those characters:
 * length, capacity, and a reference word. A reference word of 0 means one owner,
 * a positive value means shared, and -1 means leaked (never shared and never
 * freed). The empty string points at a shared static representation whose
 * storage is the `_Rep::_S_empty_rep_storage` data symbol.
 *
 * Every function operates on guest memory through callouts. Allocation and
 * release go through the libsystem guest heap, so `operator delete` and string
 * frees return blocks to the same free list. Out-of-range positions, corrupt
 * headers, and exhausted heap fail closed with a reason; nothing is silently
 * truncated.
 */
class ShimAdapter {
  public:
    /** `heap` is the libsystem adapter whose guest heap backs every string allocation. */
    explicit ShimAdapter(libsystem::ShimAdapter *heap);

    void registerBindings(ShimRegistry &registry);

    std::size_t boundSymbolCount() const noexcept { return boundSymbols_; }
    std::uint64_t calls() const noexcept { return calls_; }

  private:
    using Invoke = std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)>;


    void registerFunction(ShimRegistry &registry, const std::string &symbol,
                          const std::string &adapterName, Invoke invoke);
    void registerData(ShimRegistry &registry, const std::string &symbol,
                      const std::string &adapterName,
                      std::function<bool(GuestAddressSpace &, GuestAddress &, std::string &)> resolve);
    // Maps the shared empty representation for this address space on first use.
    bool enter(GuestAddressSpace &memory, std::string &reason, GuestAddress &empty);

    libsystem::ShimAdapter *heap_;
    std::mutex mutex_;
    std::map<const GuestAddressSpace *, GuestAddress> emptyReps_;
    GuestAddress nextCallout_ = 0xf0140000u;
    std::size_t boundSymbols_ = 0;
    std::uint64_t calls_ = 0;
};

} // namespace radek::compat_runtime::cxx_string
