// C-runtime shim coverage: the registered mem/str adapters execute over guest
// memory, and malloc/free/calloc/realloc are served by the bounded guest heap
// with real allocation reuse. A symbol that is not registered is still refused
// by the registry (fail closed), which is asserted at the end.
#include "compat_runtime/libsystem_shims.hpp"
#include "compat_runtime/guest_memory.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/virtual_file_system.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

constexpr GuestAddress kDataBase = 0x10000;
constexpr GuestAddress kTextBase = 0x20000;
constexpr GuestAddress kDestination = 0x10100;
constexpr GuestAddress kSource = 0x10200;

struct Harness {
    GuestAddressSpace memory{64U * 1024U * 1024U};
    ShimRegistry registry;
    libsystem::ShimAdapter shims;

    Harness() {
        memory.mapAt(kDataBase, 0x1000, MemoryPermission::Read | MemoryPermission::Write,
                     "guest-data");
        memory.mapAt(kTextBase, 0x1000, MemoryPermission::Read | MemoryPermission::Execute,
                     "guest-text");
        shims.registerBindings(registry);
    }

    std::uint32_t call(const std::string &symbol, std::uint32_t r0 = 0, std::uint32_t r1 = 0,
                       std::uint32_t r2 = 0, std::uint32_t r3 = 0) {
        const auto binding = registry.resolve(symbol);
        CHECK(binding.has_value());
        CpuRegisterState registers;
        registers.r[0] = r0;
        registers.r[1] = r1;
        registers.r[2] = r2;
        registers.r[3] = r3;
        std::string reason;
        const auto result = registry.invokeCallout(binding->guestAddress, registers, memory, reason);
        if (result != GuestCalloutResult::Returned)
            throw std::runtime_error(symbol + " returned " + std::to_string(static_cast<int>(result)) +
                                     ": " + reason);
        return registers.r[0];
    }

    void write(GuestAddress address, const std::string &text) {
        CHECK(memory.write(address, text.c_str(), text.size() + 1));
    }

    std::string read(GuestAddress address, std::size_t size) {
        std::vector<char> bytes(size);
        CHECK(memory.read(address, bytes.data(), size));
        return std::string(bytes.data(), size);
    }
};

void testMemoryAdapters() {
    Harness harness;
    const std::string text = "hello guest memory";
    harness.write(kSource, text);
    harness.call("_memcpy", kDestination, kSource, text.size() + 1);
    CHECK(harness.read(kDestination, text.size()) == text);

    harness.call("_memset", kDestination + 6, static_cast<std::uint32_t>('G'), 5);
    CHECK(harness.read(kDestination, text.size()) == "hello GGGGG memory");
    CHECK(harness.call("_memcmp", kDestination, kSource, 6) == 0);
    CHECK(static_cast<std::int32_t>(harness.call("_memcmp", kDestination + 6, kSource + 6, 5)) < 0);
    CHECK(harness.call("_strlen", kDestination) == 18);
    CHECK(harness.call("_memchr", kDestination, static_cast<std::uint32_t>('G'), 19) ==
          kDestination + 6);
    CHECK(harness.call("_memchr", kDestination, static_cast<std::uint32_t>('z'), 19) == 0);

    // Overlapping memmove must copy backwards so the source survives.
    harness.write(kSource, "abcdef");
    harness.call("_memmove", kSource + 2, kSource, 4);
    CHECK(harness.read(kSource, 6) == "ababcd");

    // A read that runs off the end of every mapped region fails closed.
    bool failed = false;
    try {
        harness.call("_memcpy", kSource, 0xfffff000U, 16);
    } catch (const std::exception &) {
        failed = true;
    }
    CHECK(failed);
}

