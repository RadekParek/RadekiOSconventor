// Darwin-only translation-layer coverage: the shims for names Android does not
// ship (the `__p` stream cells, the ctype sweep functions, the EAGL string keys,
// the errno accessor, the rune locale, the CoreFoundation string token and the
// OpenAL entry points) are exercised over real guest memory, and the symbol set
// is pinned so an accidental removal fails the suite.
#include "compat_runtime/darwin_compat_shims.hpp"
#include "compat_runtime/guest_memory.hpp"
#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/virtual_file_system.hpp"

#include <cstdint>
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

constexpr GuestAddress kDataBase = 0x10000;
constexpr GuestAddress kOut = 0x10100;

struct Harness {
    GuestAddressSpace memory{64U * 1024U * 1024U};
    ShimRegistry registry;
    objc::ShimAdapter objcShims;
    darwin_compat::ShimAdapter shims{&objcShims};

    Harness() {
        memory.mapAt(kDataBase, 0x1000, MemoryPermission::Read | MemoryPermission::Write,
                     "guest-data");
        objcShims.registerBindings(registry);
        shims.registerBindings(registry);
    }

    std::uint32_t call(const std::string &symbol, std::uint32_t r0 = 0, std::uint32_t r1 = 0,
                       std::uint32_t r2 = 0, std::uint32_t r3 = 0) {
        const auto binding = registry.resolve(symbol);
        if (!binding.has_value())
            throw std::runtime_error("symbol is not registered: " + symbol);
        CpuRegisterState registers;
        registers.r[0] = r0;
        registers.r[1] = r1;
        registers.r[2] = r2;
        registers.r[3] = r3;
        std::string reason;
        CHECK(registry.invokeCallout(binding->guestAddress, registers, memory, reason) ==
              GuestCalloutResult::Returned);
        return registers.r[0];
    }

    GuestAddress dataSymbol(const std::string &symbol) {
        const auto binding = registry.resolve(symbol);
        if (!binding.has_value() || !binding->resolveGuestAddress)
            throw std::runtime_error("data symbol is not registered: " + symbol);
        GuestAddress address = 0;
        std::string reason;
        CHECK(binding->resolveGuestAddress(memory, address, reason));
        CHECK(address != 0);
        return address;
    }

    std::uint32_t readWord(GuestAddress address) {
        std::uint8_t raw[4]{};
        CHECK(memory.read(address, raw, sizeof(raw)));
        return static_cast<std::uint32_t>(raw[0]) | (static_cast<std::uint32_t>(raw[1]) << 8) |
               (static_cast<std::uint32_t>(raw[2]) << 16) |
               (static_cast<std::uint32_t>(raw[3]) << 24);
    }
};

void testSymbolSetIsPinned() {
    Harness harness;
    // 32 of these are the Darwin-only imports of the Angry Birds v1.0 fixture:
    // __tolower/__toupper/__maskrune, __error, __stdinp/__stdoutp/__stderrp, the
    // four kEAGL keys, __DefaultRuneLocale, __CFConstantStringClassReference,
    // __gxx_personality_sj0 and the 19 OpenAL entry points it imports
    // (alSource3i is the one extra, bound for the general OpenAL subset).
    // 53 original symbols, plus 49 BioShock NDK-needs constants (44 NSString keys and
    // 5 raw-value constants: the CGRect/CGSize/CGAffineTransform values and the window level).
    CHECK(harness.shims.boundSymbolCount() == 102);
    for (const auto *symbol : {
             "___tolower", "___toupper", "___maskrune", "___error", "___stdinp", "___stdoutp",
             "___stderrp", "_kEAGLColorFormatRGB565", "_kEAGLColorFormatRGBA8",
             "_kEAGLDrawablePropertyColorFormat", "_kEAGLDrawablePropertyRetainedBacking",
             "__DefaultRuneLocale", "___CFConstantStringClassReference", "___gxx_personality_sj0",
             "_alGenBuffers", "_alDeleteBuffers", "_alGenSources", "_alDeleteSources",
             "_alSourcePlay", "_alSourceStop", "_alSourceQueueBuffers", "_alSourceUnqueueBuffers",
             "_alSourcei", "_alSourcef", "_alSource3f", "_alGetSourcei", "_alGetSourcef",
             "_alBufferData", "_alcOpenDevice", "_alcCloseDevice", "_alcCreateContext",
             "_alcDestroyContext", "_alcMakeContextCurrent"})
        CHECK(harness.registry.resolve(symbol).has_value());
}

