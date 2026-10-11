#include "compat_runtime/libsystem_shims.hpp"

#include "compat_runtime/virtual_file_system.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace radek::compat_runtime::libsystem {
namespace {

constexpr std::size_t kCopyChunk = 64U * 1024U;
constexpr std::size_t kMaximumStringBytes = 1U * 1024U * 1024U;
constexpr std::size_t kMinimumAllocation = 8U;
constexpr std::size_t kBlockAlignment = 8U;

/** Guest reads that tolerate a range ending exactly at a region boundary. */
bool readGuest(const GuestAddressSpace &memory, GuestAddress address, std::uint8_t *destination,
               std::size_t size) {
    if (size == 0)
        return true;
    if (memory.read(address, destination, size))
        return true;
    for (std::size_t index = 0; index < size; ++index) {
        if (!memory.read(static_cast<GuestAddress>(address + index), destination + index, 1))
            return false;
    }
    return true;
}

bool writeGuest(GuestAddressSpace &memory, GuestAddress address, const std::uint8_t *source,
                std::size_t size) {
    if (size == 0)
        return true;
    if (memory.write(address, source, size))
        return true;
    for (std::size_t index = 0; index < size; ++index) {
        if (!memory.write(static_cast<GuestAddress>(address + index), source + index, 1))
            return false;
    }
    return true;
}

bool readCString(const GuestAddressSpace &memory, GuestAddress address, std::string &result,
                 std::size_t limit = kMaximumStringBytes) {
    result.clear();
    if (address == 0)
        return false;
    GuestAddress cursor = address;
    while (result.size() < limit) {
        char chunk[64];
        if (!readGuest(memory, cursor, reinterpret_cast<std::uint8_t *>(chunk), sizeof(chunk)))
            return false;
        const auto *terminator = static_cast<const char *>(
            std::memchr(chunk, '\0', sizeof(chunk)));
        if (terminator != nullptr) {
            result.append(chunk, static_cast<std::size_t>(terminator - chunk));
            return true;
        }
        result.append(chunk, sizeof(chunk));
        cursor = static_cast<GuestAddress>(cursor + sizeof(chunk));
    }
    return false;
}

/** memcpy-style copy. A host staging buffer makes the operation region-agnostic. */
bool copyForward(GuestAddressSpace &memory, GuestAddress destination, GuestAddress source,
                 std::size_t size) {
    std::vector<std::uint8_t> buffer(std::min(size, kCopyChunk));
    std::size_t copied = 0;
    while (copied < size) {
        const std::size_t chunk = std::min(buffer.size(), size - copied);
        if (!readGuest(memory, static_cast<GuestAddress>(source + copied), buffer.data(), chunk))
            return false;
        if (!writeGuest(memory, static_cast<GuestAddress>(destination + copied), buffer.data(),
                        chunk))
            return false;
        copied += chunk;
    }
    return true;
}

/** memmove semantics: reverse order when the ranges overlap forward. */
bool copyRanges(GuestAddressSpace &memory, GuestAddress destination, GuestAddress source,
                std::size_t size) {
    const bool overlaps = destination > source && destination < source + size;
    if (!overlaps)
        return copyForward(memory, destination, source, size);
    std::vector<std::uint8_t> buffer(std::min(size, kCopyChunk));
    std::size_t remaining = size;
    while (remaining > 0) {
        const std::size_t chunk = std::min(buffer.size(), remaining);
        remaining -= chunk;
        const auto from = static_cast<GuestAddress>(source + remaining);
        const auto to = static_cast<GuestAddress>(destination + remaining);
        if (!readGuest(memory, from, buffer.data(), chunk))
            return false;
        if (!writeGuest(memory, to, buffer.data(), chunk))
            return false;
    }
    return true;
}

int compareBytes(std::uint8_t left, std::uint8_t right) {
    if (left == right)
        return 0;
    return left < right ? -1 : 1;
}

// ---- guest floating-point ABI helpers (s0..s15 / d0..d7) ----

float singleArgument(const CpuRegisterState &registers, int index) {
    const auto bits = static_cast<std::uint32_t>(registers.d[static_cast<std::size_t>(index / 2)] >>
                                                  ((index % 2) ? 32 : 0));
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double doubleArgument(const CpuRegisterState &registers, int index) {
    double value = 0.0;
    const auto bits = registers.d[static_cast<std::size_t>(index)];
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void setSingleResult(CpuRegisterState &registers, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    registers.d[0] = (registers.d[0] & ~std::uint64_t{0xFFFFFFFFULL}) | bits;
}

void setDoubleResult(CpuRegisterState &registers, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    registers.d[0] = bits;
}

bool fail(GuestAddressSpace &, std::string &reason, const std::string &detail) {
    reason = detail;
    return false;
}

// ---- individual adapters (guest ABI: r0-r3 arguments, r0 result) ----

bool memcpyAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason) {
    const auto destination = registers.r[0];
    const auto source = registers.r[1];
    const auto size = registers.r[2];
    if (!copyRanges(memory, destination, source, size))
        return fail(memory, reason, "memcpy could not read or write the guest range it was given");
    registers.r[0] = destination;
    return true;
}

bool memsetAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason) {
    const auto destination = registers.r[0];
    const auto value = static_cast<std::uint8_t>(registers.r[1] & 0xffU);
    const auto size = static_cast<std::size_t>(registers.r[2]);
    std::vector<std::uint8_t> buffer(std::min(size, kCopyChunk), value);
    std::size_t written = 0;
    while (written < size) {
        const std::size_t chunk = std::min(buffer.size(), size - written);
        if (!writeGuest(memory, static_cast<GuestAddress>(destination + written), buffer.data(),
                        chunk))
            return fail(memory, reason, "memset could not write the guest range it was given");
        written += chunk;
    }
    registers.r[0] = destination;
    return true;
}

bool memcmpAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason) {
    const auto left = registers.r[0];
    const auto right = registers.r[1];
    const auto size = static_cast<std::size_t>(registers.r[2]);
    std::vector<std::uint8_t> leftChunk(std::min(size, kCopyChunk));
    std::vector<std::uint8_t> rightChunk(std::min(size, kCopyChunk));
    std::size_t compared = 0;
    int result = 0;
    while (compared < size && result == 0) {
        const std::size_t chunk = std::min(leftChunk.size(), size - compared);
        if (!readGuest(memory, static_cast<GuestAddress>(left + compared), leftChunk.data(), chunk) ||
            !readGuest(memory, static_cast<GuestAddress>(right + compared), rightChunk.data(), chunk))
            return fail(memory, reason, "memcmp could not read the guest ranges it was given");
        for (std::size_t index = 0; index < chunk; ++index) {
            result = compareBytes(leftChunk[index], rightChunk[index]);
            if (result != 0)
                break;
        }
        compared += chunk;
    }
    registers.r[0] = static_cast<std::uint32_t>(result);
    return true;
}

