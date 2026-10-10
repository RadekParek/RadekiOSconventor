#pragma once

#include "compat_runtime/objc_runtime.hpp"
#include "compat_runtime/runner.hpp"
#include "compat_runtime/shim_registry.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace radek::compat_runtime::objc {

/**
 * Result of the application-lifecycle ("startup chain") adapters.
 *
 * The runtime drives `UIApplicationMain` far enough to instantiate the app's
 * delegate, deliver `applicationDidFinishLaunching:`, and service queued
 * background-thread work on the single guest CPU. A zero frame limit means the
 * service is unlimited (the Android game path); a non-zero value is reserved
 * for an explicitly bounded host diagnostic. None of these counters is a
 * gameplay claim: they describe the lifecycle events that were observed.
 */
struct LifecycleOutcome {
    bool applicationMainEntered = false;
    bool applicationMainReturned = false;
    std::string delegateClassName;
    std::uint32_t mainThreadFramesServiced = 0;
    std::uint32_t mainThreadFramesLimit = 0;
    bool mainThreadQueueExhausted = false;
    // Run loop / frame pump: how many sources were serviced, whether the loop
    // is still running, and whether it has returned to its caller.
    std::uint32_t runLoopIterations = 0;
    std::uint32_t runLoopPendingSources = 0;
    bool runLoopRunning = false;
    bool runLoopExited = false;
    std::vector<std::string> events;
};

/**
 * Bounded guest-facing Objective-C core adapters.
 *
 * Class and object pointers are materialized in the guest address space, while
 * selectors and method dispatch are backed by the host-testable Runtime. This
 * is not a complete Apple Objective-C ABI or framework implementation.
 */
class ShimAdapter {
    // The host reaches the running guest through these free functions rather
    // than by calling into guest code re-entrantly.
    friend void configureActiveGuestViewport(std::uint32_t width, std::uint32_t height);
    friend bool postTouchToActiveGuest(float x, float y, const char *phase);

    struct PropertyCopyContinuation {
        GuestAddress receiver = 0;
        std::int32_t offset = 0;
        GuestAddress continuationStackPointer = 0;
        GuestAddress originalStackPointer = 0;
        std::uint32_t originalStackWord = 0;
        GuestAddress originalReturnAddress = 0;
        bool atomic = false;
    };

    // A native -> guest nested call (UIApplicationMain delivering a lifecycle
    // message, performSelectorOnMainThread:, ...). The frame remembers where the
    // interrupted shim callout must resume: the continuation callout restores
    // LR and returns control to the guest caller.
    struct LifecycleFrame {
        std::uint32_t kind = 0;
        GuestAddress callerReturnAddress = 0;
        GuestAddress receiver = 0;
    };

    struct LifecycleState {
        bool applicationMainEntered = false;
        bool applicationMainReturned = false;
        std::string delegateClassName;
        GuestAddress application = 0;
        GuestAddress delegate = 0;
        GuestAddress keyWindow = 0;
        GuestAddress screen = 0;
        GuestAddress accelerometer = 0;
        GuestAddress bundle = 0;
        GuestAddress mainThread = 0;
        GuestAddress backgroundThread = 0;
        // Views and their Core Animation layers. A bounded adapter still keeps
        // one layer per view so two views cannot alias.
        std::map<GuestAddress, GuestAddress> viewLayers;
        // `mutationsPtr` handed to guest fast enumeration: one zeroed word that
        // never changes, because the bounded arrays are never mutated in place.
        GuestAddress enumerationMutations = 0;
        std::uint32_t statusBarOrientation = 1;
        std::uint32_t statusBarStyle = 0;
        bool statusBarHidden = false;
        bool idleTimerDisabled = false;
        double accelerometerInterval = 0.0;
        GuestAddress accelerometerDelegate = 0;
        GuestAddress currentContext = 0;
        GuestAddress layerClass = 0;
        // Main-thread service: the guest game thread asks the runtime to run a
        // selector on the main thread; the adapter relays it into guest code and
        // counts serviced calls. A zero limit leaves it running indefinitely for
        // the device game path; host diagnostics may opt into a finite limit.
        std::uint32_t mainThreadFramesServiced = 0;
        std::uint32_t mainThreadFramesLimit = 0;
        bool mainThreadCancelled = false;
        std::uint32_t sleepCount = 0;
        // Set while the runner executes the queued background-thread body, so
        // `+[NSThread currentThread]` answers the thread the guest is on.
        bool mainThreadEntryRunning = false;
        bool queuedMainThreadEntry = false;
        GuestAddress queuedMainThreadTarget = 0;
        Selector queuedMainThreadSelector = 0;
        GuestAddress queuedMainThreadArgument = 0;

