#include "compat_runtime/runner.hpp"

#include "compat_runtime/runtime_contract.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
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
    CHECK(name.size() < width);
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
    bool useUnixThread = false;
    bool mainBeforeSegments = false;
};

std::vector<std::uint8_t> makeMachO(const MachOptions &options = {}) {
    constexpr std::size_t headerSize = 28;
    constexpr std::size_t dataOffset = 0x1000;
    constexpr std::size_t rebaseOffset = 0x2000;
    constexpr std::size_t bindOffset = 0x2040;
    constexpr std::size_t symbolOffset = 0x2100;
    constexpr std::size_t stringOffset = symbolOffset + 12;
    constexpr std::size_t stringSize = 12;

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
    commands.push_back(segment("__TEXT", 0x1000, 0x1000, 0x0, 0x1000, 5));
    if (options.includeDataSegment)
        commands.push_back(segment("__DATA", 0x2000, 0x1000, dataOffset, 0x1000,
                                   options.dataProtection));

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
    if (options.includeUnboundSymbol) {
        std::vector<std::uint8_t> command(24, 0);
        putU32(command, 0, 0x2);
        putU32(command, 4, 24);
        putU32(command, 8, static_cast<std::uint32_t>(symbolOffset));
        putU32(command, 12, 1);
        putU32(command, 16, static_cast<std::uint32_t>(stringOffset));
        putU32(command, 20, static_cast<std::uint32_t>(stringSize));
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
    if (options.includeUnboundSymbol)
        fileSize = std::max(fileSize, stringOffset + stringSize);
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
    if (options.includeUnboundSymbol) {
        putU32(bytes, symbolOffset, 1);
        bytes[symbolOffset + 4] = 0x01; // N_UNDF | N_EXT
        bytes[symbolOffset + 6] = 0x40; // weak reference flag
        std::memcpy(bytes.data() + stringOffset, "\0_nlistOnly\0", stringSize);
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

    const std::array<std::uint8_t, 16> armCallout{{
        0x04, 0xe0, 0x2d, 0xe5, // push {lr}
        0x30, 0xff, 0x2f, 0xe1, // blx r0
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
    calloutCallbacks.invokeGuestCallout = [&callouts](GuestAddress address,
                                                       CpuRegisterState &state,
                                                       std::string &) {
        if (address != 0xf0001000)
            return GuestCalloutResult::NotRegistered;
        ++callouts;
        state.r[0] = 42;
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
    testMachOLoadAndDyldReports();
    testNlistFatAndThreadState();
    testRunnerReportsFirstMissingImport();
    testRunnerReportsTimeLimitWithoutClaimingCompatibility();
    testCpuBackendBoundary();
}
