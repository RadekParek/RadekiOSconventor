#pragma once

#include "compat_runtime/shim_registry.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace radek::compat_runtime {

/**
 * Bounded ARM32 SjLj context-chain adapters.
 *
 * This adapter implements LIFO registration/unregistration for GCC/Clang's
 * SjLj context chain. Resume is registered only as a fail-closed exception
 * boundary; search, personality dispatch, landing-pad transfer, and real resume
 * remain unsupported.
 */
class SjLjUnwindAdapter {
    struct ContextState {
        GuestAddress top = 0;
        std::set<GuestAddress> activeContexts;
    };

    std::map<const GuestAddressSpace *, ContextState> states_;
    std::mutex mutex_;
    GuestAddress nextCallout_ = 0xf0011000;
    bool registered_ = false;

    bool registerContext(CpuRegisterState &registers, GuestAddressSpace &memory,
                         std::string &reason);
    bool unregisterContext(CpuRegisterState &registers, GuestAddressSpace &memory,
                           std::string &reason);
    void registerExceptionFunction(ShimRegistry &registry, const std::string &symbol,
                                   const std::string &adapterName,
                                   std::function<bool(CpuRegisterState &, GuestAddressSpace &,
                                                      std::string &)> invoke);
    void registerFunction(ShimRegistry &registry, const std::string &symbol,
                          const std::string &adapterName,
                          std::function<bool(CpuRegisterState &, GuestAddressSpace &,
                                             std::string &)> invoke);

  public:
    /** Register the libgcc SjLj context imports and explicit resume stop boundary. */
    void registerBindings(ShimRegistry &registry);

    /**
     * Per-thread chains: a guest worker runs with its own, initially empty,
     * SjLj chain. beginThreadChain returns the creator's chain top to be
     * restored later by endThreadChain, which also unlinks any contexts the
     * worker left registered (for example when the worker is ended early).
     */
    GuestAddress beginThreadChain(GuestAddressSpace &memory);
    void endThreadChain(GuestAddressSpace &memory, GuestAddress savedTop);
};

} // namespace radek::compat_runtime
