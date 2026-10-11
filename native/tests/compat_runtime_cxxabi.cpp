// Provider coverage: every name in the complete reviewed NDK inventory and the
// observed Darwin catalog has a registered runtime provider. Typed families do
// real bounded work; framework/driver signatures use named boundaries rather
// than resolving to an anonymous trap.
#include "compat_runtime/audio_session_shims.hpp"
#include "compat_runtime/compat_import_catalog.hpp"
#include "compat_runtime/compiler_rt_shims.hpp"
#include "compat_runtime/cxxabi_shims.hpp"
#include "compat_runtime/darwin_compat_shims.hpp"
#include "compat_runtime/gles_shims.hpp"
#include "compat_runtime/guest_memory.hpp"
#include "compat_runtime/libsystem_shims.hpp"
#include "compat_runtime/ndk_compat_shims.hpp"
#include "compat_runtime/ndk_full_import_catalog.hpp"
#include "compat_runtime/ndk_import_catalog.hpp"
#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/sjlj_unwind.hpp"
#include "compat_runtime/virtual_file_system.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

#define CHECK(expression)                                                                            \
    do {                                                                                             \
        if (!(expression))                                                                           \
            throw std::runtime_error("CHECK failed: " #expression);                                  \
    } while (false)

namespace {
using namespace radek::compat_runtime;

constexpr GuestAddress kData = 0x10000;
constexpr GuestAddress kText = 0x20000;

struct Harness {
    GuestAddressSpace memory{128U * 1024U * 1024U};
    ShimRegistry registry;
    objc::ShimAdapter objc;
    libsystem::ShimAdapter libsystem;
    audio::ShimAdapter audio;
    SjLjUnwindAdapter sjlj;
    compiler_rt::ShimAdapter compiler;
    cxxabi::ShimAdapter cxx;
    ndk::ShimAdapter ndk;
    gles::Forwarder gles;
    darwin_compat::ShimAdapter darwin{&objc};

    Harness() {
        memory.mapAt(kData, 0x1000, MemoryPermission::Read | MemoryPermission::Write,
                     "cxxabi-test-data");
        memory.mapAt(kText, 0x1000, MemoryPermission::Read | MemoryPermission::Execute,
                     "cxxabi-test-text");
        objc.registerBindings(registry);
        libsystem.registerBindings(registry);
        audio.registerBindings(registry);
        sjlj.registerBindings(registry);
        compiler.registerBindings(registry);
        cxx.registerBindings(registry);
        gles.registerBindings(registry);
        darwin.registerBindings(registry);
        ndk.registerBindings(registry);
    }

    GuestCalloutResult call(const char *symbol, CpuRegisterState &registers, std::string &reason) {
        const auto binding = registry.resolve(symbol);
        CHECK(binding.has_value());
        return registry.invokeCallout(binding->guestAddress, registers, memory, reason);
    }
};

void testAllDarwinProvidersAreRegistered() {
    Harness harness;
    const auto missing = compat_import_catalog::missingProviders(harness.registry);
    CHECK(missing.empty());
    CHECK(compat_import_catalog::kDarwinOnlyProviderCount == 104);
    CHECK(compat_import_catalog::kGuestRuntimeAdapterProviderCount == 104);
    CHECK(harness.compiler.registeredSymbolCount() == 38);
    for (const char *symbol : {"___divsi3", "___modsi3", "___udivsi3", "___umodsi3",
                               "___divdi3", "___moddi3", "___floatdidf", "___floatdisf", "___fixdfdi",
                               "___fixsfsivfp", "___fixunsdfsivfp", "___floatsidfvfp",
                               "___floatsisfvfp", "___floatunssidfvfp", "___floatunssisfvfp",
                               "___floatundidf", "___muldi3", "___udivdi3", "___umoddi3",
                               "___addsf3vfp", "___subsf3vfp", "___mulsf3vfp",
                               "___divsf3vfp", "___adddf3vfp", "___subdf3vfp", "___muldf3vfp",
                               "___divdf3vfp", "___extendsfdf2vfp", "___truncdfsf2vfp",
                               "___eqsf2vfp", "___nesf2vfp", "___gtsf2vfp", "___gedf2vfp",
                               "___gtdf2vfp", "___lesf2vfp", "___ltsf2vfp", "___nedf2vfp",
                               "___unordsf2vfp"}) {
        CHECK(harness.registry.resolve(symbol).has_value());
    }
    CHECK(ndk_import_catalog::kProviderCount == 181);
    for (const auto &provider : compat_import_catalog::kGuestRuntimeAdapterProviders) {
        const auto binding = harness.registry.resolve(provider.symbol);
        CHECK(binding.has_value());
        CHECK(binding->adapterName.size() > 0);
        CHECK(binding->library.size() > 0);
        for (const auto &ndkProvider : ndk_import_catalog::kProviders)
            CHECK(std::string(provider.symbol) != ndkProvider.symbol);
    }
}

namespace {

std::uint32_t sBits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void setS(CpuRegisterState &registers, int index, float value) {
    const int shift = (index % 2) * 32;
    auto &word = registers.d[static_cast<std::size_t>(index / 2)];
    word = (word & ~(std::uint64_t{0xFFFFFFFFu} << shift)) |
           (std::uint64_t{sBits(value)} << shift);
}

void setD(CpuRegisterState &registers, int index, double value) {
    std::memcpy(&registers.d[static_cast<std::size_t>(index)], &value, sizeof(value));
}

double readD(const CpuRegisterState &registers, int index) {
    double value = 0.0;
    std::memcpy(&value, &registers.d[static_cast<std::size_t>(index)], sizeof(value));
    return value;
}

float readS(const CpuRegisterState &registers, int index) {
    const auto bits = static_cast<std::uint32_t>(
        registers.d[static_cast<std::size_t>(index / 2)] >> ((index % 2) * 32));
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

} // namespace

void testCompilerRuntimeVfpAndIntegerHelpers() {
    Harness harness;
    std::string reason;
    const float nan = std::nanf("");
    const double dnan = std::nan("");

    auto callS = [&](const char *symbol, float a, float b) {
        CpuRegisterState registers{};
        setS(registers, 0, a);
        setS(registers, 1, b);
        CHECK(harness.call(symbol, registers, reason) == GuestCalloutResult::Returned);
        return registers;
    };
    auto callD = [&](const char *symbol, double a, double b) {
        CpuRegisterState registers{};
        setD(registers, 0, a);
        setD(registers, 1, b);
        CHECK(harness.call(symbol, registers, reason) == GuestCalloutResult::Returned);
        return registers;
    };

    // Float arithmetic: s0 = s0 op s1; double arithmetic: d0 = d0 op d1.
    CHECK(readS(callS("___addsf3vfp", 1.5f, 2.25f), 0) == 3.75f);
    CHECK(readS(callS("___subsf3vfp", 1.5f, 2.25f), 0) == -0.75f);
    CHECK(readS(callS("___mulsf3vfp", 1.5f, 2.0f), 0) == 3.0f);
    CHECK(readS(callS("___divsf3vfp", 3.0f, 2.0f), 0) == 1.5f);
    CHECK(readD(callD("___adddf3vfp", 1.5, 2.25), 0) == 3.75);
    CHECK(readD(callD("___subdf3vfp", 1.5, 2.25), 0) == -0.75);
    CHECK(readD(callD("___muldf3vfp", 1.5, 2.0), 0) == 3.0);
    CHECK(readD(callD("___divdf3vfp", 3.0, 2.0), 0) == 1.5);

    // The single-precision result must not clobber the upper half of d0.
    {
        CpuRegisterState registers{};
        setS(registers, 0, 2.0f);
        setS(registers, 1, 4.0f);
        CHECK(harness.call("___divsf3vfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(readS(registers, 0) == 0.5f);
        CHECK(readS(registers, 1) == 4.0f);
    }

    // Conversions between single and double precision.
    {
        CpuRegisterState registers{};
        setS(registers, 0, 0.25f);
        CHECK(harness.call("___extendsfdf2vfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(readD(registers, 0) == 0.25);
    }
    {
        CpuRegisterState registers{};
        setD(registers, 0, 0.125);
        CHECK(harness.call("___truncdfsf2vfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(readS(registers, 0) == 0.125f);
    }

    // Comparisons follow libgcc's unordered conventions.
    CHECK(callS("___eqsf2vfp", 1.0f, 1.0f).r[0] == 0u);
    CHECK(callS("___eqsf2vfp", 1.0f, 2.0f).r[0] == 1u);
    CHECK(callS("___eqsf2vfp", nan, nan).r[0] == 1u);
    CHECK(callS("___nesf2vfp", 1.0f, 1.0f).r[0] == 0u);
    CHECK(callS("___nesf2vfp", nan, 1.0f).r[0] == 1u);
    CHECK(callS("___gtsf2vfp", 2.0f, 1.0f).r[0] == 1u);
    CHECK(callS("___gtsf2vfp", 1.0f, 1.0f).r[0] == 0u);
    CHECK(callS("___gtsf2vfp", 1.0f, 2.0f).r[0] == static_cast<std::uint32_t>(-1));
    CHECK(callS("___gtsf2vfp", nan, 1.0f).r[0] == static_cast<std::uint32_t>(-1));
    CHECK(callS("___lesf2vfp", 1.0f, 1.0f).r[0] == 0u);
    CHECK(callS("___lesf2vfp", 2.0f, 1.0f).r[0] == 1u);
    CHECK(callS("___lesf2vfp", nan, 1.0f).r[0] == 1u);
    CHECK(callS("___ltsf2vfp", 1.0f, 2.0f).r[0] == static_cast<std::uint32_t>(-1));
    CHECK(callS("___ltsf2vfp", 1.0f, nan).r[0] == 1u);
    CHECK(callS("___unordsf2vfp", nan, 1.0f).r[0] == 1u);
    CHECK(callS("___unordsf2vfp", 1.0f, 2.0f).r[0] == 0u);

    CHECK(callD("___nedf2vfp", 1.0, 1.0).r[0] == 0u);
    CHECK(callD("___nedf2vfp", dnan, dnan).r[0] == 1u);
    CHECK(callD("___gedf2vfp", 1.0, 1.0).r[0] == 0u);
    CHECK(callD("___gedf2vfp", 1.0, 2.0).r[0] == static_cast<std::uint32_t>(-1));
    CHECK(callD("___gedf2vfp", dnan, 1.0).r[0] == static_cast<std::uint32_t>(-1));
    CHECK(callD("___gtdf2vfp", 2.0, 1.0).r[0] == 1u);
    CHECK(callD("___gtdf2vfp", dnan, 1.0).r[0] == static_cast<std::uint32_t>(-1));

    // Integer -> float conversions. Inputs are in r0 (and r0:r1 for 64-bit).
    {
        CpuRegisterState registers{};
        registers.r[0] = static_cast<std::uint32_t>(-5);
        CHECK(harness.call("___floatsidfvfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(readD(registers, 0) == -5.0);
    }
    {
        CpuRegisterState registers{};
        registers.r[0] = static_cast<std::uint32_t>(-5);
        CHECK(harness.call("___floatsisfvfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(readS(registers, 0) == -5.0f);
    }
    {
        CpuRegisterState registers{};
        registers.r[0] = 0xFFFFFFFFu;
        CHECK(harness.call("___floatunssidfvfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(readD(registers, 0) == 4294967295.0);
    }
    {
        CpuRegisterState registers{};
        registers.r[0] = 0xFFFFFFFFu;
        CHECK(harness.call("___floatunssisfvfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(readS(registers, 0) == 4294967296.0f);
    }
    {
        CpuRegisterState registers{};
        registers.r[0] = 0u;
        registers.r[1] = 0x1u; // 2^32
        CHECK(harness.call("___floatundidf", registers, reason) == GuestCalloutResult::Returned);
        CHECK(readD(registers, 0) == 4294967296.0);
    }

    // Float -> integer conversions saturate; NaN maps to zero.
    {
        CpuRegisterState registers{};
        setS(registers, 0, -7.9f);
        CHECK(harness.call("___fixsfsivfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(static_cast<std::int32_t>(registers.r[0]) == -7);
    }
    {
        CpuRegisterState registers{};
        setS(registers, 0, 1.0e20f);
        CHECK(harness.call("___fixsfsivfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(static_cast<std::int32_t>(registers.r[0]) == 2147483647);
    }
    {
        CpuRegisterState registers{};
        setS(registers, 0, nan);
        CHECK(harness.call("___fixsfsivfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(registers.r[0] == 0u);
    }
    {
        CpuRegisterState registers{};
        setD(registers, 0, 4000000000.75);
        CHECK(harness.call("___fixunsdfsivfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(registers.r[0] == 4000000000u);
    }
    {
        CpuRegisterState registers{};
        setD(registers, 0, -3.0);
        CHECK(harness.call("___fixunsdfsivfp", registers, reason) == GuestCalloutResult::Returned);
        CHECK(registers.r[0] == 0u);
    }

    // 64-bit integer helpers: r0:r1 and r2:r3 pairs; division by zero yields zero.
    auto pair = [](CpuRegisterState &registers, int low, std::uint64_t value) {
        registers.r[static_cast<std::size_t>(low)] = static_cast<std::uint32_t>(value);
        registers.r[static_cast<std::size_t>(low + 1)] = static_cast<std::uint32_t>(value >> 32);
    };
    auto result64 = [](const CpuRegisterState &registers) {
        return static_cast<std::uint64_t>(registers.r[0]) |
               (static_cast<std::uint64_t>(registers.r[1]) << 32);
    };
    {
        CpuRegisterState registers{};
        pair(registers, 0, 0x100000001ULL);
        pair(registers, 2, 0x10ULL);
        CHECK(harness.call("___muldi3", registers, reason) == GuestCalloutResult::Returned);
        CHECK(result64(registers) == 0x1000000010ULL);
    }
    {
        CpuRegisterState registers{};
        pair(registers, 0, 0xFFFFFFFFFFFFFFFFULL);
        pair(registers, 2, 16ULL);
        CHECK(harness.call("___udivdi3", registers, reason) == GuestCalloutResult::Returned);
        CHECK(result64(registers) == 0x0FFFFFFFFFFFFFFFULL);
    }
    {
        CpuRegisterState registers{};
        pair(registers, 0, 100ULL);
        pair(registers, 2, 0ULL);
        CHECK(harness.call("___udivdi3", registers, reason) == GuestCalloutResult::Returned);
        CHECK(result64(registers) == 0ULL);
    }
    {
        CpuRegisterState registers{};
        pair(registers, 0, 0x100000007ULL);
        pair(registers, 2, 0x10ULL);
        CHECK(harness.call("___umoddi3", registers, reason) == GuestCalloutResult::Returned);
        CHECK(result64(registers) == 7ULL);
    }
}

void testLiveGlesSnapshotIsThreadSafeAndResettable() {
    gles::resetProgress();
    Harness harness;
    gles::resetProgress();

    auto snapshot = gles::progressSnapshot();
    CHECK(snapshot.guestCallsObserved == 0);
    CHECK(snapshot.forwardedCalls == 0);
    CHECK(snapshot.refusedCalls == 0);
    CHECK(snapshot.framesPresented == 0);

    CpuRegisterState registers;
    registers.r[0] = 3;
    registers.r[1] = 0x1406; // GL_FLOAT
    registers.r[2] = 0;
    registers.r[3] = kData;
    std::string reason;
    CHECK(harness.call("_glVertexPointer", registers, reason) == GuestCalloutResult::Returned);

    snapshot = gles::progressSnapshot();
    CHECK(snapshot.guestCallsObserved == 1);
    CHECK(snapshot.forwardedCalls == 0); // client pointer is reissued safely at draw time
    CHECK(snapshot.framesPresented == 0);

    gles::resetProgress();
    snapshot = gles::progressSnapshot();
    CHECK(snapshot.guestCallsObserved == 0);
    CHECK(snapshot.forwardedCalls == 0);
    CHECK(snapshot.framesPresented == 0);
}

void testExceptionAllocationAndGuards() {
    Harness harness;
    std::string reason;
    CpuRegisterState registers;
    registers.r[0] = 24;
    CHECK(harness.call("___cxa_allocate_exception", registers, reason) == GuestCalloutResult::Returned);
    const auto exceptionObject = registers.r[0];
    CHECK(exceptionObject != 0);
    CHECK(harness.memory.contains(exceptionObject, 24, MemoryPermission::Read | MemoryPermission::Write));

    registers.r[1] = 0x1111;
    registers.r[2] = 0x2222;
    reason.clear();
    CHECK(harness.call("___cxa_throw", registers, reason) == GuestCalloutResult::ExceptionRaised);
    CHECK(reason.find("bounded guest C++ exception boundary") != std::string::npos);
    CHECK(harness.cxx.thrownCount() == 1);

    registers.r[0] = exceptionObject;
    reason.clear();
    CHECK(harness.call("___cxa_begin_catch", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == exceptionObject);
    CHECK(harness.call("___cxa_end_catch", registers, reason) == GuestCalloutResult::Returned);
    CHECK(harness.call("___cxa_free_exception", registers, reason) == GuestCalloutResult::Returned);
    CHECK(!harness.memory.contains(exceptionObject, 4, MemoryPermission::Read));

    const GuestAddress guard = kData + 0x40;
    std::uint32_t zero = 0;
    CHECK(harness.memory.write(guard, &zero, sizeof(zero)));
    registers = {};
    registers.r[0] = guard;
    CHECK(harness.call("___cxa_guard_acquire", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 1);
    registers.r[0] = guard;
    CHECK(harness.call("___cxa_guard_acquire", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    registers.r[0] = guard;
    CHECK(harness.call("___cxa_guard_release", registers, reason) == GuestCalloutResult::Returned);
    CHECK(harness.call("___cxa_guard_acquire", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
}

void testRttiDataBindingIsGuestData() {
    Harness harness;
    const auto binding = harness.registry.resolve("__ZTVN10__cxxabiv117__class_type_infoE");
    CHECK(binding.has_value());
    CHECK(binding->resolveGuestAddress);
    GuestAddress address = 0;
    std::string reason;
    CHECK(binding->resolveGuestAddress(harness.memory, address, reason));
    CHECK(address != 0);
    CHECK(harness.memory.contains(address, 16, MemoryPermission::Read));
    GuestAddress again = 0;
    CHECK(binding->resolveGuestAddress(harness.memory, again, reason));
    CHECK(address == again);

    // Mach-O data bindings are materialized before image initializers run. The
    // initializer may reset host-side C++ ABI caches, but it must not unmap a
    // vtable page that the loader already wrote into the guest image's
    // relocation slots.
    CHECK(harness.registry.initializeImage(harness.memory, {}, reason));
    CHECK(harness.memory.contains(address, 16, MemoryPermission::Read));
}

void testAllNdkProvidersAndMinimalEmulation() {
    Harness harness;
    const auto missing = ndk_import_catalog::missingProviders(harness.registry);
    if (!missing.empty())
        throw std::runtime_error("missing fixture NDK provider: " + missing.front());
    CHECK(missing.empty());
    const auto missingFull = ndk_full_import_catalog::missingProviders(harness.registry);
    if (!missingFull.empty())
        throw std::runtime_error("missing full NDK provider: " + missingFull.front());
    CHECK(missingFull.empty());
    CHECK(ndk_import_catalog::kProviderCount == 181);
    CHECK(ndk_full_import_catalog::kProviderCount == 1229);
    CHECK(harness.ndk.registeredCalloutCount() > 0);
    CHECK(harness.ndk.genericProviderCount() > 0);
    CHECK(harness.ndk.typedProviderCount() > 0);
    CHECK(harness.ndk.genericProviderCount() + harness.ndk.typedProviderCount() ==
          ndk_full_import_catalog::kProviderCount);
    CHECK(harness.ndk.fixtureProviderCount() == 181);
    CHECK(harness.ndk.fixtureNonGenericProviderCount() == 181);
    CHECK(harness.ndk.fixtureGenericProviderCount() == 0);
    for (const auto &provider : ndk_import_catalog::kProviders) {
        const auto binding = harness.registry.resolve(provider.symbol);
        CHECK(binding.has_value());
        CHECK(binding->library.size() > 0);
        CHECK(binding->adapterName.rfind("ndk-bounded-", 0) != 0);
        CHECK(binding->invoke || binding->invokeTransfer || binding->invokeException ||
              binding->resolveGuestAddress);
    }
    for (const char *symbol : {"_AAssetManager_open", "_inflate", "_vkCreateInstance",
                               "_pthread_attr_init", "_eglGetDisplay"}) {
        const auto binding = harness.registry.resolve(symbol);
        CHECK(binding.has_value());
        CHECK(binding->adapterName.find("ndk-") == 0 || binding->adapterName.find("gles-") == 0);
    }
    const auto environBinding = harness.registry.resolve("_environ");
    CHECK(environBinding.has_value() && environBinding->resolveGuestAddress);
    GuestAddress environCell = 0;
    std::string reason;
    CHECK(environBinding->resolveGuestAddress(harness.memory, environCell, reason));
    CHECK(harness.memory.contains(environCell, sizeof(GuestAddress), MemoryPermission::Read));
    CpuRegisterState errnoRegisters;
    CHECK(harness.call("___errno", errnoRegisters, reason) == GuestCalloutResult::Returned);
    CHECK(harness.memory.contains(errnoRegisters.r[0], sizeof(std::int32_t),
                                  MemoryPermission::Read | MemoryPermission::Write));
    const auto stackFailure = harness.registry.resolve("___stack_chk_fail");
    CHECK(stackFailure.has_value() && stackFailure->invokeException);

    CpuRegisterState registers;
    float half = 0.5f;
    std::uint32_t halfBits = 0;
    std::memcpy(&halfBits, &half, sizeof(halfBits));
    registers.d[0] = halfBits;
    CHECK(harness.call("_acosf", registers, reason) == GuestCalloutResult::Returned);
    float acosResult = 0.0f;
    const auto resultBits = static_cast<std::uint32_t>(registers.d[0]);
    std::memcpy(&acosResult, &resultBits, sizeof(acosResult));
    CHECK(std::fabs(acosResult - std::acos(half)) < 0.0001f);

    registers = {};
    registers.d[0] = halfBits;
    float one = 1.0f;
    std::uint32_t oneBits = 0;
    std::memcpy(&oneBits, &one, sizeof(oneBits));
    registers.d[0] = static_cast<std::uint64_t>(halfBits) |
                     (static_cast<std::uint64_t>(oneBits) << 32);
    CHECK(harness.call("_powf", registers, reason) == GuestCalloutResult::Returned);
    float powResult = 0.0f;
    const auto powBits = static_cast<std::uint32_t>(registers.d[0]);
    std::memcpy(&powResult, &powBits, sizeof(powResult));
    CHECK(std::fabs(powResult - 0.5f) < 0.0001f);

    const std::string checksumText = "ndk-provider";
    CHECK(harness.memory.write(kData + 512, checksumText.data(), checksumText.size()));
    registers = {};
    registers.r[1] = kData + 512;
    registers.r[2] = static_cast<std::uint32_t>(checksumText.size());
    CHECK(harness.call("_adler32", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0x1ed504d6U);
    registers.r[0] = 0;
    CHECK(harness.call("_crc32", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0xf7e4aa45U);

    const std::string assetName = "radek-ndk-provider-asset.bin";
    const std::string assetPath = std::string("/tmp/") + assetName;
    {
        std::ofstream asset(assetPath, std::ios::binary | std::ios::trunc);
        CHECK(asset.good());
        asset << "asset-data";
    }
    guestFileSystem().mount(bundleGuestPath(), "/tmp", false);
    const GuestAddress guestAssetName = kData + 768;
    CHECK(harness.memory.write(guestAssetName, assetName.data(), assetName.size() + 1));
    registers = {};
    registers.r[1] = guestAssetName;
    CHECK(harness.call("_AAssetManager_open", registers, reason) == GuestCalloutResult::Returned);
    const GuestAddress assetHandle = registers.r[0];
    CHECK(assetHandle != 0);
    const GuestAddress guestAssetBuffer = kData + 832;
    registers.r[0] = assetHandle;
    registers.r[1] = guestAssetBuffer;
    registers.r[2] = 10;
    CHECK(harness.call("_AAsset_read", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 10);
    char assetBytes[11]{};
    CHECK(harness.memory.read(guestAssetBuffer, assetBytes, 10));
    CHECK(std::string(assetBytes, 10) == "asset-data");
    registers = {};
    registers.r[0] = assetHandle;
    CHECK(harness.call("_AAsset_getLength", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 10);
    registers.r[0] = assetHandle;
    CHECK(harness.call("_AAsset_getRemainingLength", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    registers.r[0] = assetHandle;
    CHECK(harness.call("_AAsset_close", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    std::remove(assetPath.c_str());

    const std::string left = "alpha,beta";
    const std::string reject = ",";
    CHECK(harness.memory.write(kData, left.data(), left.size() + 1));
    CHECK(harness.memory.write(kData + 64, reject.data(), reject.size() + 1));
    registers = {};
    registers.r[0] = kData;
    registers.r[1] = kData + 64;
    CHECK(harness.call("_strcspn", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 5);

    registers = {};
    registers.r[0] = kData + 128;
    CHECK(harness.call("_pthread_mutex_init", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    registers.r[0] = kData + 128;
    CHECK(harness.call("_pthread_mutex_lock", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    registers.r[0] = kData + 128;
    CHECK(harness.call("_pthread_mutex_trylock", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 16);
    registers.r[0] = kData + 128;
    CHECK(harness.call("_pthread_mutex_unlock", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);

    // A blocking self-lock of a normal mutex is granted as a phantom lock (HyperHLE
    // behaviour) instead of spinning, and a matching unlock consumes it.
    constexpr GuestAddress kPhantomMutex = kData + 0x880;
    registers = {};
    registers.r[0] = kPhantomMutex;
    CHECK(harness.call("_pthread_mutex_init", registers, reason) == GuestCalloutResult::Returned);
    for (int depth = 0; depth < 2; ++depth) {
        registers = {};
        registers.r[0] = kPhantomMutex;
        CHECK(harness.call("_pthread_mutex_lock", registers, reason) == GuestCalloutResult::Returned);
        CHECK(registers.r[0] == 0);
    }
    registers = {};
    registers.r[0] = kPhantomMutex;
    CHECK(harness.call("_pthread_mutex_unlock", registers, reason) == GuestCalloutResult::Returned);
    registers = {};
    registers.r[0] = kPhantomMutex;
    CHECK(harness.call("_pthread_mutex_trylock", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 16); // still held by the outer lock
    registers = {};
    registers.r[0] = kPhantomMutex;
    CHECK(harness.call("_pthread_mutex_unlock", registers, reason) == GuestCalloutResult::Returned);
    registers = {};
    registers.r[0] = kPhantomMutex;
    CHECK(harness.call("_pthread_mutex_trylock", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    registers = {};
    registers.r[0] = kPhantomMutex;
    CHECK(harness.call("_pthread_mutex_unlock", registers, reason) == GuestCalloutResult::Returned);

    // A mutex initialised from a PTHREAD_MUTEX_RECURSIVE attribute nests for its owner.
    constexpr GuestAddress kRecursiveAttr = kData + 0x800;
    constexpr GuestAddress kRecursiveMutex = kData + 0x840;
    registers = {};
    registers.r[0] = kRecursiveAttr;
    CHECK(harness.call("_pthread_mutexattr_init", registers, reason) == GuestCalloutResult::Returned);
    registers = {};
    registers.r[0] = kRecursiveAttr;
    registers.r[1] = 2; // PTHREAD_MUTEX_RECURSIVE (Darwin)
    CHECK(harness.call("_pthread_mutexattr_settype", registers, reason) == GuestCalloutResult::Returned);
    registers = {};
    registers.r[0] = kRecursiveMutex;
    registers.r[1] = kRecursiveAttr;
    CHECK(harness.call("_pthread_mutex_init", registers, reason) == GuestCalloutResult::Returned);
    for (int depth = 0; depth < 2; ++depth) {
        registers = {};
        registers.r[0] = kRecursiveMutex;
        CHECK(harness.call("_pthread_mutex_lock", registers, reason) == GuestCalloutResult::Returned);
        CHECK(registers.r[0] == 0);
    }
    registers = {};
    registers.r[0] = kRecursiveMutex;
    CHECK(harness.call("_pthread_mutex_unlock", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    registers = {};
    registers.r[0] = kRecursiveMutex;
    CHECK(harness.call("_pthread_mutex_unlock", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    registers = {};
    registers.r[0] = kRecursiveMutex;
    CHECK(harness.call("_pthread_mutex_trylock", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
    registers = {};
    registers.r[0] = kRecursiveMutex;
    CHECK(harness.call("_pthread_mutex_unlock", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);

    registers = {};
    registers.r[0] = kData + 256;
    CHECK(harness.call("_setjmp", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
}

} // namespace

int main() {
    testCompilerRuntimeVfpAndIntegerHelpers();
    testLiveGlesSnapshotIsThreadSafeAndResettable();
    testAllDarwinProvidersAreRegistered();
    testExceptionAllocationAndGuards();
    testRttiDataBindingIsGuestData();
    testAllNdkProvidersAndMinimalEmulation();
    return 0;
}
