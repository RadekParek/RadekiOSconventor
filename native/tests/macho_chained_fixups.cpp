// Chained fixups relinker coverage: a synthetic 32-bit ARM Mach-O using
// LC_DYLD_CHAINED_FIXUPS (DYLD_CHAINED_PTR_32) binds two imports through the
// same registry/trap path as the classic dyld bind stream and applies rebase
// nodes with the image slide. Unsupported pointer formats fail closed.
#include "compat_runtime/guest_memory.hpp"
#include "compat_runtime/macho_loader.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/trap_shims.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression))                                                                           \
            throw std::runtime_error("CHECK failed: " #expression);                                  \
    } while (false)

namespace {
using namespace radek::compat_runtime;

void putU32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes.at(offset + index) = static_cast<std::uint8_t>((value >> (index * 8)) & 0xff);
}

void putU16(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value & 0xff);
    bytes.at(offset + 1) = static_cast<std::uint8_t>((value >> 8) & 0xff);
}

void putName(std::vector<std::uint8_t> &bytes, std::size_t offset, const std::string &name,
             std::size_t width) {
    CHECK(name.size() <= width);
    std::memcpy(bytes.data() + offset, name.data(), name.size());
}

constexpr std::uint32_t kEntryFileOffset = 0x400;
constexpr std::uint32_t kChainedPayloadOffset = 0x2000;

