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

// The guest is a 32-bit process, so the only hard bound on its memory is the
// address space itself. Defaulting the allocation budget to the full address
// space means the runtime never refuses a guest mapping for an artificial
// budget reason; it stops only when the guest's own addressing runs out.
inline constexpr std::size_t kGuestMemoryLimitUnlimited = std::size_t{1} << 32;

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
    Transferred,
    ExceptionRaised,
    Failed,
};

struct GuestMemoryCallbacks {
    std::function<bool(GuestAddress, void *, std::size_t)> read;
    // Host-side write (a shim filling a guest buffer): records the touched pages
    // so an attached CPU engine can upload only those pages before it resumes.
    std::function<bool(GuestAddress, const void *, std::size_t)> write;
    // Engine-side store relayed back to the host copy: the engine already holds
    // these bytes, so they must not be queued for upload again.
    std::function<bool(GuestAddress, const void *, std::size_t)> writeFromGuest;
    std::function<std::vector<GuestRegionView>()> regions;
    // Forgets pending host writes, used right after an engine adopts the host
    // copy so the first shim call does not re-upload the whole address space.
    std::function<void()> clearHostWrites;
    // Hands every page written from the host since the previous drain to the
    // visitor. Returns false (leaving the pages queued) when the visitor fails.
    std::function<bool(const std::function<bool(GuestAddress, const std::uint8_t *, std::size_t)> &)>
        drainHostWrites;
    std::function<GuestCalloutResult(GuestAddress, CpuRegisterState &, std::string &)> invokeGuestCallout;
};

/** A bounds-checked 32-bit guest address space backed by host-owned memory. */
class GuestAddressSpace {
    struct Region {
        GuestRegionView view;
        std::vector<std::uint8_t> bytes;
        // One bit per memory page: host writes that the engine has not seen yet.
        std::vector<std::uint64_t> dirtyPages;
        bool dirty = false;
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
    // Slow path for an access that starts in one region and ends in an adjacent
    // one: contiguous guest memory is one address range to the guest, so the
    // access is split at region boundaries (with per-region permission checks)
    // instead of being refused.
    bool readAcross(GuestAddress address, void *destination, std::size_t size,
                    MemoryPermission required) const;
    bool writeAcross(GuestAddress address, const void *source, std::size_t size,
                     bool markHostWrite);

  public:
    explicit GuestAddressSpace(std::size_t memoryLimit = kGuestMemoryLimitUnlimited);

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
    // Engine relay for guest stores; identical to write() except that the pages
    // are not queued for upload, because the engine created those bytes.
    bool writeFromGuest(GuestAddress address, const void *source, std::size_t size);

    // Host-write tracking used by the CPU backend: an executing guest only pays
    // for the pages a shim actually touched instead of a full address-space copy
    // after every callout.
    void markDirty(GuestAddress address, std::size_t size);
    void clearDirty();
    bool drainDirtyPages(const std::function<bool(GuestAddress, const std::uint8_t *, std::size_t)> &visit);
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
