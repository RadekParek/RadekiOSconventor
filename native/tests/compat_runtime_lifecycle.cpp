// Bounded application-lifecycle ("startup chain") coverage.
//
// The fixture is guest memory only: an Objective-C class list with one class
// (`ProbeDelegate`) whose methods live in a mapped executable page. The test then
// drives the exact callouts the loader installs for a real image:
//
//   * `_UIApplicationMain` reads the delegate class name, instantiates the
//     delegate and transfers into the guest `applicationDidFinishLaunching:`
//     implementation (a real guest entry point, not a host callback);
//   * the registered continuation callout resumes the interrupted callout with
//     the caller's return address, so the guest `_main` continues normally;
//   * `+[NSThread detachNewThreadSelector:...]` queues a background entry that
//     `prepareQueuedMainThreadEntry` turns into an ABI-correct guest call;
//   * `performSelectorOnMainThread:...` relays the selector into guest code and
//     the optional `sleepForTimeInterval:` diagnostic limit cancels the thread
//     only when the test explicitly configures one.
//
// No Dynarmic: every assertion is about adapter state and callout results, which
// is exactly what the boot runner consumes.
#include "compat_runtime/objc_shims.hpp"
#include "compat_runtime/shim_registry.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
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
constexpr GuestAddress kCodeBase = 0x20000;
constexpr GuestAddress kStackBase = 0x30000;
constexpr GuestAddress kClassList = kDataBase;
constexpr GuestAddress kClassObject = kDataBase + 0x10;
constexpr GuestAddress kClassRO = kDataBase + 0x40;
constexpr GuestAddress kMetaclassObject = kDataBase + 0x80;
constexpr GuestAddress kMetaclassRO = kDataBase + 0xc0;
constexpr GuestAddress kClassName = kDataBase + 0x100;
constexpr GuestAddress kMethodList = kDataBase + 0x140;
constexpr GuestAddress kSelectorName = kDataBase + 0x180;
constexpr GuestAddress kTypeEncoding = kDataBase + 0x1c0;
constexpr GuestAddress kMainLoopName = kDataBase + 0x200;
constexpr GuestAddress kUpdateName = kDataBase + 0x220;
constexpr GuestAddress kConstantString = kDataBase + 0x240;
constexpr GuestAddress kConstantText = kDataBase + 0x260;
constexpr GuestAddress kScratchString = kDataBase + 0x300;
constexpr GuestAddress kDidFinishImplementation = kCodeBase;
constexpr GuestAddress kMainLoopImplementation = kCodeBase + 0x10;
constexpr GuestAddress kUpdateImplementation = kCodeBase + 0x20;

void writeWord(GuestAddressSpace &memory, GuestAddress address, std::uint32_t value) {
    CHECK(memory.write(address, &value, sizeof(value)));
}

void writeText(GuestAddressSpace &memory, GuestAddress address, const std::string &text) {
    CHECK(memory.write(address, text.c_str(), text.size() + 1));
}

struct Fixture {
    GuestAddressSpace memory{16U * 1024U * 1024U};
    ShimRegistry registry;
    objc::ShimAdapter objc;

