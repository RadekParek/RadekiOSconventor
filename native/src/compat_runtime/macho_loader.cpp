#include "compat_runtime/macho_loader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace radek::compat_runtime {
namespace {
constexpr std::uint32_t kMachMagic32 = 0xfeedface;
constexpr std::uint32_t kFatMagic = 0xcafebabe;
constexpr std::uint32_t kFatMagic64 = 0xcafebabf;
constexpr std::uint32_t kFatCigam = 0xbebafeca;
constexpr std::uint32_t kFatCigam64 = 0xbfbafeca;
constexpr std::uint32_t kCpuTypeArm = 12;
constexpr std::uint32_t kFileTypeExecute = 2;
constexpr std::uint32_t kLcSegment = 0x1;
constexpr std::uint32_t kLcSymtab = 0x2;
constexpr std::uint32_t kLcDysymtab = 0xb;
constexpr std::uint32_t kLcUnixThread = 0x5;
constexpr std::uint32_t kLcLoadDylib = 0xc;
constexpr std::uint32_t kLcLazyLoadDylib = 0x20;
constexpr std::uint32_t kLcLoadWeakDylib = 0x80000018;
constexpr std::uint32_t kLcLoadUpwardDylib = 0x80000023;
constexpr std::uint32_t kLcReexportDylib = 0x8000001f;
constexpr std::uint32_t kLcDyldInfo = 0x22;
constexpr std::uint32_t kLcDyldInfoOnly = 0x80000022;
constexpr std::uint32_t kLcEncryptionInfo = 0x21;
constexpr std::uint32_t kLcMain = 0x80000028;
constexpr std::uint32_t kLcDyldChainedFixups = 0x80000034;
constexpr std::size_t kMachHeader32Size = 28;
constexpr std::size_t kSegmentCommand32Size = 56;
constexpr std::size_t kSection32Size = 68;
constexpr std::size_t kMaximumLoadCommands = 65536;
constexpr std::size_t kMaximumFixups = 1000000;
constexpr std::size_t kMaximumStringLength = 4096;
constexpr std::uint32_t kThreadFlavorArm = 1;
constexpr std::uint32_t kArmThreadStateWords = 17;
constexpr std::uint8_t kRebaseTypePointer = 1;
constexpr std::uint8_t kBindTypePointer = 1;
constexpr std::uint32_t kSectionTypeMask = 0xff;
constexpr std::uint32_t kSectionNonLazySymbolPointers = 0x6;
constexpr std::uint32_t kSectionLazySymbolPointers = 0x7;
constexpr std::uint32_t kIndirectSymbolLocal = 0x80000000;
constexpr std::uint32_t kIndirectSymbolAbsolute = 0x40000000;

// DYLD_CHAINED_FIXUPS payload constants (include/mach-o/fixup-chains.h).
constexpr std::uint16_t kChainedPtrFormat32 = 3;      // DYLD_CHAINED_PTR_32
constexpr std::uint16_t kChainedPtrStartNone = 0xFFFF;
constexpr std::uint16_t kChainedPtrStartMulti = 0x8000;
constexpr std::uint16_t kChainedPtrStartLast = 0x8000; // same bit, in chain_starts[]
constexpr std::uint32_t kChainedImports = 1;          // DYLD_CHAINED_IMPORT
constexpr std::uint32_t kChainedImportsAddend = 2;    // DYLD_CHAINED_IMPORT_ADDEND
constexpr std::uint32_t kChainedImportsAddend64 = 3;  // DYLD_CHAINED_IMPORT_ADDEND64

struct SliceRange {
    std::size_t offset = 0;
    std::size_t size = 0;
};

struct RankedSlice {
    SliceRange range;
    unsigned rank = 0;
};

class Reader {
    const std::vector<std::uint8_t> &bytes_;
    std::size_t base_;
    std::size_t size_;

  public:
    Reader(const std::vector<std::uint8_t> &bytes, std::size_t base, std::size_t size)
        : bytes_(bytes), base_(base), size_(size) {
        if (base_ > bytes_.size() || size_ > bytes_.size() - base_)
            throw std::runtime_error("Mach-O selected image is outside the input buffer");
    }

    void range(std::uint64_t offset, std::uint64_t size) const {
        if (offset > size_ || size > size_ - offset)
            throw std::runtime_error("Mach-O range is outside the selected image");
    }

    std::uint8_t u8(std::size_t offset) const {
        range(offset, 1);
        return bytes_[base_ + offset];
    }

    std::uint16_t u16le(std::size_t offset) const {
        range(offset, 2);
        return static_cast<std::uint16_t>(bytes_[base_ + offset]) |
               static_cast<std::uint16_t>(bytes_[base_ + offset + 1] << 8);
    }

    std::uint32_t u32le(std::size_t offset) const {
        range(offset, 4);
        return static_cast<std::uint32_t>(bytes_[base_ + offset]) |
               (static_cast<std::uint32_t>(bytes_[base_ + offset + 1]) << 8) |
               (static_cast<std::uint32_t>(bytes_[base_ + offset + 2]) << 16) |
               (static_cast<std::uint32_t>(bytes_[base_ + offset + 3]) << 24);
    }

    std::uint64_t u64le(std::size_t offset) const {
        const auto low = u32le(offset);
        const auto high = u32le(offset + 4);
        return static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
    }

    std::uint32_t u32be(std::size_t offset) const {
        range(offset, 4);
        return (static_cast<std::uint32_t>(bytes_[base_ + offset]) << 24) |
               (static_cast<std::uint32_t>(bytes_[base_ + offset + 1]) << 16) |
               (static_cast<std::uint32_t>(bytes_[base_ + offset + 2]) << 8) |
               static_cast<std::uint32_t>(bytes_[base_ + offset + 3]);
    }

    std::uint64_t u64be(std::size_t offset) const {
        return (static_cast<std::uint64_t>(u32be(offset)) << 32) | u32be(offset + 4);
    }

    std::string fixedString(std::size_t offset, std::size_t length) const {
        range(offset, length);
        std::size_t end = 0;
        while (end < length && bytes_[base_ + offset + end] != 0)
            ++end;
        return std::string(reinterpret_cast<const char *>(bytes_.data() + base_ + offset), end);
    }

    std::string cstring(std::size_t offset, std::size_t end) const {
        if (offset > end || end > size_)
            throw std::runtime_error("Mach-O string range is invalid");
        std::string result;
        while (offset < end) {
            const auto character = u8(offset++);
            if (character == 0)
                return result;
            if (result.size() >= kMaximumStringLength)
                throw std::runtime_error("Mach-O string exceeds the loader limit");
            result.push_back(static_cast<char>(character));
        }
        throw std::runtime_error("Mach-O string is not terminated");
    }

    const std::uint8_t *pointer(std::size_t offset, std::size_t length) const {
        range(offset, length);
        return bytes_.data() + base_ + offset;
    }
};

std::uint64_t readUleb(const Reader &reader, std::size_t &cursor, std::size_t end) {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (cursor >= end)
            throw std::runtime_error("truncated dyld ULEB128 value");
        const auto byte = reader.u8(cursor++);
        if (shift == 63 && (byte & 0x7e) != 0)
            throw std::runtime_error("dyld ULEB128 value overflows 64 bits");
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
            return value;
    }
    throw std::runtime_error("dyld ULEB128 value is too long");
}

std::int64_t readSleb(const Reader &reader, std::size_t &cursor, std::size_t end) {
    std::uint64_t value = 0;
    unsigned shift = 0;
    std::uint8_t byte = 0;
    for (; shift < 64; shift += 7) {
        if (cursor >= end)
            throw std::runtime_error("truncated dyld SLEB128 value");
        byte = reader.u8(cursor++);
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0)
            break;
    }
    if ((byte & 0x80) != 0)
        throw std::runtime_error("dyld SLEB128 value is too long");
    if (shift < 63 && (byte & 0x40) != 0)
        value |= (~std::uint64_t{0}) << (shift + 7);
    return static_cast<std::int64_t>(value);
}

SliceRange chooseArm32Slice(const std::vector<std::uint8_t> &bytes) {
    if (bytes.size() < 4)
        throw std::runtime_error("input is too small for a Mach-O header");
    Reader reader(bytes, 0, bytes.size());
    const auto magic = reader.u32be(0);
    if (magic != kFatMagic && magic != kFatMagic64 && magic != kFatCigam && magic != kFatCigam64) {
        if (reader.u32le(0) != kMachMagic32)
            throw std::runtime_error("input is not a supported little-endian 32-bit Mach-O image");
        return {0, bytes.size()};
    }
    const bool fat64 = magic == kFatMagic64 || magic == kFatCigam64;
    const bool littleEndian = magic == kFatCigam || magic == kFatCigam64;
    const auto count = littleEndian ? reader.u32le(4) : reader.u32be(4);
    if (count == 0 || count > 4096)
        throw std::runtime_error("FAT Mach-O architecture count is invalid");
    const std::size_t entrySize = fat64 ? 32 : 20;
    reader.range(8, static_cast<std::uint64_t>(count) * entrySize);
    std::optional<RankedSlice> selected;
    for (std::uint32_t index = 0; index < count; ++index) {
        const std::size_t entry = 8 + static_cast<std::size_t>(index) * entrySize;
        const auto cpu = littleEndian ? reader.u32le(entry) : reader.u32be(entry);
        const auto architectureSubtype = littleEndian ? reader.u32le(entry + 4) : reader.u32be(entry + 4);
        if (cpu != kCpuTypeArm)
            continue;
        const std::uint64_t offset = fat64
                                         ? (littleEndian ? reader.u64le(entry + 8) : reader.u64be(entry + 8))
                                         : (littleEndian ? reader.u32le(entry + 8) : reader.u32be(entry + 8));
        const std::uint64_t size = fat64
                                       ? (littleEndian ? reader.u64le(entry + 16) : reader.u64be(entry + 16))
                                       : (littleEndian ? reader.u32le(entry + 12) : reader.u32be(entry + 12));
        reader.range(offset, size);
        if (size < kMachHeader32Size || reader.u32le(static_cast<std::size_t>(offset)) != kMachMagic32)
            continue;
        const auto sliceOffset = static_cast<std::size_t>(offset);
        const auto sliceCpu = reader.u32le(sliceOffset + 4);
        const auto sliceSubtype = reader.u32le(sliceOffset + 8) & 0x00ffffff;
        const auto subtype = architectureSubtype & 0x00ffffff;
        if (sliceCpu != kCpuTypeArm || sliceSubtype != subtype)
            continue;
        const unsigned rank = subtype == 9 || subtype == 11 || subtype == 12 ? 0U
                              : subtype == 6                         ? 1U
                                                                    : 2U;
        if (!selected || rank < selected->rank)
            selected = RankedSlice{{static_cast<std::size_t>(offset), static_cast<std::size_t>(size)}, rank};
    }
    if (!selected)
        throw std::runtime_error("FAT Mach-O has no supported 32-bit ARM slice");
    return selected->range;
}

