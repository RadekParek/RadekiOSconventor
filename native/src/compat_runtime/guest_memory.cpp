#include "compat_runtime/guest_memory.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace radek::compat_runtime {
namespace {
constexpr std::uint64_t kGuestAddressSpaceSize = std::uint64_t{1} << 32;

bool isPowerOfTwo(std::size_t value) { return value != 0 && (value & (value - 1)) == 0; }

std::uint64_t alignUp(std::uint64_t value, std::size_t alignment) {
    if (!isPowerOfTwo(alignment))
        throw std::invalid_argument("alignment must be a non-zero power of two");
    const auto mask = static_cast<std::uint64_t>(alignment - 1);
    if (value > std::numeric_limits<std::uint64_t>::max() - mask)
        throw std::overflow_error("guest address alignment overflow");
    return (value + mask) & ~mask;
}
} // namespace

GuestAddressSpace::GuestAddressSpace(std::size_t memoryLimit) : memoryLimit_(memoryLimit) {
    if (memoryLimit_ == 0 || memoryLimit_ > kGuestAddressSpaceSize)
        throw std::invalid_argument("guest memory limit must fit the 32-bit address space");
}

GuestAddressSpace::Region *GuestAddressSpace::findRegion(GuestAddress address, std::size_t size) {
    auto it = regions_.upper_bound(address);
    if (it == regions_.begin())
        return nullptr;
    --it;
    const std::uint64_t start = it->second.view.base;
    const std::uint64_t end = start + it->second.view.size;
    const std::uint64_t accessStart = address;
    const std::uint64_t accessEnd = accessStart + size;
    return accessStart >= start && accessEnd >= accessStart && accessEnd <= end ? &it->second : nullptr;
}

const GuestAddressSpace::Region *GuestAddressSpace::findRegion(GuestAddress address,
                                                               std::size_t size) const {
    auto it = regions_.upper_bound(address);
    if (it == regions_.begin())
        return nullptr;
    --it;
    const std::uint64_t start = it->second.view.base;
    const std::uint64_t end = start + it->second.view.size;
    const std::uint64_t accessStart = address;
    const std::uint64_t accessEnd = accessStart + size;
    return accessStart >= start && accessEnd >= accessStart && accessEnd <= end ? &it->second : nullptr;
}

GuestAddress GuestAddressSpace::mapAt(GuestAddress base, std::size_t size,
                                      MemoryPermission permissions, std::string name) {
    if (base == 0 || size == 0)
        throw std::invalid_argument("guest regions must have a non-zero base and size");
    const std::uint64_t end = static_cast<std::uint64_t>(base) + size;
    if (end > kGuestAddressSpaceSize)
        throw std::overflow_error("guest region exceeds the 32-bit address space");
    if (size > memoryLimit_ - std::min(memoryLimit_, mappedBytes_))
        throw std::length_error("guest address space memory limit exceeded");

    auto next = regions_.lower_bound(base);
    if (next != regions_.end() && end > next->first)
        throw std::runtime_error("guest region overlaps an existing mapping");
    if (next != regions_.begin()) {
        const auto previous = std::prev(next);
        const std::uint64_t previousEnd = static_cast<std::uint64_t>(previous->second.view.base) +
                                          previous->second.view.size;
        if (base < previousEnd)
            throw std::runtime_error("guest region overlaps an existing mapping");
    }

    Region region;
    region.view = GuestRegionView{base, size, permissions, std::move(name)};
    region.bytes.resize(size, 0);
    regions_.emplace(base, std::move(region));
    mappedBytes_ += size;
    return base;
}

GuestAddress GuestAddressSpace::mapAny(std::size_t size, MemoryPermission permissions,
                                       std::string name, std::size_t alignment) {
    if (size == 0)
        throw std::invalid_argument("guest mapping size must be non-zero");
    std::uint64_t candidate = alignUp(nextDynamicAddress_, alignment);
    while (candidate + size <= 0xF0000000ULL) {
        const auto address = static_cast<GuestAddress>(candidate);
        auto next = regions_.lower_bound(address);
        bool available = next == regions_.end() || candidate + size <= next->first;
        if (available && next != regions_.begin()) {
            const auto previous = std::prev(next);
            const std::uint64_t previousEnd = static_cast<std::uint64_t>(previous->second.view.base) +
                                              previous->second.view.size;
            available = candidate >= previousEnd;
        }
        if (available) {
            const auto result = mapAt(address, size, permissions, std::move(name));
            nextDynamicAddress_ = static_cast<GuestAddress>(alignUp(candidate + size, alignment));
            return result;
        }
        if (next != regions_.end())
            candidate = alignUp(static_cast<std::uint64_t>(next->second.view.base) +
                                    next->second.view.size,
                                alignment);
        else
            break;
    }
    throw std::runtime_error("no free guest address range is available");
}

void GuestAddressSpace::unmap(GuestAddress base) {
    auto found = regions_.find(base);
    if (found == regions_.end())
        throw std::runtime_error("guest region is not mapped");
    if (heapReady_ && base == heapBase_)
        heapReady_ = false;
    mappedBytes_ -= found->second.view.size;
    regions_.erase(found);
}