    Fixture() {
        memory.mapAt(kDataBase, 0x1000, MemoryPermission::Read | MemoryPermission::Write, "data");
        memory.mapAt(kCodeBase, 0x1000, MemoryPermission::Read | MemoryPermission::Execute, "code");
        memory.mapAt(kStackBase, 0x1000, MemoryPermission::Read | MemoryPermission::Write, "stack");
        // One Objective-C class whose method list has the three methods the
        // startup chain drives.
        writeWord(memory, kClassList, kClassObject);
        writeWord(memory, kClassObject + 0, kMetaclassObject);
        writeWord(memory, kClassObject + 4, 0); // null superclass: root class
        writeWord(memory, kClassObject + 16, kClassRO);
        writeWord(memory, kClassRO + 4, 4);       // instanceStart
        writeWord(memory, kClassRO + 8, 8);       // instanceSize
        writeWord(memory, kClassRO + 16, kClassName);
        writeWord(memory, kClassRO + 20, kMethodList);
        writeWord(memory, kMetaclassObject + 0, kMetaclassObject);
        writeWord(memory, kMetaclassObject + 4, 0);
        writeWord(memory, kMetaclassObject + 16, kMetaclassRO);
        writeWord(memory, kMetaclassRO + 4, 4);
        writeWord(memory, kMetaclassRO + 8, 8);
        writeWord(memory, kMetaclassRO + 16, kClassName);
        writeText(memory, kClassName, "ProbeDelegate");
        writeText(memory, kSelectorName, "applicationDidFinishLaunching:");
        writeText(memory, kTypeEncoding, "v8@0:4");
        writeText(memory, kMainLoopName, "mainloop");
        writeText(memory, kUpdateName, "update");
        writeWord(memory, kMethodList + 0, 12); // stride, no small-method-list flag
        writeWord(memory, kMethodList + 4, 3);
        writeWord(memory, kMethodList + 8, kSelectorName);
        writeWord(memory, kMethodList + 12, kTypeEncoding);
        writeWord(memory, kMethodList + 16, kDidFinishImplementation);
        writeWord(memory, kMethodList + 20, kMainLoopName);
        writeWord(memory, kMethodList + 24, kTypeEncoding);
        writeWord(memory, kMethodList + 28, kMainLoopImplementation);
        writeWord(memory, kMethodList + 32, kUpdateName);
        writeWord(memory, kMethodList + 36, kTypeEncoding);
        writeWord(memory, kMethodList + 40, kUpdateImplementation);
        // Constant string for the delegate class name (__CFConstantString).
        writeWord(memory, kConstantString + 0, 0);
        writeWord(memory, kConstantString + 4, 0x7c8);
        writeWord(memory, kConstantString + 8, kConstantText);
        writeWord(memory, kConstantString + 12, 13);
        writeText(memory, kConstantText, "ProbeDelegate");
        objc.registerBindings(registry);
        std::string reason;
        CHECK(registry.initializeImage(memory,
                                       {{"__DATA", "__objc_classlist", kClassList, 4}}, reason));
    }

    GuestAddress callout(const std::string &symbol) const {
        const auto binding = registry.resolve(symbol);
        CHECK(binding.has_value());
        return binding->guestAddress;
    }

    GuestAddress classData(const std::string &symbol) {
        const auto binding = registry.resolve(symbol);
        CHECK(binding.has_value());
        CHECK(static_cast<bool>(binding->resolveGuestAddress));
        GuestAddress address = 0;
        std::string reason;
        CHECK(binding->resolveGuestAddress(memory, address, reason));
        return address;
    }

    GuestAddress selectorAddress(const std::string &name) {
        writeText(memory, kScratchString, name);
        CpuRegisterState registers;
        registers.r[0] = kScratchString;
        std::string reason;
        CHECK(registry.invokeCallout(callout("_sel_registerName"), registers, memory, reason) ==
              GuestCalloutResult::Returned);
        return registers.r[0];
    }

    GuestCalloutResult send(GuestAddress receiver, const std::string &selector,
                            CpuRegisterState &registers, std::string &reason) {
        registers.r[0] = receiver;
        registers.r[1] = selectorAddress(selector);
        if (registers.r[13] == 0)
            registers.r[13] = kStackBase + 0x800;
        return registry.invokeCallout(callout("_objc_msgSend"), registers, memory, reason);
    }
};

void runUiApplicationMain(Fixture &fixture, const objc::LifecycleOutcome &before) {
    (void)before;
    CpuRegisterState registers;
    registers.r[13] = kStackBase + 0x800;
    registers.r[14] = 0x12345678; // caller (_main) return address
    registers.r[3] = kConstantString;
    std::string reason;
    const auto result =
        fixture.registry.invokeCallout(fixture.callout("_UIApplicationMain"), registers,
                                      fixture.memory, reason);
    CHECK(result == GuestCalloutResult::Transferred);
    CHECK((registers.r[15] & ~GuestAddress{1}) == kDidFinishImplementation);
    CHECK((registers.r[14] & ~GuestAddress{1}) ==
          (fixture.callout("_radek_lifecycle_continuation") & ~GuestAddress{1}));
    CHECK(registers.r[0] != 0); // the delegate instance

    const auto outcome = fixture.objc.lifecycleOutcome(fixture.memory);
    CHECK(outcome.applicationMainEntered);
    CHECK(outcome.delegateClassName == "ProbeDelegate");
    CHECK(!outcome.applicationMainReturned);

    // The guest `applicationDidFinishLaunching:` implementation returns: the
    // continuation restores the caller's return address so `_main` continues.
    CpuRegisterState returning = registers;
    returning.r[0] = 0;
    std::string continuationReason;
    CHECK(fixture.registry.invokeCallout(fixture.callout("_radek_lifecycle_continuation"),
                                        returning, fixture.memory, continuationReason) ==
          GuestCalloutResult::Returned);
    CHECK(returning.r[14] == 0x12345678);
    CHECK(returning.r[0] == 0);
    const auto completed = fixture.objc.lifecycleOutcome(fixture.memory);
    CHECK(completed.applicationMainReturned);
    CHECK(!completed.events.empty());
}

} // namespace