std::uint64_t addStreamOffset(std::uint64_t offset, std::uint64_t increment) {
    if (increment > std::numeric_limits<std::uint64_t>::max() - offset)
        throw std::runtime_error("dyld stream offset overflows 64 bits");
    return offset + increment;
}

std::uint32_t addGuestAddress(std::uint64_t address, GuestAddress slide, const char *what) {
    const std::uint64_t result = address + slide;
    if (result > std::numeric_limits<GuestAddress>::max())
        throw std::runtime_error(std::string(what) + " exceeds the 32-bit guest address space");
    return static_cast<GuestAddress>(result);
}

struct Section {
    std::string name;
    std::string segmentName;
    std::uint32_t address = 0;
    std::uint32_t size = 0;
    std::uint32_t fileOffset = 0;
    std::uint32_t flags = 0;
    std::uint32_t reserved1 = 0;
    std::uint32_t reserved2 = 0;
};

struct Segment {
    std::string name;
    std::uint32_t vmAddress = 0;
    std::uint32_t vmSize = 0;
    std::uint32_t fileOffset = 0;
    std::uint32_t fileSize = 0;
    std::uint32_t initialProtection = 0;
    GuestAddress guestAddress = 0;
    bool mapped = false;
    std::vector<Section> sections;
};

struct DyldStreams {
    std::uint32_t rebaseOffset = 0;
    std::uint32_t rebaseSize = 0;
    std::uint32_t bindOffset = 0;
    std::uint32_t bindSize = 0;
    std::uint32_t weakBindOffset = 0;
    std::uint32_t weakBindSize = 0;
    std::uint32_t lazyBindOffset = 0;
    std::uint32_t lazyBindSize = 0;
    bool present = false;
};

struct SymbolTable {
    std::uint32_t symbolOffset = 0;
    std::uint32_t symbolCount = 0;
    std::uint32_t stringOffset = 0;
    std::uint32_t stringSize = 0;
    bool present = false;
};

struct DynamicSymbolTable {
    std::uint32_t indirectSymbolOffset = 0;
    std::uint32_t indirectSymbolCount = 0;
    std::uint32_t externalRelocationOffset = 0;
    std::uint32_t externalRelocationCount = 0;
    bool present = false;
};

struct ChainedFixupsData {
    std::uint32_t dataOffset = 0;
    std::uint32_t dataSize = 0;
    bool present = false;
};

struct ImageState {
    std::vector<Segment> segments;
    std::vector<std::string> dependencies;
    DyldStreams streams;
    SymbolTable symbols;
    DynamicSymbolTable dynamicSymbols;
    ChainedFixupsData chainedFixups;
    GuestAddress entryPoint = 0;
    std::string entryPointSource;
    CpuRegisterState registers;
    std::optional<std::uint64_t> mainEntryOffset;
    bool hasThreadEntry = false;
    bool hasEntryPoint = false;
    bool encrypted = false;
};

MemoryPermission toPermissions(std::uint32_t protection) {
    MemoryPermission result = MemoryPermission::None;
    if (protection & 1)
        result = result | MemoryPermission::Read;
    if (protection & 2)
        result = result | MemoryPermission::Write;
    if (protection & 4)
        result = result | MemoryPermission::Execute;
    return result;
}

std::size_t checkedStreamEnd(const Reader &reader, std::uint32_t offset, std::uint32_t size,
                             const char *name) {
    if (size == 0)
        return 0;
    reader.range(offset, size);
    (void)name;
    return static_cast<std::size_t>(offset) + size;
}

std::string dependencyName(const ImageState &image, std::int64_t ordinal) {
    if (ordinal > 0 && static_cast<std::uint64_t>(ordinal) <= image.dependencies.size())
        return image.dependencies[static_cast<std::size_t>(ordinal - 1)];
    return "special-dylib-ordinal:" + std::to_string(ordinal);
}

radek::Json makeSymbolRecord(const std::string &symbol, const std::string &status,
                             const std::string &library, std::int64_t ordinal,
                             GuestAddress bindAddress, bool weakImport,
                             const ShimBinding *binding, const std::string &reason,
                             const char *source) {
    radek::Json record = radek::Json::object();
    record["symbol"] = symbol;
    record["status"] = status;
    record["library"] = library;
    record["dylibOrdinal"] = std::to_string(ordinal);
    record["bindAddress"] = static_cast<std::uint64_t>(bindAddress);
    record["weakImport"] = weakImport;
    record["source"] = source;
    if (binding) {
        record["shimLibrary"] = binding->library;
        record["adapter"] = binding->adapterName;
        record["bindingKind"] = binding->resolveGuestAddress ? "guest-data-provider" : "guest-callout";
        record["guestAddress"] = static_cast<std::uint64_t>(binding->guestAddress);
    }
    if (!reason.empty())
        record["reason"] = reason;
    return record;
}

void appendUnresolved(MachOLoadReport &report, const std::string &symbol,
                      const std::string &library, std::int64_t ordinal,
                      GuestAddress bindAddress, bool weakImport, const std::string &reason,
                      const char *source) {
    auto record = makeSymbolRecord(symbol, "unresolved", library, ordinal, bindAddress,
                                   weakImport, nullptr, reason, source);
    if (!report.firstMissingImport)
        report.firstMissingImport = symbol;
    report.unresolvedSymbols.push_back(std::move(record));
}

// Mutable trap state for loadWithTraps. Null everywhere on the fail-closed
// load() path, which never binds traps.
struct TrapContext {
    ShimRegistry *registry = nullptr;
    TrapShimAdapter *traps = nullptr;
};

// Bind one unimplemented import to its abort-on-call trap and install the
// trap address in the import slot. Traps accept any bind addend: the slot
// still lands inside the trap range, so any guest touch faults honestly and
// maps back to the import. The record keeps status "trapped" (never
// "resolved") with the original no-adapter reason preserved.
void bindTrapAt(GuestAddressSpace &memory, MachOLoadReport &report,
                TrapContext &trapContext, const std::string &symbol,
                const std::string &library, std::int64_t ordinal,
                GuestAddress target, std::int64_t addend, bool weakImport,
                const char *source) {
    GuestAddress trapAddress = 0;
    try {
        trapAddress = trapContext.traps->bind(*trapContext.registry, symbol, library);
    } catch (const std::exception &error) {
        appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                         error.what(), source);
        return;
    }
    const auto trap64 = static_cast<std::uint64_t>(trapAddress);
    std::uint64_t resolved = trap64;
    if (addend >= 0) {
        const auto positiveAddend = static_cast<std::uint64_t>(addend);
        if (positiveAddend > std::numeric_limits<GuestAddress>::max() - trap64) {
            appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                             "trap address plus addend exceeds the guest address space",
                             source);
            return;
        }
        resolved += positiveAddend;
    } else {
        const auto magnitude = static_cast<std::uint64_t>(-(addend + 1)) + 1;
        if (magnitude > trap64) {
            appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                             "trap address plus addend exceeds the guest address space",
                             source);
            return;
        }
        resolved -= magnitude;
    }
    if (resolved > std::numeric_limits<GuestAddress>::max()) {
        appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                         "trap address plus addend exceeds the guest address space",
                         source);
        return;
    }
    const auto guestAddress = static_cast<GuestAddress>(resolved);
    if (!memory.initialize(target, &guestAddress, sizeof(guestAddress))) {
        appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                         "could not write the trap address into the import slot", source);
        return;
    }
    auto record = makeSymbolRecord(symbol, "trapped", library, ordinal, target,
                                   weakImport, nullptr,
                                   "no tested native call adapter is registered for this "
                                   "symbol; bound to an abort-on-call trap",
                                   source);
    record["trapAddress"] = static_cast<std::uint64_t>(guestAddress);
    if (!report.firstMissingImport)
        report.firstMissingImport = symbol;
    report.trappedSymbols.push_back(std::move(record));
}