        // ---- run loop (the frame pump) -----------------------------------
        // `UIApplicationMain` never returns on a real device: it enters
        // CFRunLoop and keeps servicing the app's timers and delayed
        // selectors, which is what makes a game draw a *second* frame. The
        // bounded adapter therefore keeps the sources the guest scheduled and
        // re-enters the guest once per ready source instead of returning to
        // `main()` after `applicationDidFinishLaunching:`.
        struct RunLoopSource {
            enum class Kind { Timer, DelayedSelector, Touch };
            Kind kind = Kind::Timer;
            GuestAddress target = 0;    // timer target, or receiver of a delayed selector
            Selector selector = 0;
            GuestAddress argument = 0;
            // The NSTimer object itself, so `invalidate` can find the source it
            // owns (the target is the delegate, which may own several timers).
            GuestAddress timerObject = 0;
            double intervalSeconds = 0.0;
            double nextFireSeconds = 0.0;
            bool repeats = false;
            // Touch sources carry the host event; the run loop delivers them to
            // the key window's view, since only the pumping run loop may enter
            // guest code.
            std::string touchPhase;     // "touchesBegan", "touchesMoved", "touchesEnded"
            float touchX = 0.0f;
            float touchY = 0.0f;
        };
        std::vector<RunLoopSource> runLoopSources;
        bool runLoopRunning = false;
        bool runLoopEntered = false;
        bool runLoopStopped = false;
        // Virtual run-loop clock. Each serviced source advances it to that
        // source's fire time, so a repeating 1/60 s timer advances time like a
        // real display link would instead of firing instantly forever.
        double runLoopClockSeconds = 0.0;
        std::uint32_t runLoopIterations = 0;
        // Zero means unlimited, which is what the device game path needs: a
        // running game must not be stopped after an arbitrary frame count.
        std::uint32_t runLoopIterationLimit = 0;
        std::uint32_t nextTimerToken = 1;
        // Return address of whoever called CFRunLoopRun. The loop keeps it for
        // its whole lifetime and only returns there once it has no sources.
        GuestAddress runLoopCallerReturn = 0;
        // `addSubview:` bookkeeping, so an input event can be delivered to the
        // view that actually covers the touch instead of to the window.
        std::map<GuestAddress, std::vector<GuestAddress>> subviews;

        // ---- host viewport -----------------------------------------------
        // The Android surface size handed down by the launcher. It is the
        // fallback rectangle for a drawable whose frame the guest never
        // materialized, and it is what `-[UIScreen bounds]`/`applicationFrame`
        // report so a landscape game is not given a portrait rectangle.
        std::uint32_t viewportWidth = 0;
        std::uint32_t viewportHeight = 0;
        bool viewportConfigured = false;

        std::vector<LifecycleFrame> frames;
        std::vector<std::string> events;
    };

    struct GuestState {
        std::map<const Class *, GuestAddress> classAddresses;
        std::map<GuestAddress, const Class *> classesByAddress;
        std::map<Object *, GuestAddress> objectAddresses;
        std::map<GuestAddress, Object *> objectsByAddress;
        std::map<Selector, GuestAddress> selectorAddresses;
        std::map<GuestAddress, std::string> protocolNames;
        std::map<std::string, GuestAddress> protocolAddresses;
        std::map<std::string, std::vector<std::string>> protocolParents;
        std::map<std::uint32_t, PropertyCopyContinuation> pendingPropertyCopies;
        LifecycleState lifecycle;
    };

    struct AutoreleasePoolState {
        Object *poolObject = nullptr;
        std::vector<Object *> objects;
    };

    struct GuestImplementation {
        GuestAddress address = 0;
        std::string typeEncoding;
    };

    Runtime runtime_;
    Class *rootClass_ = nullptr;
    Class *autoreleasePoolClass_ = nullptr;
    std::map<std::string, Class *> classes_;
    std::map<const GuestAddressSpace *, std::unique_ptr<GuestState>> guestStates_;
    std::map<Selector, std::string> selectorNames_;
    std::map<std::string, Selector> selectorIds_;
    std::map<std::pair<const Class *, Selector>, GuestImplementation> guestImplementations_;
    std::map<GuestAddress, AutoreleasePoolState> autoreleasePools_;
    std::vector<GuestAddress> activeAutoreleasePools_;
    std::map<const GuestAddressSpace *, GuestAddress> emptyDataAddresses_;
    std::mutex mutex_;
    std::mutex propertyMutex_;
    std::uint32_t nextCallout_ = 0xf0004000;
    std::uint32_t nextPropertyCopyToken_ = 1;
    GuestAddress propertyCopyContinuationAddress_ = 0;
    GuestAddress lifecycleContinuationAddress_ = 0;
    bool registered_ = false;