bool strlenAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason) {
    std::string text;
    if (!readCString(memory, registers.r[0], text))
        return fail(memory, reason, "strlen could not read a terminated guest string");
    registers.r[0] = static_cast<std::uint32_t>(text.size());
    return true;
}

bool strcmpAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason,
                   bool bounded) {
    std::string left;
    std::string right;
    if (!readCString(memory, registers.r[0], left) || !readCString(memory, registers.r[1], right))
        return fail(memory, reason, "strcmp could not read a terminated guest string");
    // Comparing with the terminator treated as an ordinary byte gives strcmp;
    // bounding the comparison gives strncmp. Both stop at the first difference.
    const std::size_t limit =
        bounded ? std::min<std::size_t>(registers.r[2], std::max(left.size(), right.size()))
                : std::max(left.size(), right.size());
    int result = 0;
    for (std::size_t index = 0; index < limit && result == 0; ++index) {
        const auto leftByte = index < left.size() ? static_cast<std::uint8_t>(left[index]) : 0;
        const auto rightByte = index < right.size() ? static_cast<std::uint8_t>(right[index]) : 0;
        result = compareBytes(leftByte, rightByte);
    }
    registers.r[0] = static_cast<std::uint32_t>(result);
    return true;
}

bool strncmpAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason) {
    return strcmpAdapter(registers, memory, reason, true);
}

bool strcmpExactAdapter(CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) {
    return strcmpAdapter(registers, memory, reason, false);
}

bool readLengthBoundedString(const GuestAddressSpace &memory, GuestAddress address,
                             std::string &result, std::size_t maximum) {
    result.clear();
    if (address == 0)
        return false;
    GuestAddress cursor = address;
    while (result.size() < maximum) {
        char chunk[64];
        if (!readGuest(memory, cursor, reinterpret_cast<std::uint8_t *>(chunk), sizeof(chunk)))
            return false;
        const auto *terminator =
            static_cast<const char *>(std::memchr(chunk, '\0', sizeof(chunk)));
        const std::size_t available = terminator != nullptr
            ? static_cast<std::size_t>(terminator - chunk)
            : sizeof(chunk);
        const std::size_t take = std::min(available, maximum - result.size());
        result.append(chunk, take);
        if (terminator != nullptr || take < available)
            return true;
        cursor = static_cast<GuestAddress>(cursor + sizeof(chunk));
    }
    return true;
}

