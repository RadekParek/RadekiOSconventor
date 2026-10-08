#pragma once

#include "compat_runtime/shim_registry.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace radek::compat_runtime::cxxabi {

/**
 * Bounded Itanium C++ ABI support used by 32-bit Darwin images.
 *
 * The adapter is intentionally small and explicit. Allocation uses independent
 * mapped guest pages (so it cannot overlap libSystem's first-fit heap), guard
 * variables use the guest's real bytes, and exception entry points preserve the
 * thrown object/type/destructor tuple before stopping at the runtime's exception
 * boundary. It does not pretend that a host C++ exception can jump through guest
 * ARM stack frames; unsupported non-local transfer is reported as
 * GuestCalloutResult::ExceptionRaised rather than silently returning.
 *
 * This is enough for startup/static-initializer ABI traffic and makes every C++
 * ABI import a typed registry binding. Full guest catch/landing-pad execution
 * still requires a guest unwinder and remains an explicit runtime boundary.
 */
class ShimAdapter {
    struct ExceptionAllocation {
        GuestAddress mappingBase = 0;
        std::size_t mappingSize = 0;
        GuestAddress object = 0;
        GuestAddress typeInfo = 0;
        GuestAddress destructor = 0;
        bool activeCatch = false;
    };

    mutable std::mutex mutex_;
    std::map<const GuestAddressSpace *, std::map<GuestAddress, ExceptionAllocation>> exceptions_;
    std::map<const GuestAddressSpace *, std::map<GuestAddress, GuestAddress>> vtables_;
    std::map<const GuestAddressSpace *, std::map<GuestAddress, std::uint8_t>> guards_;
    std::uint32_t nextCallout_ = 0xf0060000U;
    std::uint64_t calls_ = 0;
    std::uint64_t thrown_ = 0;
    bool registered_ = false;

    using Invoke = std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)>;

    void registerFunction(ShimRegistry &registry, const std::string &symbol,
                          const std::string &adapterName, Invoke invoke);
    void registerExceptionFunction(ShimRegistry &registry, const std::string &symbol,
                                   const std::string &adapterName, Invoke invoke);
    void registerData(ShimRegistry &registry, const std::string &symbol,
                      const std::string &adapterName,
                      std::function<bool(GuestAddressSpace &, GuestAddress &, std::string &)> resolve);
    bool allocateException(CpuRegisterState &registers, GuestAddressSpace &memory,
                           std::string &reason);
    bool freeException(CpuRegisterState &registers, GuestAddressSpace &memory,
                       std::string &reason);
    bool beginCatch(CpuRegisterState &registers, GuestAddressSpace &memory,
                    std::string &reason);
    bool endCatch(CpuRegisterState &registers, GuestAddressSpace &memory,
                  std::string &reason);
    bool registerAtExit(CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason);
    bool guardAcquire(CpuRegisterState &registers, GuestAddressSpace &memory,
                      std::string &reason);
    bool guardRelease(CpuRegisterState &registers, GuestAddressSpace &memory,
                      std::string &reason);
    bool guardAbort(CpuRegisterState &registers, GuestAddressSpace &memory,
                    std::string &reason);
    bool exceptionBoundary(CpuRegisterState &registers, GuestAddressSpace &memory,
                           const char *operation, std::string &reason);
    bool materializeVtable(GuestAddressSpace &memory, const std::string &symbol,
                           GuestAddress &address, std::string &reason);

  public:
    void registerBindings(ShimRegistry &registry);

    std::uint64_t callCount() const;
    std::uint64_t thrownCount() const;
};

} // namespace radek::compat_runtime::cxxabi
