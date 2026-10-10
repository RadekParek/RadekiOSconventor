#pragma once

#include "compat_runtime/cpu.hpp"
#include "compat_runtime/guest_memory.hpp"
#include "compat_runtime/shim_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace radek::compat_runtime::ndk {

/**
 * Bounded ARM32 callouts for the exact same-name NDK imports used by the
 * Angry Birds v1.0 fixture.
 *
 * Android's arm64 dlsym address cannot be called by an ARM32 guest directly.
 * This adapter therefore provides ABI-aware wrappers for the C/POSIX/math
 * surface and explicit no-unsafe-jump boundaries for process/thread control.
 * Existing libSystem, GLES, C++ ABI, and compiler-runtime adapters win when
 * they already own a symbol; this adapter fills only the remaining catalog.
 */
class ShimAdapter {
    using Invoke = std::function<bool(CpuRegisterState &, GuestAddressSpace &, std::string &)>;

    mutable std::mutex mutex_;
    std::uint32_t nextCallout_ = 0xf00c0000U;
    std::uint64_t calls_ = 0;
    std::uint64_t genericCalls_ = 0;
    std::size_t genericProviders_ = 0;
    std::size_t typedProviders_ = 0;
    std::size_t fixtureProviderCount_ = 0;
    std::size_t fixtureNonGenericProviderCount_ = 0;
    std::size_t fixtureGenericProviderCount_ = 0;
    std::uint32_t randomState_ = 1U;
    std::map<GuestAddressSpace *, GuestAddress> strtokCursor_;
    std::map<GuestAddressSpace *, GuestAddress> environCells_;
    std::map<GuestAddressSpace *, GuestAddress> errnoCells_;
    struct PthreadFrame {
        GuestAddress callerReturnAddress = 0;
        GuestAddress threadToken = 0;
        GuestAddress threadCell = 0;
        GuestAddress savedUnwindTop = 0;
        // The creator's registers at pthread_create. A worker that is ended
        // early can leave SP and the callee-saved registers pointing into its
        // own frames, so the continuation restores them from here.
        CpuRegisterState creatorRegisters{};
    };
    std::function<GuestAddress(GuestAddressSpace &)> beginWorkerUnwindChain_;
    std::function<void(GuestAddressSpace &, GuestAddress)> endWorkerUnwindChain_;
    std::map<GuestAddressSpace *, std::vector<PthreadFrame>> pthreadFrames_;
    std::map<GuestAddressSpace *, std::vector<GuestAddress>> completedPthreads_;
    std::uint64_t guestThreadTransfers_ = 0;
    std::uint64_t guestThreadCompletions_ = 0;
    GuestAddress pthreadContinuationAddress_ = 0;
    // Consecutive EBUSY answers given to a synchronously-run guest worker, and
    // how many workers were ended because of it (see noteWorkerMutexBusy).
    std::uint32_t workerMutexBusyStreak_ = 0;
    std::uint64_t guestThreadBusyCancellations_ = 0;

    /**
     * Single-CPU pthread scheduling runs a worker to completion inside
     * pthread_create, so a worker that waits on a mutex its suspended creator
     * holds can never make progress. After a bounded streak of EBUSY answers
     * from inside such a worker, end the worker through the normal pthread
     * continuation so the creator resumes. Returns true when it was ended.
     */
    bool noteWorkerMutexBusy(CpuRegisterState &registers, GuestAddressSpace &memory);

    void registerFunction(ShimRegistry &registry, const std::string &symbol,
                          const std::string &adapterName, Invoke invoke);
    void registerTransferFunction(
        ShimRegistry &registry, const std::string &symbol, const std::string &adapterName,
        std::function<bool(CpuRegisterState &, GuestAddressSpace &, GuestAddress &,
                           std::string &)> invoke);
    void registerGenericCandidate(ShimRegistry &registry, const char *symbol,
                                  const char *family);
    void registerExceptionBoundary(ShimRegistry &registry, const std::string &symbol,
                                   const std::string &adapterName, const std::string &reason);

  public:
    void registerBindings(ShimRegistry &registry);

    std::uint64_t callCount() const noexcept;
    std::uint64_t guestThreadTransferCount() const noexcept;
    std::uint64_t guestThreadCompletionCount() const noexcept;
    /** Synchronously-run guest workers ended after a bounded EBUSY streak. */
    std::uint64_t guestThreadBusyCancellationCount() const noexcept;

    /**
     * Connects guest workers to the SjLj unwind chain so each worker gets its
     * own chain and the creator's chain is restored when the worker ends.
     * Attach the same SjLjUnwindAdapter the shims were registered with.
     */
    void attachWorkerUnwindChain(std::function<GuestAddress(GuestAddressSpace &)> begin,
                                 std::function<void(GuestAddressSpace &, GuestAddress)> end);
    std::uint64_t genericCallCount() const noexcept;
    std::size_t genericProviderCount() const noexcept;
    std::size_t typedProviderCount() const noexcept;
    std::size_t fixtureProviderCount() const noexcept;
    std::size_t fixtureNonGenericProviderCount() const noexcept;
    std::size_t fixtureGenericProviderCount() const noexcept;
    std::size_t registeredCalloutCount() const noexcept;
};

} // namespace radek::compat_runtime::ndk