void testStringAdapters() {
    Harness harness;
    harness.write(kSource, "Angry");
    harness.write(kDestination, "Angry");
    CHECK(harness.call("_strcmp", kSource, kDestination) == 0);
    CHECK(harness.call("_strncmp", kSource, kDestination, 3) == 0);
    harness.write(kDestination, "Angle");
    CHECK(static_cast<std::int32_t>(harness.call("_strcmp", kSource, kDestination)) > 0);
    CHECK(harness.call("_strncmp", kSource, kDestination, 3) == 0);

    harness.write(kDestination, "");
    CHECK(harness.call("_strcpy", kDestination, kSource) == kDestination);
    CHECK(harness.read(kDestination, 5) == "Angry");
    CHECK(harness.call("_strcat", kDestination, kSource) == kDestination);
    CHECK(harness.read(kDestination, 10) == "AngryAngry");
    CHECK(harness.call("_strlen", kDestination) == 10);
    CHECK(harness.call("_strnlen", kDestination, 4) == 4);
    CHECK(harness.call("_strchr", kDestination, static_cast<std::uint32_t>('y')) ==
          kDestination + 4);
    CHECK(harness.call("_strrchr", kDestination, static_cast<std::uint32_t>('y')) ==
          kDestination + 9);
    CHECK(harness.call("_strstr", kDestination, kSource) == kDestination);
    CHECK(harness.call("_strstr", kDestination, kSource + 1) == kDestination + 1);
    harness.write(kSource, "gnome");
    CHECK(harness.call("_strstr", kDestination, kSource) == 0);
    harness.write(kSource, "Angry");

    // strncpy pads with NULs and never terminates when the source is longer.
    harness.call("_memset", kDestination, 0x7f, 8);
    harness.write(kSource, "abcdefgh");
    harness.call("_strncpy", kDestination, kSource, 4);
    CHECK(harness.read(kDestination, 4) == "abcd");
    CHECK(static_cast<unsigned char>(harness.read(kDestination + 4, 1)[0]) == 0x7f);
    harness.write(kSource, "ab");
    harness.call("_strncpy", kDestination, kSource, 6);
    CHECK(harness.read(kDestination, 6) == std::string("ab\0\0\0\0", 6));
}

void testSyntheticBundleFiles() {
    const std::filesystem::path root = "/tmp/radek-native-vfs-synthetic-test";
    std::filesystem::remove_all(root);
    CHECK(std::filesystem::create_directories(root));
    auto &files = guestFileSystem();
    files.mount(bundleGuestPath(), root.string(), false);

    Harness harness;
    const GuestAddress path = kDataBase + 0x300;
    const GuestAddress mode = kDataBase + 0x380;
    const GuestAddress destination = kDataBase + 0x400;
    harness.write(path, "/radek-bundle/App.app/data/bundleIndex.idx");
    harness.write(mode, "rb");
    const auto index = harness.call("_fopen", path, mode);
    CHECK(index != 0);
    CHECK(harness.call("_fread", destination, 1, 64, index) == 22);
    CHECK(harness.read(destination, 22) == "__loose_files__,0,0,0\n");
    CHECK(harness.call("_fclose", index) == 0);

    harness.write(path, "/radek-bundle/App.app/highscores.lua");
    const auto state = harness.call("_fopen", path, mode);
    CHECK(state != 0);
    CHECK(harness.call("_fread", destination, 1, 1, state) == 0);
    CHECK(harness.call("_fclose", state) == 0);
    CHECK(files.refusedCount() == 0);
    bool sawSynthetic = false;
    for (const auto &diagnostic : files.diagnostics())
        sawSynthetic = sawSynthetic || diagnostic.find("synthetic") != std::string::npos;
    CHECK(sawSynthetic);
    std::filesystem::remove_all(root);
}