void GuestAddressSpace::setPermissions(GuestAddress base, MemoryPermission permissions) {
    auto found = regions_.find(base);
    if (found == regions_.end())
        throw std::runtime_error("guest region is not mapped");
    found->second.view.permissions = permissions;
}

void GuestAddressSpace::configureHeap(GuestAddress base, std::size_t size) {
    if (heapReady_)
        throw std::runtime_error("guest heap is already configured");
    mapAt(base, size, MemoryPermission::Read | MemoryPermission::Write, "guest-heap");
    heapReady_ = true;
    heapBase_ = base;
    heapSize_ = size;
    heapCursor_ = 0;
}

GuestAddress GuestAddressSpace::allocateHeap(std::size_t size, std::size_t alignment) {
    if (!heapReady_)
        throw std::runtime_error("guest heap is not configured");
    if (size == 0)
        throw std::invalid_argument("guest heap allocations must be non-empty");
    const std::uint64_t aligned = alignUp(heapCursor_, alignment);
    if (aligned > heapSize_ || size > heapSize_ - static_cast<std::size_t>(aligned))
        throw std::bad_alloc();
    const std::uint64_t address = static_cast<std::uint64_t>(heapBase_) + aligned;
    if (address + size > kGuestAddressSpaceSize)
        throw std::overflow_error("guest heap address overflow");
    heapCursor_ = static_cast<std::size_t>(aligned) + size;
    std::memset(static_cast<std::uint8_t *>(guestToHost(static_cast<GuestAddress>(address), size,
                                                        MemoryPermission::Write)),
                0, size);
    return static_cast<GuestAddress>(address);
}

bool GuestAddressSpace::read(GuestAddress address, void *destination, std::size_t size) const {
    if ((!destination && size != 0) || size == 0)
        return size == 0;
    const auto *region = findRegion(address, size);
    if (!region || !hasPermission(region->view.permissions, MemoryPermission::Read))
        return false;
    const auto offset = static_cast<std::size_t>(address - region->view.base);
    std::memcpy(destination, region->bytes.data() + offset, size);
    return true;
}

bool GuestAddressSpace::write(GuestAddress address, const void *source, std::size_t size) {
    if ((!source && size != 0) || size == 0)
        return size == 0;
    auto *region = findRegion(address, size);
    if (!region || !hasPermission(region->view.permissions, MemoryPermission::Write))
        return false;
    const auto offset = static_cast<std::size_t>(address - region->view.base);
    std::memcpy(region->bytes.data() + offset, source, size);
    return true;
}

bool GuestAddressSpace::initialize(GuestAddress address, const void *source, std::size_t size) {
    if ((!source && size != 0) || size == 0)
        return size == 0;
    auto *region = findRegion(address, size);
    if (!region)
        return false;
    const auto offset = static_cast<std::size_t>(address - region->view.base);
    std::memcpy(region->bytes.data() + offset, source, size);
    return true;
}

void *GuestAddressSpace::guestToHost(GuestAddress address, std::size_t size,
                                     MemoryPermission required) {
    auto *region = findRegion(address, size);
    if (!region || !hasPermission(region->view.permissions, required))
        return nullptr;
    return region->bytes.data() + static_cast<std::size_t>(address - region->view.base);
}

const void *GuestAddressSpace::guestToHost(GuestAddress address, std::size_t size,
                                           MemoryPermission required) const {
    const auto *region = findRegion(address, size);
    if (!region || !hasPermission(region->view.permissions, required))
        return nullptr;
    return region->bytes.data() + static_cast<std::size_t>(address - region->view.base);
}

std::optional<GuestAddress> GuestAddressSpace::hostToGuest(const void *pointer) const {
    if (!pointer)
        return std::nullopt;
    const auto host = reinterpret_cast<std::uintptr_t>(pointer);
    for (const auto &entry : regions_) {
        const auto begin = reinterpret_cast<std::uintptr_t>(entry.second.bytes.data());
        const auto end = begin + entry.second.bytes.size();
        if (host >= begin && host < end) {
            const auto guest = static_cast<std::uint64_t>(entry.second.view.base) + (host - begin);
            if (guest < kGuestAddressSpaceSize)
                return static_cast<GuestAddress>(guest);
        }
    }
    return std::nullopt;
}

bool GuestAddressSpace::contains(GuestAddress address, std::size_t size,
                                 MemoryPermission required) const {
    const auto *region = findRegion(address, size);
    return region && hasPermission(region->view.permissions, required);
}

std::vector<GuestRegionView> GuestAddressSpace::regions() const {
    std::vector<GuestRegionView> result;
    result.reserve(regions_.size());
    for (const auto &entry : regions_)
        result.push_back(entry.second.view);
    return result;
}

GuestMemoryCallbacks GuestAddressSpace::callbacks() {
    return GuestMemoryCallbacks{
        [this](GuestAddress address, void *destination, std::size_t size) {
            return read(address, destination, size);
        },
        [this](GuestAddress address, const void *source, std::size_t size) {
            return write(address, source, size);
        },
        [this]() { return regions(); },
        {},
    };
}

} // namespace radek::compat_runtime
