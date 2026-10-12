#pragma once

#include "compat_runtime/shim_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace radek::compat_runtime::libsystem {

/**
 * Bounded libSystem (C runtime) adapters for guest code.
 *
 * These are real implementations over the guest address space, not stubs: the
 * memory/string functions operate on guest memory, and malloc/free/calloc/
 * realloc are served by a first-fit allocator inside one bounded guest heap
 * region. Everything the guest imports that is *not* registered here still
 * fails closed through the trap adapter; the runtime never guesses a signature
 * it cannot honour (printf-style varargs, signals, locale, threads).
 *
 * The guest ABI is 32-bit ARM AAPCS: pointer/integer arguments in r0-r3 and the
 * result in r0, which is what every binding below assumes.
 */
class ShimAdapter {
    struct Block {
        std::size_t size = 0;
        bool free = false;
    };

    mutable std::mutex mutex_;
    // The heap is owned by whichever guest address space first calls malloc.
    // A second address space (a second boot attempt in one process) is refused
    // instead of quietly sharing block bookkeeping.
    GuestAddressSpace *heapOwner_ = nullptr;
    GuestAddress heapBase_ = 0;
    std::size_t heapSize_ = 0;
    std::size_t heapCursor_ = 0;
    /** Every guest block, ordered by address: first-fit search plus coalescing. */
    std::map<GuestAddress, Block> blocks_;
    std::size_t allocations_ = 0;
    std::size_t frees_ = 0;
    std::uint32_t nextCallout_ = 0xf0040000U;
    bool registered_ = false;

    bool ensureHeap(GuestAddressSpace &memory);
    GuestAddress allocate(GuestAddressSpace &memory, std::size_t size);
    void release(GuestAddressSpace &memory, GuestAddress address);
    void registerFunction(ShimRegistry &registry, const std::string &symbol,
                          const std::string &adapterName,
                          std::function<bool(CpuRegisterState &, GuestAddressSpace &,
                                             std::string &)> invoke);

  public:
    /** Guest heap reserve: a quarter of the runtime's memory limit, 16-128 MiB. */
    static constexpr std::size_t kMinimumHeapBytes = 16U * 1024U * 1024U;
    static constexpr std::size_t kMaximumHeapBytes = 128U * 1024U * 1024U;

    void registerBindings(ShimRegistry &registry);

    /** Guest-heap allocation shared with malloc and operator new. Takes the heap lock; 0 on failure. */
    GuestAddress guestAllocate(GuestAddressSpace &memory, std::size_t size);
    /** Guest-heap release shared with free and operator delete. Takes the heap lock. */
    void guestRelease(GuestAddressSpace &memory, GuestAddress address);

    /** Observability used by tests and the boot report; never gameplay evidence. */
    std::size_t allocationCount() const;
    std::size_t freeCount() const;
    std::size_t heapBytes() const;
};

} // namespace radek::compat_runtime::libsystem
