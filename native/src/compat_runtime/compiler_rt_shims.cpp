// Concrete implementations of the compiler-runtime helpers a 32-bit ARM
// Darwin binary imports from libgcc_s.1.dylib.
//
// These are the functions GCC/Clang emit calls to when the target has no
// hardware integer divide (or when 64-bit integer conversions are needed), so
// "the guest called __modsi3" is ordinary compiled code, not a framework call.
// Their semantics are fixed by the ARM EABI (AAPCS argument passing:
// r0/r1 and r2/r3 for 64-bit values, d0/s0 for floating point, results back in
// the same registers), which is why they can be implemented exactly instead of
// being approximated:
//
//   * __divsi3/__modsi3/__udivsi3/__umodsi3  32-bit signed/unsigned
//   * __divdi3/__moddi3                      64-bit signed (r0:r1 pair args)
//   * __floatdidf/__floatdisf                int64 -> double/float
//   * __fixdfdi                              double -> int64 (saturating; a
//                                            value outside the range cannot be
//                                            represented, and the C cast is
//                                            undefined, so the helper clamps
//                                            and NaN maps to zero)
//
// Division by zero is undefined in C and the EABI helper returns zero; the
// `INT32_MIN / -1` overflow wraps to INT32_MIN. Both cases are counted like
// every other call so the boot report shows the guest's arithmetic path.
#include "compat_runtime/compiler_rt_shims.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace radek::compat_runtime::compiler_rt {
namespace {

constexpr GuestAddress kCalloutEnd = 0xf0012C00u;

std::int32_t divsi(std::int32_t numerator, std::int32_t denominator) {
    if (denominator == 0)
        return 0;
    if (numerator == std::numeric_limits<std::int32_t>::min() && denominator == -1)
        return std::numeric_limits<std::int32_t>::min();
    return numerator / denominator;
}

std::int32_t modsi(std::int32_t numerator, std::int32_t denominator) {
    if (denominator == 0)
        return 0;
    if (numerator == std::numeric_limits<std::int32_t>::min() && denominator == -1)
        return 0;
    return numerator % denominator;
}

std::uint32_t udivsi(std::uint32_t numerator, std::uint32_t denominator) {
    return denominator == 0 ? 0u : numerator / denominator;
}

std::uint32_t umodsi(std::uint32_t numerator, std::uint32_t denominator) {
    return denominator == 0 ? 0u : numerator % denominator;
}

std::int64_t divdi(std::int64_t numerator, std::int64_t denominator) {
    if (denominator == 0)
        return 0;
    if (numerator == std::numeric_limits<std::int64_t>::min() && denominator == -1)
        return std::numeric_limits<std::int64_t>::min();
    return numerator / denominator;
}

std::int64_t moddi(std::int64_t numerator, std::int64_t denominator) {
    if (denominator == 0)
        return 0;
    if (numerator == std::numeric_limits<std::int64_t>::min() && denominator == -1)
        return 0;
    return numerator % denominator;
}

std::int64_t fixdfdi(double value) {
    if (std::isnan(value))
        return 0;
    constexpr double kTwoTo63 = 9223372036854775808.0; // 2^63
    if (value >= kTwoTo63)
        return std::numeric_limits<std::int64_t>::max();
    if (value < -kTwoTo63)
        return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(value);
}

std::uint64_t readRegisterPair(const CpuRegisterState &state, int lowRegister) {
    return static_cast<std::uint64_t>(state.r[static_cast<std::size_t>(lowRegister)]) |
           (static_cast<std::uint64_t>(state.r[static_cast<std::size_t>(lowRegister + 1)]) << 32);
}

void writeRegisterPair(CpuRegisterState &state, int lowRegister, std::uint64_t value) {
    state.r[static_cast<std::size_t>(lowRegister)] = static_cast<std::uint32_t>(value);
    state.r[static_cast<std::size_t>(lowRegister + 1)] =
        static_cast<std::uint32_t>(value >> 32);
}

double doubleFromD0(const CpuRegisterState &state) {
    double value = 0.0;
    std::memcpy(&value, &state.d[0], sizeof(value));
    return value;
}

void writeD0(CpuRegisterState &state, double value) {
    std::memcpy(&state.d[0], &value, sizeof(value));
}

void writeS0(CpuRegisterState &state, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    state.d[0] = (state.d[0] & ~std::uint64_t{0xFFFFFFFFULL}) | bits;
}

} // namespace

