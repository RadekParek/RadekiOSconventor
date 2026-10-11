#include "compat_runtime/compiler_rt_shims.hpp"
#include "compat_runtime/guest_memory.hpp"
#include "compat_runtime/guest_state.hpp"
#include "compat_runtime/shim_registry.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

using namespace radek::compat_runtime;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}

struct Harness {
    ShimRegistry registry;
    compiler_rt::ShimAdapter adapter;
    GuestAddressSpace memory;

    Harness() { adapter.registerBindings(registry); }

    CpuRegisterState call(const std::string &symbol, const CpuRegisterState &input) {
        const auto binding = registry.resolve(symbol);
        require(binding.has_value() && binding->invoke, "bound compiler-runtime symbol " + symbol);
        CpuRegisterState regs = input;
        std::string reason;
        require(binding->invoke(regs, memory, reason), symbol + " failed: " + reason);
        return regs;
    }
};

std::uint64_t pair(const CpuRegisterState &regs) {
    return static_cast<std::uint64_t>(regs.r[0]) | (static_cast<std::uint64_t>(regs.r[1]) << 32);
}

CpuRegisterState withFloatInS0(float value) {
    CpuRegisterState regs;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    regs.d[0] = bits;
    return regs;
}

CpuRegisterState withDoubleInD0(double value) {
    CpuRegisterState regs;
    std::memcpy(&regs.d[0], &value, sizeof(value));
    return regs;
}

CpuRegisterState withPair(std::uint64_t value) {
    CpuRegisterState regs;
    regs.r[0] = static_cast<std::uint32_t>(value);
    regs.r[1] = static_cast<std::uint32_t>(value >> 32);
    return regs;
}

float floatInS0(const CpuRegisterState &regs) {
    const auto bits = static_cast<std::uint32_t>(regs.d[0]);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void testFixSingleToSigned64() {
    Harness h;
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
    require(static_cast<std::int64_t>(pair(h.call("___fixsfdi", withFloatInS0(-3.7f)))) == -3,
            "fixsfdi truncates toward zero");
    require(pair(h.call("___fixsfdi", withFloatInS0(42.9f))) == 42, "fixsfdi positive");
    require(pair(h.call("___fixsfdi", withFloatInS0(std::nanf("")))) == 0, "fixsfdi NaN is zero");
    require(static_cast<std::int64_t>(pair(h.call("___fixsfdi", withFloatInS0(1e30f)))) == kMax,
            "fixsfdi saturates high");
    require(static_cast<std::int64_t>(pair(h.call("___fixsfdi", withFloatInS0(-1e30f)))) == kMin,
            "fixsfdi saturates low");
}

void testFixDoubleToUnsigned64() {
    Harness h;
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    require(pair(h.call("___fixunsdfdi", withDoubleInD0(-1.0))) == 0, "fixunsdfdi negative is zero");
    require(pair(h.call("___fixunsdfdi", withDoubleInD0(1099511627776.5))) == (1ULL << 40),
            "fixunsdfdi truncates");
    require(pair(h.call("___fixunsdfdi", withDoubleInD0(1e30))) == kMax, "fixunsdfdi saturates");
    require(pair(h.call("___fixunsdfdi", withDoubleInD0(std::nan("")))) == 0, "fixunsdfdi NaN is zero");
    require(pair(h.call("___fixunssfdi", withFloatInS0(3.0f))) == 3, "fixunssfdi");
    require(pair(h.call("___fixunssfdi", withFloatInS0(-2.0f))) == 0, "fixunssfdi negative is zero");
}

void testUnsigned64ToSingle() {
    Harness h;
    require(floatInS0(h.call("___floatundisf", withPair(5))) == 5.0f, "floatundisf small value");
    const float expected = static_cast<float>(std::numeric_limits<std::uint64_t>::max());
    require(floatInS0(h.call("___floatundisf", withPair(std::numeric_limits<std::uint64_t>::max()))) ==
                expected,
            "floatundisf max value rounds to nearest float");
}

} // namespace

int main() {
    const std::pair<const char *, void (*)()> tests[] = {
        {"fix single to signed 64", testFixSingleToSigned64},
        {"fix to unsigned 64", testFixDoubleToUnsigned64},
        {"unsigned 64 to single", testUnsigned64ToSingle},
    };
    int failures = 0;
    for (const auto &[name, body] : tests) {
        try {
            body();
        } catch (const std::exception &error) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", name, error.what());
        }
    }
    if (failures == 0)
        std::printf("compiler-runtime shims: %zu tests passed\n", std::size(tests));
    return failures == 0 ? 0 : 1;
}
