#include "compat_runtime/runner.hpp"
#include "compat_runtime/audio_session_shims.hpp"
#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/ndk_compat_shims.hpp"
#include "compat_runtime/sjlj_unwind.hpp"

#include "compat_runtime/runtime_contract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define CHECK(expression)                                                                             \
    do {                                                                                              \
        if (!(expression))                                                                            \
            throw std::runtime_error("CHECK failed: " #expression);                                   \
    } while (false)

namespace {
using namespace radek::compat_runtime;

void putU32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes.at(offset + index) = static_cast<std::uint8_t>((value >> (index * 8)) & 0xff);
}

void putU32Be(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index)
        bytes.at(offset + index) = static_cast<std::uint8_t>((value >> ((3 - index) * 8)) & 0xff);
}

void putU64(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint64_t value) {
    putU32(bytes, offset, static_cast<std::uint32_t>(value));
    putU32(bytes, offset + 4, static_cast<std::uint32_t>(value >> 32));
}

void putName(std::vector<std::uint8_t> &bytes, std::size_t offset, const std::string &name,
             std::size_t width) {
    CHECK(name.size() <= width);
    std::memcpy(bytes.data() + offset, name.data(), name.size());
}

struct MachOptions {
    std::uint32_t cpuSubtype = 9;
    std::uint32_t dataProtection = 3;
    std::uint64_t entryOffset = 0x100;
    std::vector<std::uint8_t> bindStream;
    std::vector<std::uint8_t> rebaseStream;
    bool includeDataSegment = false;
    bool includeDependency = false;
    bool encrypted = false;
    bool includeUnboundSymbol = false;
    bool includeExternalRelocation = false;
    bool includeIndirectFunctionPointer = false;
    bool includeIndirectStub = false;
    bool includeObjectiveCMetadata = false;
    std::string indirectSymbolName = "_indirectFunction";
    std::string externalSymbolName = "_dataRef";
    std::uint32_t externalRelocationTarget = 0x2000;
    bool useUnixThread = false;
    bool mainBeforeSegments = false;
};

std::vector<std::uint8_t> makeMachO(const MachOptions &options = {}) {
    constexpr std::size_t headerSize = 28;
    constexpr std::size_t dataOffset = 0x1000;
    constexpr std::size_t rebaseOffset = 0x2000;
    constexpr std::size_t bindOffset = 0x2040;
    constexpr std::size_t symbolOffset = 0x2100;
    constexpr std::size_t stringSize = 64;
    constexpr std::size_t indirectSymbolOffset = 0x2180;
    constexpr std::size_t externalRelocationOffset = 0x2190;
    struct SymbolSpec {
        std::string name;
        bool weak = false;
    };
    std::vector<SymbolSpec> symbolSpecs;
    std::size_t indirectSymbolIndex = 0;
    std::size_t externalSymbolIndex = 0;
    if (options.includeUnboundSymbol)
        symbolSpecs.push_back({"_nlistOnly", true});
    if (options.includeIndirectFunctionPointer) {
        indirectSymbolIndex = symbolSpecs.size();
        symbolSpecs.push_back({options.indirectSymbolName, false});
    }
    if (options.includeExternalRelocation) {
        const auto existing = std::find_if(symbolSpecs.begin(), symbolSpecs.end(),
            [&](const SymbolSpec &spec) { return spec.name == options.externalSymbolName; });
        if (existing == symbolSpecs.end()) {
            externalSymbolIndex = symbolSpecs.size();
            symbolSpecs.push_back({options.externalSymbolName, false});
        } else {
            externalSymbolIndex = static_cast<std::size_t>(existing - symbolSpecs.begin());
        }
    }
    const auto stringOffset = symbolOffset + symbolSpecs.size() * 12;

    std::vector<std::vector<std::uint8_t>> commands;
    auto segment = [](const std::string &name, std::uint32_t vmAddress, std::uint32_t vmSize,
                      std::uint32_t fileOffset, std::uint32_t fileSize,
                      std::uint32_t protection) {
        std::vector<std::uint8_t> command(56, 0);
        putU32(command, 0, 0x1);
        putU32(command, 4, 56);
        putName(command, 8, name, 16);
        putU32(command, 24, vmAddress);
        putU32(command, 28, vmSize);
        putU32(command, 32, fileOffset);
        putU32(command, 36, fileSize);
        putU32(command, 40, 7);
        putU32(command, 44, protection);
        return command;
    };
    auto textCommand = segment("__TEXT", 0x1000, 0x1000, 0x0, 0x1000, 5);
    if (options.includeIndirectStub) {
        textCommand.resize(56 + 68, 0);
        putU32(textCommand, 4, static_cast<std::uint32_t>(textCommand.size()));
        putU32(textCommand, 48, 1);
        const std::size_t section = 56;
        putName(textCommand, section, "__symbol_stub4", 16);
        putName(textCommand, section + 16, "__TEXT", 16);
        putU32(textCommand, section + 32, 0x1600);
        putU32(textCommand, section + 36, 12);
        putU32(textCommand, section + 40, 0x600);
        putU32(textCommand, section + 56, 0x80000408); // S_SYMBOL_STUBS
        putU32(textCommand, section + 60, 0); // first indirect symbol index
        putU32(textCommand, section + 64, 12); // stub size
    }
    commands.push_back(std::move(textCommand));
    if (options.includeDataSegment) {
        struct SectionSpec {
            std::string name;
            std::uint32_t address;
            std::uint32_t size;
            std::uint32_t fileOffset;
            std::uint32_t flags = 0;
            std::uint32_t reserved1 = 0;
            std::uint32_t reserved2 = 0;
        };
        std::vector<SectionSpec> dataSections;
        if (options.includeIndirectFunctionPointer) {
            dataSections.push_back({"__la_symbol_ptr", 0x2000, sizeof(std::uint32_t),
                                    static_cast<std::uint32_t>(dataOffset), 0x7, 0, 0});
        }
        if (options.includeObjectiveCMetadata) {
            dataSections.push_back({"__objc_classlist", 0x2100, 4, 0x1100, 0, 0, 0});
            dataSections.push_back({"__objc_data", 0x2200, 0x40, 0x1200, 0, 0, 0});
            dataSections.push_back({"__objc_const", 0x2300, 0x100, 0x1300, 0, 0, 0});
            dataSections.push_back({"__objc_methname", 0x2400, 0x80, 0x1400, 0, 0, 0});
        }
        auto dataCommand = segment("__DATA", 0x2000, 0x1000, dataOffset, 0x1000,
                                   options.dataProtection);
        if (!dataSections.empty()) {
            dataCommand.resize(56 + 68 * dataSections.size(), 0);
            putU32(dataCommand, 4, static_cast<std::uint32_t>(dataCommand.size()));
            putU32(dataCommand, 48, static_cast<std::uint32_t>(dataSections.size()));
            for (std::size_t index = 0; index < dataSections.size(); ++index) {
                const auto &spec = dataSections[index];
                const auto section = 56 + 68 * index;
                putName(dataCommand, section, spec.name, 16);
                putName(dataCommand, section + 16, "__DATA", 16);
                putU32(dataCommand, section + 32, spec.address);
                putU32(dataCommand, section + 36, spec.size);
                putU32(dataCommand, section + 40, spec.fileOffset);
                putU32(dataCommand, section + 56, spec.flags);
                putU32(dataCommand, section + 60, spec.reserved1);
                putU32(dataCommand, section + 64, spec.reserved2);
            }
        }
        commands.push_back(std::move(dataCommand));
    }

    if (options.useUnixThread) {
        std::vector<std::uint8_t> command(84, 0);
        putU32(command, 0, 0x5);
        putU32(command, 4, static_cast<std::uint32_t>(command.size()));
        putU32(command, 8, 1); // ARM_THREAD_STATE
        putU32(command, 12, 17);
        putU32(command, 16 + 15 * 4, 0x1101);
        putU32(command, 16 + 16 * 4, 1U << 5);
        commands.push_back(std::move(command));
    } else {
        std::vector<std::uint8_t> command(24, 0);
        putU32(command, 0, 0x80000028);
        putU32(command, 4, 24);
        putU64(command, 8, options.entryOffset);
        commands.push_back(std::move(command));
    }

    if (options.includeDependency || !options.bindStream.empty()) {
        const std::string name = "libSystem.B.dylib";
        const auto commandSize = (24 + name.size() + 1 + 3) & ~std::size_t{3};
        std::vector<std::uint8_t> command(commandSize, 0);
        putU32(command, 0, 0xc);
        putU32(command, 4, static_cast<std::uint32_t>(commandSize));
        putU32(command, 8, 24);
        std::memcpy(command.data() + 24, name.c_str(), name.size() + 1);
        commands.push_back(std::move(command));
    }

    std::vector<std::uint8_t> dyld(48, 0);
    putU32(dyld, 0, 0x80000022);
    putU32(dyld, 4, 48);
    if (!options.rebaseStream.empty()) {
        putU32(dyld, 8, static_cast<std::uint32_t>(rebaseOffset));
        putU32(dyld, 12, static_cast<std::uint32_t>(options.rebaseStream.size()));
    }
    if (!options.bindStream.empty()) {
        putU32(dyld, 16, static_cast<std::uint32_t>(bindOffset));
        putU32(dyld, 20, static_cast<std::uint32_t>(options.bindStream.size()));
    }
    commands.push_back(std::move(dyld));

    if (options.encrypted) {
        std::vector<std::uint8_t> command(20, 0);
        putU32(command, 0, 0x21);
        putU32(command, 4, 20);
        putU32(command, 16, 1);
        commands.push_back(std::move(command));
    }
    if (!symbolSpecs.empty()) {
        std::vector<std::uint8_t> command(24, 0);
        putU32(command, 0, 0x2);
        putU32(command, 4, 24);
        putU32(command, 8, static_cast<std::uint32_t>(symbolOffset));
        putU32(command, 12, static_cast<std::uint32_t>(symbolSpecs.size()));
        putU32(command, 16, static_cast<std::uint32_t>(stringOffset));
        putU32(command, 20, static_cast<std::uint32_t>(stringSize));
        commands.push_back(std::move(command));
    }
    if (options.includeExternalRelocation || options.includeIndirectFunctionPointer) {
        std::vector<std::uint8_t> command(80, 0);
        putU32(command, 0, 0xb);
        putU32(command, 4, 80);
        putU32(command, 24, 0); // undefined-symbol index
        putU32(command, 28, static_cast<std::uint32_t>(symbolSpecs.size()));
        if (options.includeIndirectFunctionPointer) {
            putU32(command, 56, static_cast<std::uint32_t>(indirectSymbolOffset));
            putU32(command, 60, 1);
        }
        if (options.includeExternalRelocation) {
            putU32(command, 64, static_cast<std::uint32_t>(externalRelocationOffset));
            putU32(command, 68, 1);
        }
        commands.push_back(std::move(command));
    }

    if (options.mainBeforeSegments && !options.useUnixThread) {
        const auto entryIndex = options.includeDataSegment ? 2U : 1U;
        auto mainCommand = std::move(commands.at(entryIndex));
        commands.erase(commands.begin() + static_cast<std::ptrdiff_t>(entryIndex));
        commands.insert(commands.begin(), std::move(mainCommand));
    }

    std::uint32_t commandBytes = 0;
    for (const auto &command : commands)
        commandBytes += static_cast<std::uint32_t>(command.size());
    std::size_t fileSize = dataOffset + (options.includeDataSegment ? 0x1000 : 0);
    fileSize = std::max(fileSize, bindOffset + options.bindStream.size());
    fileSize = std::max(fileSize, rebaseOffset + options.rebaseStream.size());
    if (!symbolSpecs.empty())
        fileSize = std::max(fileSize, stringOffset + stringSize);
    if (options.includeIndirectFunctionPointer)
        fileSize = std::max(fileSize, indirectSymbolOffset + 4);
    if (options.includeExternalRelocation)
        fileSize = std::max(fileSize, externalRelocationOffset + 8);
    std::vector<std::uint8_t> bytes(fileSize, 0);

    putU32(bytes, 0, 0xfeedface);
    putU32(bytes, 4, 12);
    putU32(bytes, 8, options.cpuSubtype);
    putU32(bytes, 12, 2);
    putU32(bytes, 16, static_cast<std::uint32_t>(commands.size()));
    putU32(bytes, 20, commandBytes);
    putU32(bytes, 24, 0);
    std::size_t cursor = headerSize;
    for (const auto &command : commands) {
        std::memcpy(bytes.data() + cursor, command.data(), command.size());
        cursor += command.size();
    }
    if (!options.bindStream.empty())
        std::memcpy(bytes.data() + bindOffset, options.bindStream.data(), options.bindStream.size());
    if (!options.rebaseStream.empty())
        std::memcpy(bytes.data() + rebaseOffset, options.rebaseStream.data(), options.rebaseStream.size());
    if (options.includeDataSegment)
        putU32(bytes, dataOffset + 4, 0x1120);
    if (options.includeIndirectStub) {
        putU32(bytes, 0x400, 0xe52de004); // push {lr}
        putU32(bytes, 0x404, 0xeb00007d); // bl 0x1600
        putU32(bytes, 0x408, 0xe49de004); // ldr lr, [sp], #4
        putU32(bytes, 0x40c, 0xe12fff1e); // bx lr
        putU32(bytes, 0x600, 0xe59fc000); // ldr ip, [pc]
        putU32(bytes, 0x604, 0xe59cf000); // ldr pc, [ip]
        putU32(bytes, 0x608, 0x2000); // lazy symbol pointer slot
    }
    if (options.includeObjectiveCMetadata) {
        // One app class with an instance method and a class method; the class's
        // superclass is supplied by the vanilla external relocation below.
        putU32(bytes, 0x1100, 0x2200); // __objc_classlist -> DemoClass
        putU32(bytes, 0x1200, 0x2220); // DemoClass.isa -> DemoClass metaclass
        putU32(bytes, 0x1204, 0);      // DemoClass.superclass -> external NSObject
        putU32(bytes, 0x1210, 0x2301); // DemoClass.data -> class_ro_t | flags
        putU32(bytes, 0x1220, 0x2220); // metaclass.isa -> root metaclass
        putU32(bytes, 0x1230, 0x2361); // metaclass.data -> class_ro_t | flags

        const std::array<std::uint32_t, 10> classRO{{
            0, 4, 8, 0, 0x2400, 0x2340, 0x23f8, 0, 0, 0,
        }};
        const std::array<std::uint32_t, 10> metaRO{{
            1, 0, 8, 0, 0x2400, 0x23a0, 0, 0, 0, 0,
        }};
        std::memcpy(bytes.data() + 0x1300, classRO.data(), sizeof(classRO));
        putU32(bytes, 0x1340, 12); // instance method entry size
        putU32(bytes, 0x1344, 2);
        putU32(bytes, 0x1348, 0x240a); // selector "answer"
        putU32(bytes, 0x1350, 0x1500); // instance IMP
        putU32(bytes, 0x1354, 0x2430); // selector "copy"
        putU32(bytes, 0x1358, 0);
        putU32(bytes, 0x135c, 0x1520); // guest copy IMP, returns an owned receiver
        std::memcpy(bytes.data() + 0x1360, metaRO.data(), sizeof(metaRO));
        putU32(bytes, 0x13a0, 12); // class method entry size
        putU32(bytes, 0x13a4, 1);
        putU32(bytes, 0x13a8, 0x2411); // selector "classAnswer"
        putU32(bytes, 0x13b0, 0x1510); // class IMP
        putU32(bytes, 0x13c0, 0);      // DemoProtocol.isa
        putU32(bytes, 0x13c4, 0x2420); // DemoProtocol.name
        putU32(bytes, 0x13c8, 0);      // DemoProtocol adopted protocols
        putU32(bytes, 0x13f8, 1);      // class protocol-list count
        putU32(bytes, 0x13fc, 0x23c0); // DemoProtocol

        const std::string metadataStrings = std::string("DemoClass", 9) + '\0' +
                                            "answer" + '\0' + "classAnswer" + '\0';
        std::memcpy(bytes.data() + 0x1400, metadataStrings.data(), metadataStrings.size());
        const std::string protocolName = "DemoProtocol";
        std::memcpy(bytes.data() + 0x1420, protocolName.c_str(), protocolName.size() + 1);
        const std::string copySelectorName = "copy";
        std::memcpy(bytes.data() + 0x1430, copySelectorName.c_str(),
                    copySelectorName.size() + 1);

        // Entry sends DemoClass +classAnswer through the loader-patched ObjC stub.
        putU32(bytes, 0x400, 0xe92d4000); // push {lr}
        putU32(bytes, 0x404, 0xe59f0044); // ldr r0, [pc, #0x44]
        putU32(bytes, 0x408, 0xe59f1044); // ldr r1, [pc, #0x44]
        putU32(bytes, 0x40c, 0xeb00007b); // bl 0x1600
        putU32(bytes, 0x410, 0xe8bd4000); // pop {lr}
        putU32(bytes, 0x414, 0xe12fff1e); // bx lr
        putU32(bytes, 0x450, 0x2200);     // class receiver
        putU32(bytes, 0x454, 0x2411);     // selector string
        putU32(bytes, 0x500, 0xe3a0002a); // mov r0, #42 (instance IMP)
        putU32(bytes, 0x504, 0xe12fff1e); // bx lr
        putU32(bytes, 0x510, 0xe3a0002a); // mov r0, #42 (class IMP)
        putU32(bytes, 0x514, 0xe12fff1e); // bx lr
        putU32(bytes, 0x520, 0xe92d4010); // push {r4, lr}; guest copy IMP
        putU32(bytes, 0x524, 0xe59fc008); // ldr ip, [pc, #8]
        putU32(bytes, 0x528, 0xe12fff3c); // blx ip
        putU32(bytes, 0x52c, 0xe8bd8010); // pop {r4, pc}
        putU32(bytes, 0x534, 0);          // patched to _objc_retain callout
        putU32(bytes, 0x700, 0xe92d4010); // push {r4, lr}
        putU32(bytes, 0x704, 0xe24dd008); // sub sp, sp, #8 (outgoing setter flags)
        putU32(bytes, 0x708, 0xe59f0020); // ldr r0, [pc, #32] (receiver)
        putU32(bytes, 0x70c, 0xe59f2020); // ldr r2, [pc, #32] (ivar offset)
        putU32(bytes, 0x710, 0xe59f3020); // ldr r3, [pc, #32] (new value)
        putU32(bytes, 0x714, 0xe59fc020); // ldr ip, [pc, #32] (objc_setProperty)
        putU32(bytes, 0x718, 0xe12fff3c); // blx ip
        putU32(bytes, 0x71c, 0xe28dd008); // add sp, sp, #8
        putU32(bytes, 0x720, 0xe8bd4010); // pop {r4, lr}
        putU32(bytes, 0x724, 0xe3a0002a); // mov r0, #42 after the property call
        putU32(bytes, 0x728, 0xe12fff1e); // bx lr
        putU32(bytes, 0x730, 0);          // receiver literal
        putU32(bytes, 0x734, 4);          // ivar offset
        putU32(bytes, 0x738, 0);          // new-value literal
        putU32(bytes, 0x73c, 0);          // patched to _objc_setProperty callout
    }
    if (!symbolSpecs.empty()) {
        std::size_t stringCursor = 1;
        for (std::size_t index = 0; index < symbolSpecs.size(); ++index) {
            const auto symbolOffsetForEntry = symbolOffset + index * 12;
            const auto &spec = symbolSpecs[index];
            putU32(bytes, symbolOffsetForEntry, static_cast<std::uint32_t>(stringCursor));
            bytes[symbolOffsetForEntry + 4] = 0x01; // N_UNDF | N_EXT
            if (spec.weak)
                bytes[symbolOffsetForEntry + 6] = 0x40; // weak reference flag
            else
                bytes[symbolOffsetForEntry + 7] = 0x01; // library ordinal 1 in n_desc's high byte
            std::memcpy(bytes.data() + stringOffset + stringCursor,
                        spec.name.c_str(), spec.name.size() + 1);
            stringCursor += spec.name.size() + 1;
        }
        CHECK(stringCursor <= stringSize);
    }
    if (options.includeIndirectFunctionPointer)
        putU32(bytes, indirectSymbolOffset, static_cast<std::uint32_t>(indirectSymbolIndex));
    if (options.includeExternalRelocation) {
        putU32(bytes, externalRelocationOffset, options.externalRelocationTarget);
        putU32(bytes, externalRelocationOffset + 4,
               0x0c000000 | static_cast<std::uint32_t>(externalSymbolIndex));
    }
    return bytes;
}