bool strcpyAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason,
                   bool bounded) {
    const auto destination = registers.r[0];
    const auto maximum = bounded ? static_cast<std::size_t>(registers.r[2]) : kMaximumStringBytes;
    std::string text;
    if (bounded) {
        // strncpy copies at most n bytes and pads the rest with NULs.
        if (!readLengthBoundedString(memory, registers.r[1], text, maximum))
            return fail(memory, reason, "strncpy could not read the guest source string");
        std::vector<std::uint8_t> bytes(maximum, 0);
        std::copy(text.begin(), text.end(), bytes.begin());
        if (!writeGuest(memory, destination, bytes.data(), maximum))
            return fail(memory, reason, "strncpy could not write the guest destination");
        registers.r[0] = destination;
        return true;
    }
    if (!readCString(memory, registers.r[1], text))
        return fail(memory, reason, "strcpy could not read the guest source string");
    if (!writeGuest(memory, destination, reinterpret_cast<const std::uint8_t *>(text.c_str()),
                    text.size() + 1))
        return fail(memory, reason, "strcpy could not write the guest destination");
    registers.r[0] = destination;
    return true;
}

bool strcatAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason) {
    const auto destination = registers.r[0];
    std::string base;
    std::string suffix;
    if (!readCString(memory, destination, base) ||
        !readCString(memory, registers.r[1], suffix))
        return fail(memory, reason, "strcat could not read a terminated guest string");
    const std::string combined = base + suffix;
    if (!writeGuest(memory, destination,
                    reinterpret_cast<const std::uint8_t *>(combined.c_str()),
                    combined.size() + 1))
        return fail(memory, reason, "strcat could not write the guest destination");
    registers.r[0] = destination;
    return true;
}

bool strchrAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason,
                   bool reverse) {
    const auto text = registers.r[0];
    const char needle = static_cast<char>(registers.r[1] & 0xffU);
    std::string value;
    if (!readCString(memory, text, value))
        return fail(memory, reason, "strchr could not read a terminated guest string");
    const auto position = reverse ? value.rfind(needle) : value.find(needle);
    if (needle == '\0')
        registers.r[0] = static_cast<std::uint32_t>(text + value.size());
    else if (position == std::string::npos)
        registers.r[0] = 0;
    else
        registers.r[0] = static_cast<std::uint32_t>(text + position);
    return true;
}

bool strstrAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason) {
    const auto haystack = registers.r[0];
    std::string haystackText;
    std::string needleText;
    if (!readCString(memory, haystack, haystackText) ||
        !readCString(memory, registers.r[1], needleText))
        return fail(memory, reason, "strstr could not read a terminated guest string");
    const auto position = haystackText.find(needleText);
    registers.r[0] = position == std::string::npos
        ? 0
        : static_cast<std::uint32_t>(haystack + position);
    return true;
}

bool strnlenAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason) {
    std::string text;
    if (!readLengthBoundedString(memory, registers.r[0], text, registers.r[1]))
        return fail(memory, reason, "strnlen could not read the guest string");
    registers.r[0] = static_cast<std::uint32_t>(text.size());
    return true;
}

bool memchrAdapter(CpuRegisterState &registers, GuestAddressSpace &memory, std::string &reason) {
    const auto address = registers.r[0];
    const auto needle = static_cast<std::uint8_t>(registers.r[1] & 0xffU);
    const auto size = static_cast<std::size_t>(registers.r[2]);
    std::vector<std::uint8_t> buffer(std::min(size, kCopyChunk));
    std::size_t scanned = 0;
    while (scanned < size) {
        const std::size_t chunk = std::min(buffer.size(), size - scanned);
        if (!readGuest(memory, static_cast<GuestAddress>(address + scanned), buffer.data(), chunk))
            return fail(memory, reason, "memchr could not read the guest range it was given");
        for (std::size_t index = 0; index < chunk; ++index) {
            if (buffer[index] == needle) {
                registers.r[0] = static_cast<std::uint32_t>(address + scanned + index);
                return true;
            }
        }
        scanned += chunk;
    }
    registers.r[0] = 0;
    return true;
}

} // namespace

