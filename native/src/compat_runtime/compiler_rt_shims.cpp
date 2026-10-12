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

// Saturating conversions as in libgcc: NaN converts to 0, out-of-range values clamp.
std::int64_t fixsfdi(float value) {
    return fixdfdi(static_cast<double>(value));
}

std::uint64_t fixunsdfdi(double value) {
    if (std::isnan(value) || value <= 0.0)
        return 0;
    constexpr double kTwoTo64 = 18446744073709551616.0; // 2^64
    if (value >= kTwoTo64)
        return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(value);
}

std::uint64_t fixunssfdi(float value) {
    return fixunsdfdi(static_cast<double>(value));
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

float floatFromSRegister(const CpuRegisterState &state, int index) {
    const std::uint64_t word = state.d[static_cast<std::size_t>(index / 2)];
    const auto bits = static_cast<std::uint32_t>(word >> ((index % 2) * 32));
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void writeSRegister(CpuRegisterState &state, int index, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const int shift = (index % 2) * 32;
    auto &word = state.d[static_cast<std::size_t>(index / 2)];
    word = (word & ~(std::uint64_t{0xFFFFFFFFu} << shift)) | (std::uint64_t{bits} << shift);
}

double doubleFromDRegister(const CpuRegisterState &state, int index) {
    double value = 0.0;
    std::memcpy(&value, &state.d[static_cast<std::size_t>(index)], sizeof(value));
    return value;
}

void writeDRegister(CpuRegisterState &state, int index, double value) {
    std::memcpy(&state.d[static_cast<std::size_t>(index)], &value, sizeof(value));
}

// libgcc comparison conventions: __eqsf2/__nesf2 return 0 only when ordered and
// equal (so unordered yields 1); __gesf2/__gtsf2 return -1 when unordered;
// __lesf2/__ltsf2 return 1 when unordered.
template <typename T>
std::int32_t floatCompare(T lhs, T rhs, std::int32_t unorderedResult) {
    if (std::isnan(lhs) || std::isnan(rhs))
        return unorderedResult;
    if (lhs < rhs)
        return -1;
    if (lhs > rhs)
        return 1;
    return 0;
}

template <typename T>
std::int32_t floatNotEqual(T lhs, T rhs) {
    return lhs == rhs ? 0 : 1;
}

std::int32_t fixFloatToSigned(double value) {
    if (std::isnan(value))
        return 0;
    if (value >= 2147483648.0)
        return std::numeric_limits<std::int32_t>::max();
    if (value < -2147483648.0)
        return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(value);
}

std::uint32_t fixFloatToUnsigned(double value) {
    if (std::isnan(value) || value <= 0.0)
        return 0u;
    if (value >= 4294967296.0)
        return std::numeric_limits<std::uint32_t>::max();
    return static_cast<std::uint32_t>(value);
}

std::uint64_t umuldi(std::uint64_t lhs, std::uint64_t rhs) { return lhs * rhs; }

std::uint64_t udivdi(std::uint64_t numerator, std::uint64_t denominator) {
    return denominator == 0 ? 0u : numerator / denominator;
}

std::uint64_t umoddi(std::uint64_t numerator, std::uint64_t denominator) {
    return denominator == 0 ? 0u : numerator % denominator;
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
    bind("___fixsfdi", "compiler-runtime-fixsfdi",
         [](CpuRegisterState &state) {
             writeRegisterPair(state, 0,
                               static_cast<std::uint64_t>(fixsfdi(floatFromSRegister(state, 0))));
             return true;
         });
    bind("___fixunsdfdi", "compiler-runtime-fixunsdfdi",
         [](CpuRegisterState &state) {
             writeRegisterPair(state, 0, fixunsdfdi(doubleFromD0(state)));
             return true;
         });
    bind("___fixunssfdi", "compiler-runtime-fixunssfdi",
         [](CpuRegisterState &state) {
             writeRegisterPair(state, 0, fixunssfdi(floatFromSRegister(state, 0)));
             return true;
         });
    bind("___floatundisf", "compiler-runtime-floatundisf",
         [](CpuRegisterState &state) {
             writeS0(state, static_cast<float>(readRegisterPair(state, 0)));
             return true;
         });
    bind("___fixsfsivfp", "compiler-runtime-fixsfsivfp",
         [](CpuRegisterState &state) {
             state.r[0] = static_cast<std::uint32_t>(
                 fixFloatToSigned(static_cast<double>(floatFromSRegister(state, 0))));
             return true;
         });
    bind("___fixunsdfsivfp", "compiler-runtime-fixunsdfsivfp",
         [](CpuRegisterState &state) {
             state.r[0] = fixFloatToUnsigned(doubleFromDRegister(state, 0));
             return true;
         });
    bind("___floatsidfvfp", "compiler-runtime-floatsidfvfp",
         [](CpuRegisterState &state) {
             writeDRegister(state, 0, static_cast<double>(static_cast<std::int32_t>(state.r[0])));
             return true;
         });
    bind("___floatsisfvfp", "compiler-runtime-floatsisfvfp",
         [](CpuRegisterState &state) {
             writeSRegister(state, 0, static_cast<float>(static_cast<std::int32_t>(state.r[0])));
             return true;
         });
    bind("___floatunssidfvfp", "compiler-runtime-floatunssidfvfp",
         [](CpuRegisterState &state) {
             writeDRegister(state, 0, static_cast<double>(state.r[0]));
             return true;
         });
    bind("___floatunssisfvfp", "compiler-runtime-floatunssisfvfp",
         [](CpuRegisterState &state) {
             writeSRegister(state, 0, static_cast<float>(state.r[0]));
             return true;
         });
    bind("___floatundidf", "compiler-runtime-floatundidf",
         [](CpuRegisterState &state) {
             writeDRegister(state, 0, static_cast<double>(readRegisterPair(state, 0)));
             return true;
         });
    bind("___muldi3", "compiler-runtime-muldi3",
         [](CpuRegisterState &state) {
             writeRegisterPair(state, 0,
                               umuldi(readRegisterPair(state, 0), readRegisterPair(state, 2)));
             return true;
         });
    bind("___udivdi3", "compiler-runtime-udivdi3",
         [](CpuRegisterState &state) {
             writeRegisterPair(state, 0,
                               udivdi(readRegisterPair(state, 0), readRegisterPair(state, 2)));
             return true;
         });
    bind("___umoddi3", "compiler-runtime-umoddi3",
         [](CpuRegisterState &state) {
             writeRegisterPair(state, 0,
                               umoddi(readRegisterPair(state, 0), readRegisterPair(state, 2)));
             return true;
         });

    // VFP soft-float arithmetic and comparisons: operands in s0/s1 or d0/d1.
    bind("___addsf3vfp", "compiler-runtime-addsf3vfp", [](CpuRegisterState &state) {
        writeSRegister(state, 0, floatFromSRegister(state, 0) + floatFromSRegister(state, 1));
        return true;
    });
    bind("___subsf3vfp", "compiler-runtime-subsf3vfp", [](CpuRegisterState &state) {
        writeSRegister(state, 0, floatFromSRegister(state, 0) - floatFromSRegister(state, 1));
        return true;
    });
    bind("___mulsf3vfp", "compiler-runtime-mulsf3vfp", [](CpuRegisterState &state) {
        writeSRegister(state, 0, floatFromSRegister(state, 0) * floatFromSRegister(state, 1));
        return true;
    });
    bind("___divsf3vfp", "compiler-runtime-divsf3vfp", [](CpuRegisterState &state) {
        writeSRegister(state, 0, floatFromSRegister(state, 0) / floatFromSRegister(state, 1));
        return true;
    });
    bind("___adddf3vfp", "compiler-runtime-adddf3vfp", [](CpuRegisterState &state) {
        writeDRegister(state, 0, doubleFromDRegister(state, 0) + doubleFromDRegister(state, 1));
        return true;
    });
    bind("___subdf3vfp", "compiler-runtime-subdf3vfp", [](CpuRegisterState &state) {
        writeDRegister(state, 0, doubleFromDRegister(state, 0) - doubleFromDRegister(state, 1));
        return true;
    });
    bind("___muldf3vfp", "compiler-runtime-muldf3vfp", [](CpuRegisterState &state) {
        writeDRegister(state, 0, doubleFromDRegister(state, 0) * doubleFromDRegister(state, 1));
        return true;
    });
    bind("___divdf3vfp", "compiler-runtime-divdf3vfp", [](CpuRegisterState &state) {
        writeDRegister(state, 0, doubleFromDRegister(state, 0) / doubleFromDRegister(state, 1));
        return true;
    });
    bind("___extendsfdf2vfp", "compiler-runtime-extendsfdf2vfp", [](CpuRegisterState &state) {
        writeDRegister(state, 0, static_cast<double>(floatFromSRegister(state, 0)));
        return true;
    });
    bind("___truncdfsf2vfp", "compiler-runtime-truncdfsf2vfp", [](CpuRegisterState &state) {
        writeSRegister(state, 0, static_cast<float>(doubleFromDRegister(state, 0)));
        return true;
    });
    bind("___eqsf2vfp", "compiler-runtime-eqsf2vfp", [](CpuRegisterState &state) {
        state.r[0] = static_cast<std::uint32_t>(floatNotEqual(floatFromSRegister(state, 0),
                                                              floatFromSRegister(state, 1)));
        return true;
    });
    bind("___nesf2vfp", "compiler-runtime-nesf2vfp", [](CpuRegisterState &state) {
        state.r[0] = static_cast<std::uint32_t>(floatNotEqual(floatFromSRegister(state, 0),
                                                              floatFromSRegister(state, 1)));
        return true;
    });
    bind("___gtsf2vfp", "compiler-runtime-gtsf2vfp", [](CpuRegisterState &state) {
        state.r[0] = static_cast<std::uint32_t>(floatCompare(floatFromSRegister(state, 0),
                                                             floatFromSRegister(state, 1), -1));
        return true;
    });
    bind("___gedf2vfp", "compiler-runtime-gedf2vfp", [](CpuRegisterState &state) {
        state.r[0] = static_cast<std::uint32_t>(floatCompare(doubleFromDRegister(state, 0),
                                                             doubleFromDRegister(state, 1), -1));
        return true;
    });
    bind("___gtdf2vfp", "compiler-runtime-gtdf2vfp", [](CpuRegisterState &state) {
        state.r[0] = static_cast<std::uint32_t>(floatCompare(doubleFromDRegister(state, 0),
                                                             doubleFromDRegister(state, 1), -1));
        return true;
    });
    bind("___lesf2vfp", "compiler-runtime-lesf2vfp", [](CpuRegisterState &state) {
        state.r[0] = static_cast<std::uint32_t>(floatCompare(floatFromSRegister(state, 0),
                                                             floatFromSRegister(state, 1), 1));
        return true;
    });
    bind("___ltsf2vfp", "compiler-runtime-ltsf2vfp", [](CpuRegisterState &state) {
        state.r[0] = static_cast<std::uint32_t>(floatCompare(floatFromSRegister(state, 0),
                                                             floatFromSRegister(state, 1), 1));
        return true;
    });
    bind("___nedf2vfp", "compiler-runtime-nedf2vfp", [](CpuRegisterState &state) {
        state.r[0] = static_cast<std::uint32_t>(floatNotEqual(doubleFromDRegister(state, 0),
                                                              doubleFromDRegister(state, 1)));
        return true;
    });
    bind("___unordsf2vfp", "compiler-runtime-unordsf2vfp", [](CpuRegisterState &state) {
        state.r[0] = std::isnan(floatFromSRegister(state, 0)) ||
                             std::isnan(floatFromSRegister(state, 1))
                         ? 1u
                         : 0u;
        return true;
    });
}

} // namespace radek::compat_runtime::compiler_rt