// Minimal MH_EXECUTE: __TEXT(r-x) entry, __DATA(rw) holding the chain, and an
// LC_DYLD_CHAINED_FIXUPS payload with two imports plus one rebase node. The
// __DATA page is a DYLD_CHAINED_PTR_START_MULTI page whose secondary table
// starts two chains: [bind _chained_probe] -> [rebase 0x1400], and
// [bind _chained_second].
std::vector<std::uint8_t> makeChainedMachO(std::uint16_t pointerFormat = 3) {
    constexpr std::size_t headerSize = 28;
    std::vector<std::vector<std::uint8_t>> commands;
    auto segment = [](const std::string &name, std::uint32_t vmAddress,
                      std::uint32_t fileOffset, std::uint32_t protection) {
        std::vector<std::uint8_t> command(56, 0);
        putU32(command, 0, 0x1);
        putU32(command, 4, 56);
        putName(command, 8, name, 16);
        putU32(command, 24, vmAddress);
        putU32(command, 28, 0x1000);
        putU32(command, 32, fileOffset);
        putU32(command, 36, 0x1000);
        putU32(command, 40, 0);
        putU32(command, 44, protection);
        return command;
    };
    commands.push_back(segment("__TEXT", 0x1000, 0x0, 5));
    commands.push_back(segment("__DATA", 0x2000, 0x1000, 3));

    std::vector<std::uint8_t> mainCommand(24, 0);
    putU32(mainCommand, 0, 0x80000028); // LC_MAIN
    putU32(mainCommand, 4, 24);
    putU32(mainCommand, 8, kEntryFileOffset);
    commands.push_back(std::move(mainCommand));

    const std::string dylib = "libSystem.B.dylib";
    const auto dylibSize = (24 + dylib.size() + 1 + 3) & ~std::size_t{3};
    std::vector<std::uint8_t> dylibCommand(dylibSize, 0);
    putU32(dylibCommand, 0, 0xc); // LC_LOAD_DYLIB
    putU32(dylibCommand, 4, static_cast<std::uint32_t>(dylibSize));
    putU32(dylibCommand, 8, 24);
    std::memcpy(dylibCommand.data() + 24, dylib.c_str(), dylib.size() + 1);
    commands.push_back(std::move(dylibCommand));

    // Chained fixups payload (dyld_chained_fixups_header + starts + imports +
    // symbol strings), laid out exactly as include/mach-o/fixup-chains.h.
    //   header      @ 0:  version 0, starts 28, imports 68, symbols 76,
    //                     count 2, imports format 1, symbols format 0
    //   starts      @ 28: seg_count 2; __TEXT none, __DATA at payload+40
    //   segment     @ 40: dyld_chained_starts_in_segment (page_count 1) whose
    //                     page_start[0] is a MULTI entry pointing at two
    //                     chain starts (page offset 0 and 8)
    //   imports     @ 68: (ordinal 1, name 0), (ordinal 1, name 15)
    //   symbols     @ 76: "_chained_probe\0_chained_second\0"
    std::vector<std::uint8_t> payload(0x1000, 0);
    putU32(payload, 0, 0);          // fixups_version
    putU32(payload, 4, 28);         // starts_offset
    putU32(payload, 8, 68);         // imports_offset
    putU32(payload, 12, 76);        // symbols_offset
    putU32(payload, 16, 2);         // imports_count
    putU32(payload, 20, 1);         // imports_format = DYLD_CHAINED_IMPORT
    putU32(payload, 24, 0);         // symbols_format = uncompressed
    putU32(payload, 28, 2);         // seg_count
    putU32(payload, 32, 0);         // __TEXT: no chains
    putU32(payload, 36, 12);        // __DATA: starts info at payload+28+12=40
    // dyld_chained_starts_in_segment at payload+40. Layout per fixup-chains.h:
    //   size@0 page_size@4 pointer_format@6 segment_offset@8(64-bit)
    //   max_valid_pointer@16 page_count@20 page_start@22 chain_starts...
    putU32(payload, 40, 28);        // size = 22 + 2*page_count + 2*chain_starts
    putU16(payload, 44, 0x1000);    // page_size
    putU16(payload, 46, pointerFormat); // DYLD_CHAINED_PTR_32 = 3
    putU32(payload, 48, 0);         // segment_offset low
    putU32(payload, 52, 0);         // segment_offset high
    putU32(payload, 56, 0);         // max_valid_pointer
    putU16(payload, 60, 1);         // page_count
    // page_start[0] (array index 0): MULTI | first-overflow-index(page_count=1)
    putU16(payload, 62, 0x8001);
    // chain_starts (array indexes 1 and 2, same page_start[] array).
    putU16(payload, 64, 0x0000);    // chain A starts at page offset 0
    putU16(payload, 66, 0x8008);    // chain B starts at page offset 8, LAST
    // Imports table (format 1: lib_ordinal:8, weak:1, name_offset:23).
    putU32(payload, 68, 1u | (0u << 9));   // ordinal 1 -> libSystem.B.dylib
    putU32(payload, 72, 1u | (15u << 9));  // name offset 15
    const std::string symbols = "_chained_probe";
    const std::string symbols2 = "_chained_second";
    std::memcpy(payload.data() + 76, symbols.c_str(), symbols.size() + 1);
    std::memcpy(payload.data() + 76 + symbols.size() + 1, symbols2.c_str(),
                symbols2.size() + 1);
    const std::uint32_t payloadSize =
        76 + static_cast<std::uint32_t>(symbols.size() + 1 + symbols2.size() + 1);

    std::vector<std::uint8_t> chainedCommand(16, 0);
    putU32(chainedCommand, 0, 0x80000034); // LC_DYLD_CHAINED_FIXUPS
    putU32(chainedCommand, 4, 16);
    putU32(chainedCommand, 8, kChainedPayloadOffset);
    putU32(chainedCommand, 12, payloadSize);
    commands.push_back(std::move(chainedCommand));

    std::uint32_t commandBytes = 0;
    for (const auto &command : commands)
        commandBytes += static_cast<std::uint32_t>(command.size());
    std::vector<std::uint8_t> bytes(0x3000, 0);
    putU32(bytes, 0, 0xfeedface);
    putU32(bytes, 4, 12); // CPU_TYPE_ARM
    putU32(bytes, 8, 9);
    putU32(bytes, 12, 2); // MH_EXECUTE
    putU32(bytes, 16, static_cast<std::uint32_t>(commands.size()));
    putU32(bytes, 20, commandBytes);
    std::size_t cursor = headerSize;
    for (const auto &command : commands) {
        std::memcpy(bytes.data() + cursor, command.data(), command.size());
        cursor += command.size();
    }
    std::memcpy(bytes.data() + kChainedPayloadOffset, payload.data(), payloadSize);

    // Entry code (not executed in loader tests, but must be plausible).
    putU32(bytes, kEntryFileOffset, 0xe3a00001); // mov r0, #1

    // Chain nodes at vm 0x2000 (__DATA offset 0):
    //   slot0: bind ordinal 0, next 1           -> (0<<6)|(0<<26)|(1<<1)|1
    //   slot1: rebase target 0x1400, next 0     -> (0x1400<<6)  [end of chain A]
    //   slot2: bind ordinal 1, next 0           -> (1<<6)|1     [chain B]
    putU32(bytes, 0x1000 + 0, (0u << 6) | (1u << 1) | 1u);
    putU32(bytes, 0x1000 + 4, (0x1400u << 6));
    putU32(bytes, 0x1000 + 8, (1u << 6) | 1u);
    return bytes;
}

std::uint32_t readSlot(const GuestAddressSpace &memory, GuestAddress address) {
    std::uint32_t value = 0;
    CHECK(memory.read(address, &value, sizeof(value)));
    return value;
}

