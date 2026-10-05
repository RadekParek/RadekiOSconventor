#include "compat_runtime/macho_loader.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
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

struct Segment {
    std::string name;
    std::uint32_t vmAddress = 0;
    std::uint32_t vmSize = 0;
    std::uint32_t fileOffset = 0;
    std::uint32_t fileSize = 0;
    std::uint32_t initialProtection = 0;
    GuestAddress guestAddress = 0;
    bool mapped = false;
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

struct ImageState {
    std::vector<Segment> segments;
    std::vector<std::string> dependencies;
    DyldStreams streams;
    SymbolTable symbols;
    GuestAddress entryPoint = 0;
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

void bindAt(const ImageState &image, GuestAddressSpace &memory, const ShimRegistry &shims,
            MachOLoadReport &report, std::uint32_t segmentIndex, std::uint64_t offset,
            std::uint8_t type, const std::string &symbol, std::int64_t ordinal,
            std::int64_t addend, bool weakImport, const char *source) {
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
    if (!binding) {
        appendUnresolved(report, symbol, library, ordinal, target, weakImport,
                         "no tested native call adapter is registered for this symbol", source);
        return;
    }
    const auto shimAddress = static_cast<std::uint64_t>(binding->guestAddress);
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
    if (resolved != shimAddress) {
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
    report.resolvedSymbols.push_back(makeSymbolRecord(symbol, "resolved", library, ordinal,
                                                       target, weakImport, &*binding, "", source));
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
                const char *source) {
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
               ordinal, addend, weakImport, source);
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

void appendUnboundNlistImports(const Reader &reader, const ImageState &image,
                               const ShimRegistry &shims, MachOLoadReport &report,
                               const std::set<std::string> &alreadyReported) {
    if (!image.symbols.present)
        return;
    const std::uint64_t symbolBytes = static_cast<std::uint64_t>(image.symbols.symbolCount) * 12;
    reader.range(image.symbols.symbolOffset, symbolBytes);
    reader.range(image.symbols.stringOffset, image.symbols.stringSize);
    for (std::uint32_t index = 0; index < image.symbols.symbolCount; ++index) {
        const auto entry = static_cast<std::size_t>(image.symbols.symbolOffset) + index * 12;
        const auto stringIndex = reader.u32le(entry);
        const auto type = reader.u8(entry + 4);
        const auto description = reader.u16le(entry + 6);
        if ((type & 0xe0) != 0 || (type & 0x0e) != 0 || (type & 0x01) == 0)
            continue;
        if (stringIndex >= image.symbols.stringSize)
            throw std::runtime_error("Mach-O undefined symbol has an invalid string-table offset");
        const auto stringStart = static_cast<std::uint64_t>(image.symbols.stringOffset) + stringIndex;
        const auto stringEnd = static_cast<std::uint64_t>(image.symbols.stringOffset) +
                               image.symbols.stringSize;
        if (stringEnd > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("Mach-O string-table range exceeds the host address space");
        const auto symbol = reader.cstring(static_cast<std::size_t>(stringStart),
                                           static_cast<std::size_t>(stringEnd));
        if (symbol.empty() || alreadyReported.count(symbol))
            continue;
        const bool weak = (description & 0x0040) != 0;
        const auto binding = shims.resolve(symbol);
        if (binding) {
            // Without a dyld bind location, the symbol is known but cannot be installed.
            appendUnresolved(report, symbol, "nlist-undefined", 0, 0, weak,
                             "undefined symbol has no dyld bind location", "nlist");
        } else {
            appendUnresolved(report, symbol, "nlist-undefined", 0, 0, weak,
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
            throw std::runtime_error("chained dyld fixups are unsupported by compat-runtime-v1");
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
                               const ShimRegistry &shims, MachOLoadReport &report) {
    applyRebases(reader, image, slide, memory, report);
    applyBinds(reader, image, memory, shims, report,
               image.streams.bindOffset, image.streams.bindSize, false, "bind");
    applyBinds(reader, image, memory, shims, report,
               image.streams.weakBindOffset, image.streams.weakBindSize, false, "weak-bind");
    applyBinds(reader, image, memory, shims, report,
               image.streams.lazyBindOffset, image.streams.lazyBindSize, true, "lazy-bind");
}

} // namespace

bool MachOLoadReport::imageMapped() const noexcept { return mapped; }

radek::Json MachOLoadReport::toJson() const {
    radek::Json report = radek::Json::object();
    report["status"] = status;
    report["imageMapped"] = mapped;
    report["entryPoint"] = static_cast<std::uint64_t>(entryPoint);
    report["thumb"] = thumb;
    report["segmentCount"] = static_cast<std::uint64_t>(segmentCount);
    report["rebasesApplied"] = static_cast<std::uint64_t>(rebasesApplied);
    report["resolvedSymbolCount"] = static_cast<std::uint64_t>(resolvedSymbols.size());
    report["unresolvedSymbolCount"] = static_cast<std::uint64_t>(unresolvedSymbols.size());
    if (firstMissingImport)
        report["firstMissingImport"] = *firstMissingImport;
    else
        report["firstMissingImport"] = radek::Json();
    report["resolvedSymbols"] = radek::Json::array();
    report["unresolvedSymbols"] = radek::Json::array();
    for (const auto &symbol : resolvedSymbols)
        report["resolvedSymbols"].push(symbol);
    for (const auto &symbol : unresolvedSymbols)
        report["unresolvedSymbols"].push(symbol);
    if (!error.empty())
        report["error"] = error;
    return report;
}

MachOLoadReport MachOLoader::load(const std::vector<std::uint8_t> &mainBinary,
                                  GuestAddressSpace &addressSpace,
                                  const ShimRegistry &shims,
                                  GuestAddress slide) const {
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
            image.registers.r[15] = image.entryPoint;
            image.hasEntryPoint = true;
        } else if (image.hasThreadEntry) {
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

        applyRebaseAndBindStreams(reader, image, slide, addressSpace, shims, report);
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
        appendUnboundNlistImports(reader, image, shims, report, observedSymbols);

        report.entryPoint = image.entryPoint;
        report.thumb = (image.entryPoint & 1) != 0 || (image.registers.cpsr & (1U << 5)) != 0;
        report.initialRegisters = image.registers;
        report.initialRegisters.r[15] = image.entryPoint;
        report.status = report.unresolvedSymbols.empty() ? "LOADED" : "BLOCKED_UNRESOLVED_IMPORTS";
    } catch (const std::exception &error) {
        report.status = "BLOCKED";
        report.error = error.what();
    }
    return report;
}

} // namespace radek::compat_runtime