void bindAt(const ImageState &image, GuestAddressSpace &memory, const ShimRegistry &shims,
            MachOLoadReport &report, std::uint32_t segmentIndex, std::uint64_t offset,
            std::uint8_t type, const std::string &symbol, std::int64_t ordinal,
            std::int64_t addend, bool weakImport, const char *source,
            TrapContext *trapContext = nullptr) {
    if (symbol.empty())
        throw std::runtime_error("dyld bind opcode has no symbol name");
    const auto library = dependencyName(image, ordinal);
    if (segmentIndex >= image.segments.size()) {
        appendUnresolved(report, symbol, library, ordinal, 0, weakImport,
                         "bind references an invalid segment index", source);
        return;
    }
    const auto &segment = image.segments[segmentIndex];
    if (!segment.mapped || offset > segment.vmSize || sizeof(std::uint32_t) > segment.vmSize - offset) {
        appendUnresolved(report, symbol, library, ordinal, 0, weakImport,
                         "bind target is outside a mapped guest segment", source);
        return;
    }
    const auto target64 = static_cast<std::uint64_t>(segment.guestAddress) + offset;
    if (target64 > std::numeric_limits<GuestAddress>::max()) {
        appendUnresolved(report, symbol, library, ordinal, 0, weakImport,
                         "bind target address overflows the guest address space", source);
        return;
    }
    const auto target = static_cast<GuestAddress>(target64);
    if (type != kBindTypePointer) {
        appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                         "bind type is not a supported 32-bit pointer", source);
        return;
    }

    const auto binding = shims.resolve(symbol);
    // A repeat fixup for an already-trapped import re-resolves the trap
    // binding; route it back through the trap writer (which accepts any
    // addend) instead of the native-callout addend rule.
    const bool alreadyTrapped =
        trapContext && static_cast<bool>(binding) && trapContext->traps->has(symbol);
    if (!binding || alreadyTrapped) {
        if (trapContext) {
            bindTrapAt(memory, report, *trapContext, symbol, library, ordinal,
                       target, addend, weakImport, source);
            return;
        }
        appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                         "no tested native call adapter is registered for this symbol", source);
        return;
    }
    GuestAddress resolvedAddress = binding->guestAddress;
    if (binding->resolveGuestAddress) {
        std::string resolutionError;
        if (!binding->resolveGuestAddress(memory, resolvedAddress, resolutionError) || resolvedAddress == 0) {
            appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                             resolutionError.empty() ? "guest data symbol could not be materialized"
                                                     : resolutionError,
                             source);
            return;
        }
    }
    const auto shimAddress = static_cast<std::uint64_t>(resolvedAddress);
    std::uint64_t resolved = shimAddress;
    if (addend >= 0) {
        const auto positiveAddend = static_cast<std::uint64_t>(addend);
        if (positiveAddend > std::numeric_limits<GuestAddress>::max() - shimAddress) {
            appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                             "registered shim address plus addend exceeds the guest address space", source);
            return;
        }
        resolved += positiveAddend;
    } else {
        const auto magnitude = static_cast<std::uint64_t>(-(addend + 1)) + 1;
        if (magnitude > shimAddress) {
            appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                             "registered shim address plus addend exceeds the guest address space", source);
            return;
        }
        resolved -= magnitude;
    }
    if (resolved > std::numeric_limits<GuestAddress>::max()) {
        appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                         "registered shim address plus addend exceeds the guest address space", source);
        return;
    }
    if (resolved != shimAddress && !binding->resolveGuestAddress) {
        appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                         "non-zero bind addends are unsupported for native shim callouts", source);
        return;
    }
    const auto guestAddress = static_cast<GuestAddress>(resolved);
    if (!memory.initialize(target, &guestAddress, sizeof(guestAddress))) {
        appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                         "could not write the registered shim address into the bind slot", source);
        return;
    }
    auto resolvedBinding = *binding;
    resolvedBinding.guestAddress = static_cast<GuestAddress>(resolved);
    report.resolvedSymbols.push_back(makeSymbolRecord(symbol, "resolved", library, ordinal,
                                                       target, weakImport, &resolvedBinding, "", source));
}

void applyRebases(const Reader &reader, const ImageState &image, GuestAddress slide,
                  GuestAddressSpace &memory, MachOLoadReport &report) {
    if (image.streams.rebaseSize == 0)
        return;
    const auto end = checkedStreamEnd(reader, image.streams.rebaseOffset,
                                      image.streams.rebaseSize, "rebase");
    std::size_t cursor = image.streams.rebaseOffset;
    std::uint8_t type = 0;
    std::uint32_t segmentIndex = 0;
    std::uint64_t offset = 0;
    std::size_t actions = 0;
    auto rebaseOne = [&]() {
        if (++actions > kMaximumFixups)
            throw std::runtime_error("dyld rebase action count exceeds the loader limit");
        if (type != kRebaseTypePointer)
            throw std::runtime_error("dyld rebase type is not a supported 32-bit pointer");
        if (segmentIndex >= image.segments.size())
            throw std::runtime_error("dyld rebase references an invalid segment index");
        const auto &segment = image.segments[segmentIndex];
        if (!segment.mapped || offset > segment.vmSize || sizeof(std::uint32_t) > segment.vmSize - offset)
            throw std::runtime_error("dyld rebase target is outside a mapped guest segment");
        const auto target64 = static_cast<std::uint64_t>(segment.guestAddress) + offset;
        if (target64 > std::numeric_limits<GuestAddress>::max())
            throw std::runtime_error("dyld rebase target address overflows the guest address space");
        const auto target = static_cast<GuestAddress>(target64);
        std::uint32_t pointer = 0;
        if (!memory.read(target, &pointer, sizeof(pointer)))
            throw std::runtime_error("could not read the dyld rebase pointer");
        if (pointer != 0)
            pointer = addGuestAddress(pointer, slide, "rebased pointer");
        if (!memory.initialize(target, &pointer, sizeof(pointer)))
            throw std::runtime_error("could not write the dyld rebase pointer");
        ++report.rebasesApplied;
        offset = addStreamOffset(offset, sizeof(std::uint32_t));
    };

    while (cursor < end) {
        const auto opcode = reader.u8(cursor++);
        const auto operation = opcode & 0xf0;
        const auto immediate = static_cast<std::uint8_t>(opcode & 0x0f);
        switch (operation) {
        case 0x00: // DONE
            return;
        case 0x10: // SET_TYPE_IMM
            type = immediate;
            break;
        case 0x20: // SET_SEGMENT_AND_OFFSET_ULEB
            segmentIndex = immediate;
            offset = readUleb(reader, cursor, end);
            break;
        case 0x30: // ADD_ADDR_ULEB
            offset = addStreamOffset(offset, readUleb(reader, cursor, end));
            break;
        case 0x40: // ADD_ADDR_IMM_SCALED
            offset = addStreamOffset(offset,
                static_cast<std::uint64_t>(immediate) * sizeof(std::uint32_t));
            break;
        case 0x50: // DO_REBASE_IMM_TIMES
            for (std::uint8_t count = 0; count < immediate; ++count)
                rebaseOne();
            break;
        case 0x60: { // DO_REBASE_ULEB_TIMES
            const auto count = readUleb(reader, cursor, end);
            if (count > kMaximumFixups - actions)
                throw std::runtime_error("dyld rebase action count exceeds the loader limit");
            for (std::uint64_t index = 0; index < count; ++index)
                rebaseOne();
            break;
        }
        case 0x70: // DO_REBASE_ADD_ADDR_ULEB
            rebaseOne();
            offset = addStreamOffset(offset, readUleb(reader, cursor, end));
            break;
        case 0x80: { // DO_REBASE_ULEB_TIMES_SKIPPING_ULEB
            const auto count = readUleb(reader, cursor, end);
            const auto skip = readUleb(reader, cursor, end);
            if (count > kMaximumFixups - actions)
                throw std::runtime_error("dyld rebase action count exceeds the loader limit");
            for (std::uint64_t index = 0; index < count; ++index) {
                rebaseOne();
                offset = addStreamOffset(offset, skip);
            }
            break;
        }
        default:
            throw std::runtime_error("unsupported dyld rebase opcode");
        }
    }
}