void testBioShockConstantValues() {
    // IEEE-754 single-precision bit patterns of the values the SDK headers define.
    constexpr std::uint32_t kInfinity = 0x7f800000U;
    constexpr std::uint32_t kOne = 0x3f800000U;
    constexpr std::uint32_t kWindowLevelAlert = 0x44fa0000U; // 2000.0f
    Harness harness;
    const GuestAddress nullRect = harness.dataSymbol("_CGRectNull");
    CHECK(harness.readWord(nullRect) == kInfinity);
    CHECK(harness.readWord(nullRect + 4) == kInfinity);
    CHECK(harness.readWord(nullRect + 8) == 0);
    CHECK(harness.readWord(nullRect + 12) == 0);
    const GuestAddress identity = harness.dataSymbol("_CGAffineTransformIdentity");
    CHECK(harness.readWord(identity) == kOne);
    CHECK(harness.readWord(identity + 4) == 0);
    CHECK(harness.readWord(identity + 12) == kOne);
    CHECK(harness.readWord(harness.dataSymbol("_UIWindowLevelAlert")) == kWindowLevelAlert);
    CHECK(harness.readWord(harness.dataSymbol("_CGSizeZero")) == 0);
    // Same-name keys resolve to distinct NSString objects.
    CHECK(harness.dataSymbol("_NSFontAttributeName") != harness.dataSymbol("_NSForegroundColorAttributeName"));
}

void testCtypeSweepIsAsciiAndBounded() {
    Harness harness;
    CHECK(harness.call("___tolower", 'A') == 'a');
    CHECK(harness.call("___tolower", 'a') == 'a');
    CHECK(harness.call("___toupper", 'a') == 'A');
    CHECK(harness.call("___toupper", '7') == '7');
    // Non-ASCII bytes are returned unchanged: no locale tables are reproduced.
    CHECK(harness.call("___tolower", 0xc4) == 0xc4);
    CHECK(harness.call("___maskrune", 'a', 0x00000100) != 0);
    CHECK(harness.call("___maskrune", '1', 0x00008000) == 0);
    CHECK(harness.call("___maskrune", '0', 0x00000400) != 0);
    CHECK(harness.call("___maskrune", 0x00e9, 0x00000100) == 0);
    CHECK(harness.shims.ctypeCalls() == 9);
}

void testErrnoAndStandardStreamCells() {
    Harness harness;
    const auto first = harness.call("___error");
    CHECK(first != 0);
    CHECK(first == harness.call("___error"));
    CHECK(harness.memory.contains(first, 4, MemoryPermission::Read | MemoryPermission::Write));

    auto &files = guestFileSystem();
    const auto stdoutCell = harness.dataSymbol("___stdoutp");
    CHECK(harness.readWord(stdoutCell) ==
          files.standardStream(VirtualFileSystem::StandardStream::Output));
    const auto stdinCell = harness.dataSymbol("___stdinp");
    CHECK(harness.readWord(stdinCell) ==
          files.standardStream(VirtualFileSystem::StandardStream::Input));
    const auto stderrCell = harness.dataSymbol("___stderrp");
    CHECK(harness.readWord(stderrCell) ==
          files.standardStream(VirtualFileSystem::StandardStream::Error));
    // The cells carry real process-stream handles, so a guest write reaches the
    // stream the cell names.
    std::string detail;
    const auto stdoutHandle =
        files.standardStream(VirtualFileSystem::StandardStream::Output);
    CHECK(files.write(stdoutHandle, "", 0, detail) == 0);
    CHECK(harness.shims.streamCellCount() == 3);
}