void testGuestHeapAllocator() {
    Harness harness;
    const auto first = harness.call("_malloc", 64);
    const auto second = harness.call("_malloc", 32);
    CHECK(first != 0 && second != 0 && first != second);
    CHECK(harness.shims.allocationCount() == 2);
    CHECK(harness.shims.heapBytes() > 0);

    harness.write(first, "guest block");
    harness.call("_free", first);
    CHECK(harness.shims.freeCount() == 1);
    // The freed block is reused instead of leaking, and the data is untouched
    // until the caller writes it.
    const auto reused = harness.call("_malloc", 32);
    CHECK(reused == first);

    const auto zeroed = harness.call("_calloc", 8, 4);
    CHECK(zeroed != 0);
    CHECK(harness.read(zeroed, 32) == std::string(32, '\0'));

    harness.write(zeroed, "grow me");
    const auto grown = harness.call("_realloc", zeroed, 4096);
    CHECK(grown != 0 && grown != zeroed);
    CHECK(harness.read(grown, 7) == "grow me");
    CHECK(harness.call("_free", grown) == 0);

    const auto duplicated = harness.call("_strdup", first);
    CHECK(duplicated != 0 && duplicated != first);
    CHECK(harness.read(duplicated, 11) == "guest block");
    harness.call("_free", duplicated);
    harness.call("_free", second);
    harness.call("_free", reused);

    // An unsatisfiable request stops the boot with a reason instead of handing
    // the guest a NULL pointer the runtime cannot reason about.
    bool exhausted = false;
    try {
        harness.call("_malloc", 512U * 1024U * 1024U);
    } catch (const std::exception &error) {
        exhausted = std::string(error.what()).find("bounded guest heap") != std::string::npos;
    }
    CHECK(exhausted);

    // Reusing a bogus pointer must not silently succeed.
    bool failed = false;
    try {
        harness.call("_realloc", kDataBase + 8, 32);
    } catch (const std::exception &) {
        failed = true;
    }
    CHECK(failed);
}

// The Itanium C++ ABI allocation entry points share the bounded guest heap, so
// an in-image operator new/delete pair gets real storage instead of a trap.
void testCxxOperatorAllocators() {
    Harness harness;
    const auto object = harness.call("__Znwm", 24);
    CHECK(object != 0);
    harness.write(object, "cxx object");
    const auto array = harness.call("__Znam", 64);
    CHECK(array != 0 && array != object);
    CHECK(harness.read(object, 10) == "cxx object");
    CHECK(harness.call("__ZdlPv", object) == 0);
    CHECK(harness.call("__ZdaPv", array) == 0);
    // A delete of a pointer the bounded heap does not own fails closed.
    bool refused = false;
    try {
        harness.call("__ZdlPv", kDataBase + 4);
    } catch (const std::exception &error) {
        refused = std::string(error.what()).find("not a live bounded guest-heap allocation") !=
                  std::string::npos;
    }
    CHECK(refused);
    // A null delete is valid C++ and must not fail.
    CHECK(harness.call("__ZdlPv", 0) == 0);
}

void testUnregisteredSymbolsStillFailClosed() {
    Harness harness;
    CHECK(!harness.registry.resolve("_printf").has_value());
    CHECK(!harness.registry.resolve("_pthread_create").has_value());
    CHECK(harness.registry.resolve("_memcpy").has_value());
    // The stdio/file entry points the guest filesystem serves are registered;
    // what stays unregistered above keeps failing closed.
    CHECK(harness.registry.resolve("_fopen").has_value());
    CHECK(harness.registry.resolve("_fread").has_value());
    CHECK(harness.registry.resolve("_fclose").has_value());
    const auto snapshot = harness.registry.snapshot();
    std::size_t registered = 0;
    for (const auto &binding : snapshot) {
        if (binding.library == "libSystem.B.dylib")
            ++registered;
    }
    CHECK(registered == 44);
}

} // namespace

int main() {
    try {
        testMemoryAdapters();
        testStringAdapters();
        testSyntheticBundleFiles();
        testGuestHeapAllocator();
        testCxxOperatorAllocators();
        testUnregisteredSymbolsStillFailClosed();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "compat-runtime libsystem test failed: %s\n", error.what());
        return 1;
    }
    std::puts("compat-runtime libSystem C shim tests passed");
    return 0;
}