    GuestState &guestState(GuestAddressSpace &memory);
    GuestAddress ensureClassAddress(GuestAddressSpace &memory, const Class *klass);
    GuestAddress ensureObjectAddress(GuestAddressSpace &memory, Object *object);
    std::string readGuestString(const GuestAddressSpace &memory, GuestAddress address) const;
    Selector selectorForGuest(GuestAddressSpace &memory, GuestAddress address);
    GuestAddress emptyDataAddress(GuestAddressSpace &memory);
    GuestAddress selectorStringAddress(GuestAddressSpace &memory, Selector selector);
    bool autoreleasePoolPush(CpuRegisterState &registers, GuestAddressSpace &memory,
                             std::string &reason);
    bool autoreleasePoolPop(CpuRegisterState &registers, GuestAddressSpace &memory,
                            std::string &reason);
    bool initializeImage(GuestAddressSpace &memory,
                         const std::vector<GuestImageSection> &sections,
                         std::string &reason);
    // --- application lifecycle ("startup chain") -------------------------
    GuestAddress lifecycleFrameworkObject(GuestAddressSpace &memory,
                                          const std::string &className,
                                          GuestAddress &slot);
    bool applicationMain(CpuRegisterState &registers, GuestAddressSpace &memory,
                         GuestAddress &guestTarget, std::string &reason);
    /**
     * Resumes the guest after a nested callout returns. When `guestTarget` is
     * supplied and the run loop has another ready source, the continuation
     * hands control straight to that guest callback instead of returning -
     * which is what keeps a game drawing frame after frame.
     */
    bool lifecycleContinuation(CpuRegisterState &registers, GuestAddressSpace &memory,
                               std::string &reason, GuestAddress *guestTarget = nullptr);
    bool lifecycleSelector(CpuRegisterState &registers, GuestAddressSpace &memory,
                           const std::string &selectorName, GuestAddress receiverAddress,
                           Object *receiverObject, const Class *receiverClass, Value *rawReturn,
                           GuestAddress *guestTarget, std::string &reason);
    // Bounded message groups for the startup chain. Each group returns true
    // when it produced the result for the selector and false when the selector
    // is outside that group (the caller then fails closed with a named
    // diagnostic instead of guessing a signature).
    bool lifecycleFoundationMessage(CpuRegisterState &registers, GuestAddressSpace &memory,
                                    const std::string &selectorName, GuestAddress receiverAddress,
                                    Object *receiverObject, Value *rawReturn, std::string &reason);
    bool lifecycleFoundationClassMessage(CpuRegisterState &registers, GuestAddressSpace &memory,
                                         const Class *receiverClass,
                                         const std::string &selectorName, Value *rawReturn,
                                         std::string &reason);
    bool lifecycleThreadMessage(CpuRegisterState &registers, GuestAddressSpace &memory,
                                const std::string &selectorName, GuestAddress receiverAddress,
                                Object *receiverObject, Value *rawReturn);
    bool lifecycleViewMessage(CpuRegisterState &registers, GuestAddressSpace &memory,
                             const std::string &selectorName, GuestAddress receiverAddress,
                             Object *receiverObject, Value *rawReturn, std::string &reason);
    GuestAddress lifecycleLayerForView(GuestAddressSpace &memory, GuestAddress view,
                                       Object *viewObject);
    /**
     * Thread object the guest is running on. Only one guest thread executes at a
     * time, so this is the main-thread object outside the queued background
     * entry and the background-thread object while the runner services it.
     */
    GuestAddress lifecycleCurrentThreadAddress(GuestAddressSpace &memory);

    /**
     * Publishes the host surface size (the Android window) to the guest. Call
     * before the boot runs so `-[UIScreen bounds]`/`applicationFrame` and the
     * EAGL drawable use the real rectangle instead of a hardcoded portrait one.
     */
    void configureViewport(GuestAddressSpace &memory, std::uint32_t width,
                           std::uint32_t height);
    /** Drawable rectangle: the host viewport when the guest set no frame. */
    void effectiveViewport(const GuestAddressSpace &memory, std::uint32_t &width,
                           std::uint32_t &height) const;