bool ShimAdapter::ensureHeap(GuestAddressSpace &memory) {
    if (heapOwner_ == &memory)
        return true;
    if (heapOwner_ != nullptr) {
        // One guest address space per boot attempt: a second one must not share
        // block bookkeeping that belongs to the first.
        return false;
    }
    const std::size_t limit = memory.memoryLimit();
    std::size_t size = limit / 4;
    size = std::max(size, kMinimumHeapBytes);
    size = std::min(size, kMaximumHeapBytes);
    if (limit <= size)
        return false;
    try {
        heapBase_ = memory.mapAny(size, MemoryPermission::Read | MemoryPermission::Write,
                                  "guest-heap");
    } catch (const std::exception &) {
        return false;
    }
    heapOwner_ = &memory;
    heapSize_ = size;
    heapCursor_ = 0;
    return true;
}

GuestAddress ShimAdapter::guestAllocate(GuestAddressSpace &memory, std::size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocate(memory, size);
}

void ShimAdapter::guestRelease(GuestAddressSpace &memory, GuestAddress address) {
    std::lock_guard<std::mutex> lock(mutex_);
    release(memory, address);
}

GuestAddress ShimAdapter::allocate(GuestAddressSpace &memory, std::size_t size) {
    if (!ensureHeap(memory))
        return 0;
    const std::size_t requested =
        std::max(kMinimumAllocation, (size + kBlockAlignment - 1) & ~(kBlockAlignment - 1));
    // First fit over freed blocks.
    for (auto block = blocks_.begin(); block != blocks_.end(); ++block) {
        if (!block->second.free || block->second.size < requested)
            continue;
        const auto address = block->first;
        const std::size_t leftover = block->second.size - requested;
        if (leftover >= kMinimumAllocation + kBlockAlignment) {
            block->second.size = requested;
            blocks_.emplace(static_cast<GuestAddress>(address + requested),
                            Block{leftover, true});
        }
        block->second.free = false;
        ++allocations_;
        return address;
    }
    // Bump-allocate above the highest block, trimming trailing free space first.
    while (!blocks_.empty()) {
        const auto last = std::prev(blocks_.end());
        if (!last->second.free)
            break;
        heapCursor_ = static_cast<std::size_t>(last->first - heapBase_);
        blocks_.erase(last);
    }
    const std::size_t aligned = (heapCursor_ + kBlockAlignment - 1) & ~(kBlockAlignment - 1);
    if (aligned + requested > heapSize_)
        return 0;
    const auto address = static_cast<GuestAddress>(heapBase_ + aligned);
    blocks_.emplace(address, Block{requested, false});
    heapCursor_ = aligned + requested;
    ++allocations_;
    return address;
}

void ShimAdapter::release(GuestAddressSpace &memory, GuestAddress address) {
    if (address == 0 || heapOwner_ != &memory)
        return;
    const auto found = blocks_.find(address);
    if (found == blocks_.end() || found->second.free)
        return;
    found->second.free = true;
    // Coalesce with the following block, then with the preceding one.
    auto next = std::next(found);
    if (next != blocks_.end() && next->second.free) {
        found->second.size += next->second.size;
        blocks_.erase(next);
    }
    if (found != blocks_.begin()) {
        auto previous = std::prev(found);
        if (previous->second.free) {
            previous->second.size += found->second.size;
            blocks_.erase(found);
        }
    }
    ++frees_;
}

void ShimAdapter::registerFunction(
    ShimRegistry &registry, const std::string &symbol, const std::string &adapterName,
    std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)> invoke) {
    if (nextCallout_ > 0xf004ffffU - 4U)
        throw std::overflow_error("libSystem shim callout range is exhausted");
    ShimBinding binding;
    binding.darwinSymbol = symbol;
    binding.library = "libSystem.B.dylib";
    binding.adapterName = adapterName;
    binding.guestAddress = nextCallout_;
    binding.invoke = std::move(invoke);
    nextCallout_ += 4U;
    registry.registerBinding(std::move(binding));
}