void applyBinds(const Reader &reader, const ImageState &image, GuestAddressSpace &memory,
                const ShimRegistry &shims, MachOLoadReport &report,
                std::uint32_t streamOffset, std::uint32_t streamSize, bool lazy,
                const char *source, TrapContext *trapContext = nullptr) {
    if (streamSize == 0)
        return;
    const auto end = checkedStreamEnd(reader, streamOffset, streamSize, source);
    std::size_t cursor = streamOffset;
    std::string symbol;
    std::int64_t ordinal = 0;
    std::int64_t addend = 0;
    std::uint32_t segmentIndex = 0;
    std::uint64_t offset = 0;
    std::uint8_t type = kBindTypePointer;
    bool weakImport = false;
    std::size_t actions = 0;
    auto emitBind = [&]() {
        if (++actions > kMaximumFixups)
            throw std::runtime_error("dyld bind action count exceeds the loader limit");
        bindAt(image, memory, shims, report, segmentIndex, offset, type, symbol,
               ordinal, addend, weakImport, source, trapContext);
        offset = addStreamOffset(offset, sizeof(std::uint32_t));
    };
    auto resetLazyEntry = [&]() {
        symbol.clear();
        ordinal = 0;
        addend = 0;
        segmentIndex = 0;
        offset = 0;
        type = kBindTypePointer;
        weakImport = false;
    };

    while (cursor < end) {
        const auto opcode = reader.u8(cursor++);
        const auto operation = opcode & 0xf0;
        const auto immediate = static_cast<std::uint8_t>(opcode & 0x0f);
        switch (operation) {
        case 0x00: // DONE
            if (lazy) {
                resetLazyEntry();
                continue;
            }
            return;
        case 0x10: // SET_DYLIB_ORDINAL_IMM
            ordinal = immediate;
            break;
        case 0x20: { // SET_DYLIB_ORDINAL_ULEB
            const auto rawOrdinal = readUleb(reader, cursor, end);
            if (rawOrdinal > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                throw std::runtime_error("dyld dylib ordinal exceeds the supported range");
            ordinal = static_cast<std::int64_t>(rawOrdinal);
            break;
        }
        case 0x30: // SET_DYLIB_SPECIAL_IMM (four-bit signed value)
            ordinal = immediate == 0 ? 0 : (immediate >= 8 ? static_cast<std::int64_t>(immediate) - 16
                                                            : immediate);
            break;
        case 0x40: { // SET_SYMBOL_TRAILING_FLAGS_IMM
            const auto start = cursor;
            while (cursor < end && reader.u8(cursor) != 0)
                ++cursor;
            if (cursor >= end)
                throw std::runtime_error("dyld bind symbol is not terminated");
            symbol = reader.cstring(start, cursor + 1);
            ++cursor;
            weakImport = (immediate & 0x1) != 0;
            break;
        }
        case 0x50: // SET_TYPE_IMM
            type = immediate;
            break;
        case 0x60: // SET_ADDEND_SLEB
            addend = readSleb(reader, cursor, end);
            break;
        case 0x70: // SET_SEGMENT_AND_OFFSET_ULEB
            segmentIndex = immediate;
            offset = readUleb(reader, cursor, end);
            break;
        case 0x80: // ADD_ADDR_ULEB
            offset = addStreamOffset(offset, readUleb(reader, cursor, end));
            break;
        case 0x90: // DO_BIND
            emitBind();
            break;
        case 0xa0: // DO_BIND_ADD_ADDR_ULEB
            emitBind();
            offset = addStreamOffset(offset, readUleb(reader, cursor, end));
            break;
        case 0xb0: // DO_BIND_ADD_ADDR_IMM_SCALED
            emitBind();
            offset = addStreamOffset(offset,
                static_cast<std::uint64_t>(immediate) * sizeof(std::uint32_t));
            break;
        case 0xc0: { // DO_BIND_ULEB_TIMES_SKIPPING_ULEB
            const auto count = readUleb(reader, cursor, end);
            const auto skip = readUleb(reader, cursor, end);
            if (count > kMaximumFixups - actions)
                throw std::runtime_error("dyld bind action count exceeds the loader limit");
            for (std::uint64_t index = 0; index < count; ++index) {
                emitBind();
                offset = addStreamOffset(offset, skip);
            }
            break;
        }
        default:
            throw std::runtime_error("unsupported dyld bind opcode");
        }
    }
}

struct NlistEntry {
    std::string name;
    std::uint8_t type = 0;
    std::uint16_t description = 0;
    std::uint32_t value = 0;

    bool undefinedExternal() const {
        return (type & 0xe0) == 0 && (type & 0x0e) == 0 && (type & 0x01) != 0;
    }

    bool weakImport() const { return (description & 0x0040) != 0; }

    std::int64_t libraryOrdinal() const {
        const auto raw = static_cast<std::uint8_t>(description >> 8);
        return raw >= 0x80 ? static_cast<std::int64_t>(raw) - 0x100 : raw;
    }
};

NlistEntry readNlistEntry(const Reader &reader, const ImageState &image,
                          std::uint32_t symbolIndex) {
    if (!image.symbols.present || symbolIndex >= image.symbols.symbolCount)
        throw std::runtime_error("Mach-O relocation references an invalid nlist symbol index");
    const auto entry = static_cast<std::uint64_t>(image.symbols.symbolOffset) +
                       static_cast<std::uint64_t>(symbolIndex) * 12;
    reader.range(entry, 12);
    const auto stringIndex = reader.u32le(static_cast<std::size_t>(entry));
    NlistEntry result;
    result.type = reader.u8(static_cast<std::size_t>(entry + 4));
    result.description = reader.u16le(static_cast<std::size_t>(entry + 6));
    result.value = reader.u32le(static_cast<std::size_t>(entry + 8));
    if (stringIndex >= image.symbols.stringSize)
        throw std::runtime_error("Mach-O nlist symbol has an invalid string-table offset");
    const auto stringStart = static_cast<std::uint64_t>(image.symbols.stringOffset) + stringIndex;
    const auto stringEnd = static_cast<std::uint64_t>(image.symbols.stringOffset) +
                           image.symbols.stringSize;
    if (stringStart > std::numeric_limits<std::size_t>::max() ||
        stringEnd > std::numeric_limits<std::size_t>::max())
        throw std::runtime_error("Mach-O nlist string range exceeds the host address space");
    result.name = reader.cstring(static_cast<std::size_t>(stringStart),
                                 static_cast<std::size_t>(stringEnd));
    return result;
}

bool resolveRegisteredAddress(const ShimRegistry &shims, GuestAddressSpace &memory,
                              const NlistEntry &symbol, const std::string &library,
                              GuestAddress &address,
                              std::optional<ShimBinding> &binding, std::string &reason,
                              TrapContext *trapContext = nullptr,
                              bool *wasTrapped = nullptr) {
    binding = shims.resolve(symbol.name);
    if (!binding) {
        if (!trapContext) {
            reason = "no tested native call adapter is registered for this symbol";
            return false;
        }
        try {
            address = trapContext->traps->bind(*trapContext->registry, symbol.name, library);
        } catch (const std::exception &error) {
            reason = error.what();
            return false;
        }
        // Re-resolve: the trap binding is now registered.
        binding = trapContext->registry->resolve(symbol.name);
        if (!binding) {
            reason = "trap binding was not registered for this symbol";
            return false;
        }
        if (wasTrapped)
            *wasTrapped = true;
    } else if (trapContext && trapContext->traps->has(symbol.name)) {
        // Repeat fixup for an already-trapped import: keep the trap
        // attribution (and its addend tolerance) instead of treating the
        // trap callout as a real native adapter.
        address = binding->guestAddress;
        if (wasTrapped)
            *wasTrapped = true;
    }
    address = binding->guestAddress;
    if (binding->resolveGuestAddress) {
        if (!binding->resolveGuestAddress(memory, address, reason) || address == 0) {
            if (reason.empty())
                reason = "guest data symbol could not be materialized";
            return false;
        }
    }
    return true;
}

void applyIndirectSymbolPointers(const Reader &reader, const ImageState &image,
                                GuestAddress slide, GuestAddressSpace &memory,
                                const ShimRegistry &shims, MachOLoadReport &report,
                                std::set<std::string> &observedSymbols,
                                TrapContext *trapContext = nullptr) {
    if (!image.dynamicSymbols.present)
        return;
    reader.range(image.dynamicSymbols.indirectSymbolOffset,
                 static_cast<std::uint64_t>(image.dynamicSymbols.indirectSymbolCount) * 4);

    // Lazy symbol pointers appear first in the real iOS 2-6 link products and
    // are the function targets consumed by their ARM symbol stubs. Process them
    // before other fixups so the first reported missing import has a stable,
    // address-bearing source record.
    for (const auto sectionType : {kSectionLazySymbolPointers, kSectionNonLazySymbolPointers}) {
        for (const auto &segment : image.segments) {
            for (const auto &section : segment.sections) {
                if ((section.flags & kSectionTypeMask) != sectionType)
                    continue;
                if ((section.size % sizeof(std::uint32_t)) != 0) {
                    throw std::runtime_error("Mach-O indirect symbol-pointer section has a partial slot");
                }
                const auto pointerCount = section.size / sizeof(std::uint32_t);
                if (section.reserved1 > image.dynamicSymbols.indirectSymbolCount ||
                    pointerCount > image.dynamicSymbols.indirectSymbolCount - section.reserved1)
                    throw std::runtime_error("Mach-O section exceeds the indirect symbol table");
                for (std::uint32_t index = 0; index < pointerCount; ++index) {
                    const auto indirectIndex = section.reserved1 + index;
                    const auto indirectOffset = static_cast<std::uint64_t>(
                        image.dynamicSymbols.indirectSymbolOffset) +
                        static_cast<std::uint64_t>(indirectIndex) * 4;
                    const auto symbolIndex = reader.u32le(static_cast<std::size_t>(indirectOffset));
                    if ((symbolIndex & (kIndirectSymbolLocal | kIndirectSymbolAbsolute)) != 0)
                        continue;
                    const auto symbol = readNlistEntry(reader, image, symbolIndex);
                    if (!symbol.undefinedExternal() || symbol.name.empty())
                        continue;
                    observedSymbols.insert(symbol.name);
                    const auto ordinal = symbol.libraryOrdinal();
                    const auto library = dependencyName(image, ordinal);
                    const auto target64 = static_cast<std::uint64_t>(section.address) +
                                          static_cast<std::uint64_t>(index) * sizeof(std::uint32_t);
                    const auto target = addGuestAddress(target64, slide, "indirect symbol pointer");
                    if (!memory.contains(target, sizeof(std::uint32_t), MemoryPermission::Write)) {
                        appendUnresolved(report, symbol.name, library, ordinal, target,
                                         symbol.weakImport(),
                                         "indirect symbol pointer is outside writable guest memory",
                                         "indirect-symbol");
                        continue;
                    }
                    GuestAddress resolvedAddress = 0;
                    std::optional<ShimBinding> binding;
                    std::string resolutionError;
                    bool wasTrapped = false;
                    if (!resolveRegisteredAddress(shims, memory, symbol, library,
                                                  resolvedAddress, binding, resolutionError,
                                                  trapContext, &wasTrapped)) {
                        appendUnresolved(report, symbol.name, library, ordinal, target,
                                         symbol.weakImport(), resolutionError, "indirect-symbol");
                        continue;
                    }
                    // A 32-bit indirect pointer slot holds a guest address: a
                    // callout thunk for a function import, or the materialized
                    // guest address of a data import (both fit the slot).
                    if (!memory.initialize(target, &resolvedAddress, sizeof(resolvedAddress))) {
                        appendUnresolved(report, symbol.name, library, ordinal, target,
                                         symbol.weakImport(),
                                         binding->resolveGuestAddress
                                             ? "could not write the resolved data address into an indirect symbol pointer"
                                             : "could not write the callout address into an indirect symbol pointer",
                                         "indirect-symbol");
                        continue;
                    }
                    binding->guestAddress = resolvedAddress;
                    if (wasTrapped) {
                        auto trapped = makeSymbolRecord(
                            symbol.name, "trapped", library, ordinal, target,
                            symbol.weakImport(), nullptr,
                            "no tested native call adapter is registered for this "
                            "symbol; bound to an abort-on-call trap",
                            "indirect-symbol");
                        trapped["trapAddress"] = static_cast<std::uint64_t>(resolvedAddress);
                        if (!report.firstMissingImport)
                            report.firstMissingImport = symbol.name;
                        report.trappedSymbols.push_back(std::move(trapped));
                        continue;
                    }
                    report.resolvedSymbols.push_back(makeSymbolRecord(
                        symbol.name, "resolved", library, ordinal, target,
                        symbol.weakImport(), &*binding, "", "indirect-symbol"));
                }
            }
        }
    }
}

void applyExternalRelocations(const Reader &reader, const ImageState &image,
                              GuestAddress slide, GuestAddressSpace &memory,
                              const ShimRegistry &shims, MachOLoadReport &report,
                              std::set<std::string> &observedSymbols,
                              TrapContext *trapContext = nullptr) {
    if (!image.dynamicSymbols.present || image.dynamicSymbols.externalRelocationCount == 0)
        return;
    reader.range(image.dynamicSymbols.externalRelocationOffset,
                 static_cast<std::uint64_t>(image.dynamicSymbols.externalRelocationCount) * 8);
    std::set<std::string> unresolvedSymbols;
    for (std::uint32_t index = 0; index < image.dynamicSymbols.externalRelocationCount; ++index) {
        const auto relocation = static_cast<std::uint64_t>(
            image.dynamicSymbols.externalRelocationOffset) + static_cast<std::uint64_t>(index) * 8;
        const auto rawAddress = reader.u32le(static_cast<std::size_t>(relocation));
        const auto info = reader.u32le(static_cast<std::size_t>(relocation + 4));
        if ((rawAddress & 0x80000000U) != 0)
            throw std::runtime_error("scattered relocations are invalid in the external relocation table");
        const bool external = ((info >> 27) & 1U) != 0;
        const auto length = (info >> 25) & 0x3U;
        const bool pcRelative = ((info >> 24) & 1U) != 0;
        const auto relocationType = (info >> 28) & 0xfU;
        if (!external)
            throw std::runtime_error("external relocation table contains a local relocation");
        const auto symbolIndex = info & 0x00ffffffU;
        const auto symbol = readNlistEntry(reader, image, symbolIndex);
        if (!symbol.undefinedExternal() || symbol.name.empty())
            continue;
        observedSymbols.insert(symbol.name);
        const auto ordinal = symbol.libraryOrdinal();
        const auto library = dependencyName(image, ordinal);
        const auto target = addGuestAddress(rawAddress, slide, "external relocation target");
        if (!memory.contains(target, sizeof(std::uint32_t), MemoryPermission::Write)) {
            if (unresolvedSymbols.insert(symbol.name).second)
                appendUnresolved(report, symbol.name, library, ordinal, target,
                                 symbol.weakImport(),
                                 "external relocation target is outside writable guest memory",
                                 "external-relocation");
            continue;
        }
        if (relocationType != 0 || pcRelative || length != 2) {
            if (unresolvedSymbols.insert(symbol.name).second)
                appendUnresolved(report, symbol.name, library, ordinal, target,
                                 symbol.weakImport(),
                                 "only absolute 32-bit ARM vanilla external relocations are supported",
                                 "external-relocation");
            continue;
        }
        GuestAddress resolvedAddress = 0;
        std::optional<ShimBinding> binding;
        std::string resolutionError;
        bool wasTrapped = false;
        if (!resolveRegisteredAddress(shims, memory, symbol, library, resolvedAddress,
                                      binding, resolutionError, trapContext, &wasTrapped)) {
            if (unresolvedSymbols.insert(symbol.name).second)
                appendUnresolved(report, symbol.name, library, ordinal, target,
                                 symbol.weakImport(), resolutionError, "external-relocation");
            continue;
        }
        std::uint32_t rawAddend = 0;
        if (!memory.read(target, &rawAddend, sizeof(rawAddend))) {
            if (unresolvedSymbols.insert(symbol.name).second)
                appendUnresolved(report, symbol.name, library, ordinal, target,
                                 symbol.weakImport(),
                                 "could not read the external relocation addend",
                                 "external-relocation");
            continue;
        }
        const auto addend = static_cast<std::int64_t>(static_cast<std::int32_t>(rawAddend));
        const auto signedAddress = static_cast<std::int64_t>(resolvedAddress) + addend;
        // Traps accept any addend: trap+addend still lands in the trap range,
        // so a guest touch faults honestly and maps back to the import.
        if (signedAddress < 0 ||
            signedAddress > std::numeric_limits<GuestAddress>::max() ||
            (addend != 0 && !binding->resolveGuestAddress && !wasTrapped)) {
            if (unresolvedSymbols.insert(symbol.name).second)
                appendUnresolved(report, symbol.name, library, ordinal, target,
                                 symbol.weakImport(),
                                 binding->resolveGuestAddress
                                     ? "data-symbol relocation addend exceeds the guest address space"
                                     : "non-zero relocation addend is unsupported for a native callout",
                                 "external-relocation");
            continue;
        }
        const auto finalAddress = static_cast<GuestAddress>(signedAddress);
        if (!memory.initialize(target, &finalAddress, sizeof(finalAddress))) {
            if (unresolvedSymbols.insert(symbol.name).second)
                appendUnresolved(report, symbol.name, library, ordinal, target,
                                 symbol.weakImport(),
                                 "could not write the relocated guest symbol address",
                                 "external-relocation");
            continue;
        }
        binding->guestAddress = finalAddress;
        if (wasTrapped) {
            auto trapped = makeSymbolRecord(
                symbol.name, "trapped", library, ordinal, target,
                symbol.weakImport(), nullptr,
                "no tested native call adapter is registered for this "
                "symbol; bound to an abort-on-call trap",
                "external-relocation");
            trapped["trapAddress"] = static_cast<std::uint64_t>(finalAddress);
            if (!report.firstMissingImport)
                report.firstMissingImport = symbol.name;
            report.trappedSymbols.push_back(std::move(trapped));
            continue;
        }
        report.resolvedSymbols.push_back(makeSymbolRecord(
            symbol.name, "resolved", library, ordinal, target,
            symbol.weakImport(), &*binding, "", "external-relocation"));
    }
}

void appendUnboundNlistImports(const Reader &reader, const ImageState &image,
                               const ShimRegistry &shims, MachOLoadReport &report,
                               const std::set<std::string> &alreadyReported,
                               TrapContext *trapContext = nullptr) {
    if (!image.symbols.present)
        return;
    reader.range(image.symbols.symbolOffset,
                 static_cast<std::uint64_t>(image.symbols.symbolCount) * 12);
    reader.range(image.symbols.stringOffset, image.symbols.stringSize);
    for (std::uint32_t index = 0; index < image.symbols.symbolCount; ++index) {
        const auto symbol = readNlistEntry(reader, image, index);
        if (!symbol.undefinedExternal() || symbol.name.empty() ||
            alreadyReported.count(symbol.name) != 0)
            continue;
        const auto ordinal = symbol.libraryOrdinal();
        const auto library = dependencyName(image, ordinal);
        const auto binding = shims.resolve(symbol.name);
        if (trapContext) {
            // Nlist-only names have no slot to trap, and no stub or relocation
            // can reach them. They are reported separately and never block a
            // boot attempt; any actual guest touch faults honestly.
            const char *trapReason = binding
                ? "undefined symbol has no loader binding location"
                : "no tested native call adapter is registered for this symbol and "
                  "no loader binding location exists";
            auto record = makeSymbolRecord(symbol.name, "unresolved", library, ordinal, 0,
                                           symbol.weakImport(), nullptr, trapReason, "nlist");
            if (!report.firstMissingImport && !binding)
                report.firstMissingImport = symbol.name;
            report.unboundNlistSymbols.push_back(std::move(record));
            continue;
        }
        if (binding) {
            // Without an indirect pointer or relocation, the symbol is known but cannot be installed.
            appendUnresolved(report, symbol.name, library, ordinal, 0, symbol.weakImport(),
                             "undefined symbol has no loader binding location", "nlist");
        } else {
            appendUnresolved(report, symbol.name, library, ordinal, 0, symbol.weakImport(),
                             "no tested native call adapter is registered for this symbol", "nlist");
        }
    }
}

void parseLoadCommands(const Reader &reader, ImageState &image,
                       std::size_t commandOffset, std::uint32_t commandCount,
                       std::uint32_t commandBytes) {
    reader.range(commandOffset, commandBytes);
    std::size_t cursor = commandOffset;
    const auto end = commandOffset + commandBytes;
    for (std::uint32_t index = 0; index < commandCount; ++index) {
        if (cursor + 8 > end)
            throw std::runtime_error("Mach-O load-command table is truncated");
        const auto command = reader.u32le(cursor);
        const auto commandSize = reader.u32le(cursor + 4);
        if (commandSize < 8 || (commandSize & 3) != 0 || commandSize > end - cursor)
            throw std::runtime_error("Mach-O load-command size is invalid");
        if (command == kLcSegment) {
            if (commandSize < kSegmentCommand32Size)
                throw std::runtime_error("Mach-O 32-bit segment command is truncated");
            Segment segment;
            segment.name = reader.fixedString(cursor + 8, 16);
            segment.vmAddress = reader.u32le(cursor + 24);
            segment.vmSize = reader.u32le(cursor + 28);
            segment.fileOffset = reader.u32le(cursor + 32);
            segment.fileSize = reader.u32le(cursor + 36);
            segment.initialProtection = reader.u32le(cursor + 44);
            const auto sectionCount = reader.u32le(cursor + 48);
            if (sectionCount > (commandSize - kSegmentCommand32Size) / kSection32Size)
                throw std::runtime_error("Mach-O section table exceeds its segment command");
            if (segment.fileSize > segment.vmSize)
                throw std::runtime_error("Mach-O segment file size exceeds its virtual size");
            reader.range(segment.fileOffset, segment.fileSize);
            if (static_cast<std::uint64_t>(segment.vmAddress) + segment.vmSize >
                (std::uint64_t{1} << 32))
                throw std::runtime_error("Mach-O segment exceeds the 32-bit address space");
            std::size_t sectionCursor = cursor + kSegmentCommand32Size;
            for (std::uint32_t sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex) {
                Section section;
                section.name = reader.fixedString(sectionCursor, 16);
                section.segmentName = reader.fixedString(sectionCursor + 16, 16);
                section.address = reader.u32le(sectionCursor + 32);
                section.size = reader.u32le(sectionCursor + 36);
                section.fileOffset = reader.u32le(sectionCursor + 40);
                section.flags = reader.u32le(sectionCursor + 56);
                section.reserved1 = reader.u32le(sectionCursor + 60);
                section.reserved2 = reader.u32le(sectionCursor + 64);
                if (section.segmentName != segment.name)
                    throw std::runtime_error("Mach-O section " + section.name + " names segment " +
                                             section.segmentName + " instead of " + segment.name);
                if (section.address < segment.vmAddress ||
                    static_cast<std::uint64_t>(section.address) + section.size >
                        static_cast<std::uint64_t>(segment.vmAddress) + segment.vmSize)
                    throw std::runtime_error("Mach-O section is outside its segment");
                segment.sections.push_back(std::move(section));
                sectionCursor += kSection32Size;
            }
            image.segments.push_back(std::move(segment));
        } else if (command == kLcLoadDylib || command == kLcLazyLoadDylib ||
                   command == kLcLoadWeakDylib || command == kLcLoadUpwardDylib ||
                   command == kLcReexportDylib) {
            if (commandSize < 24)
                throw std::runtime_error("Mach-O dylib load command is truncated");
            const auto nameOffset = reader.u32le(cursor + 8);
            if (nameOffset < 24 || nameOffset >= commandSize)
                throw std::runtime_error("Mach-O dylib name offset is invalid");
            image.dependencies.push_back(reader.cstring(cursor + nameOffset, cursor + commandSize));
        } else if (command == kLcDyldInfo || command == kLcDyldInfoOnly) {
            if (commandSize < 48)
                throw std::runtime_error("Mach-O dyld-info command is truncated");
            if (image.streams.present)
                throw std::runtime_error("Mach-O has duplicate dyld-info commands");
            image.streams.rebaseOffset = reader.u32le(cursor + 8);
            image.streams.rebaseSize = reader.u32le(cursor + 12);
            image.streams.bindOffset = reader.u32le(cursor + 16);
            image.streams.bindSize = reader.u32le(cursor + 20);
            image.streams.weakBindOffset = reader.u32le(cursor + 24);
            image.streams.weakBindSize = reader.u32le(cursor + 28);
            image.streams.lazyBindOffset = reader.u32le(cursor + 32);
            image.streams.lazyBindSize = reader.u32le(cursor + 36);
            image.streams.present = true;
        } else if (command == kLcSymtab) {
            if (commandSize < 24)
                throw std::runtime_error("Mach-O symbol-table command is truncated");
            if (image.symbols.present)
                throw std::runtime_error("Mach-O has duplicate symbol-table commands");
            image.symbols.symbolOffset = reader.u32le(cursor + 8);
            image.symbols.symbolCount = reader.u32le(cursor + 12);
            image.symbols.stringOffset = reader.u32le(cursor + 16);
            image.symbols.stringSize = reader.u32le(cursor + 20);
            image.symbols.present = true;
        } else if (command == kLcDysymtab) {
            if (commandSize < 80)
                throw std::runtime_error("Mach-O dynamic symbol-table command is truncated");
            if (image.dynamicSymbols.present)
                throw std::runtime_error("Mach-O has duplicate dynamic symbol-table commands");
            image.dynamicSymbols.indirectSymbolOffset = reader.u32le(cursor + 56);
            image.dynamicSymbols.indirectSymbolCount = reader.u32le(cursor + 60);
            image.dynamicSymbols.externalRelocationOffset = reader.u32le(cursor + 64);
            image.dynamicSymbols.externalRelocationCount = reader.u32le(cursor + 68);
            image.dynamicSymbols.present = true;
        } else if (command == kLcEncryptionInfo) {
            if (commandSize < 20)
                throw std::runtime_error("Mach-O encryption-info command is truncated");
            image.encrypted = image.encrypted || reader.u32le(cursor + 16) != 0;
        } else if (command == kLcUnixThread) {
            std::size_t state = cursor + 8;
            while (state + 8 <= cursor + commandSize) {
                const auto flavor = reader.u32le(state);
                const auto count = reader.u32le(state + 4);
                state += 8;
                const std::uint64_t stateBytes = static_cast<std::uint64_t>(count) * 4;
                if (stateBytes > cursor + commandSize - state)
                    throw std::runtime_error("Mach-O thread-state record exceeds its command");
                if (flavor == kThreadFlavorArm && count >= kArmThreadStateWords) {
                    for (std::size_t reg = 0; reg < image.registers.r.size(); ++reg)
                        image.registers.r[reg] = reader.u32le(state + reg * 4);
                    image.registers.cpsr = reader.u32le(state + 16 * 4);
                    image.entryPoint = image.registers.r[15];
                    image.hasThreadEntry = true;
                    image.hasEntryPoint = true;
                    break;
                }
                state += static_cast<std::size_t>(stateBytes);
            }
        } else if (command == kLcMain) {
            if (commandSize < 24)
                throw std::runtime_error("Mach-O main-entry command is truncated");
            if (image.mainEntryOffset)
                throw std::runtime_error("Mach-O has duplicate main-entry commands");
            image.mainEntryOffset = reader.u64le(cursor + 8);
        } else if (command == kLcDyldChainedFixups) {
            // LC_LINKEDIT_DATA layout: cmd, cmdsize, dataoff, datasize. The
            // payload itself is decoded by applyChainedFixups once the
            // segments are mapped.
            if (commandSize < 16)
                throw std::runtime_error("Mach-O chained-fixups command is truncated");
            if (image.chainedFixups.present)
                throw std::runtime_error("Mach-O has duplicate chained-fixups commands");
            image.chainedFixups.dataOffset = reader.u32le(cursor + 8);
            image.chainedFixups.dataSize = reader.u32le(cursor + 12);
            image.chainedFixups.present = true;
        }
        cursor += commandSize;
    }
    if (cursor != end)
        throw std::runtime_error("Mach-O load-command byte count does not match ncmds");
}

GuestAddress segmentAddressAtFileOffset(const ImageState &image, std::uint64_t fileOffset,
                                        GuestAddress slide) {
    for (const auto &segment : image.segments) {
        const std::uint64_t end = static_cast<std::uint64_t>(segment.fileOffset) + segment.fileSize;
        if (fileOffset >= segment.fileOffset && fileOffset < end)
            return addGuestAddress(static_cast<std::uint64_t>(segment.vmAddress) +
                                       (fileOffset - segment.fileOffset),
                                   slide, "entry point");
    }
    throw std::runtime_error("LC_MAIN entry offset is not in a file-backed segment");
}

void mapSegments(const Reader &reader, ImageState &image, GuestAddress slide,
                 GuestAddressSpace &memory, MachOLoadReport &report) {
    for (auto &segment : image.segments) {
        if (segment.vmSize == 0 || (segment.name == "__PAGEZERO" && segment.initialProtection == 0))
            continue;
        segment.guestAddress = addGuestAddress(segment.vmAddress, slide, "segment address");
        const auto segmentEnd = static_cast<std::uint64_t>(segment.guestAddress) + segment.vmSize;
        if (segment.guestAddress >= 0xf0000000 || segmentEnd > 0xf0000000)
            throw std::runtime_error("Mach-O segment overlaps the reserved native shim callout range");
        const auto permissions = toPermissions(segment.initialProtection);
        memory.mapAt(segment.guestAddress, segment.vmSize, permissions, segment.name);
        segment.mapped = true;
        if (segment.fileSize != 0 &&
            !memory.initialize(segment.guestAddress, reader.pointer(segment.fileOffset, segment.fileSize),
                               segment.fileSize))
            throw std::runtime_error("could not initialize mapped Mach-O segment");
        ++report.segmentCount;
    }
    report.mapped = report.segmentCount != 0;
}

void applyRebaseAndBindStreams(const Reader &reader, const ImageState &image,
                               GuestAddress slide, GuestAddressSpace &memory,
                               const ShimRegistry &shims, MachOLoadReport &report,
                               TrapContext *trapContext = nullptr) {
    applyRebases(reader, image, slide, memory, report);
    applyBinds(reader, image, memory, shims, report,
               image.streams.bindOffset, image.streams.bindSize, false, "bind",
               trapContext);
    applyBinds(reader, image, memory, shims, report,
               image.streams.weakBindOffset, image.streams.weakBindSize, false, "weak-bind",
               trapContext);
    applyBinds(reader, image, memory, shims, report,
               image.streams.lazyBindOffset, image.streams.lazyBindSize, true, "lazy-bind",
               trapContext);
}

struct ChainedImport {
    std::string name;
    std::int64_t libOrdinal = 0;
    std::int64_t addend = 0;
    bool weak = false;
};

/**
 * Relinks LC_DYLD_CHAINED_FIXUPS images (the modern dyld format that replaces
 * LC_DYLD_INFO bind opcodes). The 32-bit ARM loader supports the
 * DYLD_CHAINED_PTR_32 pointer format with the three import-table formats and
 * uncompressed symbol strings; anything else fails closed with an explicit
 * error instead of silently leaving import slots unbound.
 *
 * Bind nodes route through the same bindAt() used by the opcode stream, so the
 * shim registry, trap writer, and load report see chained imports exactly like
 * classic ones. Rebase nodes install target + slide, matching applyRebases().
 */
void applyChainedFixups(const Reader &reader, const ImageState &image, GuestAddress slide,
                        GuestAddressSpace &memory, const ShimRegistry &shims,
                        MachOLoadReport &report, TrapContext *trapContext = nullptr) {
    if (!image.chainedFixups.present)
        return;
    const auto base = static_cast<std::size_t>(image.chainedFixups.dataOffset);
    const auto size = static_cast<std::size_t>(image.chainedFixups.dataSize);
    if (size < 28)
        throw std::runtime_error("chained fixups payload is truncated");
    reader.range(base, size);
    if (reader.u32le(base) != 0)
        throw std::runtime_error("unsupported chained fixups version");
    const auto startsOffset = reader.u32le(base + 4);
    const auto importsOffset = reader.u32le(base + 8);
    const auto symbolsOffset = reader.u32le(base + 12);
    const auto importsCount = reader.u32le(base + 16);
    const auto importsFormat = reader.u32le(base + 20);
    const auto symbolsFormat = reader.u32le(base + 24);
    if (startsOffset >= size || importsOffset >= size || symbolsOffset > size)
        throw std::runtime_error("chained fixups offsets are outside the payload");
    if (symbolsFormat != 0)
        throw std::runtime_error(
            "compressed chained fixup symbol strings are unsupported by compat-runtime-v1");

    // --- import table ------------------------------------------------------
    std::size_t entrySize = 0;
    if (importsFormat == kChainedImports)
        entrySize = 4;
    else if (importsFormat == kChainedImportsAddend)
        entrySize = 8;
    else if (importsFormat == kChainedImportsAddend64)
        entrySize = 16;
    if (entrySize == 0)
        throw std::runtime_error("unsupported chained fixups import format");
    if (static_cast<std::uint64_t>(importsCount) > (size - importsOffset) / entrySize)
        throw std::runtime_error("chained fixups import table overruns the payload");
    std::vector<ChainedImport> imports;
    imports.reserve(importsCount);
    for (std::uint32_t index = 0; index < importsCount; ++index) {
        const auto at = base + importsOffset + static_cast<std::size_t>(index) * entrySize;
        ChainedImport entry;
        std::uint32_t nameOffset = 0;
        if (importsFormat == kChainedImportsAddend64) {
            const auto word = reader.u64le(at);
            entry.libOrdinal = static_cast<std::int16_t>(word & 0xffffU);
            entry.weak = ((word >> 16) & 1) != 0;
            nameOffset = static_cast<std::uint32_t>(word >> 32);
            entry.addend = static_cast<std::int64_t>(reader.u64le(at + 8));
        } else {
            const auto word = reader.u32le(at);
            entry.libOrdinal = static_cast<std::int8_t>(word & 0xffU);
            entry.weak = ((word >> 8) & 1) != 0;
            nameOffset = word >> 9;
            if (importsFormat == kChainedImportsAddend)
                entry.addend = static_cast<std::int32_t>(reader.u32le(at + 4));
        }
        const auto nameAbsolute = static_cast<std::uint64_t>(symbolsOffset) + nameOffset;
        if (nameAbsolute >= size)
            throw std::runtime_error("chained fixup symbol name is outside the payload");
        entry.name = reader.cstring(base + symbolsOffset + nameOffset, base + size);
        imports.push_back(std::move(entry));
    }

    // --- chain starts --------------------------------------------------------
    if (static_cast<std::uint64_t>(startsOffset) + 4 > size)
        throw std::runtime_error("chained fixups starts table is truncated");
    const auto segmentCount = reader.u32le(base + startsOffset);
    if (segmentCount > (size - startsOffset - 4) / 4)
        throw std::runtime_error("chained fixups starts count overruns the payload");
    if (segmentCount > image.segments.size())
        throw std::runtime_error("chained fixups reference more segments than the image has");

    std::size_t actions = 0;
    for (std::uint32_t segmentIndex = 0; segmentIndex < segmentCount; ++segmentIndex) {
        const auto relative = reader.u32le(base + startsOffset + 4 + segmentIndex * 4);
        if (relative == 0)
            continue;
        const auto segInfoAbsolute = static_cast<std::uint64_t>(startsOffset) + relative;
        if (segInfoAbsolute + 22 > size)
            throw std::runtime_error("chained fixups segment info is truncated");
        const auto segInfoAt = base + static_cast<std::size_t>(segInfoAbsolute);
        const auto infoSize = reader.u32le(segInfoAt);
        const auto pageSize = reader.u16le(segInfoAt + 4);
        const auto pointerFormat = reader.u16le(segInfoAt + 6);
        const auto pageCount = reader.u16le(segInfoAt + 20);
        if (pointerFormat != kChainedPtrFormat32)
            throw std::runtime_error(
                "chained fixups use a pointer format this 32-bit loader does not relink");
        if (pageSize != 0x1000 && pageSize != 0x4000)
            throw std::runtime_error("chained fixups declare an unsupported page size");
        if (infoSize < 22 + static_cast<std::uint64_t>(pageCount) * 2 ||
            infoSize > size - segInfoAbsolute)
            throw std::runtime_error("chained fixups segment info size is invalid");
        // The page_start[] array holds pageCount per-page entries followed by
        // any secondary chain-start entries referenced through START_MULTI.
        const auto totalStartEntries = (infoSize - 22) / 2;
        const auto &segment = image.segments[segmentIndex];
        if (!segment.mapped)
            continue; // nothing to fix up in an unmapped segment (__PAGEZERO)

        for (std::uint32_t page = 0; page < pageCount; ++page) {
            auto start = reader.u16le(segInfoAt + 22 + static_cast<std::size_t>(page) * 2);
            if (start == kChainedPtrStartNone)
                continue;
            std::vector<std::uint16_t> chainStarts;
            if ((start & kChainedPtrStartMulti) != 0) {
                // dyld stores the MULTI value as an absolute index into
                // page_start[] (page_count + extras index) and marks the last
                // secondary entry with DYLD_CHAINED_PTR_START_LAST.
                auto cursor = static_cast<std::size_t>(start & 0x7fff);
                for (;;) {
                    if (cursor >= totalStartEntries)
                        throw std::runtime_error(
                            "chained fixup multi-start index overruns the starts table");
                    const auto value = reader.u16le(segInfoAt + 22 + cursor * 2);
                    chainStarts.push_back(static_cast<std::uint16_t>(value & 0x7fff));
                    if ((value & kChainedPtrStartLast) != 0)
                        break;
                    ++cursor;
                }
            } else {
                chainStarts.push_back(start);
            }

            for (const auto chainStart : chainStarts) {
                std::uint64_t offsetInPage = chainStart;
                for (;;) {
                    const auto slotOffsetInSegment =
                        static_cast<std::uint64_t>(page) * pageSize + offsetInPage;
                    if (slotOffsetInSegment + sizeof(std::uint32_t) > segment.vmSize)
                        throw std::runtime_error("chained fixup pointer is outside its segment");
                    if (++actions > kMaximumFixups)
                        throw std::runtime_error("chained fixup count exceeds the loader limit");
                    const auto slotAddress = static_cast<GuestAddress>(
                        static_cast<std::uint64_t>(segment.guestAddress) + slotOffsetInSegment);
                    std::uint32_t word = 0;
                    if (!memory.read(slotAddress, &word, sizeof(word)))
                        throw std::runtime_error("chained fixup pointer is not readable guest memory");
                    const auto next = (word >> 1) & 0x1f;
                    if ((word & 1) != 0) {
                        // Bind node: resolve through the shim registry exactly
                        // like a classic dyld bind opcode would.
                        const auto ordinal = (word >> 6) & 0xfffff;
                        const auto chainAddend = static_cast<std::int64_t>((word >> 26) & 0x3f);
                        if (ordinal >= imports.size())
                            throw std::runtime_error("chained bind ordinal is outside the import table");
                        const auto &import = imports[ordinal];
                        bindAt(image, memory, shims, report, segmentIndex, slotOffsetInSegment,
                               kBindTypePointer, import.name, import.libOrdinal,
                               import.addend + chainAddend, import.weak, "chained-bind",
                               trapContext);
                    } else {
                        // Rebase node: target is the unslid vmaddr; install
                        // target + slide like applyRebases() does.
                        const auto target = word >> 6;
                        const auto rebased = addGuestAddress(target, slide, "chained rebase target");
                        if (!memory.initialize(slotAddress, &rebased, sizeof(rebased)))
                            throw std::runtime_error("could not apply chained rebase fixup");
                        ++report.rebasesApplied;
                    }
                    if (next == 0)
                        break;
                    offsetInPage += static_cast<std::uint64_t>(next) * sizeof(std::uint32_t);
                    if (offsetInPage >= pageSize)
                        throw std::runtime_error("chained fixup chain leaves its page");
                }
            }
        }
    }
}

} // namespace

