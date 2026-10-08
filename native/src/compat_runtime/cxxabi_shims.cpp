#include "compat_runtime/cxxabi_shims.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace radek::compat_runtime::cxxabi {
namespace {

constexpr GuestAddress kCalloutEnd = 0xf0070000U;
constexpr std::size_t kExceptionHeaderBytes = 16U;
constexpr std::size_t kMaximumExceptionBytes = 8U * 1024U * 1024U;
constexpr std::size_t kVtablePageBytes = 4096U;

} // namespace

void ShimAdapter::registerFunction(ShimRegistry &registry, const std::string &symbol,
                                   const std::string &adapterName, Invoke invoke) {
    if (nextCallout_ > kCalloutEnd - 4U)
        throw std::overflow_error("C++ ABI callout range is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libc++abi.dylib";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_;
    binding.invoke = std::move(invoke);
    nextCallout_ += 4U;
    registry.registerBinding(std::move(binding));
}

void ShimAdapter::registerExceptionFunction(ShimRegistry &registry,
                                            const std::string &symbol,
                                            const std::string &adapterName, Invoke invoke) {
    if (nextCallout_ > kCalloutEnd - 4U)
        throw std::overflow_error("C++ ABI exception callout range is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libc++abi.dylib";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_;
    binding.invokeException = std::move(invoke);
    nextCallout_ += 4U;
    registry.registerBinding(std::move(binding));
}

void ShimAdapter::registerData(
    ShimRegistry &registry, const std::string &symbol, const std::string &adapterName,
    std::function<bool(GuestAddressSpace &, GuestAddress &, std::string &)> resolve) {
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libc++abi.dylib";
    binding.adapterName = adapterName;
    binding.resolveGuestAddress = std::move(resolve);
    registry.registerBinding(std::move(binding));
}

bool ShimAdapter::allocateException(CpuRegisterState &registers, GuestAddressSpace &memory,
                                    std::string &reason) {
    const auto requested = static_cast<std::size_t>(registers.r[0]);
    if (requested > kMaximumExceptionBytes) {
        reason = "__cxa_allocate_exception request exceeds the bounded C++ ABI limit";
        return false;
    }
    const auto payload = std::max<std::size_t>(requested, 1U);
    if (payload > std::numeric_limits<std::size_t>::max() - kExceptionHeaderBytes) {
        reason = "__cxa_allocate_exception size overflowed";
        return false;
    }
    const auto mappingSize = ((payload + kExceptionHeaderBytes + 4095U) / 4096U) * 4096U;
    try {
        const auto base = memory.mapAny(mappingSize,
                                        MemoryPermission::Read | MemoryPermission::Write,
                                        "cxxabi-exception");
        const auto object64 = static_cast<std::uint64_t>(base) + kExceptionHeaderBytes;
        if (object64 > std::numeric_limits<GuestAddress>::max()) {
            memory.unmap(base);
            reason = "__cxa_allocate_exception object address overflowed";
            return false;
        }
        const auto object = static_cast<GuestAddress>(object64);
        std::lock_guard<std::mutex> lock(mutex_);
        exceptions_[&memory][object] = ExceptionAllocation{base, mappingSize, object, 0, 0, false};
        registers.r[0] = object;
        ++calls_;
        return true;
    } catch (const std::exception &error) {
        reason = std::string("__cxa_allocate_exception could not map guest storage: ") + error.what();
        return false;
    }
}

bool ShimAdapter::freeException(CpuRegisterState &registers, GuestAddressSpace &memory,
                                std::string &reason) {
    const auto object = registers.r[0];
    if (object == 0)
        return true;
    GuestAddress base = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto state = exceptions_.find(&memory);
        if (state == exceptions_.end()) {
            reason = "__cxa_free_exception received an unknown guest exception object";
            return false;
        }
        const auto found = state->second.find(object);
        if (found == state->second.end()) {
            reason = "__cxa_free_exception received an unknown guest exception object";
            return false;
        }
        base = found->second.mappingBase;
        state->second.erase(found);
        if (state->second.empty())
            exceptions_.erase(state);
    }
    memory.unmap(base);
    ++calls_;
    return true;
}

bool ShimAdapter::beginCatch(CpuRegisterState &registers, GuestAddressSpace &memory,
                             std::string &reason) {
    const auto object = registers.r[0];
    std::lock_guard<std::mutex> lock(mutex_);
    auto state = exceptions_.find(&memory);
    if (state == exceptions_.end()) {
        reason = "__cxa_begin_catch received an unknown guest exception object";
        return false;
    }
    const auto found = state->second.find(object);
    if (found == state->second.end()) {
        reason = "__cxa_begin_catch received an unknown guest exception object";
        return false;
    }
    found->second.activeCatch = true;
    registers.r[0] = object;
    ++calls_;
    return true;
}

bool ShimAdapter::endCatch(CpuRegisterState &, GuestAddressSpace &, std::string &) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++calls_;
    return true;
}

