// Boot-attempt trap coverage: unimplemented imports bind to abort-on-call
// traps, real guest instructions run, and the first used missing import stops
// execution with its name in the report. Loader assertions run everywhere;
// execution assertions run only when the Dynarmic ARM32 backend is linked.
#include "compat_runtime/runner.hpp"
#include "compat_runtime/cpu.hpp"
#include "compat_runtime/guest_memory.hpp"
#include "compat_runtime/macho_loader.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/trap_shims.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
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

void putName(std::vector<std::uint8_t> &bytes, std::size_t offset, const std::string &name,
             std::size_t width) {
    CHECK(name.size() <= width);
    std::memcpy(bytes.data() + offset, name.data(), name.size());
}

// Minimal MH_EXECUTE: __TEXT(r-x) with a 5-word ARM entry, __DATA(rw) with
// one bind slot for _trapped_call, LC_MAIN, one dylib, one bind stream.
std::vector<std::uint8_t> makeTrapMachO() {
    constexpr std::size_t headerSize = 28;
    constexpr std::uint32_t entryFileOffset = 0x400;
    constexpr std::uint32_t bindOffset = 0x800;
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
        putU32(command, 40, 7);
        putU32(command, 44, protection);
        return command;
    };
    commands.push_back(segment("__TEXT", 0x1000, 0x0, 5));
    commands.push_back(segment("__DATA", 0x2000, 0x1000, 3));

    std::vector<std::uint8_t> mainCommand(24, 0);
    putU32(mainCommand, 0, 0x80000028);
    putU32(mainCommand, 4, 24);
    putU32(mainCommand, 8, entryFileOffset);
    commands.push_back(std::move(mainCommand));

    const std::string dylib = "libSystem.B.dylib";
    const auto dylibSize = (24 + dylib.size() + 1 + 3) & ~std::size_t{3};
    std::vector<std::uint8_t> dylibCommand(dylibSize, 0);
    putU32(dylibCommand, 0, 0xc);
    putU32(dylibCommand, 4, static_cast<std::uint32_t>(dylibSize));
    putU32(dylibCommand, 8, 24);
    std::memcpy(dylibCommand.data() + 24, dylib.c_str(), dylib.size() + 1);
    commands.push_back(std::move(dylibCommand));

    // SET_DYLIB_ORDINAL_IMM(1); SET_SYMBOL(_trapped_call); SET_TYPE(POINTER);
    // SET_SEGMENT_AND_OFFSET(__DATA, 0); DO_BIND; DONE.
    std::vector<std::uint8_t> bindStream;
    bindStream.push_back(0x11);
    bindStream.push_back(0x40);
    const std::string symbol = "_trapped_call";
    bindStream.insert(bindStream.end(), symbol.begin(), symbol.end());
    bindStream.push_back(0x00);
    bindStream.push_back(0x51);
    bindStream.push_back(0x71);
    bindStream.push_back(0x00);
    bindStream.push_back(0x90);
    bindStream.push_back(0x00);

    std::vector<std::uint8_t> dyld(48, 0);
    putU32(dyld, 0, 0x80000022);
    putU32(dyld, 4, 48);
    putU32(dyld, 16, bindOffset);
    putU32(dyld, 20, static_cast<std::uint32_t>(bindStream.size()));
    commands.push_back(std::move(dyld));

    std::uint32_t commandBytes = 0;
    for (const auto &command : commands)
        commandBytes += static_cast<std::uint32_t>(command.size());
    std::vector<std::uint8_t> bytes(0x2000, 0);
    putU32(bytes, 0, 0xfeedface);
    putU32(bytes, 4, 12);
    putU32(bytes, 8, 9);
    putU32(bytes, 12, 2);
    putU32(bytes, 16, static_cast<std::uint32_t>(commands.size()));
    putU32(bytes, 20, commandBytes);
    putU32(bytes, 24, 0);
    std::size_t cursor = headerSize;
    for (const auto &command : commands) {
        std::memcpy(bytes.data() + cursor, command.data(), command.size());
        cursor += command.size();
    }
    std::memcpy(bytes.data() + bindOffset, bindStream.data(), bindStream.size());

    // Entry at vm 0x1400: r0 = 3, then a classic two-stage stub load into pc.
    putU32(bytes, entryFileOffset + 0x0, 0xe3a00001); // mov r0, #1
    putU32(bytes, entryFileOffset + 0x4, 0xe2800002); // add r0, r0, #2
    putU32(bytes, entryFileOffset + 0x8, 0xe59fc000); // ldr ip, [pc]
    putU32(bytes, entryFileOffset + 0xc, 0xe59cf000); // ldr pc, [ip]
    putU32(bytes, entryFileOffset + 0x10, 0x2000);    // import slot address
    return bytes;
}