    /**
     * `CFRunLoopRun` / `CFRunLoopRunInMode`. Enters the run loop: each ready
     * source re-enters the guest through the same nested-callout machinery
     * `UIApplicationMain` uses, so a game keeps drawing frames instead of
     * returning from `main()` after its startup callback.
     */
    bool runLoopRun(CpuRegisterState &registers, GuestAddressSpace &memory,
                    GuestAddress &guestTarget, std::string &reason);
    /** Stops the run loop (`CFRunLoopStop`). */
    void runLoopStop(GuestAddressSpace &memory);
    /**
     * Services one ready source and, when there is one, redirects execution
     * into that guest callback. Leaves `guestTarget` at 0 when the queue is
     * empty, which is the loop's signal to exit.
     */
    bool serviceRunLoop(CpuRegisterState &registers, GuestAddressSpace &memory,
                        GuestAddress &guestTarget, std::string &reason);

    /** `+[NSTimer scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:]`. */
    GuestAddress scheduleTimer(GuestAddressSpace &memory, double intervalSeconds,
                               GuestAddress target, Selector selector, GuestAddress argument,
                               bool repeats);
    /** `-[NSObject performSelector:withObject:afterDelay:]` family. */
    void scheduleDelayedSelector(GuestAddressSpace &memory, GuestAddress target,
                                 Selector selector, GuestAddress argument, double delaySeconds);
    /** Queues a host touch event for the run loop to deliver to the guest view. */
    void postTouch(GuestAddressSpace &memory, const std::string &phase, float x, float y);
    /** Cancels every scheduled source aimed at `target` (`-[NSTimer invalidate]`). */
    void cancelRunLoopSourcesFor(GuestAddressSpace &memory, GuestAddress target);
    /**
     * UIKit's layout pass for a view and its subviews: queues `layoutSubviews`
     * for each view that implements it (parents first, each at most once).
     * The run loop delivers it as an ordinary source, so the guest runs it in
     * the same place as its other callbacks.
     */
    void queueLayoutPass(GuestAddressSpace &memory, GuestAddress view);
    /** Whether a guest IMP for `selector` is reachable from `receiver`'s class chain. */
    bool guestImplements(const Object *receiver, Selector selector) const;

    /** Deepest subview of the key window covering (x, y); the window on miss. */
    GuestAddress touchTargetFor(GuestAddressSpace &memory, float x, float y);
    /** Whether the guest hid the status bar, for `applicationFrame`. */
    bool statusBarHidden(const GuestAddressSpace &memory) const;

    bool beginNestedGuestCall(CpuRegisterState &registers, GuestAddressSpace &memory,
                              GuestAddress receiver, Selector selector, std::uint32_t kind,
                              GuestAddress argument, GuestAddress &guestTarget,
                              std::string &reason);
    bool performSelectorNested(CpuRegisterState &registers, GuestAddressSpace &memory,
                               const std::string &selectorName, GuestAddress receiverAddress,
                               GuestAddress &guestTarget, std::string &reason);
    std::string readConstantString(GuestAddressSpace &memory, GuestAddress address);
    void recordLifecycleEvent(LifecycleState &state, const std::string &event);
    bool dispatch(CpuRegisterState &registers, GuestAddressSpace &memory,
                  std::string &reason, bool superDispatch = false,
                  Value *rawReturn = nullptr, GuestAddress *guestTarget = nullptr);
    bool dispatchStret(CpuRegisterState &registers, GuestAddressSpace &memory,
                       std::string &reason, GuestAddress *guestTarget = nullptr);
    bool setProperty(CpuRegisterState &registers, GuestAddressSpace &memory,
                     std::string &reason);
    bool setPropertyOrCopy(CpuRegisterState &registers, GuestAddressSpace &memory,
                           GuestAddress &guestTarget, std::string &reason);
    bool continuePropertyCopy(CpuRegisterState &registers, GuestAddressSpace &memory,
                              std::string &reason);
    bool storePropertyValue(CpuRegisterState &registers, GuestAddressSpace &memory,
                            GuestAddress receiver, std::int32_t offset, GuestAddress value,
                            bool atomic, GuestAddress originalStackPointer,
                            GuestAddress originalReturnAddress, std::string &reason);
    bool searchPaths(CpuRegisterState &registers, GuestAddressSpace &memory,
                     std::string &reason);
    GuestAddress createGuestString(GuestAddressSpace &memory, const std::string &value,
                                   bool autorelease);
    GuestAddress createGuestStringArray(GuestAddressSpace &memory,
                                       const std::vector<std::string> &values,
                                       bool autorelease);
    void autoreleaseGuestObject(Object *object);
    bool getClass(CpuRegisterState &registers, GuestAddressSpace &memory,
                  std::string &reason, bool metaclass = false);
    bool registerFunction(ShimRegistry &registry, const std::string &symbol,
                          const std::string &adapterName,
                          std::function<bool(CpuRegisterState &, GuestAddressSpace &,
                                             std::string &)> invoke);
    bool registerExceptionFunction(ShimRegistry &registry, const std::string &symbol,
                                   const std::string &adapterName,
                                   std::function<bool(CpuRegisterState &, GuestAddressSpace &,
                                                      std::string &)> invoke);
    bool registerTransferFunction(ShimRegistry &registry, const std::string &symbol,
                                  const std::string &adapterName,
                                  std::function<bool(CpuRegisterState &, GuestAddressSpace &,
                                                     GuestAddress &, std::string &)> invoke);
    void registerClassSymbols(ShimRegistry &registry);
    void registerEmptyDataSymbol(ShimRegistry &registry, const std::string &symbol);
    Object *objectForGuest(GuestAddressSpace &memory, GuestAddress address);
    const Class *classForGuest(GuestAddressSpace &memory, GuestAddress address);
    void synchronizeObject(GuestAddressSpace &memory, Object *object, GuestAddress address);
    void releaseObject(GuestAddressSpace &memory, Object *object);
    void drainPool(GuestAddressSpace &memory, Object *poolObject);