bool ShimAdapter::registerAtExit(CpuRegisterState &registers, GuestAddressSpace &memory,
                                 std::string &reason) {
    // The callback is intentionally retained as guest metadata only. Calling a
    // guest destructor from a host process at shutdown would require a guest CPU
    // continuation; the runtime reports that boundary instead of invoking a host
    // address or silently discarding the ABI arguments.
    const auto callback = registers.r[0];
    const auto object = registers.r[1];
    const auto dso = registers.r[2];
    if (callback != 0 &&
        !memory.contains(callback & ~GuestAddress{1}, 2, MemoryPermission::Execute)) {
        reason = "__cxa_atexit callback is outside executable guest memory";
        return false;
    }
    (void)object;
    (void)dso;
    registers.r[0] = 0; // __cxa_atexit returns zero on successful registration.
    ++calls_;
    return true;
}

bool ShimAdapter::guardAcquire(CpuRegisterState &registers, GuestAddressSpace &memory,
                               std::string &reason) {
    const auto address = registers.r[0];
    if ((address & 3U) != 0 || !memory.contains(address, sizeof(std::uint32_t),
                                                MemoryPermission::Read | MemoryPermission::Write)) {
        reason = "__cxa_guard_acquire received an invalid guest guard address";
        return false;
    }
    std::uint8_t state = 0;
    if (!memory.read(address, &state, sizeof(state))) {
        reason = "__cxa_guard_acquire could not read the guest guard";
        return false;
    }
    if (state == 0) {
        state = 1; // in-progress; this runtime has one guest CPU, so no waiter path is needed.
        if (!memory.write(address, &state, sizeof(state))) {
            reason = "__cxa_guard_acquire could not mark the guest guard";
            return false;
        }
        registers.r[0] = 1;
    } else {
        registers.r[0] = state == 2 ? 0 : 0;
    }
    ++calls_;
    return true;
}

bool ShimAdapter::guardRelease(CpuRegisterState &registers, GuestAddressSpace &memory,
                               std::string &reason) {
    const auto address = registers.r[0];
    const std::uint8_t state = 2;
    if ((address & 3U) != 0 || !memory.write(address, &state, sizeof(state))) {
        reason = "__cxa_guard_release could not commit the guest guard";
        return false;
    }
    ++calls_;
    return true;
}

bool ShimAdapter::guardAbort(CpuRegisterState &registers, GuestAddressSpace &memory,
                             std::string &reason) {
    const auto address = registers.r[0];
    const std::uint8_t state = 0;
    if ((address & 3U) != 0 || !memory.write(address, &state, sizeof(state))) {
        reason = "__cxa_guard_abort could not reset the guest guard";
        return false;
    }
    ++calls_;
    return true;
}

bool ShimAdapter::exceptionBoundary(CpuRegisterState &registers, GuestAddressSpace &memory,
                                    const char *operation, std::string &reason) {
    const auto object = registers.r[0];
    std::lock_guard<std::mutex> lock(mutex_);
    auto state = exceptions_.find(&memory);
    if (object != 0 && (state == exceptions_.end() || state->second.find(object) == state->second.end())) {
        reason = std::string(operation) + " received an unknown guest exception object";
        return false;
    }
    ++calls_;
    ++thrown_;
    reason = std::string(operation) + " reached the bounded guest C++ exception boundary";
    return true;
}

bool ShimAdapter::materializeVtable(GuestAddressSpace &memory, const std::string &symbol,
                                    GuestAddress &address, std::string &reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &bySymbol = vtables_[&memory];
    // Use a stable per-symbol key. The four ABI RTTI vtable names are fixed by
    // the compiler, and the runtime only uses this key inside this address-space
    // private cache.
    std::uint32_t key = 0;
    for (const unsigned char character : symbol)
        key = key * 33U + character + 1U;
    const auto cached = bySymbol.find(key);
    if (cached != bySymbol.end()) {
        address = cached->second;
        return true;
    }
    try {
        const auto page = memory.mapAny(kVtablePageBytes,
                                        MemoryPermission::Read | MemoryPermission::Write,
                                        "cxxabi-vtable");
        // The first entries are null RTTI/function slots. An eventual virtual
        // call into an absent slot therefore faults in guest memory rather than
        // jumping through a host pointer.
        std::array<std::uint32_t, 4> zeros{};
        if (!memory.write(page, zeros.data(), sizeof(zeros))) {
            memory.unmap(page);
            reason = symbol + " vtable page could not be initialized";
            return false;
        }
        bySymbol.emplace(key, page);
        address = page;
        return true;
    } catch (const std::exception &error) {
        reason = symbol + " vtable could not be materialized: " + error.what();
        return false;
    }
}