void testStringConstantsAreDistinctGuestObjects() {
    Harness harness;
    std::vector<GuestAddress> objects;
    for (const auto *symbol : {
             "_AVCaptureSessionDidStartRunningNotification",
             "_AVCaptureSessionDidStopRunningNotification",
             "_AVCaptureSessionErrorKey",
             "_AVCaptureSessionInterruptionEndedNotification",
             "_AVCaptureSessionPreset640x480",
             "_AVCaptureSessionRuntimeErrorNotification",
             "_AVCaptureSessionWasInterruptedNotification",
             "_AVLayerVideoGravityResizeAspectFill",
             "_AVMediaTypeVideo",
             "_MPMoviePlayerLoadStateDidChangeNotification",
             "_MPMoviePlayerPlaybackDidFinishNotification",
             "_NSDefaultRunLoopMode",
             "_NSInvalidArgumentException",
             "_UIImagePickerControllerOriginalImage",
             "_UIKeyboardDidShowNotification",
             "_UIKeyboardWillShowNotification",
             "_kCAFillModeForwards",
             "_kCAMediaTimingFunctionLinear",
             "_kCVPixelBufferPixelFormatTypeKey",
         }) {
        const auto object = harness.dataSymbol(symbol);
        CHECK(object != 0);
        objects.push_back(object);
    }
    for (std::size_t index = 0; index < objects.size(); ++index)
        for (std::size_t other = index + 1; other < objects.size(); ++other)
            CHECK(objects[index] != objects[other]);
}

void testEaglKeysRuneAndConstantStringToken() {
    Harness harness;
    const auto rgb565 = harness.dataSymbol("_kEAGLColorFormatRGB565");
    const auto rgba8 = harness.dataSymbol("_kEAGLColorFormatRGBA8");
    CHECK(rgb565 != 0 && rgba8 != 0 && rgb565 != rgba8);
    const auto colorFormat = harness.dataSymbol("_kEAGLDrawablePropertyColorFormat");
    const auto retained = harness.dataSymbol("_kEAGLDrawablePropertyRetainedBacking");
    CHECK(colorFormat != 0 && retained != 0);
    CHECK(harness.dataSymbol("__DefaultRuneLocale") != 0);
    CHECK(harness.dataSymbol("___CFConstantStringClassReference") != 0);
}

void testOpenAlIsStateOnly() {
    Harness harness;
    // OpenAL entry points are `void`: the evidence is the generated names and
    // the state round-trip, not a return value.
    harness.call("_alGenBuffers", 2, kOut);
    CHECK(harness.readWord(kOut) == 1);
    CHECK(harness.readWord(kOut + 4) == 2);
    harness.call("_alGenSources", 1, kOut);
    const auto source = harness.readWord(kOut);
    CHECK(source != 0);
    harness.call("_alSourcei", source, 0x1007, 5);
    harness.call("_alGetSourcei", source, 0x1007, kOut + 8);
    CHECK(harness.readWord(kOut + 8) == 5);
    harness.call("_alSourcei", source, 0x1009, 3);
    harness.call("_alGetSourcei", source, 0x1009, kOut + 8);
    CHECK(harness.readWord(kOut + 8) == 3);
    const auto device = harness.call("_alcOpenDevice", 0);
    CHECK(device != 0);
    CHECK(harness.call("_alcCreateContext", device, 0) != 0);
    CHECK(harness.call("_alcMakeContextCurrent", device) == 1);
    CHECK(harness.call("_alcCloseDevice", device) == 1);
    CHECK(harness.shims.openalCalls() >= 10);
    CHECK(!harness.shims.diagnostics().empty());
}

void testPersonalityIsAFailClosedBoundary() {
    Harness harness;
    const auto binding = harness.registry.resolve("___gxx_personality_sj0");
    CHECK(binding.has_value());
    CpuRegisterState registers;
    registers.r[0] = 0x1234;
    std::string reason;
    CHECK(harness.registry.invokeCallout(binding->guestAddress, registers, harness.memory,
                                         reason) == GuestCalloutResult::ExceptionRaised);
    CHECK(reason.find("__gxx_personality_sj0") != std::string::npos);
    CHECK(harness.shims.personalityBoundaries() == 1);
}

} // namespace

int main() {
    testSymbolSetIsPinned();
    testCtypeSweepIsAsciiAndBounded();
    testErrnoAndStandardStreamCells();
    testEaglKeysRuneAndConstantStringToken();
    testBioShockConstantValues();
    testStringConstantsAreDistinctGuestObjects();
    testOpenAlIsStateOnly();
    testPersonalityIsAFailClosedBoundary();
    return 0;
}
