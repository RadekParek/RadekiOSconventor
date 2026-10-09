#pragma once

#include "compat_runtime/shim_registry.hpp"

#include <cstddef>
#include <cstdint>

namespace radek::compat_runtime::compiler_rt {

/**
 * Concrete compiler-runtime helpers (the "compiler-rt/libgcc toolchain
 * candidates" bucket of the importer triage).
 *
 * 32-bit ARM code built with GCC/Clang calls out to these helpers for integer
 * division/modulo and for 64-bit integer conversions; Darwin binaries import
 * them from libgcc_s.1.dylib. They are not framework APIs and not NDK exports,
 * but their ARM EABI semantics are fully documented, so the runtime implements
 * them exactly instead of binding them to an abort trap:
 *
 *   __divsi3  __modsi3  __udivsi3  __umodsi3      (32-bit)
 *   __divdi3  __moddi3                            (64-bit, r0:r1 / r2:r3)
 *   __floatdidf  __floatdisf  __fixdfdi           (int64 <-> double/float)
 *
 * Division by zero and `INT32_MIN / -1` cannot occur in well-defined C, so the
 * helpers use the ARM EABI behaviour (zero, and wrapping to `INT32_MIN`) rather
 * than letting the host trap. Every call is counted so the boot report can show
 * how much of the guest's arithmetic actually ran through them.
 *
 * The C++ exception runtime (`__cxa_*`, `__gxx_personality_sj0`) is deliberately
 * *not* implemented here: guest exception unwinding is a real execution model,
 * not a helper, and the runtime keeps failing those closed through traps.
 */
class ShimAdapter {
    std::uint64_t calls_ = 0;
    std::size_t registeredSymbols_ = 0;
    GuestAddress nextCallout_ = 0xf0012000;

  public:
    static constexpr const char *kAdapterPrefix = "compiler-runtime";

    void registerBindings(ShimRegistry &registry);

    /** Number of exact helper symbols registered in the guest callout registry. */
    std::size_t registeredSymbolCount() const;
    /** Number of helper calls served; observability, never gameplay evidence. */
    std::uint64_t callCount() const;
};

} // namespace radek::compat_runtime::compiler_rt