std::size_t ShimAdapter::registeredSymbolCount() const { return registeredSymbols_; }

std::uint64_t ShimAdapter::callCount() const { return calls_; }

void ShimAdapter::registerBindings(ShimRegistry &registry) {
    auto bind = [this, &registry](const std::string &symbol, const char *adapter,
                                  std::function<bool(CpuRegisterState &)> invoke) {
        if (nextCallout_ >= kCalloutEnd)
            throw std::runtime_error("compiler-runtime callout window is exhausted");
        ShimBinding binding;
        binding.darwinSymbol = symbol;
        binding.library = "libgcc_s.1.dylib";
        binding.adapterName = adapter;
        binding.guestAddress = nextCallout_;
        nextCallout_ += 4;
        binding.invoke = [this, invoke](CpuRegisterState &registers, GuestAddressSpace &,
                                        std::string &reason) {
            ++calls_;
            (void)reason;
            return invoke(registers);
        };
        registry.registerBinding(std::move(binding));
        ++registeredSymbols_;
    };

    bind("___divsi3", "compiler-runtime-divsi3",
         [](CpuRegisterState &state) {
             state.r[0] = static_cast<std::uint32_t>(divsi(
                 static_cast<std::int32_t>(state.r[0]), static_cast<std::int32_t>(state.r[1])));
             return true;
         });
    bind("___modsi3", "compiler-runtime-modsi3",
         [](CpuRegisterState &state) {
             state.r[0] = static_cast<std::uint32_t>(modsi(
                 static_cast<std::int32_t>(state.r[0]), static_cast<std::int32_t>(state.r[1])));
             return true;
         });
    bind("___udivsi3", "compiler-runtime-udivsi3",
         [](CpuRegisterState &state) {
             state.r[0] = udivsi(state.r[0], state.r[1]);
             return true;
         });
    bind("___umodsi3", "compiler-runtime-umodsi3",
         [](CpuRegisterState &state) {
             state.r[0] = umodsi(state.r[0], state.r[1]);
             return true;
         });
    bind("___divdi3", "compiler-runtime-divdi3",
         [](CpuRegisterState &state) {
             const auto numerator = static_cast<std::int64_t>(readRegisterPair(state, 0));
             const auto denominator = static_cast<std::int64_t>(readRegisterPair(state, 2));
             writeRegisterPair(state, 0,
                               static_cast<std::uint64_t>(divdi(numerator, denominator)));
             return true;
         });
    bind("___moddi3", "compiler-runtime-moddi3",
         [](CpuRegisterState &state) {
             const auto numerator = static_cast<std::int64_t>(readRegisterPair(state, 0));
             const auto denominator = static_cast<std::int64_t>(readRegisterPair(state, 2));
             writeRegisterPair(state, 0,
                               static_cast<std::uint64_t>(moddi(numerator, denominator)));
             return true;
         });
    bind("___floatdidf", "compiler-runtime-floatdidf",
         [](CpuRegisterState &state) {
             const auto value = static_cast<std::int64_t>(readRegisterPair(state, 0));
             writeD0(state, static_cast<double>(value));
             return true;
         });
    bind("___floatdisf", "compiler-runtime-floatdisf",
         [](CpuRegisterState &state) {
             const auto value = static_cast<std::int64_t>(readRegisterPair(state, 0));
             writeS0(state, static_cast<float>(value));
             return true;
         });
    bind("___fixdfdi", "compiler-runtime-fixdfdi",
         [](CpuRegisterState &state) {
             writeRegisterPair(state, 0,
                               static_cast<std::uint64_t>(fixdfdi(doubleFromD0(state))));
             return true;
         });
}

} // namespace radek::compat_runtime::compiler_rt