void checkTrapAdapterBasics() {
    ShimRegistry registry;
    TrapShimAdapter traps;
    const auto first = traps.bind(registry, "_alpha", "libSystem.B.dylib");
    CHECK(first == TrapShimAdapter::kTrapBase);
    CHECK(traps.bind(registry, "_alpha", "libSystem.B.dylib") == first);
    const auto second = traps.bind(registry, "_beta", "libSystem.B.dylib");
    CHECK(second == TrapShimAdapter::kTrapBase + 4u);
    CHECK(traps.size() == 2);
    CHECK(traps.has("_alpha"));
    CHECK(!traps.has("_missing"));
    CHECK(traps.symbolForAddress(first) == "_alpha");
    // Trap+addend addresses from data relocations map back to the import.
    CHECK(traps.symbolForAddress(first + 1u) == "_alpha");
    CHECK(traps.symbolForAddress(first + 4u) == "_beta");
    CHECK(traps.symbolForAddress(first + 7u) == "_beta");
    // The next unallocated slot has no owner.
    CHECK(!traps.symbolForAddress(first + 8u));
    CHECK(!traps.symbolForAddress(0x1000));
    CHECK(!traps.symbolForAddress(TrapShimAdapter::kTrapEnd));

    const auto binding = registry.resolve("_alpha");
    CHECK(binding.has_value());
    CHECK(binding->adapterName == TrapShimAdapter::kAdapterName);
    CHECK(static_cast<bool>(binding->invokeException));
    CHECK(!static_cast<bool>(binding->invoke));

    GuestAddressSpace memory;
    CpuRegisterState registers;
    registers.r[0] = 0x1234;
    std::string reason;
    CHECK(registry.invokeCallout(first, registers, memory, reason) ==
          GuestCalloutResult::ExceptionRaised);
    CHECK(reason.find("_alpha") != std::string::npos);
    CHECK(traps.lastTrapped() == "_alpha");
    CHECK(traps.trapCalls() == 1);
    CHECK(registry.invokeCallout(0xf0001234, registers, memory, reason) ==
          GuestCalloutResult::NotRegistered);
}

void checkFailClosedDefaultIsUnchanged(const std::vector<std::uint8_t> &bytes) {
    ShimRegistry registry;
    GuestAddressSpace memory;
    const auto report = MachOLoader().load(bytes, memory, registry);
    CHECK(report.status == "BLOCKED_UNRESOLVED_IMPORTS");
    CHECK(report.imageMapped());
    CHECK(report.firstMissingImport == "_trapped_call");
    CHECK(report.unresolvedSymbols.size() == 1);
    CHECK(report.trappedSymbols.empty());
    CHECK(report.resolvedSymbols.empty());
    CHECK(registry.size() == 0);

    const auto cpu = createArm32CpuBackend();
    GuestRunner runner(registry, *cpu);
    const auto runReport = runner.runMainBinary(bytes, true);
    CHECK(runReport.fields.at("status").value == "not_runnable");
    CHECK(runReport.fields.at("execution").fields.at("entryPointReached").value == "false");
    CHECK(runReport.fields.at("cpu").fields.at("status").value ==
          "BLOCKED_BY_UNRESOLVED_IMPORT");
}

