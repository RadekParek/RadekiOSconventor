#pragma once

#include "compat_runtime/cpu.hpp"
#include "compat_runtime/macho_loader.hpp"
#include "compat_runtime/shim_registry.hpp"
#include "compat_runtime/trap_shims.hpp"
#include "json.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace radek::compat_runtime {

/** Loads one authorized IPA main executable and reports its first runtime blocker. */
class GuestRunner {
    const ShimRegistry &shims_;
    const CpuBackend &cpu_;
    std::size_t memoryLimit_;

  public:
    GuestRunner(const ShimRegistry &shims, const CpuBackend &cpu,
                std::size_t memoryLimit = 256U * 1024U * 1024U)
        : shims_(shims), cpu_(cpu), memoryLimit_(memoryLimit) {}

    radek::Json runMainBinary(const std::vector<std::uint8_t> &mainBinary,
                              bool authorizationConfirmed) const;
};

/**
 * Optional bounded application-lifecycle hooks for the boot attempt.
 *
 * A boot attempt that gets past `_UIApplicationMain` may leave a queued
 * background-thread entry (an `NSThread` detach) that the runtime cannot run as
 * a second host thread. These hooks let the caller describe the lifecycle trace
 * and hand the runner an ABI-correct entry point for that queued body so it can
 * be executed on the same single guest CPU.
 */
struct BootLifecycleHooks {
    std::function<bool(GuestAddressSpace &, CpuRegisterState &, GuestAddress &, std::string &)>
        prepareMainThreadEntry;
    std::function<void(GuestAddressSpace &, std::uint32_t frames)> setMainThreadServiceLimit;
    std::function<void(GuestAddressSpace &)> finishMainThreadEntry;
    std::function<void(GuestAddressSpace &, radek::Json &report)> describe;
};

/**
 * Boot-attempt runner: binds unimplemented imports to abort-on-call traps
 * and executes real guest instructions until the first actually-used missing
 * import. The registry gains trap bindings during the load. The report keeps
 * status "not_runnable" (a boot attempt is never gameplay evidence) and
 * carries the executed-instruction count plus the stopping import.
 */
class BootAttemptRunner {
    ShimRegistry &shims_;
    const CpuBackend &cpu_;
    TrapShimAdapter &traps_;
    std::size_t memoryLimit_;
    BootLifecycleHooks lifecycle_;
    // Zero means unlimited. The Android game launcher uses the default so the
    // guest remains alive for gameplay; the host probe opts into a finite time
    // window explicitly because a command-line diagnostic must eventually
    // return a JSON report when a guest enters an infinite game loop.
    std::uint64_t entryInstructionBudget_ = 0;
    std::uint64_t entryTimeLimitMicros_ = 0;
    std::uint64_t mainThreadInstructionBudget_ = 0;
    std::uint64_t mainThreadTimeLimitMicros_ = 0;

  public:
    BootAttemptRunner(ShimRegistry &shims, const CpuBackend &cpu,
                      TrapShimAdapter &traps,
                      std::size_t memoryLimit = 256U * 1024U * 1024U)
        : shims_(shims), cpu_(cpu), traps_(traps), memoryLimit_(memoryLimit) {}

    BootAttemptRunner(ShimRegistry &shims, const CpuBackend &cpu,
                      TrapShimAdapter &traps, BootLifecycleHooks lifecycle,
                      std::size_t memoryLimit = 256U * 1024U * 1024U)
        : shims_(shims), cpu_(cpu), traps_(traps), memoryLimit_(memoryLimit),
          lifecycle_(std::move(lifecycle)) {}

    /**
     * Set an optional diagnostic budget for the Mach-O entry point. A zero
     * instruction count and zero time value mean unlimited execution. The
     * Android game APK leaves both at zero; only the host probe uses this hook
     * to keep a command-line report from hanging forever in a real game loop.
     */
    void setEntryBudget(std::uint64_t instructions, std::uint64_t timeLimitMicros) noexcept {
        entryInstructionBudget_ = instructions;
        entryTimeLimitMicros_ = timeLimitMicros;
    }

    /** Set an optional diagnostic budget for the queued background-thread body. */
    void setMainThreadInstructionBudget(std::uint64_t instructions,
                                        std::uint64_t timeLimitMicros) noexcept {
        mainThreadInstructionBudget_ = instructions;
        mainThreadTimeLimitMicros_ = timeLimitMicros;
    }

    radek::Json run(const std::vector<std::uint8_t> &mainBinary,
                    bool authorizationConfirmed);
};

} // namespace radek::compat_runtime