int main() {
    try {
        // The adapters create process-lifetime host objects (the `UIApplication`
        // singleton, the main-thread `NSThread`, the instantiated delegate) the
        // same way the boot runner does, so the fixture itself has process
        // lifetime and is deliberately never destroyed. Otherwise the leak
        // sanitizer reports adapter-owned singletons as leaks even though a real
        // boot keeps them alive until the process exits.
        static Fixture *fixtureHolder = new Fixture();
        Fixture &fixture = *fixtureHolder;
        runUiApplicationMain(fixture, fixture.objc.lifecycleOutcome(fixture.memory));

        // A null delegate class name is refused with a named diagnostic.
        {
            CpuRegisterState registers;
            registers.r[13] = kStackBase + 0x800;
            registers.r[14] = 0x1;
            registers.r[3] = 0;
            std::string reason;
            CHECK(fixture.registry.invokeCallout(fixture.callout("_UIApplicationMain"), registers,
                                                fixture.memory, reason) ==
                  GuestCalloutResult::Failed);
            CHECK(reason.find("null class-name") != std::string::npos);
        }

        // Background thread detach + explicitly bounded diagnostic service.
        const auto delegate = [&]() {
            // `+[UIApplication sharedApplication] -delegate` returns the address
            // the adapter instantiated for this image.
            CpuRegisterState registers;
            std::string reason;
            CHECK(fixture.send(fixture.classData("_OBJC_CLASS_$_UIApplication"),
                               "sharedApplication", registers, reason) ==
                  GuestCalloutResult::Returned);
            const auto application = registers.r[0];
            CpuRegisterState delegateRegisters;
            std::string delegateReason;
            CHECK(fixture.send(application, "delegate", delegateRegisters, delegateReason) ==
                  GuestCalloutResult::Returned);
            return delegateRegisters.r[0];
        }();
        CHECK(delegate != 0);

        fixture.objc.setMainThreadServiceLimit(fixture.memory, 2);
        {
            CpuRegisterState registers;
            registers.r[2] = fixture.selectorAddress("mainloop");
            registers.r[3] = delegate;
            std::string reason;
            CHECK(fixture.send(fixture.classData("_OBJC_CLASS_$_NSThread"),
                               "detachNewThreadSelector:toTarget:withObject:", registers,
                               reason) == GuestCalloutResult::Returned);
            GuestAddress entry = 0;
            std::string preparation;
            CHECK(fixture.objc.prepareQueuedMainThreadEntry(fixture.memory, registers, entry,
                                                            preparation));
            CHECK((entry & ~GuestAddress{1}) == kMainLoopImplementation);
            CHECK(registers.r[0] == delegate);
            CHECK(registers.r[1] != 0);
        }
        {
            // `performSelectorOnMainThread:` relays into the guest `update`.
            CpuRegisterState registers;
            registers.r[2] = fixture.selectorAddress("update");
            registers.r[3] = 0;
            std::string reason;
            const auto result = fixture.send(delegate, "performSelectorOnMainThread:withObject:"
                                                      "waitUntilDone:", registers, reason);
            CHECK(result == GuestCalloutResult::Transferred);
            CHECK((registers.r[15] & ~GuestAddress{1}) == kUpdateImplementation);
            CHECK((registers.r[14] & ~GuestAddress{1}) ==
                  (fixture.callout("_radek_lifecycle_continuation") & ~GuestAddress{1}));
            CpuRegisterState returning = registers;
            std::string continuationReason;
            CHECK(fixture.registry.invokeCallout(fixture.callout("_radek_lifecycle_continuation"),
                                                returning, fixture.memory, continuationReason) ==
                  GuestCalloutResult::Returned);
            const auto outcome = fixture.objc.lifecycleOutcome(fixture.memory);
            CHECK(outcome.mainThreadFramesServiced == 1);
            CHECK(outcome.mainThreadFramesLimit == 2);
            CHECK(!outcome.mainThreadQueueExhausted);
        }
        {
            // An explicitly configured diagnostic service: two sleeps cancel
            // the virtual thread, so the test loop exits deterministically.
            CpuRegisterState registers;
            std::string reason;
            CHECK(fixture.send(fixture.classData("_OBJC_CLASS_$_NSThread"), "currentThread",
                               registers, reason) == GuestCalloutResult::Returned);
            const auto thread = registers.r[0];
            for (int index = 0; index < 2; ++index) {
                CpuRegisterState sleepRegisters;
                std::string sleepReason;
                CHECK(fixture.send(thread, "sleepForTimeInterval:", sleepRegisters, sleepReason) ==
                      GuestCalloutResult::Returned);
            }
            const auto outcome = fixture.objc.lifecycleOutcome(fixture.memory);
            CHECK(outcome.mainThreadQueueExhausted);
            CpuRegisterState cancelled;
            std::string cancelledReason;
            CHECK(fixture.send(thread, "isCancelled", cancelled, cancelledReason) ==
                  GuestCalloutResult::Returned);
            CHECK(cancelled.r[0] == 1);
        }
        // ---- run loop: the frame pump -------------------------------------
        // A game draws a second frame only while something services its run
        // loop. Regression guard for the black-screen failure: CFRunLoopRun
        // must keep re-entering the guest once per ready source instead of
        // returning to main() right after applicationDidFinishLaunching:.
        {
            const auto timerClass = fixture.classData("_OBJC_CLASS_$_NSTimer");
            const auto mainloopSelector = fixture.selectorAddress("mainloop");

            // +[NSTimer scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:]
            // AAPCS: the double interval occupies the even-aligned pair r2/r3,
            // so the remaining arguments spill onto the stack.
            CpuRegisterState schedule;
            schedule.r[13] = kStackBase + 0x800;
            const double interval = 1.0 / 60.0;
            std::uint64_t intervalBits = 0;
            std::memcpy(&intervalBits, &interval, sizeof(intervalBits));
            schedule.r[2] = static_cast<std::uint32_t>(intervalBits & 0xffffffffu);
            schedule.r[3] = static_cast<std::uint32_t>(intervalBits >> 32);
            const std::array<std::uint32_t, 4> timerArguments{delegate, mainloopSelector, 0u, 1u};
            CHECK(fixture.memory.write(kStackBase + 0x800, timerArguments.data(),
                                       sizeof(timerArguments)));
            std::string scheduleReason;
            CHECK(fixture.send(timerClass,
                               "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:",
                               schedule, scheduleReason) == GuestCalloutResult::Returned);
            CHECK(schedule.r[0] != 0); // the timer object

            CpuRegisterState loop;
            loop.r[13] = kStackBase + 0x800;
            loop.r[14] = 0x22334455; // the run loop's caller (_main/UIApplicationMain)
            std::string loopReason;
            CHECK(fixture.registry.invokeCallout(fixture.callout("_CFRunLoopRun"), loop,
                                                fixture.memory, loopReason) ==
                  GuestCalloutResult::Transferred);
            CHECK((loop.r[15] & ~GuestAddress{1}) == kMainLoopImplementation);
            CHECK((loop.r[14] & ~GuestAddress{1}) ==
                  (fixture.callout("_radek_lifecycle_continuation") & ~GuestAddress{1}));

            // A repeating timer must be serviced again on every return: that
            // repeated re-entry is what makes frame 2, 3, ... happen at all.
            for (int frame = 0; frame < 3; ++frame) {
                CpuRegisterState returning = loop;
                std::string continuationReason;
                CHECK(fixture.registry.invokeCallout(
                          fixture.callout("_radek_lifecycle_continuation"), returning,
                          fixture.memory, continuationReason) == GuestCalloutResult::Transferred);
                CHECK((returning.r[15] & ~GuestAddress{1}) == kMainLoopImplementation);
                loop = returning;
            }
            {
                auto outcome = fixture.objc.lifecycleOutcome(fixture.memory);
                CHECK(outcome.runLoopIterations >= 4);
                CHECK(outcome.runLoopRunning);
                CHECK(!outcome.runLoopExited);
            }

            // Invalidating the timer empties the queue, so the loop returns to
            // its caller instead of spinning forever.
            {
                CpuRegisterState invalidate;
                std::string reason;
                CHECK(fixture.send(schedule.r[0], "invalidate", invalidate, reason) ==
                      GuestCalloutResult::Returned);
            }
            CpuRegisterState exiting = loop;
            std::string exitReason;
            CHECK(fixture.registry.invokeCallout(fixture.callout("_radek_lifecycle_continuation"),
                                                exiting, fixture.memory, exitReason) ==
                  GuestCalloutResult::Returned);
            CHECK(exiting.r[14] == 0x22334455); // back to the run loop's caller
            const auto finished = fixture.objc.lifecycleOutcome(fixture.memory);
            CHECK(!finished.runLoopRunning);
            CHECK(finished.runLoopExited);
        }
        std::printf("compat-runtime lifecycle tests passed\n");
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "compat-runtime lifecycle test failed: %s\n", error.what());
        return 1;
    }
}