void testChainedImportsBindThroughRegistry() {
    ShimRegistry registry;
    ShimBinding probe;
    probe.darwinSymbol = "_chained_probe";
    probe.library = "libSystem.B.dylib";
    probe.adapterName = "test-chained-probe";
    probe.guestAddress = 0xf0060000;
    probe.invoke = [](CpuRegisterState &, GuestAddressSpace &, std::string &) { return true; };
    registry.registerBinding(probe);
    ShimBinding second;
    second.darwinSymbol = "_chained_second";
    second.library = "libSystem.B.dylib";
    second.adapterName = "test-chained-second";
    second.guestAddress = 0xf0060004;
    second.invoke = [](CpuRegisterState &, GuestAddressSpace &, std::string &) { return true; };
    registry.registerBinding(second);

    GuestAddressSpace memory;
    const auto report = MachOLoader().load(makeChainedMachO(), memory, registry);
    CHECK(report.status == "LOADED");
    CHECK(report.entryPoint == 0x1400);
    CHECK(report.unresolvedSymbols.empty());
    CHECK(report.trappedSymbols.empty());
    CHECK(report.resolvedSymbols.size() == 2);
    // Both binds and the rebase landed in guest memory.
    CHECK(readSlot(memory, 0x2000) == 0xf0060000);
    CHECK(readSlot(memory, 0x2008) == 0xf0060004);
    CHECK(readSlot(memory, 0x2004) == 0x1400); // unslid rebase target
    CHECK(report.rebasesApplied == 1);

    const auto json = report.toJson();
    CHECK(json.fields.at("resolvedSymbolCount").value == "2");
    const auto &linking = json.fields.at("runtimeLinking");
    CHECK(linking.fields.at("guestImageImportSlotsRelinked").value == "2");
    CHECK(linking.fields.at("guestImageImportSlotsUnresolved").value == "0");
    CHECK(linking.fields.at("distinctResolvedImportSymbols").value == "2");
    CHECK(linking.fields.at("mechanism").value.find("chained-fixup") != std::string::npos);
    // A chained bind is reported with its own source label.
    bool sawChainedSource = false;
    for (const auto &record : report.resolvedSymbols) {
        const auto source = record.fields.find("source");
        if (source != record.fields.end() && source->second.value == "chained-bind")
            sawChainedSource = true;
    }
    CHECK(sawChainedSource);
}

void testChainedImportsTrapWhenUnimplemented() {
    ShimRegistry registry;
    TrapShimAdapter traps;
    GuestAddressSpace memory;
    const auto report =
        MachOLoader().loadWithTraps(makeChainedMachO(), memory, registry, traps);
    CHECK(report.status == "LOADED_WITH_TRAPS");
    CHECK(report.unresolvedSymbols.empty());
    CHECK(report.trappedSymbols.size() == 2);
    CHECK(report.firstMissingImport == "_chained_probe");
    CHECK(readSlot(memory, 0x2000) == TrapShimAdapter::kTrapBase);
    CHECK(readSlot(memory, 0x2008) == TrapShimAdapter::kTrapBase + 4u);
    CHECK(readSlot(memory, 0x2004) == 0x1400);
}

void testChainedImportsFailClosedWithoutTraps() {
    ShimRegistry registry;
    GuestAddressSpace memory;
    const auto report = MachOLoader().load(makeChainedMachO(), memory, registry);
    CHECK(report.status == "BLOCKED_UNRESOLVED_IMPORTS");
    CHECK(report.firstMissingImport == "_chained_probe");
    CHECK(report.unresolvedSymbols.size() == 2);
}

void testUnsupportedPointerFormatFailsClosed() {
    ShimRegistry registry;
    GuestAddressSpace memory;
    // DYLD_CHAINED_PTR_ARM64E (1) is not a 32-bit relinkable format.
    const auto report = MachOLoader().load(makeChainedMachO(1), memory, registry);
    CHECK(report.status == "BLOCKED");
    CHECK(report.error.find("pointer format") != std::string::npos);
}

void testSlideAppliesToChainedRebases() {
    ShimRegistry registry;
    ShimBinding probe;
    probe.darwinSymbol = "_chained_probe";
    probe.library = "libSystem.B.dylib";
    probe.adapterName = "test-chained-probe";
    probe.guestAddress = 0xf0060000;
    probe.invoke = [](CpuRegisterState &, GuestAddressSpace &, std::string &) { return true; };
    registry.registerBinding(probe);
    ShimBinding second;
    second.darwinSymbol = "_chained_second";
    second.library = "libSystem.B.dylib";
    second.adapterName = "test-chained-second";
    second.guestAddress = 0xf0060004;
    second.invoke = [](CpuRegisterState &, GuestAddressSpace &, std::string &) { return true; };
    registry.registerBinding(second);

    GuestAddressSpace memory;
    const GuestAddress slide = 0x00100000;
    const auto report = MachOLoader().load(makeChainedMachO(), memory, registry, slide);
    CHECK(report.status == "LOADED");
    CHECK(report.entryPoint == 0x1400 + slide);
    // The rebase node carries the unslid target; the loader adds the slide.
    CHECK(readSlot(memory, 0x2000 + slide + 4) == 0x1400 + slide);
}
} // namespace

int main() {
    testChainedImportsBindThroughRegistry();
    testChainedImportsTrapWhenUnimplemented();
    testChainedImportsFailClosedWithoutTraps();
    testUnsupportedPointerFormatFailsClosed();
    testSlideAppliesToChainedRebases();
    return 0;
}
