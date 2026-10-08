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
    CHECK(compat_import_catalog::kDarwinOnlyProviderCount == 73);
    for (const auto &provider : compat_import_catalog::kDarwinOnlyProviders) {
        const auto binding = harness.registry.resolve(provider.symbol);
        CHECK(binding.has_value());
        CHECK(binding->adapterName.size() > 0);
        CHECK(binding->library.size() > 0);
    }
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
    for (const auto &provider : ndk_import_catalog::kProviders) {
        const auto binding = harness.registry.resolve(provider.symbol);
        CHECK(binding.has_value());
        CHECK(binding->library.size() > 0);
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

    registers = {};
    registers.r[0] = kData + 256;
    CHECK(harness.call("_setjmp", registers, reason) == GuestCalloutResult::Returned);
    CHECK(registers.r[0] == 0);
}

} // namespace

int main() {
    testAllDarwinProvidersAreRegistered();
    testExceptionAllocationAndGuards();
    testRttiDataBindingIsGuestData();
    testAllNdkProvidersAndMinimalEmulation();
    return 0;
}