std::vector<std::uint8_t> makeFatMachO(const std::vector<std::uint8_t> &older,
                                       const std::vector<std::uint8_t> &newer) {
    const std::size_t firstOffset = 48;
    const std::size_t secondOffset = (firstOffset + older.size() + 3) & ~std::size_t{3};
    std::vector<std::uint8_t> fat(secondOffset + newer.size(), 0);
    putU32Be(fat, 0, 0xcafebabe);
    putU32Be(fat, 4, 2);
    putU32Be(fat, 8, 12);
    putU32Be(fat, 12, 6); // ARMv6
    putU32Be(fat, 16, static_cast<std::uint32_t>(firstOffset));
    putU32Be(fat, 20, static_cast<std::uint32_t>(older.size()));
    putU32Be(fat, 24, 2);
    putU32Be(fat, 28, 12);
    putU32Be(fat, 32, 9); // ARMv7
    putU32Be(fat, 36, static_cast<std::uint32_t>(secondOffset));
    putU32Be(fat, 40, static_cast<std::uint32_t>(newer.size()));
    putU32Be(fat, 44, 2);
    std::memcpy(fat.data() + firstOffset, older.data(), older.size());
    std::memcpy(fat.data() + secondOffset, newer.data(), newer.size());
    return fat;
}

ShimBinding testBinding(const std::string &symbol, GuestAddress address = 0xf0001000) {
    return ShimBinding{
        symbol,
        "libcompat-foundation",
        "host-tested-test-adapter",
        address,
        [](CpuRegisterState &, GuestAddressSpace &, std::string &) { return true; },
        {},
        {},
        {},
    };
}

void testGuestMemoryAndHeap() {
    GuestAddressSpace memory(32 * 1024);
    memory.mapAt(0x1000, 4096, MemoryPermission::Read | MemoryPermission::Write, "data");
    std::uint32_t value = 0x12345678;
    CHECK(memory.write(0x1000, &value, sizeof(value)));
    std::uint32_t readBack = 0;
    CHECK(memory.read(0x1000, &readBack, sizeof(readBack)));
    CHECK(readBack == value);
    CHECK(!memory.read(0x1fff, &readBack, sizeof(readBack)));
    CHECK(memory.contains(0x1000, sizeof(value), MemoryPermission::Read));
    CHECK(memory.guestPointer<std::uint32_t>(0x1000) != nullptr);
    CHECK(memory.hostToGuest(memory.guestPointer<std::uint32_t>(0x1000)) == 0x1000);

    bool overlapFailed = false;
    try {
        memory.mapAt(0x1800, 4096, MemoryPermission::Read, "overlap");
    } catch (const std::runtime_error &) {
        overlapFailed = true;
    }
    CHECK(overlapFailed);

    memory.configureHeap(0x3000, 4096);
    const auto first = memory.allocateHeap(13, 16);
    const auto second = memory.allocateHeap(8, 8);
    CHECK(first == 0x3000);
    CHECK(second % 8 == 0);
    std::array<std::uint8_t, 13> zeroed{};
    CHECK(memory.read(first, zeroed.data(), zeroed.size()));
    for (const auto byte : zeroed)
        CHECK(byte == 0);

    const auto tinyMapping = memory.mapAny(4, MemoryPermission::Read | MemoryPermission::Write,
                                           "tiny-dynamic-cell");
    const auto regionsBeforeTinyUnmap = memory.regions();
    const auto tinyRegion = std::find_if(regionsBeforeTinyUnmap.begin(), regionsBeforeTinyUnmap.end(),
        [tinyMapping](const GuestRegionView &region) { return region.base == tinyMapping; });
    CHECK(tinyRegion != regionsBeforeTinyUnmap.end());
    CHECK(tinyRegion->size == 4096);
    memory.unmap(tinyMapping);

    memory.setPermissions(0x1000, MemoryPermission::Read);
    CHECK(!memory.write(0x1000, &value, sizeof(value)));
    CHECK(memory.read(0x1000, &readBack, sizeof(readBack)));
    CHECK(memory.callbacks().regions().size() == 2);
}