  public:
    ShimAdapter();
    ~ShimAdapter();
    ShimAdapter(const ShimAdapter &) = delete;
    ShimAdapter &operator=(const ShimAdapter &) = delete;

    /** Add only the named Objective-C symbols with an explicit native adapter. */
    void registerBindings(ShimRegistry &registry);

    /**
     * Boot-runner hooks that expose this adapter's bounded lifecycle trace and
     * the queued background-thread entry, if the image detached one.
     */
    BootLifecycleHooks lifecycleHooks();

    Runtime &runtime() noexcept { return runtime_; }
    const Runtime &runtime() const noexcept { return runtime_; }

    /**
     * Set an optional host-diagnostic frame limit for background-thread service.
     * Zero means unlimited and is the value used by the Android game launcher.
     * A non-zero value makes the virtual `sleepForTimeInterval:` callout mark
     * the guest thread cancelled after that many iterations.
     */
    void setMainThreadServiceLimit(GuestAddressSpace &memory, std::uint32_t frames);

    /**
     * Prepare an ABI-correct first call for the queued background-thread entry
     * (an `NSThread` detach target). Returns false when nothing is queued or the
     * entry cannot be represented in guest memory.
     */
    /**
     * When the run loop has nothing left, a background thread the app queued
     * (`-[NSThread start]`) runs now, on the same CPU, instead of being dropped:
     * many engines keep their frame loop in that thread. Its return goes to the
     * caller that is already in `registers.r[14]`. Returns true when a body was
     * entered (and `guestTarget` is set).
     */
    bool enterQueuedThreadAtLoopExit(GuestAddressSpace &memory, CpuRegisterState &registers,
                                     GuestAddress &guestTarget, std::string &reason);
    bool prepareQueuedMainThreadEntry(GuestAddressSpace &memory, CpuRegisterState &registers,
                                      GuestAddress &entryPoint, std::string &reason);

    /** Snapshot of the lifecycle trace; never gameplay evidence. */
    LifecycleOutcome lifecycleOutcome(GuestAddressSpace &memory) const;

    /** Guest object address of a registered framework class instance, or 0. */
    GuestAddress guestObjectAddress(GuestAddressSpace &memory, Object *object) const;

    /**
     * Guest `NSString *` for a constant the runtime itself has to expose (the
     * EAGL drawable keys). Returns 0 and fills `reason` when the bounded
     * Foundation subset cannot represent it.
     */
    GuestAddress createConstantString(GuestAddressSpace &memory, const std::string &value,
                                      std::string &reason);
};

/**
 * Publishes the host surface size to the guest that is currently running, if
 * any. The launcher calls this when its SurfaceView changes size so
 * `-[UIScreen bounds]` and the EAGL drawable use the real rectangle.
 */
void configureActiveGuestViewport(std::uint32_t width, std::uint32_t height);

/**
 * Queues a host touch event for the currently running guest's run loop. The
 * boot runs on one thread and re-enters the guest itself, so input is posted
 * into the run loop rather than called in directly.
 */
bool postTouchToActiveGuest(float x, float y, const char *phase);

} // namespace radek::compat_runtime::objc