bool MachOLoadReport::imageMapped() const noexcept { return mapped; }

radek::Json MachOLoadReport::toJson() const {
    radek::Json report = radek::Json::object();
    report["status"] = status;
    report["imageMapped"] = mapped;
    report["entryPoint"] = static_cast<std::uint64_t>(entryPoint);
    report["entryPointSource"] = entryPointSource;
    report["thumb"] = thumb;
    report["segmentCount"] = static_cast<std::uint64_t>(segmentCount);
    report["rebasesApplied"] = static_cast<std::uint64_t>(rebasesApplied);
    report["resolvedSymbolCount"] = static_cast<std::uint64_t>(resolvedSymbols.size());
    report["unresolvedSymbolCount"] = static_cast<std::uint64_t>(unresolvedSymbols.size());
    report["trappedSymbolCount"] = static_cast<std::uint64_t>(trappedSymbols.size());
    report["unboundNlistSymbolCount"] = static_cast<std::uint64_t>(unboundNlistSymbols.size());
    if (firstMissingImport)
        report["firstMissingImport"] = *firstMissingImport;
    else
        report["firstMissingImport"] = radek::Json();
    report["resolvedSymbols"] = radek::Json::array();
    report["unresolvedSymbols"] = radek::Json::array();
    report["trappedSymbols"] = radek::Json::array();
    report["unboundNlistSymbols"] = radek::Json::array();
    for (const auto &symbol : resolvedSymbols)
        report["resolvedSymbols"].push(symbol);
    for (const auto &symbol : unresolvedSymbols)
        report["unresolvedSymbols"].push(symbol);
    for (const auto &symbol : trappedSymbols)
        report["trappedSymbols"].push(symbol);
    for (const auto &symbol : unboundNlistSymbols)
        report["unboundNlistSymbols"].push(symbol);

    // Expose the work performed by the runtime import linker separately from
    // static code rewriting. Each record in resolvedSymbols represents an
    // import pointer/relocation slot whose 32-bit guest address was installed;
    // it is not an Android-native game callsite or a gameplay claim.
    struct LinkAggregate {
        bool resolved = false;
        bool trapped = false;
        bool unresolved = false;
        bool nlistOnly = false;
        std::uint64_t fixupCount = 0;
        std::string adapter;
        std::string providerLibrary;
        std::string bindingKind;
        radek::Json guestAddress;
        std::set<std::string> sources;
    };
    const auto stringField = [](const radek::Json &record, const char *key) -> std::string {
        const auto found = record.fields.find(key);
        return found == record.fields.end() ? std::string() : found->second.value;
    };
    std::map<std::string, LinkAggregate> linkMap;
    std::set<std::string> distinctImports;
    const auto collect = [&](const std::vector<radek::Json> &records, const char *kind) {
        for (const auto &record : records) {
            const auto symbol = stringField(record, "symbol");
            if (symbol.empty())
                continue;
            distinctImports.insert(symbol);
            auto &entry = linkMap[symbol];
            if (std::string(kind) == "resolved")
                entry.resolved = true;
            else if (std::string(kind) == "trapped")
                entry.trapped = true;
            else if (std::string(kind) == "unbound-nlist")
                entry.nlistOnly = true;
            else
                entry.unresolved = true;
            if (std::string(kind) != "unbound-nlist")
                ++entry.fixupCount;
            const auto source = stringField(record, "source");
            if (!source.empty())
                entry.sources.insert(source);
            const auto adapter = stringField(record, "adapter");
            if (!adapter.empty())
                entry.adapter = adapter;
            const auto providerLibrary = stringField(record, "shimLibrary");
            if (!providerLibrary.empty())
                entry.providerLibrary = providerLibrary;
            const auto bindingKind = stringField(record, "bindingKind");
            if (!bindingKind.empty())
                entry.bindingKind = bindingKind;
            const auto address = record.fields.find("guestAddress");
            if (address != record.fields.end())
                entry.guestAddress = address->second;
        }
    };
    collect(resolvedSymbols, "resolved");
    collect(trappedSymbols, "trapped");
    collect(unresolvedSymbols, "unresolved");
    collect(unboundNlistSymbols, "unbound-nlist");

    constexpr std::size_t kMaximumLinkMapRecords = 4096;
    radek::Json linking = radek::Json::object();
    linking["mechanism"] = "Mach-O dyld bind, chained-fixup, indirect-symbol, and external-relocation fixups to guest callout/data-provider addresses";
    linking["guestImageImportSlotsRelinked"] = static_cast<std::uint64_t>(resolvedSymbols.size());
    linking["guestImageImportSlotsTrapped"] = static_cast<std::uint64_t>(trappedSymbols.size());
    linking["guestImageImportSlotsUnresolved"] = static_cast<std::uint64_t>(unresolvedSymbols.size());
    linking["unboundNlistSymbolCount"] = static_cast<std::uint64_t>(unboundNlistSymbols.size());
    linking["distinctResolvedImportSymbols"] = static_cast<std::uint64_t>(
        std::count_if(linkMap.begin(), linkMap.end(), [](const auto &item) {
            return item.second.resolved;
        }));
    linking["distinctImportSymbols"] = static_cast<std::uint64_t>(distinctImports.size());
    linking["translatedGuestCodeCallsitesRewritten"] = std::uint64_t{0};
    linking["staticallyLinkedNativeGameObjects"] = std::uint64_t{0};
    linking["guestImageCodeStaticallyRecompiled"] = false;
    linking["status"] = !mapped ? "NOT_LOADED"
        : (unresolvedSymbols.size() > 0 ? "PARTIAL_UNRESOLVED"
           : (trappedSymbols.size() > 0 || unboundNlistSymbols.size() > 0
                  ? "BOUND_WITH_RUNTIME_TRAPS_OR_NLIST_ONLY"
                  : (distinctImports.empty() ? "NO_IMPORTS" : "COMPLETE")));
    radek::Json linkEntries = radek::Json::array();
    std::size_t emitted = 0;
    for (const auto &item : linkMap) {
        if (emitted >= kMaximumLinkMapRecords)
            break;
        const auto &aggregate = item.second;
        const std::string state = aggregate.resolved && (aggregate.trapped || aggregate.unresolved)
            ? "MIXED_FIXUP_STATUS"
            : aggregate.resolved ? "RELINKED_TO_GUEST_PROVIDER"
            : aggregate.trapped ? "BOUND_TO_ABORT_ON_CALL_TRAP"
            : aggregate.unresolved ? "UNRESOLVED"
            : "UNBOUND_NLIST_ONLY";
        radek::Json entry = radek::Json::object();
        entry["symbol"] = item.first;
        entry["status"] = state;
        entry["fixupCount"] = aggregate.fixupCount;
        entry["adapter"] = aggregate.adapter.empty() ? radek::Json() : radek::Json(aggregate.adapter);
        entry["providerLibrary"] = aggregate.providerLibrary.empty()
            ? radek::Json() : radek::Json(aggregate.providerLibrary);
        entry["bindingKind"] = aggregate.bindingKind.empty()
            ? radek::Json() : radek::Json(aggregate.bindingKind);
        entry["guestAddress"] = aggregate.guestAddress;
        entry["sources"] = radek::Json::array();
        for (const auto &source : aggregate.sources)
            entry["sources"].push(radek::Json(source));
        linkEntries.push(std::move(entry));
        ++emitted;
    }
    linking["providerLinkMap"] = std::move(linkEntries);
    linking["providerLinkMapTruncated"] = linkMap.size() > kMaximumLinkMapRecords;
    linking["note"] =
        "The runtime loader installs guest addresses in Mach-O import/fixup slots. This is runtime guest binding, not static Android relinking or rewriting of game code; trap and nlist-only records are not implementations.";
    report["runtimeLinking"] = std::move(linking);

    if (!error.empty())
        report["error"] = error;
    return report;
}

