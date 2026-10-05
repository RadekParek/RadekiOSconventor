#pragma once

#include "compat_runtime/guest_state.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace radek::compat_runtime {

using GuestAddress = std::uint32_t;

enum class MemoryPermission : std::uint8_t {
    None = 0,
    Read = 1,
    Write = 2,
    Execute = 4,
};

constexpr MemoryPermission operator|(MemoryPermission left, MemoryPermission right) {
    return static_cast<MemoryPermission>(static_cast<std::uint8_t>(left) |
                                         static_cast<std::uint8_t>(right));
}

constexpr MemoryPermission operator&(MemoryPermission left, MemoryPermission right) {
    return static_cast<MemoryPermission>(static_cast<std::uint8_t>(left) &
                                         static_cast<std::uint8_t>(right));
}

constexpr bool hasPermission(MemoryPermission value, MemoryPermission required) {
    return (value & required) == required;
}

struct GuestRegionView {
    GuestAddress base = 0;
    std::size_t size = 0;
    MemoryPermission permissions = MemoryPermission::None;
    std::string name;
};

enum class GuestCalloutResult {
    NotRegistered,
    Returned,
    Failed,
};

struct GuestMemoryCallbacks {
    std::function<bool(GuestAddress, void *, std::size_t)> read;
    std::function<bool(GuestAddress, const void *, std::size_t)> write;
    std::function<std::vector<GuestRegionView>()> regions;
    std::function<GuestCalloutResult(GuestAddress, CpuRegisterState &, std::string &)> invokeGuestCallout;
};

/** A bounds-checked 32-bit guest address space backed by host-owned memory. */
class GuestAddressSpace {
    struct Region {
        GuestRegionView view;
        std::vector<std::uint8_t> bytes;
    };

    std::map<GuestAddress, Region> regions_;
    std::size_t memoryLimit_;
    std::size_t mappedBytes_ = 0;
    GuestAddress nextDynamicAddress_ = 0x10000000;
    bool heapReady_ = false;
    GuestAddress heapBase_ = 0;
    std::size_t heapSize_ = 0;
    std::size_t heapCursor_ = 0;

    Region *findRegion(GuestAddress address, std::size_t size);
    const Region *findRegion(GuestAddress address, std::size_t size) const;

  public:
    explicit GuestAddressSpace(std::size_t memoryLimit = 256U * 1024U * 1024U);

    GuestAddress mapAt(GuestAddress base, std::size_t size, MemoryPermission permissions,
                       std::string name);
    GuestAddress mapAny(std::size_t size, MemoryPermission permissions, std::string name,
                        std::size_t alignment = 4096);
    void unmap(GuestAddress base);
    void setPermissions(GuestAddress base, MemoryPermission permissions);

    void configureHeap(GuestAddress base, std::size_t size);
    GuestAddress allocateHeap(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

    bool read(GuestAddress address, void *destination, std::size_t size) const;
    bool write(GuestAddress address, const void *source, std::size_t size);
    // Loader-only initialization/fixup access; guest read/write permissions are unchanged.
    bool initialize(GuestAddress address, const void *source, std::size_t size);

    void *guestToHost(GuestAddress address, std::size_t size = 1,
                      MemoryPermission required = MemoryPermission::Read);
    const void *guestToHost(GuestAddress address, std::size_t size = 1,
                            MemoryPermission required = MemoryPermission::Read) const;
    std::optional<GuestAddress> hostToGuest(const void *pointer) const;

    template <typename T>
    T *guestPointer(GuestAddress address, MemoryPermission required = MemoryPermission::Read) {
        static_assert(std::is_trivially_copyable<T>::value, "guest pointers require a trivial value type");
        if (address % alignof(T) != 0)
            return nullptr;
        return static_cast<T *>(guestToHost(address, sizeof(T), required));
    }

    template <typename T>
    const T *guestPointer(GuestAddress address,
                         MemoryPermission required = MemoryPermission::Read) const {
        static_assert(std::is_trivially_copyable<T>::value, "guest pointers require a trivial value type");
        if (address % alignof(T) != 0)
            return nullptr;
        return static_cast<const T *>(guestToHost(address, sizeof(T), required));
    }

    bool contains(GuestAddress address, std::size_t size,
                  MemoryPermission required = MemoryPermission::None) const;
    std::vector<GuestRegionView> regions() const;
    GuestMemoryCallbacks callbacks();
    std::size_t mappedBytes() const noexcept { return mappedBytes_; }
    std::size_t memoryLimit() const noexcept { return memoryLimit_; }
};

} // namespace radek::compat_runtime
