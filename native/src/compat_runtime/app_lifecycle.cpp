// iOS application-lifecycle ("startup chain") adapters.
//
// `UIApplicationMain` is the first call a UIKit app makes. Real dyld/dyld-based
// UIKit creates the UIApplication singleton, instantiates the delegate class
// named by the caller, delivers `applicationDidFinishLaunching:`, and then runs
// the main run loop. This file implements that chain without an artificial
// frame-count or sleep-count stop:
//
//   * the delegate is instantiated through the same guest object model the rest
//     of the runtime uses, so the app's own machine code runs for real;
//   * lifecycle messages are delivered with a native->guest nested call whose
//     continuation resumes the interrupted shim callout (no second host thread
//     and no fake stack rewriting: the guest returns to the call site it left);
//   * a background thread started with `+detachNewThreadSelector:...` is queued
//     and executed by the boot runner on the same guest CPU;
//   * `performSelectorOnMainThread:...` relays the selector into guest code, and
//     `sleepForTimeInterval:` remains a real no-op timing adapter so a running
//     game loop is not cancelled by the compatibility layer. A host diagnostic
//     can still opt into an external time budget.
//
// Framework objects created here (UIApplication, UIScreen, ...) are host objects
// materialized in the guest address space. Their behavior is deliberately a
// small, documented subset; anything outside it fails closed with a named
// diagnostic instead of guessing a signature. None of this is a rendered frame,
// a GPU surface, or gameplay evidence.
#include "compat_runtime/gles_shims.hpp"
#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/virtual_file_system.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace radek::compat_runtime::objc {
namespace {

constexpr std::uint32_t kFrameDidFinishLaunching = 1;
constexpr std::uint32_t kFramePerformSelector = 2;

// Name of the framework classes the startup chain can serve. Anything else must
// come from the image's own Objective-C metadata.
constexpr const char *kApplicationClass = "UIApplication";
constexpr const char *kScreenClass = "UIScreen";
constexpr const char *kAccelerometerClass = "UIAccelerometer";
constexpr const char *kBundleClass = "NSBundle";
constexpr const char *kThreadClass = "NSThread";

std::uint32_t floatBitsOf(float value) {
    std::uint32_t word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

float floatFromBits(std::uint32_t word) {
    float value = 0.0f;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

std::uint64_t doubleBitsOf(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double doubleFromBits(std::uint64_t bits) {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double readDoublePair(std::uint32_t low, std::uint32_t high) {
    return doubleFromBits((static_cast<std::uint64_t>(high) << 32) | low);
}

} // namespace

void ShimAdapter::recordLifecycleEvent(LifecycleState &state, const std::string &event) {
    constexpr std::size_t kMaximumLifecycleEvents = 64;
    if (state.events.size() < kMaximumLifecycleEvents)
        state.events.push_back(event);
}

// Reads the delegate class name passed to UIApplicationMain. The classic
// non-fragile ABI passes an `__NSConstantString` (isa + flags + C-string
// pointer + length); a plain C string is accepted as well.
std::string ShimAdapter::readConstantString(GuestAddressSpace &memory, GuestAddress address) {
    if (address == 0)
        throw std::runtime_error("UIApplicationMain received a null class-name argument");
    std::uint32_t pointer = 0;
    std::array<std::uint32_t, 4> constant{};
    if (memory.read(address, constant.data(), sizeof(constant))) {
        const auto candidate = constant[2];
        if (candidate >= 0x1000) {
            try {
                const auto text = readGuestString(memory, candidate);
                if (!text.empty() && text.size() < 256)
                    return text;
            } catch (const std::exception &) {
                // Fall through to the plain C-string interpretation.
            }
        }
    }
    (void)pointer;
    return readGuestString(memory, address);
}

GuestAddress ShimAdapter::lifecycleFrameworkObject(GuestAddressSpace &memory,
                                                   const std::string &className,
                                                   GuestAddress &slot) {
    if (slot != 0 && objectForGuest(memory, slot))
        return slot;
    const auto found = classes_.find(className);
    if (found == classes_.end() || !found->second)
        throw std::runtime_error("framework class is not registered: " + className);
    auto *object = runtime_.allocate(found->second);
    slot = ensureObjectAddress(memory, object);
    return slot;
}

void ShimAdapter::setMainThreadServiceLimit(GuestAddressSpace &memory, std::uint32_t frames) {
    // Zero is intentionally unlimited. Keep this hook for host diagnostics and
    // older probes, but the Android runner always supplies zero.
    auto &state = guestState(memory);
    state.lifecycle.mainThreadFramesLimit = frames;
}

bool ShimAdapter::prepareQueuedMainThreadEntry(GuestAddressSpace &memory,
                                               CpuRegisterState &registers,
                                               GuestAddress &entryPoint, std::string &reason) {
    try {
        auto &state = guestState(memory);
        auto &lifecycle = state.lifecycle;
        if (!lifecycle.queuedMainThreadEntry)
            return false;
        auto *targetObject = objectForGuest(memory, lifecycle.queuedMainThreadTarget);
        if (!targetObject)
            throw std::runtime_error("queued background-thread target is not a guest object");
        GuestAddress entry = 0;
        for (auto *current = targetObject->isa; current; current = current->superclass) {
            const auto method = guestImplementations_.find({current, lifecycle.queuedMainThreadSelector});
            if (method != guestImplementations_.end()) {
                entry = method->second.address;
                break;
            }
        }
        if (entry == 0)
            throw std::runtime_error(
                "queued background-thread selector has no guest implementation in this image");
        if (!memory.contains(entry & ~GuestAddress{1}, 2, MemoryPermission::Execute))
            throw std::runtime_error("queued background-thread entry is outside executable guest memory");
        lifecycle.mainThreadEntryRunning = true;
        registers.r[0] = lifecycle.queuedMainThreadTarget;
        registers.r[1] = selectorStringAddress(memory, lifecycle.queuedMainThreadSelector);
        registers.r[2] = lifecycle.queuedMainThreadArgument;
        registers.r[3] = 0;
        entryPoint = entry;
        std::lock_guard<std::mutex> lock(mutex_);
        recordLifecycleEvent(lifecycle, "background thread entry queued: -" +
                                            selectorNames_.at(lifecycle.queuedMainThreadSelector));
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

LifecycleOutcome ShimAdapter::lifecycleOutcome(GuestAddressSpace &memory) const {
    LifecycleOutcome outcome;
    const auto found = guestStates_.find(&memory);
    if (found == guestStates_.end() || !found->second)
        return outcome;
    const auto &lifecycle = found->second->lifecycle;
    outcome.applicationMainEntered = lifecycle.applicationMainEntered;
    outcome.applicationMainReturned = lifecycle.applicationMainReturned;
    outcome.delegateClassName = lifecycle.delegateClassName;
    outcome.mainThreadFramesServiced = lifecycle.mainThreadFramesServiced;
    outcome.mainThreadFramesLimit = lifecycle.mainThreadFramesLimit;
    outcome.mainThreadQueueExhausted = lifecycle.mainThreadCancelled;
    outcome.events = lifecycle.events;
    return outcome;
}

GuestAddress ShimAdapter::guestObjectAddress(GuestAddressSpace &memory, Object *object) const {
    const auto found = guestStates_.find(&memory);
    if (found == guestStates_.end() || !found->second)
        return 0;
    const auto address = found->second->objectAddresses.find(object);
    return address == found->second->objectAddresses.end() ? 0 : address->second;
}

namespace {

bool isKindOf(const Class *klass, const char *name) {
    for (auto *current = klass; current; current = current->superclass) {
        if (current->name == name)
            return true;
    }
    return false;
}

bool receiverIsKindOf(const Object *object, const char *name) {
    return object && isKindOf(object->isa, name);
}

} // namespace

bool ShimAdapter::beginNestedGuestCall(CpuRegisterState &registers, GuestAddressSpace &memory,
                                       GuestAddress receiver, Selector selector,
                                       std::uint32_t kind, GuestAddress argument,
                                       GuestAddress &guestTarget, std::string &reason) {
    auto &state = guestState(memory);
    auto &lifecycle = state.lifecycle;
    if (lifecycleContinuationAddress_ == 0) {
        reason = "lifecycle continuation callout is not registered";
        return false;
    }
    auto *receiverObject = objectForGuest(memory, receiver);
    if (!receiverObject)
        return false;
    GuestAddress target = 0;
    for (auto *current = receiverObject->isa; current; current = current->superclass) {
        const auto method = guestImplementations_.find({current, selector});
        if (method != guestImplementations_.end()) {
            target = method->second.address;
            break;
        }
    }
    if (target == 0)
        return false;
    CpuRegisterState nested = registers;
    nested.r[0] = receiver;
    nested.r[1] = selectorStringAddress(memory, selector);
    nested.r[2] = argument;
    nested.r[3] = 0;
    lifecycle.frames.push_back(LifecycleFrame{kind, registers.r[14], receiver});
    registers = nested;
    registers.r[14] = lifecycleContinuationAddress_;
    guestTarget = target;
    return true;
}

bool ShimAdapter::lifecycleContinuation(CpuRegisterState &registers, GuestAddressSpace &memory,
                                        std::string &reason) {
    try {
        auto &state = guestState(memory);
        auto &lifecycle = state.lifecycle;
        if (lifecycle.frames.empty())
            throw std::runtime_error("lifecycle continuation fired without a pending nested call");
        const auto frame = lifecycle.frames.back();
        lifecycle.frames.pop_back();
        registers.r[14] = frame.callerReturnAddress;
        switch (frame.kind) {
        case kFrameDidFinishLaunching:
            lifecycle.applicationMainReturned = true;
            recordLifecycleEvent(lifecycle, "applicationDidFinishLaunching: returned");
            registers.r[0] = 0;
            break;
        case kFramePerformSelector:
            ++lifecycle.mainThreadFramesServiced;
            registers.r[0] = 0;
            break;
        default:
            throw std::runtime_error("lifecycle continuation received an unknown frame kind");
        }
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool ShimAdapter::applicationMain(CpuRegisterState &registers, GuestAddressSpace &memory,
                                  GuestAddress &guestTarget, std::string &reason) {
    try {
        auto &state = guestState(memory);
        auto &lifecycle = state.lifecycle;
        lifecycle.applicationMainEntered = true;
        recordLifecycleEvent(lifecycle, "UIApplicationMain entered");
        const auto delegateName = readConstantString(memory, registers.r[3]);
        lifecycle.delegateClassName = delegateName;
        const auto found = classes_.find(delegateName);
        if (found == classes_.end() || !found->second)
            throw std::runtime_error("UIApplicationMain delegate class '" + delegateName +
                                     "' is not registered by this image");
        auto *delegateObject = runtime_.allocate(found->second);
        lifecycle.delegate = ensureObjectAddress(memory, delegateObject);
        recordLifecycleEvent(lifecycle, "delegate instantiated: " + delegateName);
        lifecycle.application =
            lifecycleFrameworkObject(memory, kApplicationClass, lifecycle.application);
        auto *applicationObject = objectForGuest(memory, lifecycle.application);
        if (!applicationObject || applicationObject->ivars.empty())
            throw std::runtime_error("UIApplication singleton has no delegate slot");
        applicationObject->ivars[0] = lifecycle.delegate;
        synchronizeObject(memory, applicationObject, lifecycle.application);
        recordLifecycleEvent(lifecycle, "application delegate wired");

        const auto selectorValue = runtime_.selector("applicationDidFinishLaunching:");
        {
            std::lock_guard<std::mutex> lock(mutex_);
            selectorNames_[selectorValue] = "applicationDidFinishLaunching:";
            selectorIds_["applicationDidFinishLaunching:"] = selectorValue;
        }
        GuestAddress target = 0;
        for (auto *current = delegateObject->isa; current; current = current->superclass) {
            const auto method = guestImplementations_.find({current, selectorValue});
            if (method != guestImplementations_.end()) {
                target = method->second.address;
                break;
            }
        }
        if (target == 0) {
            recordLifecycleEvent(lifecycle,
                                 "delegate implements no applicationDidFinishLaunching: (skipped)");
            lifecycle.applicationMainReturned = true;
            registers.r[0] = 0;
            return true;
        }
        if (lifecycleContinuationAddress_ == 0)
            throw std::runtime_error("lifecycle continuation callout is not registered");
        CpuRegisterState nested = registers;
        nested.r[0] = lifecycle.delegate;
        nested.r[1] = selectorStringAddress(memory, selectorValue);
        nested.r[2] = 0;
        nested.r[3] = 0;
        lifecycle.frames.push_back(
            LifecycleFrame{kFrameDidFinishLaunching, registers.r[14], lifecycle.delegate});
        registers = nested;
        registers.r[14] = lifecycleContinuationAddress_;
        guestTarget = target;
        recordLifecycleEvent(lifecycle, "delivering applicationDidFinishLaunching:");
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool ShimAdapter::performSelectorNested(CpuRegisterState &registers, GuestAddressSpace &memory,
                                        const std::string &selectorName, GuestAddress receiverAddress,
                                        GuestAddress &guestTarget, std::string &reason) {
    try {
        auto &state = guestState(memory);
        auto &lifecycle = state.lifecycle;
        Selector selector = 0;
        GuestAddress argument = 0;
        auto mainThread = false;
        if (selectorName == "performSelector:") {
            selector = selectorForGuest(memory, registers.r[2]);
        } else if (selectorName == "performSelector:withObject:") {
            selector = selectorForGuest(memory, registers.r[2]);
            argument = registers.r[3];
        } else if (selectorName == "performSelectorOnMainThread:withObject:waitUntilDone:") {
            selector = selectorForGuest(memory, registers.r[2]);
            argument = registers.r[3];
            mainThread = true;
        } else {
            return false;
        }
        if (selector == 0)
            throw std::runtime_error("performSelector received a null selector");
        std::string name;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            name = selectorNames_.at(selector);
        }
        if (!beginNestedGuestCall(registers, memory, receiverAddress, selector,
                                  kFramePerformSelector, argument, guestTarget, reason))
            return false;
        if (mainThread)
            recordLifecycleEvent(lifecycle, "main thread served: " + name);
        else
            recordLifecycleEvent(lifecycle, "performSelector served: " + name);
        return true;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}


// --- bounded Foundation/UIKit message groups ------------------------------
//
// The startup chain needs a handful of boxed values, containers and view
// messages. They are host objects with a documented bounded representation:
//
//   * NSNumber      - value bits in ivars 0/1 (low/high), encoding tag in ivar 2;
//   * NSDictionary  - alternating key/value objects in `arrayItems`;
//   * NSString      - the runtime's own guest string payload;
//   * NSURL         - the URL text in the string payload;
//   * NSArray       - the runtime's own guest array payload plus fast
//                     enumeration over it;
//   * UIView/UIWindow/CAEAGLLayer - frame slots 0..3 and center slots 4..5.
//
// None of this is a full Foundation or UIKit: anything outside these messages
// fails closed with a named diagnostic instead of guessing a signature.
namespace {

constexpr std::uint32_t kNumberTagBool = 0;
constexpr std::uint32_t kNumberTagInt = 1;
constexpr std::uint32_t kNumberTagFloat = 2;
constexpr std::uint32_t kNumberTagDouble = 3;

void storeNumber(std::vector<Value> &ivars, double value, std::uint32_t tag) {
    if (ivars.size() < 3)
        ivars.resize(3, 0);
    if (tag == kNumberTagFloat) {
        ivars[0] = floatBitsOf(static_cast<float>(value));
        ivars[1] = 0;
    } else if (tag == kNumberTagBool) {
        ivars[0] = value != 0.0 ? 1 : 0;
        ivars[1] = 0;
    } else if (tag == kNumberTagInt) {
        ivars[0] = static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
        ivars[1] = 0;
    } else {
        const auto bits = doubleBitsOf(value);
        ivars[0] = static_cast<std::uint32_t>(bits & 0xffffffffu);
        ivars[1] = static_cast<std::uint32_t>(bits >> 32);
    }
    ivars[2] = tag;
}

double numberValue(const std::vector<Value> &ivars) {
    if (ivars.size() < 3)
        return 0.0;
    switch (ivars[2]) {
    case kNumberTagFloat:
        return static_cast<double>(floatFromBits(static_cast<std::uint32_t>(ivars[0])));
    case kNumberTagBool:
        return ivars[0] != 0 ? 1.0 : 0.0;
    case kNumberTagInt:
        return static_cast<double>(static_cast<std::int32_t>(ivars[0]));
    case kNumberTagDouble:
    default:
        return doubleFromBits((static_cast<std::uint64_t>(ivars[1]) << 32) |
                              static_cast<std::uint32_t>(ivars[0]));
    }
}

} // namespace

// NSThread instance slots: 0 = target, 1 = selector, 2 = argument,
// 3 = cancelled, 4 = is-main. The guest sees slot i at address + 4 * (i + 1)
// because slot 0 of the materialized instance is the class pointer.
constexpr std::size_t kThreadCancelledSlot = 4;

// The app asks its own view class for the layer class; a bounded adapter answers
// with CAEAGLLayer, which is exactly what this game's `+layerClass` returns.
GuestAddress ShimAdapter::lifecycleLayerForView(GuestAddressSpace &memory, GuestAddress view,
                                                Object *viewObject) {
    auto &lifecycle = guestState(memory).lifecycle;
    const auto existing = lifecycle.viewLayers.find(view);
    if (existing != lifecycle.viewLayers.end())
        return existing->second;
    const auto found = classes_.find("CAEAGLLayer");
    if (found == classes_.end() || !found->second)
        throw std::runtime_error("CAEAGLLayer is not registered by this runtime");
    auto *layer = runtime_.allocate(found->second);
    const auto address = ensureObjectAddress(memory, layer);
    if (viewObject && viewObject->ivars.size() >= 4) {
        // The layer starts with the view's frame so a guest that reads the layer
        // bounds before setting them sees the same rectangle.
        if (layer->ivars.size() < 4)
            layer->ivars.resize(4, 0);
        for (std::size_t index = 0; index < 4; ++index)
            layer->ivars[index] = viewObject->ivars[index];
        synchronizeObject(memory, layer, address);
    }
    lifecycle.viewLayers.emplace(view, address);
    recordLifecycleEvent(lifecycle, "-[UIView layer] -> CAEAGLLayer");
    return address;
}

bool ShimAdapter::lifecycleViewMessage(CpuRegisterState &registers, GuestAddressSpace &memory,
                                       const std::string &selectorName, GuestAddress receiverAddress,
                                       Object *receiverObject, Value *rawReturn,
                                       std::string &reason) {
    try {
        auto &lifecycle = guestState(memory).lifecycle;
        auto finish = [&registers, rawReturn](Value value) {
            registers.r[0] = static_cast<GuestAddress>(value);
            if (rawReturn)
                *rawReturn = value;
            return true;
        };
        const auto isLayer = receiverIsKindOf(receiverObject, "CAEAGLLayer");
        if (!isLayer && !receiverIsKindOf(receiverObject, "UIView") &&
            !receiverIsKindOf(receiverObject, "UIWindow"))
            return false;

        // Frame/bounds/center live in the materialized instance slots:
        //   0..3 = frame (x, y, width, height), 4..5 = center (x, y).
        if (selectorName == "initWithFrame:" || selectorName == "setFrame:" ||
            selectorName == "setBounds:") {
            std::array<std::uint32_t, 2> stackArguments{};
            if (registers.r[13] == 0 ||
                !memory.read(registers.r[13], stackArguments.data(), sizeof(stackArguments)))
                throw std::runtime_error("CGRect argument stack frame is unreadable");
            const std::array<std::uint32_t, 4> frame{registers.r[2], registers.r[3],
                                                    stackArguments[0], stackArguments[1]};
            auto &ivars = receiverObject->ivars;
            if (ivars.size() < 4)
                ivars.resize(4, 0);
            for (std::size_t index = 0; index < frame.size(); ++index)
                ivars[index] = frame[index];
            synchronizeObject(memory, receiverObject, receiverAddress);
            // `initWithFrame:` returns the receiver; a `super` dispatch leaves an
            // `objc_super` pointer in r0, not the receiver.
            return finish(receiverAddress);
        }
        if (selectorName == "setCenter:") {
            auto &ivars = receiverObject->ivars;
            if (ivars.size() < 6)
                ivars.resize(6, 0);
            ivars[4] = registers.r[2];
            ivars[5] = registers.r[3];
            synchronizeObject(memory, receiverObject, receiverAddress);
            return finish(0);
        }
        if (selectorName == "layer")
            return finish(lifecycleLayerForView(memory, receiverAddress, receiverObject));
        if (selectorName == "makeKeyAndVisible") {
            lifecycle.keyWindow = receiverAddress;
            recordLifecycleEvent(lifecycle, "-[UIWindow makeKeyAndVisible]");
            return finish(0);
        }
        if (selectorName == "window")
            return finish(lifecycle.keyWindow);
        if (selectorName == "superview")
            return finish(0);
        if (selectorName == "contentScaleFactor" || selectorName == "contentsScale")
            return finish(floatBitsOf(1.0f));
        if (selectorName == "alpha")
            return finish(floatBitsOf(1.0f));
        if (selectorName == "isHidden" || selectorName == "hidden")
            return finish(0);
        if (selectorName == "opaque" || selectorName == "isOpaque" || selectorName == "isOpaque")
            return finish(1);
        if (selectorName == "drawableProperties" || selectorName == "backgroundColor" ||
            selectorName == "delegate" || selectorName == "transform")
            return finish(0);
        if (selectorName == "addSubview:" || selectorName == "removeFromSuperview" ||
            selectorName == "setNeedsDisplay" || selectorName == "setNeedsDisplayInRect:" ||
            selectorName == "setNeedsLayout" || selectorName == "layoutIfNeeded" ||
            selectorName == "layoutSubviews" || selectorName == "setOpaque:" ||
            selectorName == "setHidden:" || selectorName == "setAlpha:" ||
            selectorName == "setContentScaleFactor:" || selectorName == "setContentsScale:" ||
            selectorName == "setMultipleTouchEnabled:" ||
            selectorName == "setUserInteractionEnabled:" || selectorName == "setExclusiveTouch:" ||
            selectorName == "setAutoresizingMask:" || selectorName == "setAutoresizesSubviews:" ||
            selectorName == "setContentMode:" || selectorName == "setClearsContextBeforeDrawing:" ||
            selectorName == "setClipsToBounds:" || selectorName == "setMasksToBounds:" ||
            selectorName == "setBackgroundColor:" || selectorName == "setTransform:" ||
            selectorName == "setDrawableProperties:" || selectorName == "setNeedsDisplayOnBoundsChange:" ||
            selectorName == "setAsynchronous:" || selectorName == "setShadowOpacity:" ||
            selectorName == "setBorderWidth:" || selectorName == "setCornerRadius:")
            return finish(0);
        if (selectorName == "bounds" || selectorName == "frame" || selectorName == "center") {
            // Non-stret struct access is not supported; the app calls these
            // through objc_msgSend_stret, which the adapter serves directly.
            throw std::runtime_error("struct-return selector '" + selectorName +
                                     "' requires objc_msgSend_stret dispatch");
        }
        (void)reason;
        return false;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

GuestAddress ShimAdapter::lifecycleCurrentThreadAddress(GuestAddressSpace &memory) {
    auto &lifecycle = guestState(memory).lifecycle;
    if (lifecycle.mainThreadEntryRunning) {
        if (lifecycle.backgroundThread == 0) {
            auto *thread = runtime_.allocate(classes_.at("NSThread"));
            thread->ivars.resize(5, 0);
            lifecycle.backgroundThread = ensureObjectAddress(memory, thread);
        }
        return lifecycle.backgroundThread;
    }
    if (lifecycle.mainThread == 0) {
        auto *thread = runtime_.allocate(classes_.at("NSThread"));
        thread->ivars.resize(5, 0);
        thread->ivars[4] = 1;
        lifecycle.mainThread = ensureObjectAddress(memory, thread);
    }
    return lifecycle.mainThread;
}

bool ShimAdapter::lifecycleThreadMessage(CpuRegisterState &registers, GuestAddressSpace &memory,
                                         const std::string &selectorName,
                                         GuestAddress receiverAddress, Object *receiverObject,
                                         Value *rawReturn) {
    auto &lifecycle = guestState(memory).lifecycle;
    auto finish = [&registers, rawReturn](Value value) {
        registers.r[0] = static_cast<GuestAddress>(value);
        if (rawReturn)
            *rawReturn = value;
        return true;
    };
    if (!receiverIsKindOf(receiverObject, "NSThread"))
        return false;
    // Thread objects keep their state in instance slots:
    //   0 = target, 1 = selector, 2 = argument, 3 = cancelled, 4 = is-main.
    auto &ivars = receiverObject->ivars;
    if (ivars.size() < 5)
        ivars.resize(5, 0);
    if (selectorName == "initWithTarget:selector:object:") {
        ivars[0] = registers.r[2];
        ivars[1] = registers.r[3];
        std::uint32_t argument = 0;
        if (registers.r[13] != 0)
            (void)memory.read(registers.r[13], &argument, sizeof(argument));
        ivars[2] = argument;
        synchronizeObject(memory, receiverObject, receiverAddress);
        recordLifecycleEvent(lifecycle, "-[NSThread initWithTarget:selector:object:]");
        return finish(receiverAddress);
    }
    if (selectorName == "start") {
        if (ivars[0] == 0 || ivars[1] == 0)
            throw std::runtime_error("-[NSThread start] was called on a thread without a target");
        lifecycle.queuedMainThreadEntry = true;
        lifecycle.queuedMainThreadTarget = ivars[0];
        lifecycle.queuedMainThreadSelector = ivars[1];
        lifecycle.queuedMainThreadArgument = ivars[2];
        {
            std::lock_guard<std::mutex> lock(mutex_);
            recordLifecycleEvent(lifecycle, "background thread started: -" + selectorNames_.at(ivars[1]));
        }
        return finish(0);
    }
    if (selectorName == "cancel") {
        ivars[3] = 1;
        synchronizeObject(memory, receiverObject, receiverAddress);
        recordLifecycleEvent(lifecycle, "-[NSThread cancel]");
        return finish(0);
    }
    if (selectorName == "isCancelled")
        return finish(ivars[3] != 0 ? 1 : 0);
    if (selectorName == "isMainThread")
        return finish(ivars[4] != 0 ? 1 : 0);
    if (selectorName == "threadPriority")
        return finish(floatBitsOf(0.5f));
    if (selectorName == "setThreadPriority:" || selectorName == "setName:")
        return finish(0);
    if (selectorName == "name")
        return finish(createGuestString(memory, "radek-guest-thread", true));
    if (selectorName == "sleepForTimeInterval:") {
        // A zero frame limit is the normal Android/game value: sleep is an
        // ordinary compatibility call and never cancels the guest. The optional
        // non-zero limit exists only for an explicitly bounded host diagnostic.
        ++lifecycle.sleepCount;
        if (lifecycle.mainThreadFramesLimit != 0 &&
            lifecycle.sleepCount >= lifecycle.mainThreadFramesLimit) {
            lifecycle.mainThreadCancelled = true;
            const auto current = lifecycleCurrentThreadAddress(memory);
            auto *threadObject = objectForGuest(memory, current);
            if (threadObject && threadObject->ivars.size() >= 4) {
                // Guest memory is the source of truth for the shared instance
                // slots, so the cancellation flag is written to the guest slot
                // first and only then mirrored into the host object.
                const std::uint32_t cancelled = 1;
                if (!memory.write(current + kThreadCancelledSlot * sizeof(std::uint32_t),
                                  &cancelled, sizeof(cancelled)))
                    throw std::runtime_error("thread cancellation flag is not writable");
                threadObject->ivars[3] = cancelled;
            }
            recordLifecycleEvent(lifecycle, "bounded main-loop service complete");
        }
        return finish(0);
    }
    return false;
}

bool ShimAdapter::lifecycleFoundationMessage(CpuRegisterState &registers, GuestAddressSpace &memory,
                                             const std::string &selectorName,
                                             GuestAddress receiverAddress, Object *receiverObject,
                                             Value *rawReturn, std::string &reason) {
    try {
        auto finish = [&registers, rawReturn](Value value) {
            registers.r[0] = static_cast<GuestAddress>(value);
            if (rawReturn)
                *rawReturn = value;
            return true;
        };
        if (receiverIsKindOf(receiverObject, "NSNumber")) {
            if (selectorName == "initWithBool:" || selectorName == "initWithInt:" ||
                selectorName == "initWithFloat:" || selectorName == "initWithDouble:") {
                const auto tag = selectorName == "initWithBool:" ? kNumberTagBool
                    : selectorName == "initWithInt:"                 ? kNumberTagInt
                    : selectorName == "initWithFloat:"               ? kNumberTagFloat
                                                                     : kNumberTagDouble;
                double value = 0.0;
                if (tag == kNumberTagFloat)
                    value = static_cast<double>(floatFromBits(registers.r[2]));
                else if (tag == kNumberTagInt)
                    value = static_cast<double>(static_cast<std::int32_t>(registers.r[2]));
                else if (tag == kNumberTagBool)
                    value = registers.r[2] != 0 ? 1.0 : 0.0;
                else {
                    std::uint32_t high = 0;
                    if (registers.r[13] != 0 &&
                        !memory.read(registers.r[13], &high, sizeof(high)))
                        throw std::runtime_error("NSNumber double argument is unreadable");
                    value = doubleFromBits((static_cast<std::uint64_t>(high) << 32) |
                                           static_cast<std::uint32_t>(registers.r[2]));
                }
                storeNumber(receiverObject->ivars, value, tag);
                synchronizeObject(memory, receiverObject, receiverAddress);
                return finish(receiverAddress);
            }
            if (selectorName == "boolValue")
                return finish(numberValue(receiverObject->ivars) != 0.0 ? 1 : 0);
            if (selectorName == "intValue")
                return finish(static_cast<Value>(
                    static_cast<std::uint32_t>(static_cast<std::int32_t>(
                        numberValue(receiverObject->ivars)))));
            if (selectorName == "floatValue")
                return finish(floatBitsOf(static_cast<float>(numberValue(receiverObject->ivars))));
            if (selectorName == "doubleValue")
                return finish(static_cast<Value>(static_cast<std::uint32_t>(
                    doubleBitsOf(numberValue(receiverObject->ivars)) & 0xffffffffu)));
            if (selectorName == "hash" || selectorName == "unsignedIntValue" ||
                selectorName == "unsignedIntegerValue")
                return finish(receiverObject->ivars.empty() ? 0 : receiverObject->ivars[0]);
            return false;
        }
        if (receiverIsKindOf(receiverObject, "NSDictionary")) {
            // `arrayItems` holds alternating key/value object pointers.
            const auto &items = receiverObject->arrayItems;
            if (selectorName == "objectForKey:") {
                for (std::size_t index = 0; index + 1 < items.size(); index += 2) {
                    if (items[index] == nullptr)
                        continue;
                    if (ensureObjectAddress(memory, items[index]) == registers.r[2])
                        return finish(ensureObjectAddress(memory, items[index + 1]));
                }
                return finish(0);
            }
            if (selectorName == "count")
                return finish(static_cast<Value>(items.size() / 2));
            if (selectorName == "allKeys") {
                auto *keys = runtime_.allocate(classes_.at("NSArray"));
                for (std::size_t index = 0; index + 1 < items.size(); index += 2) {
                    if (items[index])
                        keys->arrayItems.push_back(retain(items[index]));
                }
                return finish(ensureObjectAddress(memory, keys));
            }
            return false;
        }
        if (receiverIsKindOf(receiverObject, "NSURL")) {
            if (selectorName == "absoluteString" || selectorName == "description" ||
                selectorName == "path")
                return finish(createGuestString(memory, receiverObject->stringValue, true));
            if (selectorName == "scheme") {
                const auto separator = receiverObject->stringValue.find(':');
                const auto text = separator == std::string::npos
                    ? receiverObject->stringValue
                    : receiverObject->stringValue.substr(0, separator);
                return finish(createGuestString(memory, text, true));
            }
            return false;
        }
        if (receiverIsKindOf(receiverObject, "NSArray") &&
            selectorName == "countByEnumeratingWithState:objects:count:") {
            // Bounded fast enumeration over the host array payload. The guest
            // provides the buffer; `state->state` carries the cursor (the ABI
            // leaves that word to the implementation) and the mutation pointer
            // is a never-changing zero word.
            const auto stateAddress = registers.r[2];
            const auto bufferAddress = registers.r[3];
            std::uint32_t capacity = 0;
            if (registers.r[13] == 0 || !memory.read(registers.r[13], &capacity, sizeof(capacity)))
                throw std::runtime_error("fast enumeration count argument is unreadable");
            auto &lifecycle = guestState(memory).lifecycle;
            if (lifecycle.enumerationMutations == 0) {
                lifecycle.enumerationMutations =
                    memory.mapAny(4096, MemoryPermission::Read | MemoryPermission::Write,
                                  "objc-fast-enumeration-mutations");
            }
            std::uint32_t cursor = 0;
            if (!memory.read(stateAddress, &cursor, sizeof(cursor)))
                throw std::runtime_error("fast enumeration state is unreadable");
            const auto &items = receiverObject->arrayItems;
            if (cursor > items.size())
                cursor = static_cast<std::uint32_t>(items.size());
            std::vector<std::uint32_t> addresses;
            while (addresses.size() < capacity && cursor < items.size()) {
                addresses.push_back(ensureObjectAddress(memory, items[cursor]));
                ++cursor;
            }
            if (!addresses.empty() &&
                !memory.write(bufferAddress, addresses.data(),
                              addresses.size() * sizeof(std::uint32_t)))
                throw std::runtime_error("fast enumeration buffer is not writable");
            std::uint32_t itemsPointer = bufferAddress;
            std::uint32_t mutationsPointer = lifecycle.enumerationMutations;
            if (!memory.write(stateAddress, &cursor, sizeof(cursor)) ||
                !memory.write(stateAddress + 4, &itemsPointer, sizeof(itemsPointer)) ||
                !memory.write(stateAddress + 8, &mutationsPointer, sizeof(mutationsPointer)))
                throw std::runtime_error("fast enumeration state could not be filled");
            return finish(static_cast<Value>(addresses.size()));
        }
        if (receiverIsKindOf(receiverObject, "NSString")) {
            if (selectorName == "cStringUsingEncoding:") {
                const auto bufferAddress = registers.r[2];
                if (bufferAddress == 0)
                    return finish(0);
                if (!memory.write(bufferAddress, receiverObject->stringValue.c_str(),
                                  receiverObject->stringValue.size() + 1))
                    throw std::runtime_error("string copy target is not writable guest memory");
                return finish(bufferAddress);
            }
            if (selectorName == "getCString:maxLength:encoding:") {
                const auto bufferAddress = registers.r[2];
                std::uint32_t capacity = 0;
                if (registers.r[13] != 0)
                    (void)memory.read(registers.r[13], &capacity, sizeof(capacity));
                if (bufferAddress == 0 || capacity == 0 ||
                    capacity < receiverObject->stringValue.size() + 1)
                    return finish(0);
                if (!memory.write(bufferAddress, receiverObject->stringValue.c_str(),
                                  receiverObject->stringValue.size() + 1))
                    throw std::runtime_error("string copy target is not writable guest memory");
                return finish(1);
            }
            if (selectorName == "isEqualToString:") {
                auto *other = registers.r[2] == 0 ? nullptr : objectForGuest(memory, registers.r[2]);
                return finish(other && other->stringValue == receiverObject->stringValue ? 1 : 0);
            }
            if (selectorName == "length")
                return finish(static_cast<Value>(receiverObject->stringValue.size()));
            if (selectorName == "description")
                return finish(createGuestString(memory, receiverObject->stringValue, true));
            return false;
        }
        if (selectorName == "description")
            return finish(createGuestString(memory, receiverObject->isa->name, true));
        if (selectorName == "hash")
            return finish(receiverAddress);
        if (selectorName == "isEqual:")
            return finish(registers.r[2] == receiverAddress ? 1 : 0);
        return false;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool ShimAdapter::lifecycleFoundationClassMessage(CpuRegisterState &registers,
                                                  GuestAddressSpace &memory,
                                                  const Class *receiverClass,
                                                  const std::string &selectorName, Value *rawReturn,
                                                  std::string &reason) {
    try {
        auto finish = [&registers, rawReturn](Value value) {
            registers.r[0] = static_cast<GuestAddress>(value);
            if (rawReturn)
                *rawReturn = value;
            return true;
        };
        const auto &className = receiverClass->name;
        const auto allocate = [this](const std::string &name) {
            return runtime_.allocate(classes_.at(name));
        };
        if (className == "NSNumber") {
            if (selectorName == "numberWithBool:" || selectorName == "numberWithInt:" ||
                selectorName == "numberWithFloat:" || selectorName == "numberWithDouble:") {
                const auto tag = selectorName == "numberWithBool:" ? kNumberTagBool
                    : selectorName == "numberWithInt:"               ? kNumberTagInt
                    : selectorName == "numberWithFloat:"             ? kNumberTagFloat
                                                                    : kNumberTagDouble;
                double value = 0.0;
                if (tag == kNumberTagFloat)
                    value = static_cast<double>(floatFromBits(registers.r[2]));
                else if (tag == kNumberTagInt)
                    value = static_cast<double>(static_cast<std::int32_t>(registers.r[2]));
                else if (tag == kNumberTagBool)
                    value = registers.r[2] != 0 ? 1.0 : 0.0;
                else {
                    std::uint32_t high = 0;
                    if (registers.r[13] == 0 ||
                        !memory.read(registers.r[13], &high, sizeof(high)))
                        throw std::runtime_error("numberWithDouble: argument is unreadable");
                    value = doubleFromBits((static_cast<std::uint64_t>(high) << 32) |
                                           static_cast<std::uint32_t>(registers.r[2]));
                }
                auto *number = allocate("NSNumber");
                storeNumber(number->ivars, value, tag);
                return finish(ensureObjectAddress(memory, number));
            }
            return false;
        }
        if (className == "NSDictionary") {
            if (selectorName == "dictionary")
                return finish(ensureObjectAddress(memory, allocate("NSDictionary")));
            if (selectorName == "dictionaryWithObject:forKey:") {
                auto *dictionary = allocate("NSDictionary");
                dictionary->arrayItems.push_back(objectForGuest(memory, registers.r[3]));
                dictionary->arrayItems.push_back(objectForGuest(memory, registers.r[2]));
                return finish(ensureObjectAddress(memory, dictionary));
            }
            if (selectorName == "dictionaryWithObjectsAndKeys:") {
                // Pairs arrive in r2/r3 and then on the stack, nil-terminated.
                std::vector<std::uint32_t> arguments{registers.r[2], registers.r[3]};
                std::uint32_t stackIndex = 0;
                while (arguments.size() < 64) {
                    std::uint32_t word = 0;
                    if (registers.r[13] == 0 ||
                        !memory.read(registers.r[13] + stackIndex * 4, &word, sizeof(word)))
                        throw std::runtime_error(
                            "dictionaryWithObjectsAndKeys: argument list is unreadable");
                    ++stackIndex;
                    if (word == 0)
                        break;
                    arguments.push_back(word);
                }
                if (arguments.size() % 2 != 0)
                    throw std::runtime_error(
                        "dictionaryWithObjectsAndKeys: received an odd argument count");
                auto *dictionary = allocate("NSDictionary");
                for (std::size_t index = 0; index + 1 < arguments.size(); index += 2) {
                    dictionary->arrayItems.push_back(objectForGuest(memory, arguments[index + 1]));
                    dictionary->arrayItems.push_back(objectForGuest(memory, arguments[index]));
                }
                return finish(ensureObjectAddress(memory, dictionary));
            }
            return false;
        }
        if (className == "NSString") {
            if (selectorName == "string" || selectorName == "stringWithCString:" ||
                selectorName == "stringWithString:" || selectorName == "stringWithUTF8String:")
                return finish(createGuestString(
                    memory, registers.r[2] == 0 ? std::string() : readGuestString(memory, registers.r[2]),
                    true));
            return false;
        }
        if (className == "NSURL") {
            if (selectorName == "URLWithString:" || selectorName == "fileURLWithPath:") {
                auto *url = allocate("NSURL");
                url->stringValue = readGuestString(memory, registers.r[2]);
                return finish(ensureObjectAddress(memory, url));
            }
            return false;
        }
        if (className == "NSArray") {
            if (selectorName == "array")
                return finish(ensureObjectAddress(memory, allocate("NSArray")));
            return false;
        }
        if (className == "NSThread") {
            // The guest-visible thread objects are materialized NSThread
            // instances; only one guest thread runs at a time and the boot
            // runner services that thread cooperatively.
            auto &lifecycle = guestState(memory).lifecycle;
            if (selectorName == "mainThread") {
                lifecycle.mainThreadEntryRunning = false;
                return finish(lifecycleCurrentThreadAddress(memory));
            }
            if (selectorName == "currentThread")
                return finish(lifecycleCurrentThreadAddress(memory));
            if (selectorName == "isMainThread")
                return finish(lifecycle.mainThreadEntryRunning ? 0 : 1);
            if (selectorName == "sleepForTimeInterval:")
                return finish(0);
            if (selectorName == "detachNewThreadSelector:toTarget:withObject:") {
                const auto selector = selectorForGuest(memory, registers.r[2]);
                std::uint32_t argument = 0;
                if (registers.r[13] != 0)
                    (void)memory.read(registers.r[13], &argument, sizeof(argument));
                lifecycle.queuedMainThreadEntry = true;
                lifecycle.queuedMainThreadTarget = registers.r[3];
                lifecycle.queuedMainThreadSelector = selector;
                lifecycle.queuedMainThreadArgument = argument;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    recordLifecycleEvent(lifecycle, "background thread detached: -" +
                                                        selectorNames_.at(selector));
                }
                return finish(0);
            }
            return false;
        }
        (void)reason;
        return false;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

bool ShimAdapter::lifecycleSelector(CpuRegisterState &registers, GuestAddressSpace &memory,
                                    const std::string &selectorName, GuestAddress receiverAddress,
                                    Object *receiverObject, const Class *receiverClass,
                                    Value *rawReturn, GuestAddress *guestTarget,
                                    std::string &reason) {
    try {
        auto &lifecycle = guestState(memory).lifecycle;
        auto finish = [&registers, rawReturn](Value value) {
            registers.r[0] = static_cast<GuestAddress>(value);
            if (rawReturn)
                *rawReturn = value;
            return true;
        };
        const auto classMessage = receiverObject == nullptr && receiverClass != nullptr;

        // ---- class messages: +[Owner selector] ----------------------------
        if (classMessage) {
            const auto &className = receiverClass->name;
            if (selectorName == "sharedApplication" && className == kApplicationClass) {
                const auto application =
                    lifecycleFrameworkObject(memory, kApplicationClass, lifecycle.application);
                if (!lifecycle.applicationMainEntered)
                    recordLifecycleEvent(lifecycle, "+[UIApplication sharedApplication]");
                return finish(application);
            }
            if (selectorName == "mainScreen" && className == kScreenClass) {
                const auto screen = lifecycleFrameworkObject(memory, kScreenClass, lifecycle.screen);
                recordLifecycleEvent(lifecycle, "+[UIScreen mainScreen]");
                return finish(screen);
            }
            if (selectorName == "sharedAccelerometer" && className == kAccelerometerClass) {
                const auto accelerometer = lifecycleFrameworkObject(
                    memory, kAccelerometerClass, lifecycle.accelerometer);
                recordLifecycleEvent(lifecycle, "+[UIAccelerometer sharedAccelerometer]");
                return finish(accelerometer);
            }
            if (selectorName == "mainBundle" && className == kBundleClass) {
                const auto bundle = lifecycleFrameworkObject(memory, kBundleClass, lifecycle.bundle);
                recordLifecycleEvent(lifecycle, "+[NSBundle mainBundle]");
                return finish(bundle);
            }
            if (className == kThreadClass) {
                if (lifecycleFoundationClassMessage(registers, memory, receiverClass, selectorName,
                                                   rawReturn, reason))
                    return true;
                if (!reason.empty())
                    return false;
            }
            if (className == "EAGLContext") {
                if (selectorName == "currentContext")
                    return finish(lifecycle.currentContext);
                if (selectorName == "setCurrentContext:") {
                    lifecycle.currentContext = registers.r[2];
                    return finish(0);
                }
            }
            if (selectorName == "layerClass" &&
                (isKindOf(receiverClass, "UIView") || isKindOf(receiverClass, "UIWindow"))) {
                const auto found = classes_.find("CAEAGLLayer");
                if (found == classes_.end())
                    throw std::runtime_error("CAEAGLLayer is not registered by this runtime");
                lifecycle.layerClass = ensureClassAddress(memory, found->second);
                return finish(lifecycle.layerClass);
            }
            if (lifecycleFoundationClassMessage(registers, memory, receiverClass, selectorName,
                                                rawReturn, reason))
                return true;
            return false;
        }

        if (!receiverObject)
            return false;

        // ---- view messages (UIView, UIWindow, CAEAGLLayer) -----------------
        if (lifecycleViewMessage(registers, memory, selectorName, receiverAddress, receiverObject,
                                rawReturn, reason))
            return true;
        if (!reason.empty())
            return false;

        // ---- NSThread ------------------------------------------------------
        if (lifecycleThreadMessage(registers, memory, selectorName, receiverAddress, receiverObject,
                                   rawReturn))
            return true;

        // ---- UIApplication ------------------------------------------------
        if (receiverIsKindOf(receiverObject, kApplicationClass)) {
            if (selectorName == "setDelegate:") {
                lifecycle.delegate = registers.r[2];
                return finish(0);
            }
            if (selectorName == "delegate")
                return finish(lifecycle.delegate);
            if (selectorName == "keyWindow")
                return finish(lifecycle.keyWindow);
            if (selectorName == "setKeyWindow:") {
                lifecycle.keyWindow = registers.r[2];
                return finish(0);
            }
            if (selectorName == "setStatusBarHidden:" || selectorName == "setIdleTimerDisabled:")
                return finish(0);
            if (selectorName == "setStatusBarOrientation:") {
                lifecycle.statusBarOrientation = registers.r[2];
                return finish(0);
            }
            if (selectorName == "statusBarOrientation")
                return finish(lifecycle.statusBarOrientation);
            if (selectorName == "run" || selectorName == "stop" ||
                selectorName == "beginIgnoringInteractionEvents" ||
                selectorName == "endIgnoringInteractionEvents")
                return finish(0);
            if (selectorName == "applicationFrame") {
                throw std::runtime_error(
                    "struct-return selector 'applicationFrame' requires objc_msgSend_stret dispatch");
            }
            if (selectorName == "openURL:") {
                throw std::runtime_error(
                    "openURL: is outside the bounded startup-chain runtime (no host browser step)");
            }
            return false;
        }
        // ---- UIAccelerometer / NSBundle ------------------------------------
        if (receiverIsKindOf(receiverObject, kAccelerometerClass)) {
            if (selectorName == "setUpdateInterval:") {
                lifecycle.accelerometerInterval = readDoublePair(registers.r[2], registers.r[3]);
                return finish(0);
            }
            if (selectorName == "setDelegate:") {
                lifecycle.accelerometerDelegate = registers.r[2];
                return finish(0);
            }
            if (selectorName == "delegate")
                return finish(lifecycle.accelerometerDelegate);
            return false;
        }
        if (receiverIsKindOf(receiverObject, kBundleClass)) {
            if (selectorName == "bundlePath" || selectorName == "resourcePath")
                return finish(createGuestString(memory, bundleGuestPath(), true));
            // Resource lookups must name the file the bundle mount actually
            // holds; a canned "/resource" path would make every fopen fail.
            if (selectorName == "pathForResource:ofType:" ||
                selectorName == "pathForResource:ofType:inDirectory:") {
                std::string name;
                std::string type;
                if (auto *nameObject = objectForGuest(memory, registers.r[2]))
                    name = nameObject->stringValue;
                if (auto *typeObject = objectForGuest(memory, registers.r[3]))
                    type = typeObject->stringValue;
                if (name.empty())
                    return finish(0);
                std::string path = std::string(bundleGuestPath()) + "/";
                if (selectorName == "pathForResource:ofType:inDirectory:") {
                    // The fourth selector argument arrives on the guest stack.
                    GuestAddress extraArgument = 0;
                    if (registers.r[13] != 0)
                        (void)memory.read(registers.r[13], &extraArgument,
                                          sizeof(extraArgument));
                    std::string directory;
                    if (auto *directoryObject = objectForGuest(memory, extraArgument))
                        directory = directoryObject->stringValue;
                    if (!directory.empty()) {
                        path += directory;
                        path += "/";
                    }
                }
                path += name;
                if (!type.empty()) {
                    path += ".";
                    path += type;
                }
                return finish(createGuestString(memory, path, true));
            }
            return false;
        }
        // ---- EAGLContext ---------------------------------------------------
        if (receiverIsKindOf(receiverObject, "EAGLContext")) {
            if (selectorName == "initWithAPI:") {
                lifecycle.currentContext = receiverAddress;
                recordLifecycleEvent(lifecycle, "-[EAGLContext initWithAPI:]");
                return finish(receiverAddress);
            }
            if (selectorName == "presentRenderbuffer:") {
                const bool presented = gles::forwarderFor(memory).presentDrawable();
                recordLifecycleEvent(lifecycle,
                                     presented
                                         ? "-[EAGLContext presentRenderbuffer:] -> frame presented"
                                         : "-[EAGLContext presentRenderbuffer:] -> no drawable storage");
                return finish(1);
            }
            if (selectorName == "renderbufferStorage:fromDrawable:") {
                // The drawable's rectangle lives in the layer's materialized instance
                // slots (0..3 = frame); a size the runtime cannot read stays
                // unattached instead of being guessed.
                std::uint32_t width = 0;
                std::uint32_t height = 0;
                if (auto *drawable = objectForGuest(memory, registers.r[2]);
                    drawable != nullptr && drawable->ivars.size() >= 4) {
                    const float rawWidth = floatFromBits(drawable->ivars[2]);
                    const float rawHeight = floatFromBits(drawable->ivars[3]);
                    if (rawWidth >= 1.0f && rawHeight >= 1.0f && rawWidth <= 4096.0f &&
                        rawHeight <= 4096.0f) {
                        width = static_cast<std::uint32_t>(rawWidth);
                        height = static_cast<std::uint32_t>(rawHeight);
                    }
                }
                if (width == 0 || height == 0) {
                    recordLifecycleEvent(lifecycle,
                                         "-[EAGLContext renderbufferStorage:fromDrawable:] -> "
                                         "drawable size unavailable");
                    return finish(1);
                }
                gles::forwarderFor(memory).attachDrawable(memory, width, height);
                recordLifecycleEvent(lifecycle,
                                     "-[EAGLContext renderbufferStorage:fromDrawable:] -> " +
                                         std::to_string(width) + "x" +
                                         std::to_string(height));
                return finish(1);
            }
            if (selectorName == "setCurrentContext:") {
                lifecycle.currentContext = registers.r[2];
                return finish(0);
            }
            if (selectorName == "isMultiThreaded")
                return finish(1);
            if (selectorName == "setMultiThreaded:")
                return finish(0);
            if (selectorName == "sharegroup")
                return finish(0);
            return false;
        }
        // ---- bounded Foundation objects ------------------------------------
        if (lifecycleFoundationMessage(registers, memory, selectorName, receiverAddress,
                                      receiverObject, rawReturn, reason))
            return true;
        if (!reason.empty())
            return false;

        // ---- selectors served generically on any receiver -------------------
        if (selectorName == "performSelector:" || selectorName == "performSelector:withObject:" ||
            selectorName == "performSelectorOnMainThread:withObject:waitUntilDone:") {
            if (performSelectorNested(registers, memory, selectorName, receiverAddress, *guestTarget,
                                      reason))
                return true;
            recordLifecycleEvent(lifecycle, "performSelector skipped (no guest implementation)");
            return finish(0);
        }
        return false;
    } catch (const std::exception &error) {
        reason = error.what();
        return false;
    }
}

BootLifecycleHooks ShimAdapter::lifecycleHooks() {
    BootLifecycleHooks hooks;
    hooks.setMainThreadServiceLimit = [this](GuestAddressSpace &memory, std::uint32_t frames) {
        setMainThreadServiceLimit(memory, frames);
    };
    hooks.finishMainThreadEntry = [this](GuestAddressSpace &memory) {
        guestState(memory).lifecycle.mainThreadEntryRunning = false;
    };
    hooks.prepareMainThreadEntry = [this](GuestAddressSpace &memory, CpuRegisterState &registers,
                                          GuestAddress &entryPoint, std::string &reason) {
        return prepareQueuedMainThreadEntry(memory, registers, entryPoint, reason);
    };
    hooks.describe = [this](GuestAddressSpace &memory, radek::Json &report) {
        const auto outcome = lifecycleOutcome(memory);
        radek::Json lifecycle = radek::Json::object();
        lifecycle["applicationMainEntered"] = outcome.applicationMainEntered;
        lifecycle["applicationMainReturned"] = outcome.applicationMainReturned;
        lifecycle["delegateClassName"] = outcome.delegateClassName;
        lifecycle["mainThreadFramesServiced"] =
            static_cast<std::uint64_t>(outcome.mainThreadFramesServiced);
        lifecycle["mainThreadFramesLimit"] =
            static_cast<std::uint64_t>(outcome.mainThreadFramesLimit);
        lifecycle["mainThreadQueueExhausted"] = outcome.mainThreadQueueExhausted;
        radek::Json events = radek::Json::array();
        for (const auto &event : outcome.events)
            events.push(radek::Json(event));
        lifecycle["events"] = std::move(events);
        lifecycle["note"] =
            "startup-chain trace: lifecycle messages delivered to real guest code; "
            "the trace itself is not a rendered-frame, GPU-surface, or gameplay claim";
        report["lifecycle"] = lifecycle;
    };
    return hooks;
}

} // namespace radek::compat_runtime::objc