void checkTrapLoad(const std::vector<std::uint8_t> &bytes) {
    ShimRegistry registry;
    TrapShimAdapter traps;
    GuestAddressSpace memory;
    const auto report = MachOLoader().loadWithTraps(bytes, memory, registry, traps);
    CHECK(report.status == "LOADED_WITH_TRAPS");
    CHECK(report.imageMapped());
    CHECK(report.entryPoint == 0x1400);
    CHECK(report.entryPointSource == "LC_MAIN");
    // Traps are not implementations: the first missing import is still named.
    CHECK(report.firstMissingImport == "_trapped_call");
    CHECK(report.unresolvedSymbols.empty());
    CHECK(report.trappedSymbols.size() == 1);
    CHECK(report.unboundNlistSymbols.empty());
    const auto &trapped = report.trappedSymbols.front();
    CHECK(trapped.fields.at("status").value == "trapped");
    CHECK(trapped.fields.at("symbol").value == "_trapped_call");
    CHECK(trapped.fields.at("trapAddress").value ==
          std::to_string(TrapShimAdapter::kTrapBase));

    std::uint32_t slot = 0;
    CHECK(memory.read(0x2000, &slot, sizeof(slot)));
    CHECK(slot == TrapShimAdapter::kTrapBase);
    CHECK(registry.size() == 1);

    const auto json = report.toJson();
    CHECK(json.fields.at("trappedSymbolCount").value == "1");
    CHECK(json.fields.at("unresolvedSymbolCount").value == "0");
    CHECK(json.fields.at("unboundNlistSymbolCount").value == "0");
    CHECK(json.fields.at("trappedSymbols").items.size() == 1);
}

void checkBootAttemptRunner(const std::vector<std::uint8_t> &bytes) {
    {
        ShimRegistry registry;
        TrapShimAdapter traps;
        const auto cpu = createArm32CpuBackend();
        BootAttemptRunner runner(registry, *cpu, traps);
        const auto denied = runner.run(bytes, false);
        CHECK(denied.fields.at("reason").value == "User authorization was not confirmed.");
        const auto empty = runner.run({}, true);
        CHECK(empty.fields.at("reason").value == "IPA main executable is empty.");
    }
#ifdef RADEK_TEST_REQUIRE_DYNARMIC
    {
        ShimRegistry registry;
        TrapShimAdapter traps;
        const auto cpu = createArm32CpuBackend();
        CHECK(cpu->available());
        BootAttemptRunner runner(registry, *cpu, traps);
        const auto report = runner.run(bytes, true);
        CHECK(report.fields.at("status").value == "not_runnable");
        CHECK(report.fields.at("trapMode").value == "true");
        CHECK(report.fields.at("trappedImport").value == "_trapped_call");
        const auto &execution = report.fields.at("execution").fields;
        CHECK(execution.at("entryPointReached").value == "true");
        CHECK(execution.at("status").value == "GUEST_EXCEPTION_RAISED");
        // mov, add, ldr, ldr, trap fetch.
        CHECK(execution.at("instructions").value == "5");
        CHECK(execution.at("registers").fields.at("r0").value == "3");
        CHECK(report.fields.at("reason").value.find("_trapped_call") != std::string::npos);
        CHECK(traps.trapCalls() == 1);
    }
#else
    {
        ShimRegistry registry;
        TrapShimAdapter traps;
        const auto cpu = createArm32CpuBackend();
        CHECK(!cpu->available());
        BootAttemptRunner runner(registry, *cpu, traps);
        const auto report = runner.run(bytes, true);
        CHECK(report.fields.at("status").value == "not_runnable");
        CHECK(report.fields.at("execution").fields.at("entryPointReached").value == "false");
        CHECK(report.fields.at("cpu").fields.at("status").value == "BACKEND_UNAVAILABLE");
        // The trap load still happened before the backend boundary.
        CHECK(report.fields.at("loader").fields.at("status").value == "LOADED_WITH_TRAPS");
    }
#endif
}

} // namespace

int main() {
    try {
        const auto bytes = makeTrapMachO();
        checkTrapAdapterBasics();
        checkFailClosedDefaultIsUnchanged(bytes);
        checkTrapLoad(bytes);
        checkBootAttemptRunner(bytes);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "compat-runtime-traps test failed: %s\n", error.what());
        return 1;
    }
    std::puts("compat-runtime trap boot-attempt tests passed");
    return 0;
}