void ShimAdapter::registerBindings(ShimRegistry &registry) {
    if (registered_)
        throw std::runtime_error("libSystem shim adapter is already registered");
    // OSAtomicAnd32OrigBarrier(mask, address): atomically stores (*address & mask) and
    // returns the value *address held before the store. The barrier is trivially
    // satisfied because the guest runs on one CPU thread at a time.
    registerFunction(registry, "_OSAtomicAnd32OrigBarrier", "libsystem-osatomic-and32-orig-barrier",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) {
                         const auto mask = static_cast<std::uint32_t>(registers.r[0]);
                         const auto address = static_cast<GuestAddress>(registers.r[1]);
                         std::uint32_t previous = 0;
                         if (address == 0 || !memory.contains(address, sizeof(previous), MemoryPermission::Write) ||
                             !readGuest(memory, address, reinterpret_cast<std::uint8_t *>(&previous), sizeof(previous))) {
                             reason = "OSAtomicAnd32OrigBarrier received an unreadable or unwritable address";
                             return false;
                         }
                         const std::uint32_t next = previous & mask;
                         if (!writeGuest(memory, address, reinterpret_cast<const std::uint8_t *>(&next), sizeof(next))) {
                             reason = "OSAtomicAnd32OrigBarrier could not store the result";
                             return false;
                         }
                         registers.r[0] = previous;
                         return true;
                     });
    registerFunction(registry, "_memcpy", "libsystem-memcpy",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return memcpyAdapter(registers, memory, reason); });
    registerFunction(registry, "_memmove", "libsystem-memmove",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return memcpyAdapter(registers, memory, reason); });
    registerFunction(registry, "_memset", "libsystem-memset",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return memsetAdapter(registers, memory, reason); });
    registerFunction(registry, "_memcmp", "libsystem-memcmp",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return memcmpAdapter(registers, memory, reason); });
    registerFunction(registry, "_memchr", "libsystem-memchr",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return memchrAdapter(registers, memory, reason); });
    registerFunction(registry, "_strlen", "libsystem-strlen",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return strlenAdapter(registers, memory, reason); });
    registerFunction(registry, "_strnlen", "libsystem-strnlen",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return strnlenAdapter(registers, memory, reason); });
    registerFunction(registry, "_strcmp", "libsystem-strcmp",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return strcmpExactAdapter(registers, memory, reason); });
    registerFunction(registry, "_strncmp", "libsystem-strncmp",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return strncmpAdapter(registers, memory, reason); });
    registerFunction(registry, "_strcpy", "libsystem-strcpy",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return strcpyAdapter(registers, memory, reason, false); });
    registerFunction(registry, "_strncpy", "libsystem-strncpy",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return strcpyAdapter(registers, memory, reason, true); });
    registerFunction(registry, "_strcat", "libsystem-strcat",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return strcatAdapter(registers, memory, reason); });
    registerFunction(registry, "_strchr", "libsystem-strchr",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return strchrAdapter(registers, memory, reason, false); });
    registerFunction(registry, "_strrchr", "libsystem-strrchr",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return strchrAdapter(registers, memory, reason, true); });
    registerFunction(registry, "_strstr", "libsystem-strstr",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) { return strstrAdapter(registers, memory, reason); });
    registerFunction(registry, "_strdup", "libsystem-strdup",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         std::string text;
                         if (!readCString(memory, registers.r[0], text))
                             return fail(memory, reason,
                                         "strdup could not read a terminated guest string");
                         std::lock_guard<std::mutex> lock(mutex_);
                         const auto copy = allocate(memory, text.size() + 1);
                         if (copy == 0)
                             return fail(memory, reason,
                                         "strdup could not allocate from the bounded guest heap");
                         if (!writeGuest(memory, copy,
                                         reinterpret_cast<const std::uint8_t *>(text.c_str()),
                                         text.size() + 1))
                             return fail(memory, reason, "strdup could not write the guest copy");
                         registers.r[0] = copy;
                         return true;
                     });
    registerFunction(registry, "_malloc", "libsystem-malloc",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         const std::size_t requested = registers.r[0];
                         std::lock_guard<std::mutex> lock(mutex_);
                         const auto block = allocate(memory, requested);
                         if (block == 0)
                             return fail(memory, reason,
                                         "malloc could not satisfy " +
                                             std::to_string(requested) +
                                             " bytes from the bounded guest heap");
                         registers.r[0] = block;
                         registers.r[0] = block;
                         return true;
                     });
    registerFunction(registry, "_calloc", "libsystem-calloc",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         const std::size_t count = registers.r[0];
                         const std::size_t size = registers.r[1];
                         if (size != 0 && count > (kMaximumHeapBytes * 4) / size)
                             return fail(memory, reason,
                                         "calloc element count overflows the bounded guest heap");
                         std::lock_guard<std::mutex> lock(mutex_);
                         const auto block = allocate(memory, count * size);
                         if (block == 0)
                             return fail(memory, reason,
                                         "calloc could not satisfy the request from the bounded "
                                         "guest heap");
                         registers.r[0] = block;
                         return true;
                     });
    registerFunction(registry, "_realloc", "libsystem-realloc",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         const auto previous = registers.r[0];
                         const std::size_t size = registers.r[1];
                         std::lock_guard<std::mutex> lock(mutex_);
                         if (previous == 0) {
                             const auto block = allocate(memory, size);
                             if (block == 0)
                                 return fail(memory, reason,
                                             "realloc could not allocate from the bounded guest "
                                             "heap");
                             registers.r[0] = block;
                             return true;
                         }
                         const auto found = blocks_.find(previous);
                         if (found == blocks_.end() || found->second.free)
                             return fail(memory, reason,
                                         "realloc received a pointer that is not a live guest "
                                         "allocation");
                         if (size == 0) {
                             release(memory, previous);
                             registers.r[0] = 0;
                             return true;
                         }
                         if (found->second.size >= size) {
                             registers.r[0] = previous;
                             return true;
                         }
                         const auto block = allocate(memory, size);
                         if (block == 0)
                             return fail(memory, reason,
                                         "realloc could not grow the block in the bounded guest "
                                         "heap");
                         std::vector<std::uint8_t> buffer(found->second.size);
                         if (!readGuest(memory, previous, buffer.data(), buffer.size()) ||
                             !writeGuest(memory, block, buffer.data(), buffer.size()))
                             return fail(memory, reason, "realloc could not copy the guest block");
                         release(memory, previous);
                         registers.r[0] = block;
                         return true;
                     });
    registerFunction(registry, "_free", "libsystem-free",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         std::lock_guard<std::mutex> lock(mutex_);
                         if (registers.r[0] != 0 && heapOwner_ != &memory)
                             return fail(memory, reason,
                                         "free received a pointer for an address space that does "
                                         "not own the guest heap");
                         release(memory, registers.r[0]);
                         // free returns void; a deterministic r0 keeps the guest
                         // register file reproducible for diagnostics.
                         registers.r[0] = 0;
                         return true;
                     });
    // Itanium C++ ABI allocation entry points. The 32-bit ARM mangled names are
    // exact (`__Znwm`, `__Znam`, `__ZdlPv`, `__ZdaPv`); the runtime serves them
    // from the same bounded guest heap as malloc/free, so an in-image C++
    // operator new/delete pair gets real, trackable storage instead of a trap.
    // A size that overflows the 32-bit request or a delete of a pointer the
    // heap does not own fails closed.
    // ---- stdio / file I/O served by the mounted guest filesystem ----------
    // A path the runtime cannot serve yields a NULL FILE the way a missing file
    // does, so the guest keeps its own error handling; the refusal itself is
    // recorded in the report's guestFileSystem diagnostics.
    registerFunction(registry, "_fopen", "libsystem-fopen",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) {
                         std::string path;
                         std::string mode;
                         if (!readCString(memory, registers.r[0], path))
                             return fail(memory, reason, "fopen could not read the guest path");
                         if (!readCString(memory, registers.r[1], mode))
                             return fail(memory, reason, "fopen could not read the guest mode");
                         std::string detail;
                         registers.r[0] = guestFileSystem().open(path, mode, detail);
                         return true;
                     });
    registerFunction(registry, "_fclose", "libsystem-fclose",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         registers.r[0] =
                             guestFileSystem().close(registers.r[0]) ? 0U : 0xFFFFFFFFU;
                         return true;
                     });
    registerFunction(registry, "_fread", "libsystem-fread",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) {
                         const std::size_t elementSize = registers.r[1];
                         const std::size_t elementCount = registers.r[2];
                         if (elementSize == 0 || elementCount == 0) {
                             registers.r[0] = 0;
                             return true;
                         }
                         const std::size_t requested = elementSize * elementCount;
                         std::vector<std::uint8_t> buffer(requested);
                         std::string detail;
                         const auto read = guestFileSystem().read(registers.r[3], buffer.data(),
                                                                  requested, detail);
                         if (read != 0 &&
                             !writeGuest(memory, registers.r[0], buffer.data(), read))
                             return fail(memory, reason,
                                         "fread could not write the guest destination buffer");
                         registers.r[0] = static_cast<std::uint32_t>(read / elementSize);
                         return true;
                     });
    registerFunction(registry, "_fwrite", "libsystem-fwrite",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) {
                         const std::size_t elementSize = registers.r[1];
                         const std::size_t elementCount = registers.r[2];
                         if (elementSize == 0 || elementCount == 0) {
                             registers.r[0] = 0;
                             return true;
                         }
                         const std::size_t requested = elementSize * elementCount;
                         std::vector<std::uint8_t> buffer(requested);
                         if (!readGuest(memory, registers.r[0], buffer.data(), requested))
                             return fail(memory, reason,
                                         "fwrite could not read the guest source buffer");
                         std::string detail;
                         const auto written = guestFileSystem().write(registers.r[3], buffer.data(),
                                                                      requested, detail);
                         registers.r[0] = static_cast<std::uint32_t>(written / elementSize);
                         return true;
                     });
    registerFunction(registry, "_fseek", "libsystem-fseek",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         std::string detail;
                         const bool ok = guestFileSystem().seek(
                             registers.r[0], static_cast<long>(registers.r[1]),
                             static_cast<int>(registers.r[2]), detail);
                         registers.r[0] = ok ? 0U : 0xFFFFFFFFU;
                         return true;
                     });
    registerFunction(registry, "_ftell", "libsystem-ftell",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         std::string detail;
                         const auto position = guestFileSystem().tell(registers.r[0], detail);
                         registers.r[0] = position < 0 ? 0xFFFFFFFFU
                                                       : static_cast<std::uint32_t>(position);
                         return true;
                     });
    registerFunction(registry, "_feof", "libsystem-feof",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         registers.r[0] = guestFileSystem().eof(registers.r[0]) ? 1U : 0U;
                         return true;
                     });
    registerFunction(registry, "_ferror", "libsystem-ferror",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         registers.r[0] = guestFileSystem().failed(registers.r[0]) ? 1U : 0U;
                         return true;
                     });
    registerFunction(registry, "_fflush", "libsystem-fflush",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         if (registers.r[0] == 0) {
                             registers.r[0] = 0;
                             return true;
                         }
                         std::string detail;
                         registers.r[0] =
                             guestFileSystem().flush(registers.r[0], detail) ? 0U : 0xFFFFFFFFU;
                         return true;
                     });
    registerFunction(registry, "_fgets", "libsystem-fgets",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) {
                         const std::size_t size = registers.r[1];
                         if (size == 0) {
                             registers.r[0] = 0;
                             return true;
                         }
                         std::vector<std::uint8_t> buffer(size);
                         std::string detail;
                         const auto read = guestFileSystem().read(registers.r[2], buffer.data(),
                                                                  size - 1, detail);
                         if (read == 0) {
                             registers.r[0] = 0;
                             return true;
                         }
                         std::size_t length = 0;
                         while (length < read && buffer[length] != '\n')
                             ++length;
                         if (length < read)
                             ++length; // include the newline like stdio does
                         buffer[length] = 0;
                         if (!writeGuest(memory, registers.r[0], buffer.data(), length + 1))
                             return fail(memory, reason,
                                         "fgets could not write the guest destination buffer");
                         return true;
                     });
    registerFunction(registry, "_remove", "libsystem-remove",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) {
                         std::string path;
                         if (!readCString(memory, registers.r[0], path))
                             return fail(memory, reason, "remove could not read the guest path");
                         std::string hostPath;
                         bool writable = false;
                         if (!guestFileSystem().mounted() ||
                             !guestFileSystem().resolve(path, hostPath, writable) || !writable) {
                             registers.r[0] = 0xFFFFFFFFU;
                             return true;
                         }
                         registers.r[0] = std::remove(hostPath.c_str()) == 0 ? 0U : 0xFFFFFFFFU;
                         return true;
                     });

    // ---- time --------------------------------------------------------------
    registerFunction(registry, "_gettimeofday", "libsystem-gettimeofday",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) {
                         // 32-bit Darwin struct timeval is two 32-bit words.
                         std::array<std::uint32_t, 2> value{};
                         const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::system_clock::now()
                                                     .time_since_epoch())
                                                 .count();
                         value[0] = static_cast<std::uint32_t>(micros / 1000000);
                         value[1] = static_cast<std::uint32_t>(micros % 1000000);
                         if (registers.r[0] != 0 &&
                             !writeGuest(memory, registers.r[0],
                                         reinterpret_cast<const std::uint8_t *>(value.data()),
                                         sizeof(value)))
                             return fail(memory, reason,
                                         "gettimeofday could not write the guest timeval");
                         registers.r[0] = 0;
                         return true;
                     });
    registerFunction(registry, "_time", "libsystem-time",
                     [](CpuRegisterState &registers, GuestAddressSpace &memory,
                        std::string &reason) {
                         const auto seconds = static_cast<std::uint32_t>(
                             std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count());
                         if (registers.r[0] != 0 &&
                             !writeGuest(memory, registers.r[0],
                                         reinterpret_cast<const std::uint8_t *>(&seconds),
                                         sizeof(seconds)))
                             return fail(memory, reason, "time could not write the guest time_t");
                         registers.r[0] = seconds;
                         return true;
                     });

    // ---- math (scalar VFP arguments) ---------------------------------------
    registerFunction(registry, "_sin", "libsystem-sin",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         setDoubleResult(registers, std::sin(doubleArgument(registers, 0)));
                         return true;
                     });
    registerFunction(registry, "_cos", "libsystem-cos",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         setDoubleResult(registers, std::cos(doubleArgument(registers, 0)));
                         return true;
                     });
    registerFunction(registry, "_pow", "libsystem-pow",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         setDoubleResult(registers, std::pow(doubleArgument(registers, 0),
                                                             doubleArgument(registers, 1)));
                         return true;
                     });
    registerFunction(registry, "_sqrt", "libsystem-sqrt",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         setDoubleResult(registers, std::sqrt(doubleArgument(registers, 0)));
                         return true;
                     });
    registerFunction(registry, "_floorf", "libsystem-floorf",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         setSingleResult(registers, std::floor(singleArgument(registers, 0)));
                         return true;
                     });
    registerFunction(registry, "_ceilf", "libsystem-ceilf",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         setSingleResult(registers, std::ceil(singleArgument(registers, 0)));
                         return true;
                     });
    registerFunction(registry, "_fabs", "libsystem-fabs",
                     [](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
                         setDoubleResult(registers, std::fabs(doubleArgument(registers, 0)));
                         return true;
                     });
    registerFunction(registry, "__Znwm", "libsystem-cxx-operator-new",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         const std::size_t requested = registers.r[0];
                         std::lock_guard<std::mutex> lock(mutex_);
                         const auto block = allocate(memory, requested);
                         if (block == 0)
                             return fail(memory, reason,
                                         "operator new could not satisfy " +
                                             std::to_string(requested) +
                                             " bytes from the bounded guest heap");
                         registers.r[0] = block;
                         return true;
                     });
    registerFunction(registry, "__Znam", "libsystem-cxx-operator-new-array",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         const std::size_t requested = registers.r[0];
                         std::lock_guard<std::mutex> lock(mutex_);
                         const auto block = allocate(memory, requested);
                         if (block == 0)
                             return fail(memory, reason,
                                         "operator new[] could not satisfy " +
                                             std::to_string(requested) +
                                             " bytes from the bounded guest heap");
                         registers.r[0] = block;
                         return true;
                     });
    registerFunction(registry, "__ZdlPv", "libsystem-cxx-operator-delete",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         const auto address = registers.r[0];
                         std::lock_guard<std::mutex> lock(mutex_);
                         if (address != 0) {
                             const auto found = blocks_.find(address);
                             if (found == blocks_.end() || found->second.free)
                                 return fail(memory, reason,
                                             "operator delete received a pointer that is not a "
                                             "live bounded guest-heap allocation");
                             release(memory, address);
                         }
                         registers.r[0] = 0;
                         return true;
                     });
    registerFunction(registry, "__ZdaPv", "libsystem-cxx-operator-delete-array",
                     [this](CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason) {
                         const auto address = registers.r[0];
                         std::lock_guard<std::mutex> lock(mutex_);
                         if (address != 0) {
                             const auto found = blocks_.find(address);
                             if (found == blocks_.end() || found->second.free)
                                 return fail(memory, reason,
                                             "operator delete[] received a pointer that is not a "
                                             "live bounded guest-heap allocation");
                             release(memory, address);
                         }
                         registers.r[0] = 0;
                         return true;
                     });
    registered_ = true;
}

std::size_t ShimAdapter::allocationCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return allocations_;
}

std::size_t ShimAdapter::freeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frees_;
}

std::size_t ShimAdapter::heapBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return heapSize_;
}

} // namespace radek::compat_runtime::libsystem