void testShimRegistry() {
    ShimRegistry registry;
    int calls = 0;
    auto binding = testBinding("_exactName", 0xf0001001);
    binding.invoke = [&calls](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
        ++calls;
        registers.r[0] = 77;
        return true;
    };
    registry.registerBinding(binding);
    CHECK(registry.size() == 1);
    CHECK(registry.resolve("_exactName").has_value());
    CHECK(!registry.resolve("_ExactName").has_value());
    CpuRegisterState registers;
    GuestAddressSpace memory;
    std::string reason;
    CHECK(registry.invokeCallout(0xf0001000, registers, memory, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 77);
    CHECK(calls == 1);
    CHECK(registry.invokeCallout(0xf0002000, registers, memory, reason) ==
          GuestCalloutResult::NotRegistered);

    bool duplicateCalloutFailed = false;
    try {
        registry.registerBinding(testBinding("_anotherName", 0xf0001000));
    } catch (const std::runtime_error &) {
        duplicateCalloutFailed = true;
    }
    CHECK(duplicateCalloutFailed);
    bool lowAddressFailed = false;
    try {
        registry.registerBinding(testBinding("_invalidAddress", 0x1000));
    } catch (const std::invalid_argument &) {
        lowAddressFailed = true;
    }
    CHECK(lowAddressFailed);
}

void testGuestPthreadCreateTransfersAndJoins() {
    ShimRegistry registry;
    radek::compat_runtime::ndk::ShimAdapter ndkShims;
    ndkShims.registerBindings(registry);
    GuestAddressSpace memory;
    memory.mapAt(0x1000, 4096, MemoryPermission::Read | MemoryPermission::Execute,
                 "guest-pthread-entry");
    memory.mapAt(0x2000, 4096, MemoryPermission::Read | MemoryPermission::Write,
                 "guest-pthread-state");

    const auto create = registry.resolve("_pthread_create");
    const auto continuation = registry.resolve("_radek_pthread_continuation");
    const auto exit = registry.resolve("_pthread_exit");
    const auto join = registry.resolve("_pthread_join");
    CHECK(create.has_value() && create->invokeTransfer);
    CHECK(continuation.has_value() && continuation->invoke);
    CHECK(exit.has_value() && exit->invoke);
    CHECK(join.has_value() && join->invoke);

    CpuRegisterState registers;
    registers.r[0] = 0x2000;
    registers.r[2] = 0x1000;
    registers.r[3] = 0x12345678;
    registers.r[14] = 0x3456;
    std::string reason;
    CHECK(registry.invokeCallout(create->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Transferred);
    CHECK(registers.r[15] == 0x1000);
    CHECK(registers.r[0] == 0x12345678);
    CHECK(registers.r[14] == continuation->guestAddress);
    std::uint32_t token = 0;
    CHECK(memory.read(0x2000, &token, sizeof(token)));
    CHECK(token != 0);
    CHECK(ndkShims.guestThreadTransferCount() == 1);

    CHECK(registry.invokeCallout(continuation->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    CHECK(registers.r[14] == 0x3456);
    CHECK(ndkShims.guestThreadCompletionCount() == 1);

    // A worker that uses pthread_exit must take the same continuation path
    // rather than turning a normal guest thread return into a host exception.
    registers = CpuRegisterState{};
    registers.r[0] = 0x2000;
    registers.r[2] = 0x1000;
    registers.r[3] = 0x87654321;
    registers.r[14] = 0x4567;
    CHECK(registry.invokeCallout(create->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Transferred);
    std::uint32_t exitToken = 0;
    CHECK(memory.read(0x2000, &exitToken, sizeof(exitToken)));
    CHECK(registry.invokeCallout(exit->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[14] == continuation->guestAddress);
    CHECK(registry.invokeCallout(continuation->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[14] == 0x4567);
    CHECK(ndkShims.guestThreadCompletionCount() == 2);

    registers.r[0] = token;
    CHECK(registry.invokeCallout(join->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    registers.r[0] = exitToken;
    CHECK(registry.invokeCallout(join->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
}

void testObjectiveCShimsResolveThroughLoaderAndReturn() {
    MachOptions options;
    options.includeDataSegment = true;
    options.includeDependency = true;
    auto &bind = options.bindStream;
    const std::vector<std::string> importedSymbols{
        "_objc_msgSend", "_OBJC_CLASS_$_NSAutoreleasePool", "_objc_msgSendSuper2",
        "_OBJC_CLASS_$_NSBundle", "_OBJC_CLASS_$_NSNumber", "_OBJC_CLASS_$_NSString",
        "_OBJC_CLASS_$_NSThread", "_OBJC_CLASS_$_NSURL", "_OBJC_CLASS_$_UIAccelerometer",
        "_OBJC_CLASS_$_UIApplication", "_OBJC_CLASS_$_UIScreen", "_OBJC_CLASS_$_UIView",
        "_OBJC_CLASS_$_UIWindow", "_OBJC_METACLASS_$_UIView", "_OBJC_CLASS_$_EAGLContext",
        "_OBJC_CLASS_$_CAEAGLLayer", "_OBJC_CLASS_$_NSDictionary", "_OBJC_CLASS_$_NSObject",
        "_OBJC_METACLASS_$_NSObject", "__objc_empty_cache", "__objc_empty_vtable",
        "_objc_msgSend_stret", "_objc_setProperty"};
    bind.push_back(0x11); // dependency ordinal 1
    for (std::size_t index = 0; index < importedSymbols.size(); ++index) {
        bind.push_back(0x40);
        for (const auto character : importedSymbols[index])
            bind.push_back(static_cast<std::uint8_t>(character));
        bind.push_back(0);
        if (index == 0)
            bind.insert(bind.end(), {0x71, 0x00}); // __DATA + 0
        bind.push_back(0x90); // bind, then advance by one pointer slot
    }
    bind.push_back(0x00); // DONE

    ShimRegistry registry;
    radek::compat_runtime::objc::ShimAdapter objcShims;
    auto *rootClass = objcShims.runtime().findClass("NSObject");
    CHECK(rootClass != nullptr);
    rootClass->ivarNames.push_back("_property");
    ++rootClass->instanceSlots;
    objcShims.registerBindings(registry);
    GuestAddressSpace memory;
    const auto loaded = MachOLoader().load(makeMachO(options), memory, registry);
    CHECK(loaded.status == "LOADED");
    CHECK(loaded.resolvedSymbols.size() == importedSymbols.size());
    CHECK(loaded.unresolvedSymbols.empty());
    for (std::size_t index = 0; index < importedSymbols.size(); ++index) {
        std::uint32_t boundAddress = 0;
        CHECK(memory.read(0x2000 + static_cast<GuestAddress>(index * sizeof(boundAddress)),
                          &boundAddress, sizeof(boundAddress)));
        const auto binding = registry.resolve(importedSymbols[index]);
        CHECK(binding.has_value());
        if (binding->resolveGuestAddress)
            CHECK(boundAddress != 0 && boundAddress < 0xf0000000);
        else
            CHECK(boundAddress == binding->guestAddress);
    }

    std::uint32_t messageCallout = 0;
    std::uint32_t poolClass = 0;
    std::uint32_t superCalloutAddress = 0;
    CHECK(memory.read(0x2000, &messageCallout, sizeof(messageCallout)));
    CHECK(memory.read(0x2004, &poolClass, sizeof(poolClass)));
    CHECK(memory.read(0x2008, &superCalloutAddress, sizeof(superCalloutAddress)));
    CHECK(messageCallout == registry.resolve("_objc_msgSend")->guestAddress);
    CHECK(superCalloutAddress == registry.resolve("_objc_msgSendSuper2")->guestAddress);
    CHECK(poolClass < 0xf0000000);
    memory.mapAt(0x3000, 4096, MemoryPermission::Read | MemoryPermission::Write,
                 "objc-test-strings");
    const std::string poolName = "NSAutoreleasePool";
    CHECK(memory.write(0x3040, poolName.c_str(), poolName.size() + 1));
    const auto getClass = registry.resolve("_objc_getClass");
    CHECK(getClass.has_value());
    CpuRegisterState classLookupRegisters;
    classLookupRegisters.r[0] = 0x3040;
    std::string classLookupReason;
    CHECK(registry.invokeCallout(getClass->guestAddress, classLookupRegisters, memory,
                                 classLookupReason) == GuestCalloutResult::Returned);
    CHECK(classLookupRegisters.r[0] == poolClass);
    const auto classNameCallout = registry.resolve("_class_getName");
    CHECK(classNameCallout.has_value());
    classLookupRegisters.r[0] = poolClass;
    CHECK(registry.invokeCallout(classNameCallout->guestAddress, classLookupRegisters, memory,
                                 classLookupReason) == GuestCalloutResult::Returned);
    std::array<char, 32> className{};
    CHECK(memory.read(classLookupRegisters.r[0], className.data(), className.size()));
    CHECK(std::string(className.data()) == poolName);
    CpuRegisterState classRegisters;
    std::string classReason;
    CHECK(registry.invokeCallout(poolClass, classRegisters, memory, classReason) ==
          GuestCalloutResult::NotRegistered);

    const std::array<char, 17> strings{{'n','e','w','\0','i','n','i','t','\0',
                                        'r','e','l','e','a','s','e','\0'}};
    CHECK(memory.write(0x3000, strings.data(), strings.size()));
    CpuRegisterState registers;
    registers.r[0] = poolClass;
    registers.r[1] = 0x3000;
    std::string reason;
    CHECK(registry.invokeCallout(messageCallout, registers, memory, reason) == GuestCalloutResult::Returned);
    const auto poolObject = registers.r[0];
    CHECK(poolObject != 0 && poolObject != poolClass);
    std::uint32_t objectClass = 0;
    CHECK(memory.read(poolObject, &objectClass, sizeof(objectClass)));
    CHECK(objectClass == poolClass);

    registers.r[0] = poolObject;
    registers.r[1] = 0x3004;
    CHECK(registry.invokeCallout(messageCallout, registers, memory, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == poolObject);

    const auto probeSelector = objcShims.runtime().selector("shimSuperProbe");
    objcShims.runtime().addMethod(rootClass, probeSelector,
                                  [](const radek::compat_runtime::objc::Receiver &,
                                     const radek::compat_runtime::objc::Arguments &) {
                                      return radek::compat_runtime::objc::Value{91};
                                  });
    const std::string probeName = "shimSuperProbe";
    CHECK(memory.write(0x3100, probeName.c_str(), probeName.size() + 1));
    const std::array<std::uint32_t, 2> superInfo{{poolObject, poolClass}};
    CHECK(memory.write(0x3200, superInfo.data(), sizeof(superInfo)));
    const auto superCallout = registry.resolve("_objc_msgSendSuper2");
    CHECK(superCallout.has_value());
    registers.r[0] = 0x3200;
    registers.r[1] = 0x3100;
    CHECK(registry.invokeCallout(superCallout->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 91);

    std::size_t rootClassIndex = 0;
    while (rootClassIndex < importedSymbols.size() &&
           importedSymbols[rootClassIndex] != "_OBJC_CLASS_$_NSObject")
        ++rootClassIndex;
    CHECK(rootClassIndex < importedSymbols.size());
    std::uint32_t rootClassAddress = 0;
    CHECK(memory.read(0x2000 + static_cast<GuestAddress>(rootClassIndex * sizeof(std::uint32_t)),
                      &rootClassAddress, sizeof(rootClassAddress)));
    const auto newNSObject = [&]() {
        registers.r[0] = rootClassAddress;
        registers.r[1] = 0x3000;
        CHECK(registry.invokeCallout(messageCallout, registers, memory, reason) ==
              GuestCalloutResult::Returned);
        CHECK(registers.r[0] != 0 && registers.r[0] != rootClassAddress);
        return registers.r[0];
    };
    const auto propertyReceiver = newNSObject();
    const auto propertyValue = newNSObject();
    const auto propertyStack = memory.mapAny(4096,
        MemoryPermission::Read | MemoryPermission::Write, "objc-property-stack");
    const auto propertySetter = registry.resolve("_objc_setProperty");
    CHECK(propertySetter.has_value());
    registers.r[0] = propertyReceiver;
    registers.r[1] = 0;
    registers.r[2] = sizeof(std::uint32_t);
    registers.r[3] = propertyValue;
    registers.r[13] = propertyStack;
    std::array<std::uint32_t, 2> propertyFlags{{0, 1}}; // copy is explicitly unsupported
    CHECK(memory.write(propertyStack, propertyFlags.data(), sizeof(propertyFlags)));
    std::string propertyReason;
    CHECK(registry.invokeCallout(propertySetter->guestAddress, registers, memory,
                                 propertyReason) == GuestCalloutResult::Failed);
    CHECK(propertyReason.find("unrecognized Objective-C selector") != std::string::npos);
    std::uint32_t propertySlot = 1;
    CHECK(memory.read(propertyReceiver + sizeof(std::uint32_t), &propertySlot,
                      sizeof(propertySlot)));
    CHECK(propertySlot == 0);

    const auto copySelector = objcShims.runtime().selector("copy");
    objcShims.runtime().addMethod(rootClass, copySelector,
        [&objcShims](const radek::compat_runtime::objc::Receiver &receiver,
                     const radek::compat_runtime::objc::Arguments &) {
            auto *copy = objcShims.runtime().allocate(receiver.object->isa);
            copy->ivars = receiver.object->ivars;
            return reinterpret_cast<radek::compat_runtime::objc::Value>(copy);
        });
    propertyFlags = {{0, 1}};
    CHECK(memory.write(propertyStack, propertyFlags.data(), sizeof(propertyFlags)));
    registers.r[0] = propertyReceiver;
    registers.r[2] = sizeof(std::uint32_t);
    registers.r[3] = propertyValue;
    CHECK(registry.invokeCallout(propertySetter->guestAddress, registers, memory,
                                 propertyReason) == GuestCalloutResult::Returned);
    CHECK(memory.read(propertyReceiver + sizeof(std::uint32_t), &propertySlot,
                      sizeof(propertySlot)));
    CHECK(propertySlot != 0 && propertySlot != propertyValue);
    const auto copiedByHost = propertySlot;
    CHECK(memory.contains(copiedByHost, sizeof(std::uint32_t)));

    propertyFlags = {{1, 0}}; // atomic retain property
    CHECK(memory.write(propertyStack, propertyFlags.data(), sizeof(propertyFlags)));
    registers.r[0] = propertyReceiver;
    registers.r[2] = sizeof(std::uint32_t);
    registers.r[3] = propertyValue;
    CHECK(registry.invokeCallout(propertySetter->guestAddress, registers, memory,
                                 propertyReason) == GuestCalloutResult::Returned);
    CHECK(memory.read(propertyReceiver + sizeof(std::uint32_t), &propertySlot,
                      sizeof(propertySlot)));
    CHECK(propertySlot == propertyValue);
    CHECK(!memory.contains(copiedByHost, sizeof(std::uint32_t)));

    const auto release = registry.resolve("_objc_release");
    CHECK(release.has_value());
    registers.r[0] = propertyValue;
    CHECK(registry.invokeCallout(release->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(memory.contains(propertyValue, sizeof(std::uint32_t)));
    propertyFlags = {{0, 0}};
    CHECK(memory.write(propertyStack, propertyFlags.data(), sizeof(propertyFlags)));
    registers.r[0] = propertyReceiver;
    registers.r[2] = sizeof(std::uint32_t);
    registers.r[3] = 0;
    CHECK(registry.invokeCallout(propertySetter->guestAddress, registers, memory,
                                 propertyReason) == GuestCalloutResult::Returned);
    CHECK(memory.read(propertyReceiver + sizeof(std::uint32_t), &propertySlot,
                      sizeof(propertySlot)));
    CHECK(propertySlot == 0);
    CHECK(!memory.contains(propertyValue, sizeof(std::uint32_t)));
    registers.r[0] = propertyReceiver;
    CHECK(registry.invokeCallout(release->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(!memory.contains(propertyReceiver, sizeof(std::uint32_t)));

    auto touchClassBinding = registry.resolve("_OBJC_CLASS_$_UITouch");
    CHECK(touchClassBinding.has_value() && touchClassBinding->resolveGuestAddress);
    GuestAddress touchClassAddress = 0;
    CHECK(touchClassBinding->resolveGuestAddress(memory, touchClassAddress, reason));
    registers.r[0] = touchClassAddress;
    registers.r[1] = 0x3000;
    CHECK(registry.invokeCallout(messageCallout, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    const auto touchObject = registers.r[0];
    const std::array<float, 2> point{{1.5F, -2.25F}};
    std::array<std::uint32_t, 2> pointBits{};
    std::memcpy(pointBits.data(), point.data(), sizeof(point));
    CHECK(memory.write(touchObject + sizeof(std::uint32_t), pointBits.data(), sizeof(pointBits)));
    const auto pointBuffer = memory.mapAny(4096,
        MemoryPermission::Read | MemoryPermission::Write, "objc-stret-result");
    const std::string locationSelectorName = "locationInView:";
    CHECK(memory.write(0x3100, locationSelectorName.c_str(), locationSelectorName.size() + 1));
    const auto stretCallout = registry.resolve("_objc_msgSend_stret");
    CHECK(stretCallout.has_value());
    registers.r[0] = pointBuffer;
    registers.r[1] = touchObject;
    registers.r[2] = 0x3100;
    registers.r[3] = rootClassAddress; // locationInView: ignores the view in this bounded model
    CHECK(memory.write(propertyStack, &rootClassAddress, sizeof(rootClassAddress)));
    registers.r[13] = propertyStack;
    CHECK(registry.invokeCallout(stretCallout->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == pointBuffer);
    std::array<std::uint32_t, 2> returnedPoint{};
    CHECK(memory.read(pointBuffer, returnedPoint.data(), sizeof(returnedPoint)));
    CHECK(returnedPoint == pointBits);
    registers.r[0] = touchObject;
    CHECK(registry.invokeCallout(release->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(!memory.contains(touchObject, sizeof(std::uint32_t)));

    registers.r[0] = poolObject;
    CHECK(registry.invokeCallout(release->guestAddress, registers, memory, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    CHECK(!memory.contains(poolObject, sizeof(std::uint32_t)));

    const auto pushPool = registry.resolve("_objc_autoreleasePoolPush");
    const auto popPool = registry.resolve("_objc_autoreleasePoolPop");
    CHECK(pushPool.has_value() && popPool.has_value());
    CHECK(registry.invokeCallout(pushPool->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    const auto poolToken = registers.r[0];
    CHECK(poolToken != 0 && memory.contains(poolToken, sizeof(std::uint32_t)));
    registers.r[0] = poolToken;
    CHECK(registry.invokeCallout(popPool->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    CHECK(!memory.contains(poolToken, sizeof(std::uint32_t)));
}

void testAudioSessionInitializeAndSetActiveResolveAndReturnStatus() {
    MachOptions options;
    options.entryOffset = 0x400;
    options.includeDataSegment = true;
    options.includeDependency = true;
    options.includeIndirectFunctionPointer = true;
    options.indirectSymbolName = "_AudioSessionInitialize";
    options.includeIndirectStub = true;
    const auto bytes = makeMachO(options);

    ShimRegistry registry;
    radek::compat_runtime::audio::ShimAdapter audioShims;
    audioShims.registerBindings(registry);
    const auto binding = registry.resolve("_AudioSessionInitialize");
    CHECK(binding.has_value());
    CHECK(binding->library == "AudioToolbox");
    CHECK(binding->adapterName == "audio-session-initialize-state-only");
    const auto setActiveBinding = registry.resolve("_AudioSessionSetActive");
    CHECK(setActiveBinding.has_value());
    CHECK(setActiveBinding->library == "AudioToolbox");
    CHECK(setActiveBinding->adapterName == "audio-session-set-active-state-only");

    GuestAddressSpace memory;
    const auto loaded = MachOLoader().load(bytes, memory, registry);
    CHECK(loaded.status == "LOADED");
    CHECK(loaded.firstMissingImport == std::nullopt);
    CHECK(loaded.resolvedSymbols.size() == 1);
    CHECK(loaded.resolvedSymbols[0].fields.at("symbol").value == "_AudioSessionInitialize");
    CHECK(loaded.resolvedSymbols[0].fields.at("adapter").value ==
          "audio-session-initialize-state-only");

    CpuRegisterState registers;
    registers.r[0] = 0;
    registers.r[1] = 0;
    registers.r[2] = 0x1201; // opaque guest interruption-listener address
    registers.r[3] = 0x3456; // opaque guest client context
    std::string reason;
    registers.r[0] = 1;
    CHECK(registry.invokeCallout(setActiveBinding->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0x21696e69U); // kAudioSessionNotInitialized ('!ini')
    CHECK(!audioShims.activeState().has_value());

    registers.r[0] = 0;
    registers.r[1] = 0;
    registers.r[2] = 0x1201; // opaque guest interruption-listener address
    registers.r[3] = 0x3456; // opaque guest client context
    CHECK(registry.invokeCallout(binding->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0); // OSStatus noErr
    const auto state = audioShims.initialization();
    CHECK(state.has_value());
    CHECK(state->runLoop == 0 && state->runLoopMode == 0);
    CHECK(state->interruptionListener == 0x1201 && state->clientData == 0x3456);
    CHECK(audioShims.activeState().has_value() && !audioShims.activeState().value());

    registers.r[0] = 1;
    CHECK(registry.invokeCallout(setActiveBinding->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    CHECK(audioShims.activeState().has_value() && audioShims.activeState().value());
    registers.r[0] = 0;
    CHECK(registry.invokeCallout(setActiveBinding->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    CHECK(audioShims.activeState().has_value() && !audioShims.activeState().value());

    auto setActiveOptions = options;
    setActiveOptions.indirectSymbolName = "_AudioSessionSetActive";
    const auto setActiveBytes = makeMachO(setActiveOptions);
    ShimRegistry setActiveRegistry;
    radek::compat_runtime::audio::ShimAdapter setActiveAudioShims;
    setActiveAudioShims.registerBindings(setActiveRegistry);
    GuestAddressSpace setActiveMemory;
    const auto setActiveLoad = MachOLoader().load(setActiveBytes, setActiveMemory,
                                                   setActiveRegistry);
    CHECK(setActiveLoad.status == "LOADED");
    CHECK(setActiveLoad.firstMissingImport == std::nullopt);
    CHECK(setActiveLoad.resolvedSymbols.size() == 1);
    CHECK(setActiveLoad.resolvedSymbols[0].fields.at("symbol").value ==
          "_AudioSessionSetActive");
    CHECK(setActiveLoad.resolvedSymbols[0].fields.at("adapter").value ==
          "audio-session-set-active-state-only");

    auto cpu = createArm32CpuBackend();
#ifdef RADEK_TEST_REQUIRE_DYNARMIC
    CHECK(cpu->available());
#endif
    if (cpu->available()) {
        ShimRegistry runnerRegistry;
        radek::compat_runtime::audio::ShimAdapter runnerAudioShims;
        runnerAudioShims.registerBindings(runnerRegistry);
        GuestRunner runner(runnerRegistry, *cpu);
        const auto run = runner.runMainBinary(bytes, true);
        CHECK(run.fields.at("loader").fields.at("status").value == "LOADED");
        CHECK(run.fields.at("execution").fields.at("status").value == "RETURNED");
        CHECK(run.fields.at("execution").fields.at("registers").fields.at("r0").value == "0");
        CHECK(run.fields.at("execution").fields.at("entryPointReached").value == "true");

        ShimRegistry activeRunnerRegistry;
        radek::compat_runtime::audio::ShimAdapter activeRunnerAudioShims;
        activeRunnerAudioShims.registerBindings(activeRunnerRegistry);
        GuestRunner activeRunner(activeRunnerRegistry, *cpu);
        const auto activeRun = activeRunner.runMainBinary(setActiveBytes, true);
        CHECK(activeRun.fields.at("loader").fields.at("status").value == "LOADED");
        CHECK(activeRun.fields.at("execution").fields.at("status").value == "RETURNED");
        CHECK(activeRun.fields.at("execution").fields.at("registers").fields.at("r0").value ==
              std::to_string(0x21696e69U));
    }
}

void testFoundationSearchPathsReturnGuestNSStringArray() {
    ShimRegistry registry;
    radek::compat_runtime::objc::ShimAdapter objcShims;
    objcShims.registerBindings(registry);
    const auto searchPaths = registry.resolve("_NSSearchPathForDirectoriesInDomains");
    const auto homeDirectory = registry.resolve("_NSHomeDirectory");
    const auto temporaryDirectory = registry.resolve("_NSTemporaryDirectory");
    const auto messageSend = registry.resolve("_objc_msgSend");
    const auto release = registry.resolve("_objc_release");
    CHECK(searchPaths.has_value() && searchPaths->invoke);
    CHECK(homeDirectory.has_value() && homeDirectory->invoke);
    CHECK(temporaryDirectory.has_value() && temporaryDirectory->invoke);
    CHECK(homeDirectory->library == "Foundation");
    CHECK(temporaryDirectory->library == "Foundation");
    CHECK(homeDirectory->adapterName == "foundation-home-directory-app-sandbox-root");
    CHECK(temporaryDirectory->adapterName == "foundation-temporary-directory-app-sandbox");
    CHECK(searchPaths->library == "Foundation");
    CHECK(searchPaths->adapterName == "foundation-search-paths-virtual-user-domain");
    CHECK(messageSend.has_value() && messageSend->invokeTransfer);
    CHECK(release.has_value() && release->invoke);

    GuestAddressSpace memory;
    CpuRegisterState registers;
    std::string reason;
    registers.r[0] = 9; // NSDocumentDirectory
    registers.r[1] = 1; // NSUserDomainMask
    registers.r[2] = 1; // expand tilde
    CHECK(registry.invokeCallout(searchPaths->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    const auto directories = registers.r[0];
    CHECK(directories != 0);

    const auto selectorMemory = memory.mapAny(4096,
        MemoryPermission::Read | MemoryPermission::Write, "foundation-path-selectors");
    const std::string countName = "count";
    const std::string objectAtIndexName = "objectAtIndex:";
    const std::string utf8StringName = "UTF8String";
    const std::string lengthName = "length";
    const std::string copyName = "copy";
    CHECK(memory.write(selectorMemory, countName.c_str(), countName.size() + 1));
    CHECK(memory.write(selectorMemory + 16, objectAtIndexName.c_str(),
                       objectAtIndexName.size() + 1));
    CHECK(memory.write(selectorMemory + 48, utf8StringName.c_str(),
                       utf8StringName.size() + 1));
    CHECK(memory.write(selectorMemory + 64, lengthName.c_str(), lengthName.size() + 1));
    CHECK(memory.write(selectorMemory + 80, copyName.c_str(), copyName.size() + 1));

    registers = {};
    registers.r[0] = directories;
    registers.r[1] = selectorMemory;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 1);

    registers.r[0] = directories;
    registers.r[1] = selectorMemory + 16;
    registers.r[2] = 0;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    const auto directoryString = registers.r[0];
    CHECK(directoryString != 0);

    registers.r[0] = directoryString;
    registers.r[1] = selectorMemory + 48;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    std::array<char, 32> path{};
    CHECK(memory.read(registers.r[0], path.data(), path.size()));
    CHECK(std::string(path.data()) == "/Documents");

    std::vector<GuestAddress> foundationPathObjects;
    const auto assertFoundationPath = [&](const ShimBinding &binding,
                                          const std::string &expectedPath) {
        CpuRegisterState pathRegisters;
        std::string pathReason;
        CHECK(registry.invokeCallout(binding.guestAddress, pathRegisters, memory, pathReason) ==
              GuestCalloutResult::Returned);
        const auto pathObject = pathRegisters.r[0];
        CHECK(pathObject != 0);
        foundationPathObjects.push_back(pathObject);
        pathRegisters = {};
        pathRegisters.r[0] = pathObject;
        pathRegisters.r[1] = selectorMemory + 48;
        CHECK(registry.invokeCallout(messageSend->guestAddress, pathRegisters, memory, pathReason) ==
              GuestCalloutResult::Returned);
        std::array<char, 32> actualPath{};
        CHECK(memory.read(pathRegisters.r[0], actualPath.data(), actualPath.size()));
        CHECK(std::string(actualPath.data()) == expectedPath);
    };
    const auto pushPool = registry.resolve("_objc_autoreleasePoolPush");
    const auto popPool = registry.resolve("_objc_autoreleasePoolPop");
    CHECK(pushPool.has_value() && pushPool->invoke);
    CHECK(popPool.has_value() && popPool->invoke);
    CpuRegisterState poolRegisters;
    CHECK(registry.invokeCallout(pushPool->guestAddress, poolRegisters, memory, reason) ==
          GuestCalloutResult::Returned);
    const auto foundationPool = poolRegisters.r[0];
    CHECK(foundationPool != 0);

    assertFoundationPath(*homeDirectory, "/");
    assertFoundationPath(*temporaryDirectory, "/tmp");

    poolRegisters = {};
    poolRegisters.r[0] = foundationPool;
    CHECK(registry.invokeCallout(popPool->guestAddress, poolRegisters, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(poolRegisters.r[0] == 0);
    for (const auto pathObject : foundationPathObjects)
        CHECK(!memory.contains(pathObject, sizeof(std::uint32_t)));

    registers.r[0] = directoryString;
    registers.r[1] = selectorMemory + 64;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 10);

    registers.r[0] = directoryString;
    registers.r[1] = selectorMemory + 80;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == directoryString); // immutable NSString copy retains itself
    registers.r[0] = directoryString;
    CHECK(registry.invokeCallout(release->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(memory.contains(directoryString, sizeof(std::uint32_t)));

    registers.r[0] = directories;
    registers.r[1] = selectorMemory + 80;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    const auto copiedDirectories = registers.r[0];
    CHECK(copiedDirectories != directories);
    registers.r[0] = directories;
    CHECK(registry.invokeCallout(release->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(memory.contains(directoryString, sizeof(std::uint32_t)));
    registers.r[0] = copiedDirectories;
    CHECK(registry.invokeCallout(release->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(!memory.contains(directoryString, sizeof(std::uint32_t)));
    CHECK(!memory.contains(copiedDirectories, sizeof(std::uint32_t)));

    registers.r[0] = 0xffffffffU; // unsupported directory
    registers.r[1] = 1;
    registers.r[2] = 1;
    CHECK(registry.invokeCallout(searchPaths->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    const auto emptyDirectories = registers.r[0];
    registers.r[0] = emptyDirectories;
    registers.r[1] = selectorMemory;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    registers.r[0] = emptyDirectories;
    CHECK(registry.invokeCallout(release->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
}

void testSjLjContextRegistrationAndUnregistration() {
    ShimRegistry registry;
    SjLjUnwindAdapter unwind;
    unwind.registerBindings(registry);
    const auto registerContext = registry.resolve("__Unwind_SjLj_Register");
    const auto unregisterContext = registry.resolve("__Unwind_SjLj_Unregister");
    const auto resume = registry.resolve("__Unwind_SjLj_Resume");
    CHECK(registerContext.has_value() && registerContext->invoke);
    CHECK(unregisterContext.has_value() && unregisterContext->invoke);
    CHECK(resume.has_value() && resume->invokeException);
    CHECK(registerContext->adapterName == "sjlj-context-register");
    CHECK(unregisterContext->adapterName == "sjlj-context-unregister");
    CHECK(resume->adapterName == "sjlj-resume-unsupported-exception-boundary");

    GuestAddressSpace memory;
    const auto stack = memory.mapAny(4096,
        MemoryPermission::Read | MemoryPermission::Write, "sjlj-context-stack");
    const std::array<std::uint32_t, 8> blankContext{};
    const auto firstContext = stack + 0x20;
    const auto secondContext = stack + 0x80;
    const auto resetContext = stack + 0xe0;
    CHECK(memory.write(firstContext, blankContext.data(), sizeof(blankContext)));
    CHECK(memory.write(secondContext, blankContext.data(), sizeof(blankContext)));
    CHECK(memory.write(resetContext, blankContext.data(), sizeof(blankContext)));

    CpuRegisterState registers;
    std::string reason;
    registers.r[0] = firstContext;
    CHECK(registry.invokeCallout(registerContext->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    GuestAddress previous = 1;
    CHECK(memory.read(firstContext, &previous, sizeof(previous)));
    CHECK(previous == 0);
    reason.clear();
    CHECK(registry.invokeCallout(registerContext->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Failed);
    CHECK(reason.find("already registered") != std::string::npos);

    registers.r[0] = secondContext;
    reason.clear();
    CHECK(registry.invokeCallout(registerContext->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(memory.read(secondContext, &previous, sizeof(previous)));
    CHECK(previous == firstContext);
    registers.r[0] = firstContext;
    reason.clear();
    CHECK(registry.invokeCallout(unregisterContext->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Failed);
    CHECK(reason.find("LIFO") != std::string::npos);

    registers.r[0] = secondContext;
    reason.clear();
    CHECK(registry.invokeCallout(unregisterContext->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    registers.r[0] = firstContext;
    reason.clear();
    CHECK(registry.invokeCallout(unregisterContext->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);

    registers.r[0] = resetContext;
    reason.clear();
    CHECK(registry.invokeCallout(registerContext->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registry.initializeImage(memory, {}, reason));
    registers.r[0] = secondContext;
    reason.clear();
    CHECK(registry.invokeCallout(registerContext->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(memory.read(secondContext, &previous, sizeof(previous)));
    CHECK(previous == 0);

    registers = {};
    registers.r[0] = 0xdecafbad; // opaque _Unwind_Exception guest address
    reason.clear();
    CHECK(registry.invokeCallout(resume->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::ExceptionRaised);
    CHECK(registers.r[0] == 0xdecafbad);
    CHECK(reason.find("SJLJ resume, personality dispatch, and landing-pad transfer are unsupported") !=
          std::string::npos);
}

void testSjLjResumeBoundaryStopsGuestExecution() {
    MachOptions options;
    options.entryOffset = 0x400;
    options.includeDataSegment = true;
    options.includeDependency = true;
    options.includeIndirectFunctionPointer = true;
    options.indirectSymbolName = "__Unwind_SjLj_Resume";
    options.includeIndirectStub = true;
    const auto bytes = makeMachO(options);

    ShimRegistry registry;
    SjLjUnwindAdapter unwind;
    unwind.registerBindings(registry);
    auto cpu = createArm32CpuBackend();
#ifdef RADEK_TEST_REQUIRE_DYNARMIC
    CHECK(cpu->available());
#endif
    if (!cpu->available())
        return;

    GuestRunner runner(registry, *cpu);
    const auto run = runner.runMainBinary(bytes, true);
    CHECK(run.fields.at("loader").fields.at("status").value == "LOADED");
    CHECK(run.fields.at("resolvedSymbols").items.size() == 1);
    CHECK(run.fields.at("resolvedSymbols").items[0].fields.at("symbol").value ==
          "__Unwind_SjLj_Resume");
    CHECK(run.fields.at("execution").fields.at("status").value ==
          "GUEST_EXCEPTION_RAISED");
    CHECK(run.fields.at("reason").value.find("guest SJLJ resume") != std::string::npos);
}

void testObjectiveCMutationExceptionBoundaryIsReported() {
    MachOptions options;
    options.entryOffset = 0x400;
    options.includeDataSegment = true;
    options.includeDependency = true;
    options.includeIndirectFunctionPointer = true;
    options.indirectSymbolName = "_objc_enumerationMutation";
    options.includeIndirectStub = true;
    const auto bytes = makeMachO(options);

    ShimRegistry registry;
    radek::compat_runtime::objc::ShimAdapter objcShims;
    objcShims.registerBindings(registry);
    const auto binding = registry.resolve("_objc_enumerationMutation");
    CHECK(binding.has_value() && binding->invokeException);
    CHECK(binding->adapterName == "objc_enumerationMutation_exception-boundary");

    GuestAddressSpace memory;
    CpuRegisterState registers;
    registers.r[0] = 0x12345678;
    std::string reason;
    CHECK(registry.invokeCallout(binding->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::ExceptionRaised);
    CHECK(registers.r[0] == 0x12345678);
    CHECK(reason.find("fast-enumeration mutation") != std::string::npos);
    CHECK(reason.find("guest catch/unwind is unsupported") != std::string::npos);

    auto cpu = createArm32CpuBackend();
#ifdef RADEK_TEST_REQUIRE_DYNARMIC
    CHECK(cpu->available());
#endif
    if (cpu->available()) {
        ShimRegistry runnerRegistry;
        radek::compat_runtime::objc::ShimAdapter runnerObjcShims;
        runnerObjcShims.registerBindings(runnerRegistry);
        GuestRunner runner(runnerRegistry, *cpu);
        const auto run = runner.runMainBinary(bytes, true);
        CHECK(run.fields.at("loader").fields.at("status").value == "LOADED");
        CHECK(run.fields.at("resolvedSymbols").items.size() == 1);
        CHECK(run.fields.at("resolvedSymbols").items[0].fields.at("symbol").value ==
              "_objc_enumerationMutation");
        CHECK(run.fields.at("execution").fields.at("status").value ==
              "GUEST_EXCEPTION_RAISED");
        CHECK(run.fields.at("execution").fields.at("entryPointReached").value == "true");
        CHECK(run.fields.at("reason").value.find("guest catch/unwind is unsupported") !=
              std::string::npos);
    }
}

void testImageObjectiveCMetadataDispatchesGuestMethods() {
    MachOptions options;
    options.entryOffset = 0x400;
    options.includeDataSegment = true;
    options.includeDependency = true;
    options.includeExternalRelocation = true;
    options.externalSymbolName = "_OBJC_CLASS_$_NSObject";
    options.externalRelocationTarget = 0x2204;
    options.includeIndirectFunctionPointer = true;
    options.indirectSymbolName = "_objc_msgSend";
    options.includeIndirectStub = true;
    options.includeObjectiveCMetadata = true;
    const auto bytes = makeMachO(options);
    ShimRegistry registry;
    radek::compat_runtime::objc::ShimAdapter objcShims;
    objcShims.registerBindings(registry);
    GuestAddressSpace memory;
    const auto loaded = MachOLoader().load(bytes, memory, registry);
    if (loaded.status != "LOADED")
        throw std::runtime_error("Objective-C metadata fixture loader status " + loaded.status +
                                 ": " + loaded.error +
                                 (loaded.firstMissingImport ? " missing=" + *loaded.firstMissingImport : ""));
    CHECK(loaded.unresolvedSymbols.empty());
    const auto *demoClass = objcShims.runtime().findClass("DemoClass");
    CHECK(demoClass != nullptr);
    CHECK(demoClass->superclass == objcShims.runtime().findClass("NSObject"));
    CHECK(demoClass->instanceSizeBytes == 8);
    CHECK(demoClass->instanceSlots == 1);

    GuestAddress classAddress = 0;
    CHECK(memory.read(0x2100, &classAddress, sizeof(classAddress)));
    CHECK(classAddress == 0x2200);
    const auto selectorMemory = memory.mapAny(4096,
        MemoryPermission::Read | MemoryPermission::Write, "objc-image-test-selectors");
    const std::string allocName = "alloc";
    const std::string answerName = "answer";
    const std::string classAnswerName = "classAnswer";
    const std::string respondsToSelectorName = "respondsToSelector:";
    const std::string missingSelectorName = "missingSelector";
    const std::string isKindOfClassName = "isKindOfClass:";
    const std::string isMemberOfClassName = "isMemberOfClass:";
    const std::string conformsToProtocolName = "conformsToProtocol:";
    const std::string demoProtocolName = "DemoProtocol";
    const std::string absentProtocolName = "AbsentProtocol";
    CHECK(memory.write(selectorMemory, allocName.c_str(), allocName.size() + 1));
    CHECK(memory.write(selectorMemory + 16, answerName.c_str(), answerName.size() + 1));
    CHECK(memory.write(selectorMemory + 32, classAnswerName.c_str(), classAnswerName.size() + 1));
    CHECK(memory.write(selectorMemory + 48, respondsToSelectorName.c_str(),
                       respondsToSelectorName.size() + 1));
    CHECK(memory.write(selectorMemory + 80, missingSelectorName.c_str(),
                       missingSelectorName.size() + 1));
    CHECK(memory.write(selectorMemory + 96, isKindOfClassName.c_str(),
                       isKindOfClassName.size() + 1));
    CHECK(memory.write(selectorMemory + 112, isMemberOfClassName.c_str(),
                       isMemberOfClassName.size() + 1));
    CHECK(memory.write(selectorMemory + 144, conformsToProtocolName.c_str(),
                       conformsToProtocolName.size() + 1));
    CHECK(memory.write(selectorMemory + 176, demoProtocolName.c_str(),
                       demoProtocolName.size() + 1));
    CHECK(memory.write(selectorMemory + 192, absentProtocolName.c_str(),
                       absentProtocolName.size() + 1));
    const auto messageSend = registry.resolve("_objc_msgSend");
    CHECK(messageSend.has_value() && messageSend->invokeTransfer);

    CpuRegisterState registers;
    std::string reason;
    registers.r[0] = classAddress;
    registers.r[1] = selectorMemory;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    const auto instanceAddress = registers.r[0];
    CHECK(instanceAddress != 0 && instanceAddress != classAddress);
    std::uint32_t isa = 0;
    CHECK(memory.read(instanceAddress, &isa, sizeof(isa)));
    CHECK(isa == classAddress);

    registers.r[0] = instanceAddress;
    registers.r[1] = selectorMemory + 48;
    registers.r[2] = selectorMemory + 16;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 1);
    registers.r[0] = instanceAddress;
    registers.r[1] = selectorMemory + 48;
    registers.r[2] = selectorMemory + 80;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);

    const auto rootClassBinding = registry.resolve("_OBJC_CLASS_$_NSObject");
    CHECK(rootClassBinding.has_value() && rootClassBinding->resolveGuestAddress);
    GuestAddress rootClassAddress = 0;
    CHECK(rootClassBinding->resolveGuestAddress(memory, rootClassAddress, reason));
    registers.r[0] = instanceAddress;
    registers.r[1] = selectorMemory + 96;
    registers.r[2] = rootClassAddress;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 1);
    registers.r[0] = instanceAddress;
    registers.r[1] = selectorMemory + 112;
    registers.r[2] = classAddress;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 1);
    registers.r[0] = instanceAddress;
    registers.r[1] = selectorMemory + 96;
    registers.r[2] = classAddress;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 1);
    registers.r[0] = instanceAddress;
    registers.r[1] = selectorMemory + 112;
    registers.r[2] = rootClassAddress;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);

    const auto protocolGetter = registry.resolve("_objc_getProtocol");
    CHECK(protocolGetter.has_value() && protocolGetter->invoke);
    registers.r[0] = selectorMemory + 176;
    CHECK(registry.invokeCallout(protocolGetter->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    const auto protocolAddress = registers.r[0];
    CHECK(protocolAddress == 0x23c0);
    registers.r[0] = selectorMemory + 192;
    CHECK(registry.invokeCallout(protocolGetter->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    registers.r[0] = instanceAddress;
    registers.r[1] = selectorMemory + 144;
    registers.r[2] = protocolAddress;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 1);
    registers.r[0] = instanceAddress;
    registers.r[1] = selectorMemory + 144;
    registers.r[2] = 0;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);

    registers.r[0] = instanceAddress;
    registers.r[1] = selectorMemory + 16;
    registers.r[14] = 0x1418;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Transferred);
    CHECK(registers.r[15] == 0x1500);
    CHECK(registers.r[14] == 0x1418);

    registers.r[0] = classAddress;
    registers.r[1] = selectorMemory + 32;
    registers.r[14] = 0x1420;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Transferred);
    CHECK(registers.r[15] == 0x1510);
    CHECK(registers.r[14] == 0x1420);

    const auto propertySetter = registry.resolve("_objc_setProperty");
    const auto retainCallout = registry.resolve("_objc_retain");
    const auto releaseCallout = registry.resolve("_objc_release");
    CHECK(propertySetter.has_value() && propertySetter->invokeTransfer);
    CHECK(retainCallout.has_value() && retainCallout->invoke);
    CHECK(releaseCallout.has_value() && releaseCallout->invoke);
    registers.r[0] = classAddress;
    registers.r[1] = selectorMemory;
    CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    const auto copiedValue = registers.r[0];
    const auto propertyStack = memory.mapAny(4096,
        MemoryPermission::Read | MemoryPermission::Write, "objc-copy-property-stack");
    const auto originalStackPointer = propertyStack + 0x100;
    const auto continuationStackPointer = originalStackPointer - 8;
    const std::array<std::uint32_t, 2> copyFlags{{0, 1}};
    const std::uint32_t originalStackWord = 0xdeadbeef;
    CHECK(memory.write(originalStackPointer, copyFlags.data(), sizeof(copyFlags)));
    CHECK(memory.write(continuationStackPointer, &originalStackWord, sizeof(originalStackWord)));
    registers.r[0] = instanceAddress;
    registers.r[2] = sizeof(std::uint32_t);
    registers.r[3] = copiedValue;
    registers.r[13] = originalStackPointer;
    registers.r[14] = 0x1418;
    CHECK(registry.invokeCallout(propertySetter->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Transferred);
    CHECK(registers.r[15] == 0x1520);
    CHECK(registers.r[13] == continuationStackPointer);
    const auto copyContinuationAddress = registers.r[14];
    const auto copyContinuation = registry.resolve(
        "_radek_objc_setProperty_copy_continuation");
    CHECK(copyContinuation.has_value() && copyContinuation->invoke);
    CHECK(copyContinuationAddress == copyContinuation->guestAddress);

    // Host-only builds simulate a successful +copy IMP return; pinned-Dynarmic
    // additionally executes the guest IMP and continuation end to end below.
    registers.r[0] = copiedValue;
    CHECK(registry.invokeCallout(retainCallout->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    registers.r[13] = continuationStackPointer;
    registers.r[14] = copyContinuationAddress;
    CHECK(registry.invokeCallout(copyContinuationAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    CHECK(registers.r[13] == originalStackPointer);
    CHECK(registers.r[14] == 0x1418);
    std::uint32_t restoredStackWord = 0;
    CHECK(memory.read(continuationStackPointer, &restoredStackWord, sizeof(restoredStackWord)));
    CHECK(restoredStackWord == originalStackWord);
    std::uint32_t copiedPropertySlot = 0;
    CHECK(memory.read(instanceAddress + sizeof(std::uint32_t), &copiedPropertySlot,
                      sizeof(copiedPropertySlot)));
    CHECK(copiedPropertySlot == copiedValue);

    auto cpu = createArm32CpuBackend();
#ifdef RADEK_TEST_REQUIRE_DYNARMIC
    CHECK(cpu->available());
#endif
    if (cpu->available()) {
        const auto allocateDemoObject = [&]() {
            registers.r[0] = classAddress;
            registers.r[1] = selectorMemory;
            CHECK(registry.invokeCallout(messageSend->guestAddress, registers, memory, reason) ==
                  GuestCalloutResult::Returned);
            CHECK(registers.r[0] != 0 && registers.r[0] != classAddress);
            return registers.r[0];
        };
        const auto cpuPropertyReceiver = allocateDemoObject();
        const auto cpuPropertyValue = allocateDemoObject();
        const auto cpuStack = memory.mapAny(4096,
            MemoryPermission::Read | MemoryPermission::Write, "objc-copy-property-cpu-stack");
        const auto cpuStackPointer = cpuStack + 0x100;
        const std::array<std::uint32_t, 2> cpuCopyFlags{{0, 1}};
        CHECK(memory.write(cpuStackPointer, cpuCopyFlags.data(), sizeof(cpuCopyFlags)));
        const auto retainAddress = retainCallout->guestAddress;
        const auto propertySetterAddress = propertySetter->guestAddress;
        CHECK(memory.initialize(0x1534, &retainAddress, sizeof(retainAddress)));
        CHECK(memory.initialize(0x1730, &cpuPropertyReceiver, sizeof(cpuPropertyReceiver)));
        const std::uint32_t propertyOffset = sizeof(std::uint32_t);
        CHECK(memory.initialize(0x1734, &propertyOffset, sizeof(propertyOffset)));
        CHECK(memory.initialize(0x1738, &cpuPropertyValue, sizeof(cpuPropertyValue)));
        CHECK(memory.initialize(0x173c, &propertySetterAddress, sizeof(propertySetterAddress)));

        std::set<GuestAddress> calloutPages;
        for (const auto &binding : registry.snapshot()) {
            if (binding.resolveGuestAddress)
                continue;
            const auto page = binding.guestAddress & ~GuestAddress{0xfff};
            if (calloutPages.insert(page).second)
                memory.mapAt(page, 4096, MemoryPermission::Read | MemoryPermission::Execute,
                             "objc-copy-property-callouts");
        }
        auto callbacks = memory.callbacks();
        callbacks.invokeGuestCallout = [&registry, &memory](GuestAddress address,
                CpuRegisterState &guestRegisters, std::string &guestReason) {
            return registry.invokeCallout(address, guestRegisters, memory, guestReason);
        };
        GuestFunction copyPropertyFunction;
        copyPropertyFunction.entryPoint = 0x1700;
        copyPropertyFunction.instructionLimit = 1000;
        copyPropertyFunction.timeLimitMicros = 1000000;
        copyPropertyFunction.origin = "synthetic guest objc_setProperty copy IMP";
        PreparedGuestFunction preparedCopyProperty;
        std::string prepareReason;
        CHECK(cpu->prepareGuestFunction(copyPropertyFunction, callbacks,
                                        preparedCopyProperty, prepareReason));
        CpuRegisterState initialCopyRegisters;
        initialCopyRegisters.r[13] = cpuStackPointer + 16;
        const auto copyExecution = cpu->executeGuestFunction(preparedCopyProperty, callbacks,
                                                              initialCopyRegisters);
        if (copyExecution.status != CpuExecutionStatus::Returned)
            throw std::runtime_error("guest copy-property execution failed after " +
                std::to_string(copyExecution.instructions) + " instruction(s), pc=" +
                std::to_string(copyExecution.registers.r[15]) + ", lr=" +
                std::to_string(copyExecution.registers.r[14]) + ", sp=" +
                std::to_string(copyExecution.registers.r[13]) + ": " + copyExecution.message);
        CHECK(copyExecution.registers.r[0] == 42);
        std::uint32_t cpuPropertySlot = 0;
        CHECK(memory.read(cpuPropertyReceiver + sizeof(std::uint32_t), &cpuPropertySlot,
                          sizeof(cpuPropertySlot)));
        CHECK(cpuPropertySlot == cpuPropertyValue);

        const std::array<std::uint32_t, 2> nonCopyFlags{{0, 0}};
        CHECK(memory.write(cpuStackPointer, nonCopyFlags.data(), sizeof(nonCopyFlags)));
        registers.r[0] = cpuPropertyReceiver;
        registers.r[2] = propertyOffset;
        registers.r[3] = 0;
        registers.r[13] = cpuStackPointer;
        CHECK(registry.invokeCallout(propertySetterAddress, registers, memory, reason) ==
              GuestCalloutResult::Returned);
        registers.r[0] = cpuPropertyValue;
        CHECK(registry.invokeCallout(releaseCallout->guestAddress, registers, memory, reason) ==
              GuestCalloutResult::Returned);
        CHECK(!memory.contains(cpuPropertyValue, sizeof(std::uint32_t)));
        registers.r[0] = cpuPropertyReceiver;
        CHECK(registry.invokeCallout(releaseCallout->guestAddress, registers, memory, reason) ==
              GuestCalloutResult::Returned);
        CHECK(!memory.contains(cpuPropertyReceiver, sizeof(std::uint32_t)));

        ShimRegistry runnerShims;
        radek::compat_runtime::objc::ShimAdapter runnerObjcShims;
        runnerObjcShims.registerBindings(runnerShims);
        GuestRunner runner(runnerShims, *cpu);
        const auto run = runner.runMainBinary(bytes, true);
        CHECK(run.fields.at("loader").fields.at("status").value == "LOADED");
        CHECK(run.fields.at("execution").fields.at("status").value == "RETURNED");
        CHECK(run.fields.at("execution").fields.at("registers").fields.at("r0").value == "42");
    }

    const std::array<std::uint32_t, 2> nonCopyFlags{{0, 0}};
    CHECK(memory.write(originalStackPointer, nonCopyFlags.data(), sizeof(nonCopyFlags)));
    registers.r[0] = instanceAddress;
    registers.r[2] = sizeof(std::uint32_t);
    registers.r[3] = 0;
    registers.r[13] = originalStackPointer;
    CHECK(registry.invokeCallout(propertySetter->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    registers.r[0] = copiedValue;
    CHECK(registry.invokeCallout(releaseCallout->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(!memory.contains(copiedValue, sizeof(std::uint32_t)));
    registers.r[0] = instanceAddress;
    CHECK(registry.invokeCallout(releaseCallout->guestAddress, registers, memory, reason) ==
          GuestCalloutResult::Returned);
    CHECK(!memory.contains(instanceAddress, sizeof(std::uint32_t)));
}

void testMachOLoadAndDyldReports() {
    MachOptions options;
    options.includeDataSegment = true;
    options.includeDependency = true;
    options.bindStream = {0x11, 0x40, '_', 'm', 'i', 's', 's', 'i', 'n', 'g', 0,
                          0x71, 0x00, 0x90, 0x00};
    options.rebaseStream = {0x11, 0x21, 0x04, 0x51, 0x00};
    auto bytes = makeMachO(options);
    ShimRegistry registry;
    registry.registerBinding(testBinding("_missing", 0xf0001234));
    GuestAddressSpace memory;
    const auto report = MachOLoader().load(bytes, memory, registry, 0x1000);
    CHECK(report.status == "LOADED");
    CHECK(report.imageMapped());
    CHECK(report.entryPoint == 0x2100);
    CHECK(report.resolvedSymbols.size() == 1);
    CHECK(report.unresolvedSymbols.empty());
    CHECK(report.rebasesApplied == 1);
    CHECK(report.resolvedSymbols[0].fields.at("symbol").value == "_missing");
    CHECK(report.resolvedSymbols[0].fields.at("dylibOrdinal").value == "1");
    std::uint32_t bound = 0;
    CHECK(memory.read(0x3000, &bound, sizeof(bound)));
    CHECK(bound == 0xf0001234);
    std::uint32_t rebased = 0;
    CHECK(memory.read(0x3004, &rebased, sizeof(rebased)));
    CHECK(rebased == 0x2120);

    GuestAddressSpace unresolvedMemory;
    const auto unresolved = MachOLoader().load(bytes, unresolvedMemory, ShimRegistry{});
    CHECK(unresolved.status == "BLOCKED_UNRESOLVED_IMPORTS");
    CHECK(unresolved.firstMissingImport == "_missing");
    CHECK(unresolved.resolvedSymbols.empty());
    CHECK(unresolved.unresolvedSymbols.size() == 1);

    const auto json = unresolved.toJson();
    CHECK(json.fields.at("firstMissingImport").value == "_missing");
    CHECK(json.fields.at("unresolvedSymbols").items.size() == 1);

    GuestAddressSpace encryptedMemory;
    MachOptions encryptedOptions;
    encryptedOptions.encrypted = true;
    const auto encrypted = MachOLoader().load(makeMachO(encryptedOptions), encryptedMemory,
                                               ShimRegistry{});
    CHECK(encrypted.status == "BLOCKED_ENCRYPTED");
    CHECK(!encrypted.imageMapped());
    CHECK(encrypted.error.find("FairPlay") != std::string::npos);

    MachOptions unreadableRebaseOptions;
    unreadableRebaseOptions.includeDataSegment = true;
    unreadableRebaseOptions.dataProtection = 2;
    unreadableRebaseOptions.rebaseStream = {0x11, 0x21, 0x04, 0x51, 0x00};
    GuestAddressSpace unreadableMemory;
    const auto unreadableRebase = MachOLoader().load(makeMachO(unreadableRebaseOptions),
                                                      unreadableMemory, ShimRegistry{});
    CHECK(unreadableRebase.status == "BLOCKED");
    CHECK(unreadableRebase.error.find("could not read the dyld rebase pointer") != std::string::npos);
    CHECK(unreadableRebase.rebasesApplied == 0);
}

void testNlistFatAndThreadState() {
    MachOptions mainBeforeSegmentsOptions;
    mainBeforeSegmentsOptions.mainBeforeSegments = true;
    GuestAddressSpace reorderedMemory;
    const auto reordered = MachOLoader().load(makeMachO(mainBeforeSegmentsOptions),
                                               reorderedMemory, ShimRegistry{});
    CHECK(reordered.status == "LOADED");
    CHECK(reordered.entryPoint == 0x1100);
    CHECK(reordered.entryPointSource == "LC_MAIN");

    MachOptions nlistOptions;
    nlistOptions.includeUnboundSymbol = true;
    const auto nlistBytes = makeMachO(nlistOptions);
    GuestAddressSpace nlistMemory;
    const auto nlist = MachOLoader().load(nlistBytes, nlistMemory, ShimRegistry{});
    CHECK(nlist.status == "BLOCKED_UNRESOLVED_IMPORTS");
    CHECK(nlist.firstMissingImport == "_nlistOnly");
    CHECK(nlist.unresolvedSymbols.size() == 1);

    MachOptions olderOptions;
    olderOptions.cpuSubtype = 6;
    olderOptions.entryOffset = 0x100;
    MachOptions newerOptions;
    newerOptions.cpuSubtype = 9;
    newerOptions.entryOffset = 0x200;
    const auto older = makeMachO(olderOptions);
    const auto newer = makeMachO(newerOptions);
    GuestAddressSpace fatMemory;
    const auto fat = MachOLoader().load(makeFatMachO(older, newer), fatMemory, ShimRegistry{});
    if (fat.status != "LOADED")
        throw std::runtime_error("FAT fixture failed: " + fat.status + " / " + fat.error);

    if (fat.entryPoint != 0x1200)
        throw std::runtime_error("FAT selected unexpected entry point: " + std::to_string(fat.entryPoint));

    MachOptions threadOptions;
    threadOptions.useUnixThread = true;
    GuestAddressSpace threadMemory;
    const auto thread = MachOLoader().load(makeMachO(threadOptions), threadMemory, ShimRegistry{}, 0x1000);
    if (thread.status != "LOADED")
        throw std::runtime_error("thread fixture failed: " + thread.status + " / " + thread.error);
    CHECK(thread.thumb);
    CHECK(thread.entryPointSource == "LC_UNIXTHREAD");
    CHECK(thread.entryPoint == 0x2101);
    CHECK(thread.initialRegisters.r[15] == 0x2101);
}

class TimeLimitCpuBackend final : public CpuBackend {
  public:
    const char *name() const noexcept override { return "time-limit-test-backend"; }
    bool available() const noexcept override { return true; }

    bool prepareGuestFunction(const GuestFunction &function, const GuestMemoryCallbacks &,
                              PreparedGuestFunction &prepared,
                              std::string &reason) const override {
        prepared.entryPoint = function.entryPoint;
        prepared.thumb = function.thumb;
        prepared.backendName = name();
        reason.clear();
        return true;
    }

    CpuExecutionResult executeGuestFunction(const PreparedGuestFunction &,
                                             const GuestMemoryCallbacks &,
                                             const CpuRegisterState &registers) const override {
        CpuExecutionResult result;
        result.status = CpuExecutionStatus::TimeLimit;
        result.registers = registers;
        result.instructions = 1;
        result.started = true;
        result.message = "guest function reached its time limit.";
        return result;
    }
};

void testLegacyIndirectSymbolPointerBindsCallout() {
    MachOptions options;
    options.includeDataSegment = true;
    options.includeDependency = true;
    options.includeIndirectFunctionPointer = true;
    const auto bytes = makeMachO(options);

    ShimRegistry registry;
    registry.registerBinding(testBinding("_indirectFunction", 0xf0002340));
    GuestAddressSpace memory;
    const auto loaded = MachOLoader().load(bytes, memory, registry);
    CHECK(loaded.status == "LOADED");
    CHECK(loaded.resolvedSymbols.size() == 1);
    CHECK(loaded.resolvedSymbols[0].fields.at("source").value == "indirect-symbol");
    std::uint32_t callout = 0;
    CHECK(memory.read(0x2000, &callout, sizeof(callout)));
    CHECK(callout == 0xf0002340);

    GuestAddressSpace unresolvedMemory;
    const auto unresolved = MachOLoader().load(bytes, unresolvedMemory, ShimRegistry{});
    CHECK(unresolved.status == "BLOCKED_UNRESOLVED_IMPORTS");
    CHECK(unresolved.firstMissingImport == "_indirectFunction");
    CHECK(unresolved.unresolvedSymbols.size() == 1);
    CHECK(unresolved.unresolvedSymbols[0].fields.at("source").value == "indirect-symbol");
}

void testLegacyExternalRelocationBindsGuestData() {
    MachOptions options;
    options.includeDataSegment = true;
    options.includeDependency = true;
    options.includeExternalRelocation = true;
    const auto bytes = makeMachO(options);

    ShimRegistry registry;
    ShimBinding dataBinding{"_dataRef", "libSystem.B.dylib", "test-guest-data", 0, {}, {},
        [](GuestAddressSpace &memory, GuestAddress &address, std::string &) {
            address = memory.mapAny(4096, MemoryPermission::Read | MemoryPermission::Write,
                                    "test-relocated-data");
            return true;
        }, {}};
    registry.registerBinding(std::move(dataBinding));
    GuestAddressSpace memory;
    const auto loaded = MachOLoader().load(bytes, memory, registry);
    CHECK(loaded.status == "LOADED");
    CHECK(loaded.resolvedSymbols.size() == 1);
    CHECK(loaded.resolvedSymbols[0].fields.at("source").value == "external-relocation");
    std::uint32_t resolvedData = 0;
    CHECK(memory.read(0x2000, &resolvedData, sizeof(resolvedData)));
    CHECK(resolvedData != 0 && resolvedData < 0xf0000000);

    GuestAddressSpace unresolvedMemory;
    const auto unresolved = MachOLoader().load(bytes, unresolvedMemory, ShimRegistry{});
    CHECK(unresolved.status == "BLOCKED_UNRESOLVED_IMPORTS");
    CHECK(unresolved.firstMissingImport == "_dataRef");
    CHECK(unresolved.unresolvedSymbols.size() == 1);
    CHECK(unresolved.unresolvedSymbols[0].fields.at("source").value == "external-relocation");
    CHECK(unresolved.unresolvedSymbols[0].fields.at("dylibOrdinal").value == "1");
}

void testRuntimeLinkReportCountsOnlyInstalledProviders() {
    MachOptions options;
    options.includeDataSegment = true;
    options.includeDependency = true;
    options.includeIndirectFunctionPointer = true;
    options.indirectSymbolName = "_runtimeLinked";
    const auto bytes = makeMachO(options);

    ShimRegistry shims;
    shims.registerBinding(testBinding("_runtimeLinked", 0xf0002340));
    GuestAddressSpace memory;
    const auto loaded = MachOLoader().load(bytes, memory, shims);
    CHECK(loaded.status == "LOADED");
    const auto linked = loaded.toJson().fields.at("runtimeLinking");
    CHECK(linked.fields.at("status").value == "COMPLETE");
    CHECK(linked.fields.at("guestImageImportSlotsRelinked").value == "1");
    CHECK(linked.fields.at("distinctResolvedImportSymbols").value == "1");
    CHECK(linked.fields.at("translatedGuestCodeCallsitesRewritten").value == "0");
    CHECK(linked.fields.at("providerLinkMap").items.size() == 1);
    CHECK(linked.fields.at("providerLinkMap").items[0].fields.at("symbol").value ==
          "_runtimeLinked");
    CHECK(linked.fields.at("providerLinkMap").items[0].fields.at("status").value ==
          "RELINKED_TO_GUEST_PROVIDER");
    CHECK(linked.fields.at("providerLinkMap").items[0].fields.at("adapter").value ==
          "host-tested-test-adapter");

    GuestAddressSpace trappedMemory;
    ShimRegistry emptyRegistry;
    TrapShimAdapter traps;
    const auto trapped = MachOLoader().loadWithTraps(bytes, trappedMemory, emptyRegistry, traps);
    CHECK(trapped.status == "LOADED_WITH_TRAPS");
    const auto trappedLinking = trapped.toJson().fields.at("runtimeLinking");
    CHECK(trappedLinking.fields.at("status").value ==
          "BOUND_WITH_RUNTIME_TRAPS_OR_NLIST_ONLY");
    CHECK(trappedLinking.fields.at("guestImageImportSlotsRelinked").value == "0");
    CHECK(trappedLinking.fields.at("guestImageImportSlotsTrapped").value == "1");
    CHECK(trappedLinking.fields.at("providerLinkMap").items[0].fields.at("status").value ==
          "BOUND_TO_ABORT_ON_CALL_TRAP");
    CHECK(trappedLinking.fields.at("providerLinkMap").items[0].fields.at("adapter").kind ==
          radek::Json::Null);
}

void testRunnerReportsFirstMissingImport() {
    MachOptions options;
    options.includeDataSegment = true;
    options.includeDependency = true;
    options.bindStream = {0x11, 0x40, '_', 'b', 'l', 'o', 'c', 'k', 'e', 'r', 0,
                          0x71, 0x00, 0x90, 0x00};
    const auto bytes = makeMachO(options);
    ShimRegistry shims;
    auto cpu = createArm32CpuBackend();
    const auto report = GuestRunner(shims, *cpu).runMainBinary(bytes, true);
    CHECK(report.fields.at("status").value == "not_runnable");
    CHECK(report.fields.at("firstMissingImport").value == "_blocker");
    CHECK(report.fields.at("inputEmbeddedInRuntimeArtifact").value == "false");
    CHECK(report.fields.at("reportArtifactName").value == "compat-runtime-v1-report.json");
    CHECK(report.fields.at("runtimeLibrary").value == "libcompat_runtime_v1.so");
    CHECK(report.fields.at("cpu").fields.at("status").value == "BLOCKED_BY_UNRESOLVED_IMPORT");
    CHECK(report.fields.at("runtimeLinking").fields.at("status").value == "PARTIAL_UNRESOLVED");
    CHECK(report.fields.at("runtimeLinking").fields.at("translatedGuestCodeCallsitesRewritten").value == "0");

    const auto unauthorized = GuestRunner(shims, *cpu).runMainBinary(bytes, false);
    CHECK(unauthorized.fields.at("status").value == "not_runnable");
    CHECK(unauthorized.fields.at("authorizationConfirmed").value == "false");
    CHECK(unauthorized.fields.at("firstMissingImport").kind == radek::Json::Null);
}

void testRunnerReportsTimeLimitWithoutClaimingCompatibility() {
    ShimRegistry shims;
    TimeLimitCpuBackend cpu;
    const auto report = GuestRunner(shims, cpu).runMainBinary(makeMachO(), true);
    CHECK(report.fields.at("status").value == "not_runnable");
    CHECK(report.fields.at("cpu").fields.at("status").value == "TIME_LIMIT");
    CHECK(report.fields.at("execution").fields.at("status").value == "TIME_LIMIT");
    CHECK(report.fields.at("execution").fields.at("entryPointReached").value == "true");
}

void testGuestFunctionReachesImportedCalloutAndReturns() {
    MachOptions options;
    options.includeDataSegment = true;
    options.includeDependency = true;
    options.includeIndirectFunctionPointer = true;
    options.includeIndirectStub = true;
    options.entryOffset = 0x400;
    const auto bytes = makeMachO(options);

    ShimRegistry registry;
    int callouts = 0;
    auto binding = testBinding("_indirectFunction", 0xf0002340);
    binding.invoke = [&callouts](CpuRegisterState &registers, GuestAddressSpace &, std::string &) {
        ++callouts;
        registers.r[0] = 42;
        return true;
    };
    registry.registerBinding(std::move(binding));
    const auto cpu = createArm32CpuBackend();
    if (!cpu->available())
        return;

    const auto report = GuestRunner(registry, *cpu).runMainBinary(bytes, true);
    if (report.fields.at("loader").fields.at("status").value != "LOADED")
        throw std::runtime_error("legacy indirect callout image did not load: " + report.dump());
    CHECK(report.fields.at("loader").fields.at("resolvedSymbolCount").value == "1");
    CHECK(report.fields.at("loader").fields.at("entryPointSource").value == "LC_MAIN");
    if (report.fields.at("cpu").fields.at("status").value != "RETURNED")
        throw std::runtime_error("legacy indirect callout did not return: " + report.dump());
    CHECK(report.fields.at("cpu").fields.at("status").value == "RETURNED");
    CHECK(report.fields.at("execution").fields.at("status").value == "RETURNED");
    CHECK(report.fields.at("execution").fields.at("entryPointReached").value == "true");
    CHECK(report.fields.at("functionOrigin").fields.at("sourceImage").value == "main-executable");
    CHECK(report.fields.at("functionOrigin").fields.at("loadCommand").value == "LC_MAIN");
    CHECK(report.fields.at("functionOrigin").fields.at("guestAddress").value == "5120");
    CHECK(report.fields.at("functionOrigin").fields.at("function").value ==
          "main-executable/LC_MAIN");
    CHECK(report.fields.at("functionOrigin").fields.at("executionAttempted").value == "true");
    CHECK(report.fields.at("execution").fields.at("registers").fields.at("r0").value == "42");
    CHECK(callouts == 1);
    // A returned fixture entry point is still not an app/menu/gameplay proof.
    CHECK(report.fields.at("status").value == "not_runnable");
}

void testCpuBackendBoundary() {
    GuestAddressSpace memory;
    memory.mapAt(0x1000, 4096,
                 MemoryPermission::Read | MemoryPermission::Execute, "guest-code");
    const std::array<std::uint8_t, 4> armReturn{{0x1e, 0xff, 0x2f, 0xe1}};
    CHECK(memory.initialize(0x1000, armReturn.data(), armReturn.size()));

    auto cpu = createArm32CpuBackend();
    GuestFunction function;
    function.entryPoint = 0x1000;
    PreparedGuestFunction prepared;
    std::string reason;
    const auto callbacks = memory.callbacks();
    if (!cpu->available()) {
        CHECK(!cpu->prepareGuestFunction(function, callbacks, prepared, reason));
        CHECK(reason.find("not linked") != std::string::npos);
        const auto result = cpu->executeGuestFunction(prepared, callbacks, CpuRegisterState{});
        CHECK(result.status == CpuExecutionStatus::BackendUnavailable);
        CHECK(!result.started);
        return;
    }

    CHECK(cpu->prepareGuestFunction(function, callbacks, prepared, reason));
    CpuRegisterState initial;
    initial.r[13] = 0x2000;
    const auto result = cpu->executeGuestFunction(prepared, callbacks, initial);
    if (result.status != CpuExecutionStatus::Returned)
        throw std::runtime_error("ARM fixture failed with status " +
                                 std::to_string(static_cast<int>(result.status)) +
                                 " after " + std::to_string(result.instructions) +
                                 " instructions: " + result.message);
    CHECK(result.started);
    CHECK(result.instructions == 1);

    memory.initialize(0x1000, "\x70\x47", 2);
    function.entryPoint = 0x1001;
    function.thumb = true;
    CHECK(cpu->prepareGuestFunction(function, callbacks, prepared, reason));
    const auto thumbResult = cpu->executeGuestFunction(prepared, callbacks, initial);
    if (thumbResult.status != CpuExecutionStatus::Returned)
        throw std::runtime_error("Thumb fixture failed with status " +
                                 std::to_string(static_cast<int>(thumbResult.status)) +
                                 ": " + thumbResult.message);

    const std::array<std::uint8_t, 20> armCallout{{
        0x04, 0xe0, 0x2d, 0xe5, // push {lr}
        0x30, 0xff, 0x2f, 0xe1, // blx r0
        0x00, 0x00, 0x90, 0xe5, // ldr r0, [r0] (from memory mapped by the callout)
        0x04, 0xe0, 0x9d, 0xe4, // ldr lr, [sp], #4
        0x1e, 0xff, 0x2f, 0xe1, // bx lr
    }};
    CHECK(memory.initialize(0x1000, armCallout.data(), armCallout.size()));
    memory.mapAt(0x2000, 4096,
                 MemoryPermission::Read | MemoryPermission::Write, "guest-stack");
    memory.mapAt(0xf0001000, 4096,
                 MemoryPermission::Read | MemoryPermission::Execute, "shim-callout");
    int callouts = 0;
    auto calloutCallbacks = memory.callbacks();
    calloutCallbacks.invokeGuestCallout = [&callouts, &memory](GuestAddress address,
                                                                CpuRegisterState &state,
                                                                std::string &calloutReason) {
        if (address != 0xf0001000)
            return GuestCalloutResult::NotRegistered;
        ++callouts;
        try {
            memory.mapAt(0x4000, 4096,
                         MemoryPermission::Read | MemoryPermission::Write,
                         "callout-created-memory");
        } catch (const std::exception &error) {
            calloutReason = error.what();
            return GuestCalloutResult::Failed;
        }
        const std::uint32_t result = 42;
        if (!memory.write(0x4000, &result, sizeof(result))) {
            calloutReason = "could not initialize callout-created guest memory.";
            return GuestCalloutResult::Failed;
        }
        state.r[0] = 0x4000;
        return GuestCalloutResult::Returned;
    };
    function.entryPoint = 0x1000;
    function.thumb = false;
    CHECK(cpu->prepareGuestFunction(function, calloutCallbacks, prepared, reason));
    initial.r[0] = 0xf0001000;
    initial.r[13] = 0x3000;
    const auto calloutResult = cpu->executeGuestFunction(prepared, calloutCallbacks, initial);
    if (calloutResult.status != CpuExecutionStatus::Returned)
        throw std::runtime_error("shim callout fixture failed with status " +
                                 std::to_string(static_cast<int>(calloutResult.status)) +
                                 " after " + std::to_string(calloutResult.instructions) +
                                 " instructions: " + calloutResult.message +
                                 "; adapter calls=" + std::to_string(callouts) +
                                 "; pc=" + std::to_string(calloutResult.registers.r[15]) +
                                 "; lr=" + std::to_string(calloutResult.registers.r[14]) +
                                 "; r0=" + std::to_string(calloutResult.registers.r[0]));
    CHECK(calloutResult.registers.r[0] == 42);
    CHECK(callouts == 1);
}
} // namespace

int main() {
    testGuestMemoryAndHeap();
    testShimRegistry();
    testGuestPthreadCreateTransfersAndJoins();
    testSjLjContextRegistrationAndUnregistration();
    testSjLjResumeBoundaryStopsGuestExecution();
    testObjectiveCShimsResolveThroughLoaderAndReturn();
    testAudioSessionInitializeAndSetActiveResolveAndReturnStatus();
    testFoundationSearchPathsReturnGuestNSStringArray();
    testObjectiveCMutationExceptionBoundaryIsReported();
    testImageObjectiveCMetadataDispatchesGuestMethods();
    testMachOLoadAndDyldReports();
    testNlistFatAndThreadState();
    testLegacyIndirectSymbolPointerBindsCallout();
    testLegacyExternalRelocationBindsGuestData();
    testRuntimeLinkReportCountsOnlyInstalledProviders();
    testRunnerReportsFirstMissingImport();
    testRunnerReportsTimeLimitWithoutClaimingCompatibility();
    testGuestFunctionReachesImportedCalloutAndReturns();
    testCpuBackendBoundary();
}