namespace {

MachOLoadReport loadImpl(const std::vector<std::uint8_t> &mainBinary,
                         GuestAddressSpace &addressSpace,
                         const ShimRegistry &shims,
                         GuestAddress slide,
                         TrapContext *trapContext) {
    MachOLoadReport report;
    try {
        if (mainBinary.empty())
            throw std::runtime_error("IPA main executable is empty");
        const auto slice = chooseArm32Slice(mainBinary);
        Reader reader(mainBinary, slice.offset, slice.size);
        if (reader.u32le(0) != kMachMagic32)
            throw std::runtime_error("selected Mach-O slice is not 32-bit little-endian ARM");
        reader.range(0, kMachHeader32Size);
        const auto cpuType = reader.u32le(4);
        const auto fileType = reader.u32le(12);
        const auto commandCount = reader.u32le(16);
        const auto commandBytes = reader.u32le(20);
        if (cpuType != kCpuTypeArm)
            throw std::runtime_error("selected Mach-O slice is not ARM32");
        if (fileType != kFileTypeExecute)
            throw std::runtime_error("Mach-O image is not an executable main binary");
        if (commandCount > kMaximumLoadCommands)
            throw std::runtime_error("Mach-O load-command count exceeds the loader limit");
        ImageState image;
        parseLoadCommands(reader, image, kMachHeader32Size, commandCount, commandBytes);
        if (image.encrypted) {
            report.status = "BLOCKED_ENCRYPTED";
            report.error = "Encrypted or FairPlay-protected Mach-O input is not supported.";
            return report;
        }
        if (image.segments.empty())
            throw std::runtime_error("Mach-O executable has no loadable segments");

        mapSegments(reader, image, slide, addressSpace, report);
        if (!report.imageMapped())
            throw std::runtime_error("Mach-O executable has no mappable segments");

        if (image.mainEntryOffset) {
            image.entryPoint = segmentAddressAtFileOffset(image, *image.mainEntryOffset, slide);
            image.entryPointSource = "LC_MAIN";
            image.registers.r[15] = image.entryPoint;
            image.hasEntryPoint = true;
        } else if (image.hasThreadEntry) {
            image.entryPointSource = "LC_UNIXTHREAD";
            const auto thumbBit = image.entryPoint & 1;
            image.entryPoint = addGuestAddress(image.entryPoint & ~GuestAddress{1}, slide,
                                               "initial thread PC") | thumbBit;
            image.registers.r[15] = image.entryPoint;
        }
        if (!image.hasEntryPoint)
            throw std::runtime_error("Mach-O executable has no supported ARM thread-state or LC_MAIN entry");
        const auto codeAddress = image.entryPoint & ~GuestAddress{1};
        const bool entryIsExecutable = std::any_of(image.segments.begin(), image.segments.end(),
            [&](const Segment &segment) {
                return segment.mapped && (segment.initialProtection & 4) != 0 &&
                       codeAddress >= segment.guestAddress &&
                       static_cast<std::uint64_t>(codeAddress) <
                           static_cast<std::uint64_t>(segment.guestAddress) + segment.vmSize;
            });
        if (!entryIsExecutable)
            throw std::runtime_error("Mach-O entry point is not inside an executable segment");

        applyRebaseAndBindStreams(reader, image, slide, addressSpace, shims, report,
                                  trapContext);
        applyChainedFixups(reader, image, slide, addressSpace, shims, report, trapContext);
        std::set<std::string> observedSymbols;
        for (const auto &record : report.resolvedSymbols) {
            const auto found = record.fields.find("symbol");
            if (found != record.fields.end())
                observedSymbols.insert(found->second.value);
        }
        for (const auto &record : report.unresolvedSymbols) {
            const auto found = record.fields.find("symbol");
            if (found != record.fields.end())
                observedSymbols.insert(found->second.value);
        }
        for (const auto &record : report.trappedSymbols) {
            const auto found = record.fields.find("symbol");
            if (found != record.fields.end())
                observedSymbols.insert(found->second.value);
        }
        applyIndirectSymbolPointers(reader, image, slide, addressSpace, shims, report,
                                    observedSymbols, trapContext);
        applyExternalRelocations(reader, image, slide, addressSpace, shims, report,
                                 observedSymbols, trapContext);
        appendUnboundNlistImports(reader, image, shims, report, observedSymbols,
                                  trapContext);

        report.entryPoint = image.entryPoint;
        report.entryPointSource = image.entryPointSource;
        report.thumb = (image.entryPoint & 1) != 0 || (image.registers.cpsr & (1U << 5)) != 0;
        report.initialRegisters = image.registers;
        report.initialRegisters.r[15] = image.entryPoint;

        for (const auto &segment : image.segments) {
            if (!segment.mapped)
                continue;
            for (const auto &section : segment.sections) {
                if (section.name != "__mod_init_func")
                    continue;
                const auto base = addGuestAddress(section.address, slide, "static initializer section");
                for (std::uint32_t offset = 0; offset + sizeof(std::uint32_t) <= section.size;
                     offset += sizeof(std::uint32_t)) {
                    std::uint32_t value = 0;
                    if (addressSpace.read(base + offset, &value, sizeof(value)) && value != 0)
                        report.initializers.push_back(static_cast<GuestAddress>(value));
                }
            }
        }

        std::vector<GuestImageSection> guestSections;
        for (const auto &segment : image.segments) {
            if (!segment.mapped)
                continue;
            for (const auto &section : segment.sections) {
                guestSections.push_back(GuestImageSection{
                    segment.name, section.name,
                    addGuestAddress(section.address, slide, "section address"),
                    section.size,
                });
            }
        }
        std::string metadataError;
        if (!shims.initializeImage(addressSpace, guestSections, metadataError)) {
            report.status = "BLOCKED_RUNTIME_METADATA";
            report.error = metadataError.empty()
                ? "A registered runtime rejected the mapped Mach-O metadata."
                : metadataError;
            return report;
        }
        if (!report.unresolvedSymbols.empty()) {
            report.status = "BLOCKED_UNRESOLVED_IMPORTS";
        } else if (trapContext && !report.trappedSymbols.empty()) {
            report.status = "LOADED_WITH_TRAPS";
        } else {
            report.status = "LOADED";
        }
    } catch (const std::exception &error) {
        report.status = "BLOCKED";
        report.error = error.what();
    }
    return report;
}

} // namespace

MachOLoadReport MachOLoader::load(const std::vector<std::uint8_t> &mainBinary,
                                  GuestAddressSpace &addressSpace,
                                  const ShimRegistry &shims,
                                  GuestAddress slide) const {
    return loadImpl(mainBinary, addressSpace, shims, slide, nullptr);
}

MachOLoadReport MachOLoader::loadWithTraps(const std::vector<std::uint8_t> &mainBinary,
                                           GuestAddressSpace &addressSpace,
                                           ShimRegistry &shims,
                                           TrapShimAdapter &traps,
                                           GuestAddress slide) const {
    TrapContext trapContext{&shims, &traps};
    return loadImpl(mainBinary, addressSpace, shims, slide, &trapContext);
}

} // namespace radek::compat_runtime