void ShimAdapter::registerBindings(ShimRegistry &registry) {
    if (registered_)
        throw std::runtime_error("C++ ABI shim adapter is already registered");

    registerFunction(registry, "___cxa_allocate_exception", "cxxabi-allocate-exception",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return allocateException(registers, memory, reason);
                     });
    registerFunction(registry, "___cxa_free_exception", "cxxabi-free-exception",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return freeException(registers, memory, reason);
                     });
    registerFunction(registry, "___cxa_begin_catch", "cxxabi-begin-catch",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return beginCatch(registers, memory, reason);
                     });
    registerFunction(registry, "___cxa_end_catch", "cxxabi-end-catch",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return endCatch(registers, memory, reason);
                     });
    registerFunction(registry, "___cxa_atexit", "cxxabi-atexit-guest-metadata",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return registerAtExit(registers, memory, reason);
                     });
    registerFunction(registry, "___cxa_guard_acquire", "cxxabi-guard-acquire",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return guardAcquire(registers, memory, reason);
                     });
    registerFunction(registry, "___cxa_guard_release", "cxxabi-guard-release",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return guardRelease(registers, memory, reason);
                     });
    registerFunction(registry, "___cxa_guard_abort", "cxxabi-guard-abort",
                     [this](auto &registers, auto &memory, auto &reason) {
                         return guardAbort(registers, memory, reason);
                     });
    registerExceptionFunction(registry, "___cxa_throw", "cxxabi-throw-boundary",
                              [this](auto &registers, auto &memory, auto &reason) {
                                  const auto object = registers.r[0];
                                  const auto typeInfo = registers.r[1];
                                  const auto destructor = registers.r[2];
                                  {
                                      std::lock_guard<std::mutex> lock(mutex_);
                                      auto state = exceptions_.find(&memory);
                                      if (state == exceptions_.end() || state->second.find(object) == state->second.end()) {
                                          reason = "___cxa_throw received an unknown guest exception object";
                                          return false;
                                      }
                                      auto &entry = state->second.at(object);
                                      entry.typeInfo = typeInfo;
                                      entry.destructor = destructor;
                                  }
                                  return exceptionBoundary(registers, memory, "___cxa_throw", reason);
                              });
    registerExceptionFunction(registry, "___cxa_rethrow", "cxxabi-rethrow-boundary",
                              [this](auto &registers, auto &memory, auto &reason) {
                                  return exceptionBoundary(registers, memory, "___cxa_rethrow", reason);
                              });
    registerExceptionFunction(registry, "___cxa_pure_virtual", "cxxabi-pure-virtual-boundary",
                              [this](auto &registers, auto &memory, auto &reason) {
                                  return exceptionBoundary(registers, memory, "___cxa_pure_virtual", reason);
                              });
    registerExceptionFunction(registry, "__ZSt9terminatev", "cxxabi-terminate-boundary",
                              [this](auto &registers, auto &memory, auto &reason) {
                                  return exceptionBoundary(registers, memory, "__ZSt9terminatev", reason);
                              });

    for (const auto *symbol : {"__ZTVN10__cxxabiv117__class_type_infoE",
                               "__ZTVN10__cxxabiv119__pointer_type_infoE",
                               "__ZTVN10__cxxabiv120__si_class_type_infoE",
                               "__ZTVN10__cxxabiv121__vmi_class_type_infoE"}) {
        registerData(registry, symbol, "cxxabi-vtable-data",
                     [this, symbol](GuestAddressSpace &memory, GuestAddress &address,
                                    std::string &reason) {
                         return materializeVtable(memory, symbol, address, reason);
                     });
    }

    registry.registerImageInitializer(
        "cxxabi-exception-state",
        [this](GuestAddressSpace &memory, const std::vector<GuestImageSection> &, std::string &) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto state = exceptions_.find(&memory);
            if (state != exceptions_.end()) {
                for (const auto &[object, allocation] : state->second)
                    (void)object, memory.unmap(allocation.mappingBase);
                exceptions_.erase(state);
            }
            guards_.erase(&memory);
            auto tables = vtables_.find(&memory);
            if (tables != vtables_.end()) {
                for (const auto &[key, address] : tables->second)
                    (void)key, memory.unmap(address);
                vtables_.erase(tables);
            }
            return true;
        });
    registered_ = true;
}

std::uint64_t ShimAdapter::callCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
}

std::uint64_t ShimAdapter::thrownCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return thrown_;
}

} // namespace radek::compat_runtime::cxxabi
