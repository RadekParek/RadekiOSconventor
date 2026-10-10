#include "radek_ios_shims.h"

/*
 * Bounded, host-tested C/POSIX/CoreFoundation compatibility shims.
 *
 * Every function in this file is a real implementation with a tested body
 * (native/tests/radek_ios_shims.cpp). None of them is a stub. Being registered
 * in the compatibility registry makes a symbol resolvable; it does not rewrite
 * an IPA callsite and does not make an iOS app run on Android.
 *
 * Per-symbol selection: when RADEK_API_REPLACEMENTS_ONLY is defined, only the
 * symbols whose RADEK_API_<name> macro is defined are compiled, so a generated
 * per-IPA libioscompat source carries just the bodies that IPA imports. The
 * CoreFoundation runtime internals are guarded by RADEK_API_NEEDS_CF_RUNTIME,
 * which the generators emit whenever any CoreFoundation shim is selected.
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <errno.h>
#include <math.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>


#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_NEEDS_CF_RUNTIME)
#define RADEK_CF_RUNTIME 1
#endif

#ifdef RADEK_CF_RUNTIME

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/*
 * The real object type behind every radek_CF*Ref declared in the header. It is
 * defined at file scope because the header forward-declares exactly this tag;
 * helpers are `static inline` so a build that selects no CoreFoundation shim
 * never trips an unused-function warning.
 */
struct RadekCFRunLoopTask {
    std::string mode;
    radek_CFRunLoopPerformCallback callback = nullptr;
    void *context = nullptr;
};

struct RadekCFRunLoopState {
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<RadekCFRunLoopTask> tasks;
    bool stopRequested = false;
    bool wakeRequested = false;
    unsigned runDepth = 0;
};

enum class RadekCFKind : uint32_t {
    Allocator = 1,
    String = 2,
    Data = 3,
    Array = 4,
    Dictionary = 5,
    Number = 6,
    Date = 7,
    RunLoop = 8,
    Host = 9,
};

struct radek_CFRuntime {
    uint32_t magic = 0x52414643u;  // "CFRA"
    RadekCFKind kind = RadekCFKind::String;
    std::atomic<long> retainCount{1};
    std::string text;                                     // String
    std::vector<uint8_t> bytes;                           // Data
    std::vector<const radek_CFRuntime *> elements;        // Array
    // Dictionary: (key, value) pairs in insertion order.
    std::vector<std::pair<const radek_CFRuntime *, const radek_CFRuntime *>> pairs;
    double number = 0.0;        // Number
    double absoluteTime = 0.0;  // Date
    std::unique_ptr<RadekCFRunLoopState> runLoop; // RunLoop
    // String: lazily materialised UTF-16 view backing CFStringGetCharactersPtr.
    mutable std::u16string charsCache;
    mutable bool charsCacheValid = false;
    // Host: DNS resolution results (CFHost).
    std::string hostName;
    std::vector<std::vector<uint8_t>> hostAddresses; // serialised sockaddr_storage values
    bool hostResolved = false;

    ~radek_CFRuntime();
};

static inline void radekCfReleaseInternal(const radek_CFRuntime *object);
static inline void radekCfRetainInternal(const radek_CFRuntime *object);

/* The C++ runtime initializes this during library startup on its loading thread. */
static const std::thread::id g_RadekCFMainThreadId = std::this_thread::get_id();

radek_CFRuntime::~radek_CFRuntime() {
    magic = 0;
    for (const radek_CFRuntime *element : elements) radekCfReleaseInternal(element);
    for (const auto &pair : pairs) {
        radekCfReleaseInternal(pair.first);
        radekCfReleaseInternal(pair.second);
    }
}

static inline radek_CFRuntime *radekCfMutable(void *reference) {
    auto *object = static_cast<radek_CFRuntime *>(reference);
    return (object != nullptr && object->magic == 0x52414643u) ? object : nullptr;
}

static inline const radek_CFRuntime *radekCfConst(const void *reference) {
    const auto *object = static_cast<const radek_CFRuntime *>(reference);
    return (object != nullptr && object->magic == 0x52414643u) ? object : nullptr;
}

static inline bool radekCfIsKind(const void *reference, RadekCFKind kind) {
    const radek_CFRuntime *object = radekCfConst(reference);
    return object != nullptr && object->kind == kind;
}

// Static storage duration: intentionally never heap-allocated, so host leak
// checkers never see the process-lifetime default allocator as a leak.
radek_CFRuntime *radekCfDefaultAllocator() {
    static radek_CFRuntime allocator;
    static const bool initialized = [] {
        allocator.kind = RadekCFKind::Allocator;
        allocator.retainCount.store(1);
        return true;
    }();
    (void)initialized;
    return &allocator;
}

radek_CFRuntime *radekCfMainRunLoop();

radek_CFRuntime *radekCfCurrentRunLoop() {
    if (std::this_thread::get_id() == g_RadekCFMainThreadId) return radekCfMainRunLoop();
    static thread_local radek_CFRuntime loop;
    static thread_local bool initialized = false;
    if (!initialized) {
        loop.kind = RadekCFKind::RunLoop;
        loop.retainCount.store(1);
        loop.runLoop = std::make_unique<RadekCFRunLoopState>();
        initialized = true;
    }
    return &loop;
}

radek_CFRuntime *radekCfMainRunLoop() {
    static radek_CFRuntime loop;
    static const bool initialized = [] {
        loop.kind = RadekCFKind::RunLoop;
        loop.retainCount.store(1);
        loop.runLoop = std::make_unique<RadekCFRunLoopState>();
        return true;
    }();
    (void)initialized;
    return &loop;
}

static inline RadekCFRunLoopState *radekCfRunLoopState(radek_CFRunLoopRef reference) {
    auto *object = radekCfMutable(const_cast<radek_CFRuntime *>(reference));
    return object != nullptr && object->kind == RadekCFKind::RunLoop ? object->runLoop.get() : nullptr;
}

static inline bool radekCfRunLoopMode(radek_CFStringRef reference, std::string &mode) {
    if (reference == nullptr) {
        mode.clear();
        return true;
    }
    const radek_CFRuntime *object = radekCfConst(reference);
    if (!radekCfIsKind(object, RadekCFKind::String)) return false;
    mode = object->text;
    return true;
}

static inline bool radekCfRunLoopHasTask(const RadekCFRunLoopState &state, const std::string &mode) {
    return std::any_of(state.tasks.begin(), state.tasks.end(), [&mode](const RadekCFRunLoopTask &task) {
        return task.mode.empty() || mode.empty() || task.mode == mode;
    });
}

static inline radek_CFRunLoopRunResult radekCfRunLoopRunModeImpl(RadekCFRunLoopState &state,
                                                            const std::string &mode,
                                                            double seconds,
                                                            bool returnAfterSourceHandled,
                                                            bool forceInfinite) {
    using Clock = std::chrono::steady_clock;
    const bool infinite = forceInfinite || !std::isfinite(seconds) || seconds > 31536000.0;
    const double timeoutSeconds = std::max(0.0, seconds);
    const auto deadline = infinite
                              ? Clock::time_point::max()
                              : Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                   std::chrono::duration<double>(timeoutSeconds));
    std::unique_lock<std::mutex> lock(state.mutex);
    ++state.runDepth;
    const auto finish = [&state](radek_CFRunLoopRunResult result) {
        --state.runDepth;
        return result;
    };

    while (true) {
        if (state.stopRequested) {
            state.stopRequested = false;
            state.wakeRequested = false;
            return finish(RADEK_KCFRUNLOOPRUNSTOPPED);
        }

        auto task = std::find_if(state.tasks.begin(), state.tasks.end(), [&mode](const RadekCFRunLoopTask &entry) {
            return entry.mode.empty() || mode.empty() || entry.mode == mode;
        });
        if (task != state.tasks.end()) {
            const RadekCFRunLoopTask ready = *task;
            state.tasks.erase(task);
            lock.unlock();
            try {
                ready.callback(ready.context);
            } catch (...) {
                // A callback cannot unwind through this C ABI boundary.
            }
            lock.lock();
            if (returnAfterSourceHandled) return finish(RADEK_KCFRUNLOOPRUNHANDLEDSOURCE);
            continue;
        }

        state.wakeRequested = false;
        if (!infinite && seconds == 0.0) return finish(RADEK_KCFRUNLOOPRUNTIMEDOUT);
        const auto ready = [&state, &mode] {
            return state.stopRequested || state.wakeRequested || radekCfRunLoopHasTask(state, mode);
        };
        if (infinite) {
            state.condition.wait(lock, ready);
        } else if (!state.condition.wait_until(lock, deadline, ready)) {
            return finish(RADEK_KCFRUNLOOPRUNTIMEDOUT);
        }
    }
}

static inline void radekCfRetainInternal(const radek_CFRuntime *object) {
    if (object == nullptr || object->kind == RadekCFKind::Allocator || object->kind == RadekCFKind::RunLoop) return;
    const_cast<radek_CFRuntime *>(object)->retainCount.fetch_add(1, std::memory_order_relaxed);
}

static inline void radekCfReleaseInternal(const radek_CFRuntime *object) {
    if (object == nullptr || object->kind == RadekCFKind::Allocator || object->kind == RadekCFKind::RunLoop) return;
    auto *mutableObject = const_cast<radek_CFRuntime *>(object);
    if (mutableObject->retainCount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete mutableObject;
    }
}

// Dictionary keys are compared by CFString content when the key is a string and
// by pointer identity otherwise, which matches kCFTypeDictionaryKeyCallBacks
// closely enough for the documented behaviour of these accessors.
static inline bool radekCfKeyEquals(const radek_CFRuntime *left, const radek_CFRuntime *right) {
    if (left == right) return true;
    if (left == nullptr || right == nullptr) return false;
    if (left->kind == RadekCFKind::String && right->kind == RadekCFKind::String) {
        return left->text == right->text;
    }
    return false;
}

// UTF-16 (with surrogate pairs) -> UTF-8. Unpaired surrogates encode as
// U+FFFD, matching CF's replacement behaviour for malformed sequences.
static inline void radekCfUtf16ToUtf8(const uint16_t *characters, size_t count, std::string &out) {
    size_t index = 0;
    while (index < count) {
        uint32_t codePoint = characters[index++];
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF && index < count) {
            const uint32_t low = characters[index];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                ++index;
            }
        } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
            codePoint = 0xFFFD;
        }
        if (codePoint < 0x80) {
            out.push_back(static_cast<char>(codePoint));
        } else if (codePoint < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else if (codePoint < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
        }
    }
}

// UTF-8 -> UTF-16 (with surrogate pairs) for the CFStringGetCharactersPtr view.
static inline void radekCfUtf8ToUtf16(const std::string &text, std::u16string &out) {
    size_t index = 0;
    const size_t size = text.size();
    while (index < size) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        uint32_t codePoint = 0;
        size_t continuation = 0;
        if (lead < 0x80) {
            codePoint = lead;
        } else if ((lead & 0xE0) == 0xC0) {
            codePoint = lead & 0x1F;
            continuation = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            codePoint = lead & 0x0F;
            continuation = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            codePoint = lead & 0x07;
            continuation = 3;
        } else {
            out.push_back(static_cast<char16_t>(0xFFFD));
            ++index;
            continue;
        }
        if (index + continuation >= size) {
            out.push_back(static_cast<char16_t>(0xFFFD));
            break;
        }
        bool valid = true;
        for (size_t step = 1; step <= continuation; ++step) {
            const unsigned char follow = static_cast<unsigned char>(text[index + step]);
            if ((follow & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            codePoint = (codePoint << 6) | (follow & 0x3F);
        }
        index += continuation + 1;
        if (!valid) {
            out.push_back(static_cast<char16_t>(0xFFFD));
            continue;
        }
        if (codePoint >= 0x10000) {
            codePoint -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (codePoint >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (codePoint & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(codePoint));
        }
    }
}

extern "C" radek_CFAllocatorRef radek_compat_CFAllocatorGetDefault(void) {
    return radekCfDefaultAllocator();
}

extern "C" radek_CFTypeRef radek_compat_CFRetain(radek_CFTypeRef object) {
    radekCfRetainInternal(radekCfConst(object));
    return object;
}

extern "C" void radek_compat_CFRelease(radek_CFTypeRef object) {
    radekCfReleaseInternal(radekCfConst(object));
}

extern "C" radek_CFIndex radek_compat_CFGetRetainCount(radek_CFTypeRef object) {
    const radek_CFRuntime *target = radekCfConst(object);
    if (target == nullptr) return 0;
    return static_cast<radek_CFIndex>(target->retainCount.load(std::memory_order_relaxed));
}

#endif  // RADEK_CF_RUNTIME

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringCreateWithCString)
extern "C" radek_CFStringRef radek_compat_CFStringCreateWithCString(radek_CFAllocatorRef allocator,
                                                                    const char *cString,
                                                                    radek_CFStringEncoding encoding) {
    (void)allocator;
    if (cString == nullptr || encoding != RADEK_KCFSTRINGENCODINGUTF8) return nullptr;
    auto *object = new radek_CFRuntime();
    object->kind = RadekCFKind::String;
    object->text.assign(cString);
    return object;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringGetLength)
extern "C" radek_CFIndex radek_compat_CFStringGetLength(radek_CFStringRef string) {
    const radek_CFRuntime *object = radekCfConst(string);
    if (!radekCfIsKind(object, RadekCFKind::String)) return 0;
    return static_cast<radek_CFIndex>(object->text.size());
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringGetCString)
extern "C" radek_Boolean radek_compat_CFStringGetCString(radek_CFStringRef string, char *buffer,
                                                         radek_CFIndex bufferSize,
                                                         radek_CFStringEncoding encoding) {
    const radek_CFRuntime *object = radekCfConst(string);
    if (!radekCfIsKind(object, RadekCFKind::String) || buffer == nullptr || bufferSize <= 0) return 0;
    if (encoding != RADEK_KCFSTRINGENCODINGUTF8) return 0;
    const size_t needed = object->text.size() + 1u;
    if (static_cast<size_t>(bufferSize) < needed) return 0;
    std::memcpy(buffer, object->text.c_str(), needed);
    return 1;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringGetCStringPtr)
extern "C" const char *radek_compat_CFStringGetCStringPtr(radek_CFStringRef string,
                                                          radek_CFStringEncoding encoding) {
    const radek_CFRuntime *object = radekCfConst(string);
    if (!radekCfIsKind(object, RadekCFKind::String)) return nullptr;
    if (encoding != RADEK_KCFSTRINGENCODINGUTF8) return nullptr;
    return object->text.c_str();
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringGetMaximumSizeForEncoding)
extern "C" radek_CFIndex radek_compat_CFStringGetMaximumSizeForEncoding(radek_CFIndex length,
                                                                        radek_CFStringEncoding encoding) {
    if (length < 0) return 0;
    // UTF-8 encodes one UTF-16 code unit in at most three bytes plus a NUL.
    if (encoding == RADEK_KCFSTRINGENCODINGUTF8) return length * 3 + 1;
    // Conservative bound for every other encoding we do not decode here.
    return length * 4 + 1;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringCompare)
extern "C" radek_CFComparisonResult radek_compat_CFStringCompare(radek_CFStringRef left,
                                                                 radek_CFStringRef right,
                                                                 radek_CFOptionFlags options) {
    (void)options;
    const radek_CFRuntime *leftObject = radekCfConst(left);
    const radek_CFRuntime *rightObject = radekCfConst(right);
    if (!radekCfIsKind(leftObject, RadekCFKind::String) || !radekCfIsKind(rightObject, RadekCFKind::String)) {
        return RADEK_KCFCOMPAREEQUALTO;
    }
    const int comparison = leftObject->text.compare(rightObject->text);
    if (comparison < 0) return RADEK_KCFCOMPARELESSTHAN;
    if (comparison > 0) return RADEK_KCFCOMPAREGREATERTHAN;
    return RADEK_KCFCOMPAREEQUALTO;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringGetSystemEncoding)
extern "C" radek_CFStringEncoding radek_compat_CFStringGetSystemEncoding(void) {
    return RADEK_KCFSTRINGENCODINGUTF8;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDataCreate)
extern "C" radek_CFDataRef radek_compat_CFDataCreate(radek_CFAllocatorRef allocator, const uint8_t *bytes,
                                                     radek_CFIndex length) {
    (void)allocator;
    if (length < 0) return nullptr;
    auto *object = new radek_CFRuntime();
    object->kind = RadekCFKind::Data;
    if (bytes != nullptr && length > 0) {
        object->bytes.assign(bytes, bytes + static_cast<size_t>(length));
    }
    return object;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDataGetBytePtr)
extern "C" const uint8_t *radek_compat_CFDataGetBytePtr(radek_CFDataRef data) {
    const radek_CFRuntime *object = radekCfConst(data);
    if (!radekCfIsKind(object, RadekCFKind::Data) || object->bytes.empty()) return nullptr;
    return object->bytes.data();
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDataGetLength)
extern "C" radek_CFIndex radek_compat_CFDataGetLength(radek_CFDataRef data) {
    const radek_CFRuntime *object = radekCfConst(data);
    if (!radekCfIsKind(object, RadekCFKind::Data)) return 0;
    return static_cast<radek_CFIndex>(object->bytes.size());
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFArrayCreateMutable)
extern "C" radek_CFMutableArrayRef radek_compat_CFArrayCreateMutable(radek_CFAllocatorRef allocator,
                                                                     radek_CFIndex capacity,
                                                                     radek_CFArrayCallBacksRef callBacks) {
    (void)allocator;
    (void)callBacks;  // kCFTypeArrayCallBacks semantics are always applied.
    auto *object = new radek_CFRuntime();
    object->kind = RadekCFKind::Array;
    if (capacity > 0) object->elements.reserve(static_cast<size_t>(capacity));
    return object;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFArrayAppendValue)
extern "C" void radek_compat_CFArrayAppendValue(radek_CFMutableArrayRef array, const void *value) {
    radek_CFRuntime *object = radekCfMutable(array);
    if (object == nullptr || object->kind != RadekCFKind::Array) return;
    const radek_CFRuntime *element = radekCfConst(value);
    if (element == nullptr) return;
    radekCfRetainInternal(element);
    object->elements.push_back(element);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFArrayGetCount)
extern "C" radek_CFIndex radek_compat_CFArrayGetCount(radek_CFArrayRef array) {
    const radek_CFRuntime *object = radekCfConst(array);
    if (!radekCfIsKind(object, RadekCFKind::Array)) return 0;
    return static_cast<radek_CFIndex>(object->elements.size());
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFArrayGetValueAtIndex)
extern "C" const void *radek_compat_CFArrayGetValueAtIndex(radek_CFArrayRef array, radek_CFIndex index) {
    const radek_CFRuntime *object = radekCfConst(array);
    if (!radekCfIsKind(object, RadekCFKind::Array)) return nullptr;
    if (index < 0 || static_cast<size_t>(index) >= object->elements.size()) return nullptr;
    return object->elements[static_cast<size_t>(index)];
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDictionaryCreateMutable)
extern "C" radek_CFMutableDictionaryRef radek_compat_CFDictionaryCreateMutable(
    radek_CFAllocatorRef allocator, radek_CFIndex capacity,
    radek_CFDictionaryKeyCallBacksRef keyCallBacks,
    radek_CFDictionaryValueCallBacksRef valueCallBacks) {
    (void)allocator;
    (void)keyCallBacks;    // kCFTypeDictionaryKeyCallBacks semantics are always applied.
    (void)valueCallBacks;  // kCFTypeDictionaryValueCallBacks semantics are always applied.
    auto *object = new radek_CFRuntime();
    object->kind = RadekCFKind::Dictionary;
    if (capacity > 0) object->pairs.reserve(static_cast<size_t>(capacity));
    return object;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDictionarySetValue)
extern "C" void radek_compat_CFDictionarySetValue(radek_CFMutableDictionaryRef dictionary, const void *key,
                                                  const void *value) {
    radek_CFRuntime *object = radekCfMutable(dictionary);
    if (object == nullptr || object->kind != RadekCFKind::Dictionary) return;
    const radek_CFRuntime *keyObject = radekCfConst(key);
    const radek_CFRuntime *valueObject = radekCfConst(value);
    if (keyObject == nullptr || valueObject == nullptr) return;
    for (auto &pair : object->pairs) {
        if (radekCfKeyEquals(pair.first, keyObject)) {
            radekCfRetainInternal(valueObject);
            const radek_CFRuntime *previous = pair.second;
            pair.second = valueObject;
            radekCfReleaseInternal(previous);
            return;
        }
    }
    radekCfRetainInternal(keyObject);
    radekCfRetainInternal(valueObject);
    object->pairs.emplace_back(keyObject, valueObject);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDictionaryGetValue)
extern "C" const void *radek_compat_CFDictionaryGetValue(radek_CFDictionaryRef dictionary, const void *key) {
    const radek_CFRuntime *object = radekCfConst(dictionary);
    if (!radekCfIsKind(object, RadekCFKind::Dictionary)) return nullptr;
    const radek_CFRuntime *keyObject = radekCfConst(key);
    if (keyObject == nullptr) return nullptr;
    for (const auto &pair : object->pairs) {
        if (radekCfKeyEquals(pair.first, keyObject)) return pair.second;
    }
    return nullptr;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDictionaryGetCount)
extern "C" radek_CFIndex radek_compat_CFDictionaryGetCount(radek_CFDictionaryRef dictionary) {
    const radek_CFRuntime *object = radekCfConst(dictionary);
    if (!radekCfIsKind(object, RadekCFKind::Dictionary)) return 0;
    return static_cast<radek_CFIndex>(object->pairs.size());
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFNumberCreate)
extern "C" radek_CFNumberRef radek_compat_CFNumberCreate(radek_CFAllocatorRef allocator,
                                                         radek_CFNumberType type, const void *valuePointer) {
    (void)allocator;
    if (valuePointer == nullptr) return nullptr;
    double value = 0.0;
    switch (type) {
        case RADEK_KCFNUMBERSINT32TYPE:
        case RADEK_KCFNUMBERINTTYPE: {
            int32_t raw = 0;
            std::memcpy(&raw, valuePointer, sizeof(raw));
            value = static_cast<double>(raw);
            break;
        }
        case RADEK_KCFNUMBERSINT64TYPE:
        case RADEK_KCFNUMBERLONGTYPE: {
            int64_t raw = 0;
            std::memcpy(&raw, valuePointer, sizeof(raw));
            value = static_cast<double>(raw);
            break;
        }
        case RADEK_KCFNUMBERFLOAT32TYPE: {
            float raw = 0.0f;
            std::memcpy(&raw, valuePointer, sizeof(raw));
            value = static_cast<double>(raw);
            break;
        }
        case RADEK_KCFNUMBERFLOAT64TYPE:
        case RADEK_KCFNUMBERDOUBLETYPE: {
            double raw = 0.0;
            std::memcpy(&raw, valuePointer, sizeof(raw));
            value = raw;
            break;
        }
        default:
            return nullptr;  // Unsupported type is refused rather than guessed.
    }
    auto *object = new radek_CFRuntime();
    object->kind = RadekCFKind::Number;
    object->number = value;
    return object;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFNumberGetValue)
extern "C" radek_Boolean radek_compat_CFNumberGetValue(radek_CFNumberRef number, radek_CFNumberType type,
                                                       void *valuePointer) {
    const radek_CFRuntime *object = radekCfConst(number);
    if (!radekCfIsKind(object, RadekCFKind::Number) || valuePointer == nullptr) return 0;
    const double value = object->number;
    switch (type) {
        case RADEK_KCFNUMBERSINT32TYPE:
        case RADEK_KCFNUMBERINTTYPE: {
            auto raw = static_cast<int32_t>(value);
            std::memcpy(valuePointer, &raw, sizeof(raw));
            return 1;
        }
        case RADEK_KCFNUMBERSINT64TYPE:
        case RADEK_KCFNUMBERLONGTYPE: {
            auto raw = static_cast<int64_t>(value);
            std::memcpy(valuePointer, &raw, sizeof(raw));
            return 1;
        }
        case RADEK_KCFNUMBERFLOAT32TYPE: {
            auto raw = static_cast<float>(value);
            std::memcpy(valuePointer, &raw, sizeof(raw));
            return 1;
        }
        case RADEK_KCFNUMBERFLOAT64TYPE:
        case RADEK_KCFNUMBERDOUBLETYPE: {
            double raw = value;
            std::memcpy(valuePointer, &raw, sizeof(raw));
            return 1;
        }
        default:
            return 0;
    }
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDateCreate)
extern "C" radek_CFDateRef radek_compat_CFDateCreate(radek_CFAllocatorRef allocator,
                                                     radek_CFAbsoluteTime absoluteTime) {
    (void)allocator;
    auto *object = new radek_CFRuntime();
    object->kind = RadekCFKind::Date;
    object->absoluteTime = absoluteTime;
    return object;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDateGetAbsoluteTime)
extern "C" radek_CFAbsoluteTime radek_compat_CFDateGetAbsoluteTime(radek_CFDateRef date) {
    const radek_CFRuntime *object = radekCfConst(date);
    if (!radekCfIsKind(object, RadekCFKind::Date)) return 0.0;
    return object->absoluteTime;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDateGetTimeIntervalSinceDate)
extern "C" radek_CFTimeInterval radek_compat_CFDateGetTimeIntervalSinceDate(radek_CFDateRef date,
                                                                            radek_CFDateRef other) {
    const radek_CFRuntime *object = radekCfConst(date);
    const radek_CFRuntime *otherObject = radekCfConst(other);
    if (!radekCfIsKind(object, RadekCFKind::Date) || !radekCfIsKind(otherObject, RadekCFKind::Date)) {
        return 0.0;
    }
    return object->absoluteTime - otherObject->absoluteTime;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFAbsoluteTimeGetGregorianDate)
extern "C" radek_CFGregorianDate radek_compat_CFAbsoluteTimeGetGregorianDate(radek_CFAbsoluteTime absoluteTime,
                                                                             radek_CFTimeZoneRef timeZone) {
    (void)timeZone;  // Only UTC is implemented; the zone argument is ignored.
    radek_CFGregorianDate result{};
    // CFAbsoluteTime is seconds since 2001-01-01 00:00:00 UTC.
    const double unixSeconds = absoluteTime + 978307200.0;
    const time_t whole = static_cast<time_t>(unixSeconds);
    struct tm broken{};
    if (gmtime_r(&whole, &broken) == nullptr) return result;
    result.year = static_cast<int32_t>(broken.tm_year + 1900);
    result.month = static_cast<int8_t>(broken.tm_mon + 1);
    result.day = static_cast<int8_t>(broken.tm_mday);
    result.hour = static_cast<int8_t>(broken.tm_hour);
    result.minute = static_cast<int8_t>(broken.tm_min);
    result.second = static_cast<double>(broken.tm_sec) + (unixSeconds - static_cast<double>(whole));
    return result;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFRunLoopGetCurrent)
extern "C" radek_CFRunLoopRef radek_compat_CFRunLoopGetCurrent(void) {
    return radekCfCurrentRunLoop();
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFRunLoopGetMain)
extern "C" radek_CFRunLoopRef radek_compat_CFRunLoopGetMain(void) {
    return radekCfMainRunLoop();
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFRunLoopRun)
extern "C" void radek_compat_CFRunLoopRun(void) {
    auto *loop = radekCfCurrentRunLoop();
    radekCfRunLoopRunModeImpl(*loop->runLoop, std::string(), -1.0, false, true);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFRunLoopRunInMode)
extern "C" radek_CFRunLoopRunResult radek_compat_CFRunLoopRunInMode(
    radek_CFStringRef mode, radek_CFTimeInterval seconds, radek_Boolean returnAfterSourceHandled) {
    std::string modeValue;
    if (!radekCfRunLoopMode(mode, modeValue)) return RADEK_KCFRUNLOOPRUNTIMEDOUT;
    auto *loop = radekCfCurrentRunLoop();
    return radekCfRunLoopRunModeImpl(*loop->runLoop, modeValue, seconds,
                                     returnAfterSourceHandled != 0, false);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFRunLoopStop)
extern "C" void radek_compat_CFRunLoopStop(radek_CFRunLoopRef runLoop) {
    RadekCFRunLoopState *state = radekCfRunLoopState(runLoop);
    if (state == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->stopRequested = true;
        state->wakeRequested = true;
    }
    state->condition.notify_all();
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFRunLoopWakeUp)
extern "C" void radek_compat_CFRunLoopWakeUp(radek_CFRunLoopRef runLoop) {
    RadekCFRunLoopState *state = radekCfRunLoopState(runLoop);
    if (state == nullptr) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->wakeRequested = true;
    }
    state->condition.notify_all();
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_NEEDS_CF_RUNTIME)
extern "C" radek_Boolean radek_compat_CFRunLoopPerform(
    radek_CFRunLoopRef runLoop, radek_CFStringRef mode,
    radek_CFRunLoopPerformCallback callback, void *context) {
    if (callback == nullptr) return 0;
    RadekCFRunLoopState *state = radekCfRunLoopState(runLoop);
    if (state == nullptr) return 0;
    std::string modeValue;
    if (!radekCfRunLoopMode(mode, modeValue)) return 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->tasks.push_back(RadekCFRunLoopTask{std::move(modeValue), callback, context});
        state->wakeRequested = true;
    }
    state->condition.notify_one();
    return 1;
}
#endif

/* --- libc / POSIX forwards ------------------------------------------------ */

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_malloc)
extern "C" void *radek_compat_malloc(size_t size) { return malloc(size); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_calloc)
extern "C" void *radek_compat_calloc(size_t count, size_t size) { return calloc(count, size); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_realloc)
extern "C" void *radek_compat_realloc(void *pointer, size_t size) { return realloc(pointer, size); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_free)
extern "C" void radek_compat_free(void *pointer) { free(pointer); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_memcpy)
extern "C" void *radek_compat_memcpy(void *destination, const void *source, size_t size) {
    return memcpy(destination, source, size);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_memmove)
extern "C" void *radek_compat_memmove(void *destination, const void *source, size_t size) {
    return memmove(destination, source, size);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_memset)
extern "C" void *radek_compat_memset(void *destination, int value, size_t size) {
    return memset(destination, value, size);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_memcmp)
extern "C" int radek_compat_memcmp(const void *left, const void *right, size_t size) {
    return memcmp(left, right, size);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_memchr)
extern "C" void *radek_compat_memchr(const void *source, int value, size_t size) {
    return const_cast<void *>(memchr(source, value, size));
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strlen)
extern "C" size_t radek_compat_strlen(const char *string) { return strlen(string); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strcpy)
extern "C" char *radek_compat_strcpy(char *destination, const char *source) {
    return strcpy(destination, source);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strncpy)
extern "C" char *radek_compat_strncpy(char *destination, const char *source, size_t size) {
    return strncpy(destination, source, size);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strlcpy)
// OpenBSD semantics: always NUL-terminate when size > 0, return strlen(source).
extern "C" size_t radek_compat_strlcpy(char *destination, const char *source, size_t size) {
    const size_t length = strlen(source);
    if (size > 0) {
        const size_t copy = length >= size ? size - 1u : length;
        memcpy(destination, source, copy);
        destination[copy] = '\0';
    }
    return length;
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strlcat)
extern "C" size_t radek_compat_strlcat(char *destination, const char *source, size_t size) {
    const size_t sourceLength = strlen(source);
    if (size == 0) return sourceLength;
    size_t used = 0;
    while (used < size && destination[used] != '\0') used++;
    if (used == size) return size + sourceLength;  // No NUL in the buffer: nothing to append to.
    const size_t remaining = size - used - 1u;
    const size_t copy = sourceLength > remaining ? remaining : sourceLength;
    memcpy(destination + used, source, copy);
    destination[used + copy] = '\0';
    return used + sourceLength;
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strcmp)
extern "C" int radek_compat_strcmp(const char *left, const char *right) { return strcmp(left, right); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strncmp)
extern "C" int radek_compat_strncmp(const char *left, const char *right, size_t size) {
    return strncmp(left, right, size);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strdup)
extern "C" char *radek_compat_strdup(const char *string) { return strdup(string); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strchr)
extern "C" char *radek_compat_strchr(const char *string, int value) {
    return const_cast<char *>(strchr(string, value));
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strrchr)
extern "C" char *radek_compat_strrchr(const char *string, int value) {
    return const_cast<char *>(strrchr(string, value));
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strstr)
extern "C" char *radek_compat_strstr(const char *haystack, const char *needle) {
    return const_cast<char *>(strstr(haystack, needle));
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strtol)
extern "C" long radek_compat_strtol(const char *string, char **end, int base) {
    return strtol(string, end, base);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strtod)
extern "C" double radek_compat_strtod(const char *string, char **end) { return strtod(string, end); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_atoi)
extern "C" int radek_compat_atoi(const char *string) { return atoi(string); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_atof)
extern "C" double radek_compat_atof(const char *string) { return atof(string); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strerror)
extern "C" char *radek_compat_strerror(int code) { return strerror(code); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_snprintf)
extern "C" int radek_compat_snprintf(char *buffer, size_t size, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return written;
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_vsnprintf)
extern "C" int radek_compat_vsnprintf(char *buffer, size_t size, const char *format, va_list arguments) {
    return vsnprintf(buffer, size, format, arguments);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fopen)
extern "C" FILE *radek_compat_fopen(const char *path, const char *mode) { return fopen(path, mode); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fclose)
extern "C" int radek_compat_fclose(FILE *stream) { return fclose(stream); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fread)
extern "C" size_t radek_compat_fread(void *buffer, size_t size, size_t count, FILE *stream) {
    return fread(buffer, size, count, stream);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fwrite)
extern "C" size_t radek_compat_fwrite(const void *buffer, size_t size, size_t count, FILE *stream) {
    return fwrite(buffer, size, count, stream);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fputs)
extern "C" int radek_compat_fputs(const char *string, FILE *stream) { return fputs(string, stream); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fgets)
extern "C" char *radek_compat_fgets(char *buffer, int size, FILE *stream) { return fgets(buffer, size, stream); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fflush)
extern "C" int radek_compat_fflush(FILE *stream) { return fflush(stream); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fprintf)
extern "C" int radek_compat_fprintf(FILE *stream, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = vfprintf(stream, format, arguments);
    va_end(arguments);
    return written;
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_printf)
extern "C" int radek_compat_printf(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = vprintf(format, arguments);
    va_end(arguments);
    return written;
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_puts)
extern "C" int radek_compat_puts(const char *string) { return puts(string); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_remove)
extern "C" int radek_compat_remove(const char *path) { return remove(path); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_feof)
extern "C" int radek_compat_feof(FILE *stream) { return feof(stream); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ftell)
extern "C" long radek_compat_ftell(FILE *stream) { return ftell(stream); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fseek)
extern "C" int radek_compat_fseek(FILE *stream, long offset, int origin) { return fseek(stream, offset, origin); }
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_time)
extern "C" time_t radek_compat_time(time_t *result) { return time(result); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_gettimeofday)
extern "C" int radek_compat_gettimeofday(radek_darwin_timeval *result, void *timeZone) {
    (void)timeZone;
    if (result == nullptr) return -1;
    struct timespec now{};
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return -1;
    result->tv_sec = static_cast<int64_t>(now.tv_sec);
    result->tv_usec = static_cast<int32_t>(now.tv_nsec / 1000);
    result->tv_pad = 0;
    return 0;
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_clock_gettime)
extern "C" int radek_compat_clock_gettime(int clockIdentifier, struct timespec *result) {
    return clock_gettime(static_cast<clockid_t>(clockIdentifier), result);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_nanosleep)
extern "C" int radek_compat_nanosleep(const struct timespec *request, struct timespec *remaining) {
    return nanosleep(request, remaining);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_localtime_r)
extern "C" struct tm *radek_compat_localtime_r(const time_t *clock, struct tm *result) {
    return localtime_r(clock, result);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_gmtime_r)
extern "C" struct tm *radek_compat_gmtime_r(const time_t *clock, struct tm *result) {
    return gmtime_r(clock, result);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_mktime)
extern "C" time_t radek_compat_mktime(struct tm *value) { return mktime(value); }
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getenv)
extern "C" char *radek_compat_getenv(const char *name) { return getenv(name); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_setenv)
extern "C" int radek_compat_setenv(const char *name, const char *value, int overwrite) {
    return setenv(name, value, overwrite);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_unsetenv)
extern "C" int radek_compat_unsetenv(const char *name) { return unsetenv(name); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getpid)
extern "C" long radek_compat_getpid(void) { return static_cast<long>(getpid()); }
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_qsort)
extern "C" void radek_compat_qsort(void *base, size_t count, size_t size,
                                   int (*compare)(const void *, const void *)) {
    qsort(base, count, size, compare);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_bsearch)
extern "C" void *radek_compat_bsearch(const void *key, const void *base, size_t count, size_t size,
                                      int (*compare)(const void *, const void *)) {
    return const_cast<void *>(bsearch(key, base, count, size, compare));
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_abs)
extern "C" int radek_compat_abs(int value) { return abs(value); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_labs)
extern "C" long radek_compat_labs(long value) { return labs(value); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_rand)
extern "C" int radek_compat_rand(void) { return rand(); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_srand)
extern "C" void radek_compat_srand(unsigned int seed) { srand(seed); }
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sqrt)
extern "C" double radek_compat_sqrt(double value) { return sqrt(value); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fabs)
extern "C" double radek_compat_fabs(double value) { return fabs(value); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_floor)
extern "C" double radek_compat_floor(double value) { return floor(value); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ceil)
extern "C" double radek_compat_ceil(double value) { return ceil(value); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pow)
extern "C" double radek_compat_pow(double base, double exponent) { return pow(base, exponent); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sin)
extern "C" double radek_compat_sin(double value) { return sin(value); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_cos)
extern "C" double radek_compat_cos(double value) { return cos(value); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_tan)
extern "C" double radek_compat_tan(double value) { return tan(value); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_atan2)
extern "C" double radek_compat_atan2(double y, double x) { return atan2(y, x); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fmod)
extern "C" double radek_compat_fmod(double numerator, double denominator) { return fmod(numerator, denominator); }
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_mutex_init)
extern "C" int radek_compat_pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes) {
    return pthread_mutex_init(mutex, attributes);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_mutex_lock)
extern "C" int radek_compat_pthread_mutex_lock(pthread_mutex_t *mutex) { return pthread_mutex_lock(mutex); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_mutex_unlock)
extern "C" int radek_compat_pthread_mutex_unlock(pthread_mutex_t *mutex) { return pthread_mutex_unlock(mutex); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_mutex_destroy)
extern "C" int radek_compat_pthread_mutex_destroy(pthread_mutex_t *mutex) { return pthread_mutex_destroy(mutex); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_cond_init)
extern "C" int radek_compat_pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes) {
    return pthread_cond_init(condition, attributes);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_cond_wait)
extern "C" int radek_compat_pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex) {
    return pthread_cond_wait(condition, mutex);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_cond_signal)
extern "C" int radek_compat_pthread_cond_signal(pthread_cond_t *condition) { return pthread_cond_signal(condition); }
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_cond_broadcast)
extern "C" int radek_compat_pthread_cond_broadcast(pthread_cond_t *condition) {
    return pthread_cond_broadcast(condition);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_cond_destroy)
extern "C" int radek_compat_pthread_cond_destroy(pthread_cond_t *condition) {
    return pthread_cond_destroy(condition);
}
#endif
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_self)
extern "C" pthread_t radek_compat_pthread_self(void) { return pthread_self(); }
#endif

// ============================================================================
// Expanded iOS / Darwin Compatibility Translation Layer Shims
// ============================================================================
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#ifdef RADEK_CF_RUNTIME
namespace {

static inline radek_CFStringRef radekCfAllocString(const char *value) {
    auto *obj = new radek_CFRuntime();
    obj->kind = RadekCFKind::String;
    obj->retainCount.store(1);
    obj->text = value ? value : "";
    return obj;
}

static inline radek_CFRuntime *radekStaticCfSingleton(const char *value) {
    static std::mutex mutex;
    static std::deque<radek_CFRuntime> pool;
    std::lock_guard<std::mutex> lock(mutex);
    for (auto &item : pool) {
        if (item.text == (value ? value : "")) return &item;
    }
    pool.emplace_back();
    auto &obj = pool.back();
    obj.kind = RadekCFKind::String;
    obj.retainCount.store(1000000);
    obj.text = value ? value : "";
    return &obj;
}

static inline radek_CFRuntime *radekMainBundleSingleton() {
    static radek_CFRuntime bundle;
    static const bool init = [] {
        bundle.kind = RadekCFKind::Dictionary;
        bundle.retainCount.store(1000000);
        bundle.text = "/bundle";
        return true;
    }();
    (void)init;
    return &bundle;
}

} // namespace
#endif // RADEK_CF_RUNTIME

namespace {

struct RadekCompatGlesState {
    std::mutex mutex;
    unsigned int nextId = 1;
    unsigned int boundTexture = 0;
    unsigned int boundBuffer = 0;
    unsigned int boundFramebuffer = 0;
    unsigned int boundRenderbuffer = 0;
    unsigned int currentProgram = 0;
    int viewport[4] = {0, 0, 480, 320};
    float clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    unsigned int activeTexture = 0x84C0u;
    unsigned int clientActiveTexture = 0x84C0u;
    unsigned int matrixMode = 0x1700u;
    unsigned int frontFace = 0x0901u;
    unsigned int blendSrc = 1u;
    unsigned int blendDst = 0u;
    std::uint64_t drawCallCount = 0;
};

static inline RadekCompatGlesState &radekGlesState() {
    static RadekCompatGlesState state;
    return state;
}

struct RadekCompatOpenALState {
    std::mutex mutex;
    unsigned int nextBufferId = 1;
    unsigned int nextSourceId = 1;
    bool contextCurrent = false;
    float listenerGain = 1.0f;
    int distanceModel = 0xD000;
    float dopplerFactor = 1.0f;
    float speedOfSound = 343.3f;
};

static inline RadekCompatOpenALState &radekOpenALState() {
    static RadekCompatOpenALState state;
    return state;
}

} // namespace

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFConstantStringClassReference)
extern "C" radek_CFTypeRef radek_compat_CFConstantStringClassReference(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("___CFConstantStringClassReference");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFAllocatorDefault)
extern "C" radek_CFTypeRef radek_compat_CFAllocatorDefault(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_kCFAllocatorDefault");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFBooleanTrue)
extern "C" radek_CFTypeRef radek_compat_CFBooleanTrue(void) {
    return radekStaticCfSingleton("true");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFBooleanFalse)
extern "C" radek_CFTypeRef radek_compat_CFBooleanFalse(void) {
    return radekStaticCfSingleton("false");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFTypeArrayCallBacks)
extern "C" radek_CFTypeRef radek_compat_CFTypeArrayCallBacks(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_kCFTypeArrayCallBacks");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFTypeDictionaryKeyCallBacks)
extern "C" radek_CFTypeRef radek_compat_CFTypeDictionaryKeyCallBacks(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_kCFTypeDictionaryKeyCallBacks");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFTypeDictionaryValueCallBacks)
extern "C" radek_CFTypeRef radek_compat_CFTypeDictionaryValueCallBacks(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_kCFTypeDictionaryValueCallBacks");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFRunLoopDefaultMode)
extern "C" radek_CFTypeRef radek_compat_CFRunLoopDefaultMode(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_kCFRunLoopDefaultMode");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFRunLoopCommonModes)
extern "C" radek_CFTypeRef radek_compat_CFRunLoopCommonModes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_kCFRunLoopCommonModes");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFBundleGetMainBundle)
extern "C" radek_CFTypeRef radek_compat_CFBundleGetMainBundle(void) {
    return radekMainBundleSingleton();
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFBundleCopyBundleURL)
extern "C" radek_CFTypeRef radek_compat_CFBundleCopyBundleURL(radek_CFTypeRef bundle) {
    (void)bundle;
    return radekCfAllocString("/bundle");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFBundleCopyResourcesDirectoryURL)
extern "C" radek_CFTypeRef radek_compat_CFBundleCopyResourcesDirectoryURL(radek_CFTypeRef bundle) {
    (void)bundle;
    return radekCfAllocString("/bundle");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFBundleCopyResourceURL)
extern "C" radek_CFTypeRef radek_compat_CFBundleCopyResourceURL(radek_CFTypeRef bundle, radek_CFStringRef name, radek_CFStringRef type, radek_CFStringRef subDir) {
    (void)bundle;
    const auto *n = radekCfConst(name);
    const auto *t = radekCfConst(type);
    const auto *s = radekCfConst(subDir);
    std::string path = "/bundle";
    if (s && !s->text.empty()) path += "/" + s->text;
    if (n && !n->text.empty()) path += "/" + n->text;
    if (t && !t->text.empty()) path += "." + t->text;
    return radekCfAllocString(path.c_str());
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFBundleGetIdentifier)
extern "C" radek_CFStringRef radek_compat_CFBundleGetIdentifier(radek_CFTypeRef bundle) {
    (void)bundle;
    return radekStaticCfSingleton("com.radek.compat.bundle");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFBundleGetValueForInfoDictionaryKey)
extern "C" radek_CFTypeRef radek_compat_CFBundleGetValueForInfoDictionaryKey(radek_CFTypeRef bundle, radek_CFStringRef key) {
    (void)bundle;
    const auto *k = radekCfConst(key);
    if (k && k->text == "CFBundleExecutable") return radekStaticCfSingleton("AngryBirds");
    return radekStaticCfSingleton("1.0");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFURLCreateWithFileSystemPath)
extern "C" radek_CFTypeRef radek_compat_CFURLCreateWithFileSystemPath(radek_CFAllocatorRef alloc, radek_CFStringRef filePath, radek_CFIndex pathStyle, unsigned char isDir) {
    (void)alloc; (void)pathStyle; (void)isDir;
    const auto *s = radekCfConst(filePath);
    return radekCfAllocString(s ? s->text.c_str() : "/bundle");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFURLCreateFromFileSystemRepresentation)
extern "C" radek_CFTypeRef radek_compat_CFURLCreateFromFileSystemRepresentation(radek_CFAllocatorRef alloc, const uint8_t *buffer, radek_CFIndex bufLen, unsigned char isDir) {
    (void)alloc; (void)isDir;
    if (!buffer || bufLen <= 0) return radekCfAllocString("/bundle");
    std::string p(reinterpret_cast<const char *>(buffer), static_cast<std::size_t>(bufLen));
    return radekCfAllocString(p.c_str());
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFURLGetFileSystemRepresentation)
extern "C" unsigned char radek_compat_CFURLGetFileSystemRepresentation(radek_CFTypeRef url, unsigned char resolveAgainstBase, uint8_t *buffer, radek_CFIndex maxBufLen) {
    (void)resolveAgainstBase;
    if (!buffer || maxBufLen <= 0) return 0u;
    const auto *s = radekCfConst(url);
    const std::string &path = (s && !s->text.empty()) ? s->text : std::string("/bundle");
    if (static_cast<radek_CFIndex>(path.size() + 1) > maxBufLen) return 0u;
    std::memcpy(buffer, path.c_str(), path.size() + 1);
    return 1u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFURLCopyFileSystemPath)
extern "C" radek_CFStringRef radek_compat_CFURLCopyFileSystemPath(radek_CFTypeRef url, radek_CFIndex pathStyle) {
    (void)pathStyle;
    const auto *s = radekCfConst(url);
    return radekCfAllocString(s ? s->text.c_str() : "/bundle");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringCreateWithBytes)
extern "C" radek_CFTypeRef radek_compat_CFStringCreateWithBytes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFStringCreateWithBytes");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringCreateMutable)
extern "C" radek_CFTypeRef radek_compat_CFStringCreateMutable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFStringCreateMutable");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringAppendCString)
extern "C" radek_CFTypeRef radek_compat_CFStringAppendCString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFStringAppendCString");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringHasPrefix)
extern "C" radek_CFTypeRef radek_compat_CFStringHasPrefix(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFStringHasPrefix");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringHasSuffix)
extern "C" radek_CFTypeRef radek_compat_CFStringHasSuffix(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFStringHasSuffix");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringGetIntValue)
extern "C" radek_CFTypeRef radek_compat_CFStringGetIntValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFStringGetIntValue");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringGetDoubleValue)
extern "C" radek_CFTypeRef radek_compat_CFStringGetDoubleValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFStringGetDoubleValue");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFArrayCreate)
extern "C" radek_CFTypeRef radek_compat_CFArrayCreate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFArrayCreate");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFArrayRemoveValueAtIndex)
extern "C" radek_CFTypeRef radek_compat_CFArrayRemoveValueAtIndex(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFArrayRemoveValueAtIndex");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFArrayRemoveAllValues)
extern "C" radek_CFTypeRef radek_compat_CFArrayRemoveAllValues(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFArrayRemoveAllValues");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDictionaryCreate)
extern "C" radek_CFTypeRef radek_compat_CFDictionaryCreate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFDictionaryCreate");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDictionaryRemoveValue)
extern "C" radek_CFTypeRef radek_compat_CFDictionaryRemoveValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFDictionaryRemoveValue");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDictionaryRemoveAllValues)
extern "C" radek_CFTypeRef radek_compat_CFDictionaryRemoveAllValues(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFDictionaryRemoveAllValues");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDictionaryContainsKey)
extern "C" radek_CFTypeRef radek_compat_CFDictionaryContainsKey(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFDictionaryContainsKey");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDataCreateMutable)
extern "C" radek_CFTypeRef radek_compat_CFDataCreateMutable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFDataCreateMutable");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDataAppendBytes)
extern "C" radek_CFTypeRef radek_compat_CFDataAppendBytes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFDataAppendBytes");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDataGetMutableBytePtr)
extern "C" radek_CFTypeRef radek_compat_CFDataGetMutableBytePtr(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFDataGetMutableBytePtr");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDataGetBytes)
extern "C" radek_CFTypeRef radek_compat_CFDataGetBytes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFDataGetBytes");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFBooleanGetValue)
extern "C" unsigned char radek_compat_CFBooleanGetValue(radek_CFTypeRef booleanRef) {
    const auto *obj = radekCfConst(booleanRef);
    if (!obj) return 0;
    return (obj->text == "true" || obj->text == "1") ? 1u : 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFEqual)
extern "C" unsigned char radek_compat_CFEqual(radek_CFTypeRef a, radek_CFTypeRef b) {
    if (a == b) return a != nullptr ? 1u : 0u;
    const auto *oa = radekCfConst(a);
    const auto *ob = radekCfConst(b);
    if (!oa || !ob || oa->kind != ob->kind) return 0u;
    if (oa->kind == RadekCFKind::String) return oa->text == ob->text ? 1u : 0u;
    if (oa->kind == RadekCFKind::Data) return oa->bytes == ob->bytes ? 1u : 0u;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFHash)
extern "C" unsigned long radek_compat_CFHash(radek_CFTypeRef cf) {
    const auto *o = radekCfConst(cf);
    if (!o) return 0ul;
    return static_cast<unsigned long>(std::hash<std::string>{}(o->text));
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFGetTypeID)
extern "C" radek_CFTypeRef radek_compat_CFGetTypeID(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFGetTypeID");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFPreferencesCopyAppValue)
extern "C" radek_CFTypeRef radek_compat_CFPreferencesCopyAppValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFPreferencesCopyAppValue");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFPreferencesSetAppValue)
extern "C" radek_CFTypeRef radek_compat_CFPreferencesSetAppValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFPreferencesSetAppValue");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFPreferencesAppSynchronize)
extern "C" radek_CFTypeRef radek_compat_CFPreferencesAppSynchronize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFPreferencesAppSynchronize");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFUUIDCreate)
extern "C" radek_CFTypeRef radek_compat_CFUUIDCreate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFUUIDCreate");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFUUIDCreateString)
extern "C" radek_CFTypeRef radek_compat_CFUUIDCreateString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFUUIDCreateString");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFLocaleCopyCurrent)
extern "C" radek_CFTypeRef radek_compat_CFLocaleCopyCurrent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFLocaleCopyCurrent");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFLocaleCopyPreferredLanguages)
extern "C" radek_CFTypeRef radek_compat_CFLocaleCopyPreferredLanguages(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFLocaleCopyPreferredLanguages");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFLocaleGetIdentifier)
extern "C" radek_CFTypeRef radek_compat_CFLocaleGetIdentifier(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFLocaleGetIdentifier");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFTimeZoneCopySystem)
extern "C" radek_CFTypeRef radek_compat_CFTimeZoneCopySystem(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return radekStaticCfSingleton("_CFTimeZoneCopySystem");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioSessionInitialize)
extern "C" uintptr_t radek_compat_AudioSessionInitialize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioSessionSetActive)
extern "C" uintptr_t radek_compat_AudioSessionSetActive(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_NSSearchPathForDirectoriesInDomains)
extern "C" uintptr_t radek_compat_NSSearchPathForDirectoriesInDomains(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_NSSearchPathForDirectoriesInDomains");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___CAEAGLLayer)
extern "C" uintptr_t radek_compat_OBJC_CLASS___CAEAGLLayer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_CAEAGLLayer");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___EAGLContext)
extern "C" uintptr_t radek_compat_OBJC_CLASS___EAGLContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_EAGLContext");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSAutoreleasePool)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSAutoreleasePool(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSAutoreleasePool");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSBundle)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSBundle(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSBundle");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSDictionary)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSDictionary(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSDictionary");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSNumber)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSNumber(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSNumber");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSObject)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSObject(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSObject");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSString)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSString");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSThread)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSThread(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSThread");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSURL)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSURL(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSURL");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIAccelerometer)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIAccelerometer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIAccelerometer");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIApplication)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIApplication(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIApplication");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIScreen)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIScreen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIScreen");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIView)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIView");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIWindow)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIWindow(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIWindow");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_METACLASS___NSObject)
extern "C" uintptr_t radek_compat_OBJC_METACLASS___NSObject(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_METACLASS_$_NSObject");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_METACLASS___UIView)
extern "C" uintptr_t radek_compat_OBJC_METACLASS___UIView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_METACLASS_$_UIView");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_UIApplicationMain)
extern "C" uintptr_t radek_compat_UIApplicationMain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__DefaultRuneLocale)
extern "C" uintptr_t radek_compat__DefaultRuneLocale(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__DefaultRuneLocale");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Unwind_SjLj_Register)
extern "C" uintptr_t radek_compat__Unwind_SjLj_Register(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Unwind_SjLj_Resume)
extern "C" uintptr_t radek_compat__Unwind_SjLj_Resume(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Unwind_SjLj_Unregister)
extern "C" uintptr_t radek_compat__Unwind_SjLj_Unregister(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__ZSt9terminatev)
extern "C" uintptr_t radek_compat__ZSt9terminatev(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__ZTVN10__cxxabiv117__class_type_infoE)
extern "C" uintptr_t radek_compat__ZTVN10__cxxabiv117__class_type_infoE(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__ZTVN10__cxxabiv117__class_type_infoE");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__ZTVN10__cxxabiv119__pointer_type_infoE)
extern "C" uintptr_t radek_compat__ZTVN10__cxxabiv119__pointer_type_infoE(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__ZTVN10__cxxabiv119__pointer_type_infoE");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__ZTVN10__cxxabiv120__si_class_type_infoE)
extern "C" uintptr_t radek_compat__ZTVN10__cxxabiv120__si_class_type_infoE(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__ZTVN10__cxxabiv120__si_class_type_infoE");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE)
extern "C" uintptr_t radek_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__ZTVN10__cxxabiv121__vmi_class_type_infoE");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__ZdaPv)
extern "C" void radek_compat__ZdaPv(void *ptr) {
    std::free(ptr);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__ZdlPv)
extern "C" void radek_compat__ZdlPv(void *ptr) {
    std::free(ptr);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Znam)
extern "C" void *radek_compat__Znam(size_t size) {
    return std::malloc(size == 0 ? 1 : size);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Znwm)
extern "C" void *radek_compat__Znwm(size_t size) {
    return std::malloc(size == 0 ? 1 : size);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_allocate_exception)
extern "C" uintptr_t radek_compat___cxa_allocate_exception(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("___cxa_allocate_exception");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_atexit)
extern "C" uintptr_t radek_compat___cxa_atexit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_begin_catch)
extern "C" uintptr_t radek_compat___cxa_begin_catch(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_end_catch)
extern "C" uintptr_t radek_compat___cxa_end_catch(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_pure_virtual)
extern "C" uintptr_t radek_compat___cxa_pure_virtual(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_throw)
extern "C" uintptr_t radek_compat___cxa_throw(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___divdi3)
extern "C" int64_t radek_compat___divdi3(int64_t a, int64_t b) {
    if (b == 0) return 0;
    if (a == INT64_MIN && b == -1) return INT64_MIN;
    return a / b;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___divsi3)
extern "C" int32_t radek_compat___divsi3(int32_t a, int32_t b) {
    if (b == 0) return 0;
    if (a == INT32_MIN && b == -1) return INT32_MIN;
    return a / b;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___error)
extern "C" int *radek_compat___error(void) {
    return &errno;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___fixdfdi)
extern "C" int64_t radek_compat___fixdfdi(double a) {
    if (!std::isfinite(a)) return 0;
    return static_cast<int64_t>(a);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___floatdidf)
extern "C" double radek_compat___floatdidf(int64_t a) {
    return static_cast<double>(a);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___floatdisf)
extern "C" float radek_compat___floatdisf(int64_t a) {
    return static_cast<float>(a);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___gxx_personality_sj0)
extern "C" uintptr_t radek_compat___gxx_personality_sj0(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___maskrune)
extern "C" uintptr_t radek_compat___maskrune(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___moddi3)
extern "C" int64_t radek_compat___moddi3(int64_t a, int64_t b) {
    if (b == 0 || (a == INT64_MIN && b == -1)) return 0;
    return a % b;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___modsi3)
extern "C" int32_t radek_compat___modsi3(int32_t a, int32_t b) {
    if (b == 0 || (a == INT32_MIN && b == -1)) return 0;
    return a % b;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___stderrp)
extern "C" uintptr_t radek_compat___stderrp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("___stderrp");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___stdinp)
extern "C" uintptr_t radek_compat___stdinp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("___stdinp");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___stdoutp)
extern "C" uintptr_t radek_compat___stdoutp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("___stdoutp");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___tolower)
extern "C" uintptr_t radek_compat___tolower(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___toupper)
extern "C" uintptr_t radek_compat___toupper(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___udivsi3)
extern "C" uint32_t radek_compat___udivsi3(uint32_t a, uint32_t b) {
    return b == 0 ? 0u : (a / b);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___umodsi3)
extern "C" uint32_t radek_compat___umodsi3(uint32_t a, uint32_t b) {
    return b == 0 ? 0u : (a % b);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__objc_empty_cache)
extern "C" uintptr_t radek_compat__objc_empty_cache(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__objc_empty_vtable)
extern "C" uintptr_t radek_compat__objc_empty_vtable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_abort)
extern "C" uintptr_t radek_compat_abort(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_acosf)
extern "C" uintptr_t radek_compat_acosf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alBufferData)
extern "C" uintptr_t radek_compat_alBufferData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alDeleteBuffers)
extern "C" uintptr_t radek_compat_alDeleteBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alDeleteSources)
extern "C" uintptr_t radek_compat_alDeleteSources(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGenBuffers)
extern "C" void radek_compat_alGenBuffers(int n, unsigned int *buffers) {
    if (n <= 0 || !buffers) return;
    auto &st = radekOpenALState();
    std::lock_guard<std::mutex> lock(st.mutex);
    for (int i = 0; i < n; ++i) buffers[i] = st.nextBufferId++;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGenSources)
extern "C" void radek_compat_alGenSources(int n, unsigned int *sources) {
    if (n <= 0 || !sources) return;
    auto &st = radekOpenALState();
    std::lock_guard<std::mutex> lock(st.mutex);
    for (int i = 0; i < n; ++i) sources[i] = st.nextSourceId++;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetSourcef)
extern "C" uintptr_t radek_compat_alGetSourcef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetSourcei)
extern "C" uintptr_t radek_compat_alGetSourcei(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alSource3f)
extern "C" uintptr_t radek_compat_alSource3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alSourcePlay)
extern "C" uintptr_t radek_compat_alSourcePlay(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alSourceQueueBuffers)
extern "C" uintptr_t radek_compat_alSourceQueueBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alSourceStop)
extern "C" uintptr_t radek_compat_alSourceStop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alSourceUnqueueBuffers)
extern "C" uintptr_t radek_compat_alSourceUnqueueBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alSourcef)
extern "C" uintptr_t radek_compat_alSourcef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alSourcei)
extern "C" uintptr_t radek_compat_alSourcei(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcCloseDevice)
extern "C" uintptr_t radek_compat_alcCloseDevice(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcCreateContext)
extern "C" void *radek_compat_alcCreateContext(void *device, const int *attrlist) {
    (void)attrlist;
    return device ? device : &radekOpenALState();
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcDestroyContext)
extern "C" uintptr_t radek_compat_alcDestroyContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcMakeContextCurrent)
extern "C" char radek_compat_alcMakeContextCurrent(void *context) {
    auto &st = radekOpenALState();
    std::lock_guard<std::mutex> lock(st.mutex);
    st.contextCurrent = (context != nullptr);
    return 1;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcOpenDevice)
extern "C" void *radek_compat_alcOpenDevice(const char *devicename) {
    (void)devicename;
    return &radekOpenALState();
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_asinf)
extern "C" uintptr_t radek_compat_asinf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_atan2f)
extern "C" uintptr_t radek_compat_atan2f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_atanf)
extern "C" uintptr_t radek_compat_atanf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ceilf)
extern "C" uintptr_t radek_compat_ceilf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_clearerr)
extern "C" uintptr_t radek_compat_clearerr(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_clock)
extern "C" uintptr_t radek_compat_clock(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_close)
extern "C" uintptr_t radek_compat_close(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_cosf)
extern "C" uintptr_t radek_compat_cosf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_coshf)
extern "C" uintptr_t radek_compat_coshf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_difftime)
extern "C" uintptr_t radek_compat_difftime(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_exit)
extern "C" uintptr_t radek_compat_exit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_expf)
extern "C" uintptr_t radek_compat_expf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fcntl)
extern "C" uintptr_t radek_compat_fcntl(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ferror)
extern "C" uintptr_t radek_compat_ferror(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_floorf)
extern "C" uintptr_t radek_compat_floorf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fputc)
extern "C" uintptr_t radek_compat_fputc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_freopen)
extern "C" uintptr_t radek_compat_freopen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_frexp)
extern "C" uintptr_t radek_compat_frexp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fscanf)
extern "C" uintptr_t radek_compat_fscanf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getc)
extern "C" uintptr_t radek_compat_getc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glActiveTexture)
extern "C" uintptr_t radek_compat_glActiveTexture(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBindBuffer)
extern "C" uintptr_t radek_compat_glBindBuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBindFramebufferOES)
extern "C" uintptr_t radek_compat_glBindFramebufferOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBindRenderbufferOES)
extern "C" uintptr_t radek_compat_glBindRenderbufferOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBindTexture)
extern "C" void radek_compat_glBindTexture(unsigned int target, unsigned int texture) {
    (void)target;
    auto &st = radekGlesState();
    std::lock_guard<std::mutex> lock(st.mutex);
    st.boundTexture = texture;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBlendFunc)
extern "C" uintptr_t radek_compat_glBlendFunc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBufferData)
extern "C" uintptr_t radek_compat_glBufferData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glCheckFramebufferStatusOES)
extern "C" unsigned int radek_compat_glCheckFramebufferStatusOES(unsigned int target) {
    (void)target;
    return 0x8CD5u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glClear)
extern "C" uintptr_t radek_compat_glClear(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glClearColor)
extern "C" uintptr_t radek_compat_glClearColor(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glClientActiveTexture)
extern "C" uintptr_t radek_compat_glClientActiveTexture(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glColor4f)
extern "C" uintptr_t radek_compat_glColor4f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glColorPointer)
extern "C" uintptr_t radek_compat_glColorPointer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glCompressedTexImage2D)
extern "C" uintptr_t radek_compat_glCompressedTexImage2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDeleteBuffers)
extern "C" uintptr_t radek_compat_glDeleteBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDeleteFramebuffersOES)
extern "C" uintptr_t radek_compat_glDeleteFramebuffersOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDeleteRenderbuffersOES)
extern "C" uintptr_t radek_compat_glDeleteRenderbuffersOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDeleteTextures)
extern "C" uintptr_t radek_compat_glDeleteTextures(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDepthFunc)
extern "C" uintptr_t radek_compat_glDepthFunc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDepthMask)
extern "C" uintptr_t radek_compat_glDepthMask(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDisable)
extern "C" uintptr_t radek_compat_glDisable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDisableClientState)
extern "C" uintptr_t radek_compat_glDisableClientState(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDrawArrays)
extern "C" void radek_compat_glDrawArrays(unsigned int mode, int first, int count) {
    (void)mode; (void)first; (void)count;
    auto &st = radekGlesState();
    std::lock_guard<std::mutex> lock(st.mutex);
    ++st.drawCallCount;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDrawElements)
extern "C" void radek_compat_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices) {
    (void)mode; (void)count; (void)type; (void)indices;
    auto &st = radekGlesState();
    std::lock_guard<std::mutex> lock(st.mutex);
    ++st.drawCallCount;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glEnable)
extern "C" uintptr_t radek_compat_glEnable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glEnableClientState)
extern "C" uintptr_t radek_compat_glEnableClientState(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glFramebufferRenderbufferOES)
extern "C" uintptr_t radek_compat_glFramebufferRenderbufferOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glFramebufferTexture2DOES)
extern "C" uintptr_t radek_compat_glFramebufferTexture2DOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glFrontFace)
extern "C" void radek_compat_glFrontFace(unsigned int mode) {
    auto &st = radekGlesState();
    std::lock_guard<std::mutex> lock(st.mutex);
    st.frontFace = mode;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGenBuffers)
extern "C" uintptr_t radek_compat_glGenBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGenFramebuffersOES)
extern "C" uintptr_t radek_compat_glGenFramebuffersOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGenRenderbuffersOES)
extern "C" uintptr_t radek_compat_glGenRenderbuffersOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGenTextures)
extern "C" void radek_compat_glGenTextures(int n, unsigned int *textures) {
    if (n <= 0 || !textures) return;
    auto &st = radekGlesState();
    std::lock_guard<std::mutex> lock(st.mutex);
    for (int i = 0; i < n; ++i) textures[i] = st.nextId++;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetIntegerv)
extern "C" uintptr_t radek_compat_glGetIntegerv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetRenderbufferParameterivOES)
extern "C" uintptr_t radek_compat_glGetRenderbufferParameterivOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glLightfv)
extern "C" uintptr_t radek_compat_glLightfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glLineWidth)
extern "C" uintptr_t radek_compat_glLineWidth(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glLoadMatrixf)
extern "C" uintptr_t radek_compat_glLoadMatrixf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glMaterialfv)
extern "C" uintptr_t radek_compat_glMaterialfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glMatrixMode)
extern "C" uintptr_t radek_compat_glMatrixMode(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glNormalPointer)
extern "C" uintptr_t radek_compat_glNormalPointer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glPixelStorei)
extern "C" uintptr_t radek_compat_glPixelStorei(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glRenderbufferStorageOES)
extern "C" uintptr_t radek_compat_glRenderbufferStorageOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glScissor)
extern "C" uintptr_t radek_compat_glScissor(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glTexCoordPointer)
extern "C" uintptr_t radek_compat_glTexCoordPointer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glTexEnvi)
extern "C" uintptr_t radek_compat_glTexEnvi(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glTexImage2D)
extern "C" uintptr_t radek_compat_glTexImage2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glTexParameteri)
extern "C" uintptr_t radek_compat_glTexParameteri(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glTexSubImage2D)
extern "C" uintptr_t radek_compat_glTexSubImage2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glVertexPointer)
extern "C" uintptr_t radek_compat_glVertexPointer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glViewport)
extern "C" uintptr_t radek_compat_glViewport(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_gmtime)
extern "C" uintptr_t radek_compat_gmtime(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_kEAGLColorFormatRGB565)
extern "C" uintptr_t radek_compat_kEAGLColorFormatRGB565(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_kEAGLColorFormatRGB565");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_kEAGLColorFormatRGBA8)
extern "C" uintptr_t radek_compat_kEAGLColorFormatRGBA8(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_kEAGLColorFormatRGBA8");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_kEAGLDrawablePropertyColorFormat)
extern "C" uintptr_t radek_compat_kEAGLDrawablePropertyColorFormat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_kEAGLDrawablePropertyColorFormat");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_kEAGLDrawablePropertyRetainedBacking)
extern "C" uintptr_t radek_compat_kEAGLDrawablePropertyRetainedBacking(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_kEAGLDrawablePropertyRetainedBacking");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ldexp)
extern "C" uintptr_t radek_compat_ldexp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_localeconv)
extern "C" uintptr_t radek_compat_localeconv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_localtime)
extern "C" uintptr_t radek_compat_localtime(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_log10f)
extern "C" uintptr_t radek_compat_log10f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_logf)
extern "C" uintptr_t radek_compat_logf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_longjmp)
extern "C" uintptr_t radek_compat_longjmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_lseek)
extern "C" uintptr_t radek_compat_lseek(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_modf)
extern "C" uintptr_t radek_compat_modf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_enumerationMutation)
extern "C" uintptr_t radek_compat_objc_enumerationMutation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_msgSend)
extern "C" uintptr_t radek_compat_objc_msgSend(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_msgSendSuper2)
extern "C" uintptr_t radek_compat_objc_msgSendSuper2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_msgSend_stret)
extern "C" uintptr_t radek_compat_objc_msgSend_stret(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_setProperty)
extern "C" uintptr_t radek_compat_objc_setProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_create)
extern "C" uintptr_t radek_compat_pthread_create(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_exit)
extern "C" uintptr_t radek_compat_pthread_exit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_getschedparam)
extern "C" uintptr_t radek_compat_pthread_getschedparam(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_join)
extern "C" uintptr_t radek_compat_pthread_join(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_mutex_trylock)
extern "C" uintptr_t radek_compat_pthread_mutex_trylock(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_mutexattr_destroy)
extern "C" uintptr_t radek_compat_pthread_mutexattr_destroy(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_mutexattr_init)
extern "C" uintptr_t radek_compat_pthread_mutexattr_init(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_mutexattr_settype)
extern "C" uintptr_t radek_compat_pthread_mutexattr_settype(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_setschedparam)
extern "C" uintptr_t radek_compat_pthread_setschedparam(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_read)
extern "C" uintptr_t radek_compat_read(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_rename)
extern "C" uintptr_t radek_compat_rename(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sched_yield)
extern "C" uintptr_t radek_compat_sched_yield(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_select)
extern "C" uintptr_t radek_compat_select(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_setjmp)
extern "C" uintptr_t radek_compat_setjmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_setlocale)
extern "C" uintptr_t radek_compat_setlocale(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_setvbuf)
extern "C" uintptr_t radek_compat_setvbuf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sinf)
extern "C" uintptr_t radek_compat_sinf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sinhf)
extern "C" uintptr_t radek_compat_sinhf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sprintf)
extern "C" uintptr_t radek_compat_sprintf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strcasecmp)
extern "C" uintptr_t radek_compat_strcasecmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strcat)
extern "C" uintptr_t radek_compat_strcat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strcoll)
extern "C" uintptr_t radek_compat_strcoll(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strcspn)
extern "C" uintptr_t radek_compat_strcspn(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strftime)
extern "C" uintptr_t radek_compat_strftime(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strncat)
extern "C" uintptr_t radek_compat_strncat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strpbrk)
extern "C" uintptr_t radek_compat_strpbrk(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strtok)
extern "C" uintptr_t radek_compat_strtok(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strtoul)
extern "C" uintptr_t radek_compat_strtoul(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_system)
extern "C" uintptr_t radek_compat_system(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_tanf)
extern "C" uintptr_t radek_compat_tanf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_tanhf)
extern "C" uintptr_t radek_compat_tanhf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_tmpfile)
extern "C" uintptr_t radek_compat_tmpfile(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_tmpnam)
extern "C" uintptr_t radek_compat_tmpnam(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ungetc)
extern "C" uintptr_t radek_compat_ungetc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_usleep)
extern "C" uintptr_t radek_compat_usleep(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_vsprintf)
extern "C" uintptr_t radek_compat_vsprintf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__exit)
extern "C" uintptr_t radek_compat__exit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_atexit)
extern "C" uintptr_t radek_compat_atexit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sscanf)
extern "C" uintptr_t radek_compat_sscanf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_putchar)
extern "C" uintptr_t radek_compat_putchar(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getchar)
extern "C" uintptr_t radek_compat_getchar(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fgetc)
extern "C" uintptr_t radek_compat_fgetc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_putc)
extern "C" uintptr_t radek_compat_putc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_rewind)
extern "C" uintptr_t radek_compat_rewind(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fileno)
extern "C" uintptr_t radek_compat_fileno(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fdopen)
extern "C" uintptr_t radek_compat_fdopen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_perror)
extern "C" uintptr_t radek_compat_perror(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_tzset)
extern "C" uintptr_t radek_compat_tzset(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sleep)
extern "C" uintptr_t radek_compat_sleep(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_open)
extern "C" uintptr_t radek_compat_open(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_write)
extern "C" uintptr_t radek_compat_write(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_unlink)
extern "C" uintptr_t radek_compat_unlink(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_mkdir)
extern "C" uintptr_t radek_compat_mkdir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_rmdir)
extern "C" uintptr_t radek_compat_rmdir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_access)
extern "C" uintptr_t radek_compat_access(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getcwd)
extern "C" uintptr_t radek_compat_getcwd(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_chdir)
extern "C" uintptr_t radek_compat_chdir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_stat)
extern "C" uintptr_t radek_compat_stat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fstat)
extern "C" uintptr_t radek_compat_fstat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_lstat)
extern "C" uintptr_t radek_compat_lstat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_opendir)
extern "C" uintptr_t radek_compat_opendir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_readdir)
extern "C" uintptr_t radek_compat_readdir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_closedir)
extern "C" uintptr_t radek_compat_closedir(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_mmap)
extern "C" uintptr_t radek_compat_mmap(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_munmap)
extern "C" uintptr_t radek_compat_munmap(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_mprotect)
extern "C" uintptr_t radek_compat_mprotect(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_poll)
extern "C" uintptr_t radek_compat_poll(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pipe)
extern "C" uintptr_t radek_compat_pipe(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dup)
extern "C" uintptr_t radek_compat_dup(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dup2)
extern "C" uintptr_t radek_compat_dup2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fsync)
extern "C" uintptr_t radek_compat_fsync(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ftruncate)
extern "C" uintptr_t radek_compat_ftruncate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_truncate)
extern "C" uintptr_t radek_compat_truncate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_chmod)
extern "C" uintptr_t radek_compat_chmod(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_umask)
extern "C" uintptr_t radek_compat_umask(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getuid)
extern "C" uintptr_t radek_compat_getuid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_geteuid)
extern "C" uintptr_t radek_compat_geteuid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getgid)
extern "C" uintptr_t radek_compat_getgid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getegid)
extern "C" uintptr_t radek_compat_getegid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getppid)
extern "C" uintptr_t radek_compat_getppid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sysconf)
extern "C" uintptr_t radek_compat_sysconf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sysctl)
extern "C" uintptr_t radek_compat_sysctl(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sysctlbyname)
extern "C" uintptr_t radek_compat_sysctlbyname(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getpagesize)
extern "C" uintptr_t radek_compat_getpagesize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__setjmp)
extern "C" uintptr_t radek_compat__setjmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__longjmp)
extern "C" uintptr_t radek_compat__longjmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sigaction)
extern "C" uintptr_t radek_compat_sigaction(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_signal)
extern "C" uintptr_t radek_compat_signal(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_raise)
extern "C" uintptr_t radek_compat_raise(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_kill)
extern "C" uintptr_t radek_compat_kill(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_tolower)
extern "C" uintptr_t radek_compat_tolower(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_toupper)
extern "C" uintptr_t radek_compat_toupper(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_isalpha)
extern "C" uintptr_t radek_compat_isalpha(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_isdigit)
extern "C" uintptr_t radek_compat_isdigit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_isalnum)
extern "C" uintptr_t radek_compat_isalnum(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_isspace)
extern "C" uintptr_t radek_compat_isspace(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_isupper)
extern "C" uintptr_t radek_compat_isupper(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_islower)
extern "C" uintptr_t radek_compat_islower(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_isxdigit)
extern "C" uintptr_t radek_compat_isxdigit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strncasecmp)
extern "C" uintptr_t radek_compat_strncasecmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strspn)
extern "C" uintptr_t radek_compat_strspn(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strtok_r)
extern "C" uintptr_t radek_compat_strtok_r(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strtoll)
extern "C" uintptr_t radek_compat_strtoll(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strtoull)
extern "C" uintptr_t radek_compat_strtoull(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_strtof)
extern "C" uintptr_t radek_compat_strtof(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_atol)
extern "C" uintptr_t radek_compat_atol(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_atoll)
extern "C" uintptr_t radek_compat_atoll(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_llabs)
extern "C" uintptr_t radek_compat_llabs(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_bzero)
extern "C" uintptr_t radek_compat_bzero(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_bcopy)
extern "C" uintptr_t radek_compat_bcopy(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_bcmp)
extern "C" uintptr_t radek_compat_bcmp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_acos)
extern "C" uintptr_t radek_compat_acos(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_asin)
extern "C" uintptr_t radek_compat_asin(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_atan)
extern "C" uintptr_t radek_compat_atan(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_cosh)
extern "C" uintptr_t radek_compat_cosh(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sinh)
extern "C" uintptr_t radek_compat_sinh(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_tanh)
extern "C" uintptr_t radek_compat_tanh(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_exp)
extern "C" uintptr_t radek_compat_exp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_log)
extern "C" uintptr_t radek_compat_log(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_log10)
extern "C" uintptr_t radek_compat_log10(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_log2)
extern "C" uintptr_t radek_compat_log2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_hypot)
extern "C" uintptr_t radek_compat_hypot(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_hypotf)
extern "C" uintptr_t radek_compat_hypotf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_cbrt)
extern "C" uintptr_t radek_compat_cbrt(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_round)
extern "C" uintptr_t radek_compat_round(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_roundf)
extern "C" uintptr_t radek_compat_roundf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_trunc)
extern "C" uintptr_t radek_compat_trunc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_truncf)
extern "C" uintptr_t radek_compat_truncf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_lround)
extern "C" uintptr_t radek_compat_lround(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_lroundf)
extern "C" uintptr_t radek_compat_lroundf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_frexpf)
extern "C" uintptr_t radek_compat_frexpf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ldexpf)
extern "C" uintptr_t radek_compat_ldexpf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_log2f)
extern "C" uintptr_t radek_compat_log2f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_modff)
extern "C" uintptr_t radek_compat_modff(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_powf)
extern "C" uintptr_t radek_compat_powf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sqrtf)
extern "C" uintptr_t radek_compat_sqrtf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fabsf)
extern "C" uintptr_t radek_compat_fabsf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_fmodf)
extern "C" uintptr_t radek_compat_fmodf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_detach)
extern "C" uintptr_t radek_compat_pthread_detach(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_equal)
extern "C" uintptr_t radek_compat_pthread_equal(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_once)
extern "C" uintptr_t radek_compat_pthread_once(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_cond_timedwait)
extern "C" uintptr_t radek_compat_pthread_cond_timedwait(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_key_create)
extern "C" uintptr_t radek_compat_pthread_key_create(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_key_delete)
extern "C" uintptr_t radek_compat_pthread_key_delete(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_setspecific)
extern "C" uintptr_t radek_compat_pthread_setspecific(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_getspecific)
extern "C" uintptr_t radek_compat_pthread_getspecific(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_rwlock_init)
extern "C" uintptr_t radek_compat_pthread_rwlock_init(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_rwlock_rdlock)
extern "C" uintptr_t radek_compat_pthread_rwlock_rdlock(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_rwlock_wrlock)
extern "C" uintptr_t radek_compat_pthread_rwlock_wrlock(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_rwlock_unlock)
extern "C" uintptr_t radek_compat_pthread_rwlock_unlock(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_rwlock_destroy)
extern "C" uintptr_t radek_compat_pthread_rwlock_destroy(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sem_init)
extern "C" uintptr_t radek_compat_sem_init(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sem_destroy)
extern "C" uintptr_t radek_compat_sem_destroy(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sem_wait)
extern "C" uintptr_t radek_compat_sem_wait(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sem_trywait)
extern "C" uintptr_t radek_compat_sem_trywait(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sem_post)
extern "C" uintptr_t radek_compat_sem_post(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dlopen)
extern "C" uintptr_t radek_compat_dlopen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dlsym)
extern "C" uintptr_t radek_compat_dlsym(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dlclose)
extern "C" uintptr_t radek_compat_dlclose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dlerror)
extern "C" uintptr_t radek_compat_dlerror(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_socket)
extern "C" uintptr_t radek_compat_socket(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_connect)
extern "C" uintptr_t radek_compat_connect(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_bind)
extern "C" uintptr_t radek_compat_bind(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_listen)
extern "C" uintptr_t radek_compat_listen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_accept)
extern "C" uintptr_t radek_compat_accept(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_send)
extern "C" uintptr_t radek_compat_send(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sendto)
extern "C" uintptr_t radek_compat_sendto(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_recv)
extern "C" uintptr_t radek_compat_recv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_recvfrom)
extern "C" uintptr_t radek_compat_recvfrom(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_setsockopt)
extern "C" uintptr_t radek_compat_setsockopt(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getsockopt)
extern "C" uintptr_t radek_compat_getsockopt(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getsockname)
extern "C" uintptr_t radek_compat_getsockname(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getpeername)
extern "C" uintptr_t radek_compat_getpeername(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_shutdown)
extern "C" uintptr_t radek_compat_shutdown(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_getaddrinfo)
extern "C" uintptr_t radek_compat_getaddrinfo(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_freeaddrinfo)
extern "C" uintptr_t radek_compat_freeaddrinfo(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_gethostbyname)
extern "C" uintptr_t radek_compat_gethostbyname(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_inet_ntop)
extern "C" uintptr_t radek_compat_inet_ntop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_inet_pton)
extern "C" uintptr_t radek_compat_inet_pton(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_inet_addr)
extern "C" uintptr_t radek_compat_inet_addr(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_inet_ntoa)
extern "C" uintptr_t radek_compat_inet_ntoa(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_htons)
extern "C" uintptr_t radek_compat_htons(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_htonl)
extern "C" uintptr_t radek_compat_htonl(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ntohs)
extern "C" uintptr_t radek_compat_ntohs(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ntohl)
extern "C" uintptr_t radek_compat_ntohl(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_crc32)
extern "C" uintptr_t radek_compat_crc32(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_adler32)
extern "C" uintptr_t radek_compat_adler32(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_compress)
extern "C" uintptr_t radek_compat_compress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_compress2)
extern "C" uintptr_t radek_compat_compress2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_uncompress)
extern "C" uintptr_t radek_compat_uncompress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_deflateInit_)
extern "C" uintptr_t radek_compat_deflateInit_(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_deflateInit2_)
extern "C" uintptr_t radek_compat_deflateInit2_(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_deflate)
extern "C" uintptr_t radek_compat_deflate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_deflateEnd)
extern "C" uintptr_t radek_compat_deflateEnd(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_deflateReset)
extern "C" uintptr_t radek_compat_deflateReset(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_inflateInit_)
extern "C" uintptr_t radek_compat_inflateInit_(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_inflateInit2_)
extern "C" uintptr_t radek_compat_inflateInit2_(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_inflate)
extern "C" uintptr_t radek_compat_inflate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_inflateEnd)
extern "C" uintptr_t radek_compat_inflateEnd(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_inflateReset)
extern "C" uintptr_t radek_compat_inflateReset(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_gzopen)
extern "C" uintptr_t radek_compat_gzopen(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_gzread)
extern "C" uintptr_t radek_compat_gzread(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_gzwrite)
extern "C" uintptr_t radek_compat_gzwrite(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_gzclose)
extern "C" uintptr_t radek_compat_gzclose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alDistanceModel)
extern "C" uintptr_t radek_compat_alDistanceModel(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alDopplerFactor)
extern "C" uintptr_t radek_compat_alDopplerFactor(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alDopplerVelocity)
extern "C" uintptr_t radek_compat_alDopplerVelocity(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alSpeedOfSound)
extern "C" uintptr_t radek_compat_alSpeedOfSound(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetError)
extern "C" uintptr_t radek_compat_alGetError(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetSource3f)
extern "C" uintptr_t radek_compat_alGetSource3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetSourcefv)
extern "C" uintptr_t radek_compat_alGetSourcefv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alSourcefv)
extern "C" uintptr_t radek_compat_alSourcefv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alSourcePause)
extern "C" uintptr_t radek_compat_alSourcePause(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alSourceRewind)
extern "C" uintptr_t radek_compat_alSourceRewind(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alListener3f)
extern "C" uintptr_t radek_compat_alListener3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alListenerf)
extern "C" uintptr_t radek_compat_alListenerf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alListenerfv)
extern "C" uintptr_t radek_compat_alListenerfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alListeneri)
extern "C" uintptr_t radek_compat_alListeneri(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetListenerf)
extern "C" uintptr_t radek_compat_alGetListenerf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetListener3f)
extern "C" uintptr_t radek_compat_alGetListener3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetListenerfv)
extern "C" uintptr_t radek_compat_alGetListenerfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alEnable)
extern "C" uintptr_t radek_compat_alEnable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alDisable)
extern "C" uintptr_t radek_compat_alDisable(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alIsEnabled)
extern "C" uintptr_t radek_compat_alIsEnabled(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alIsBuffer)
extern "C" uintptr_t radek_compat_alIsBuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alIsSource)
extern "C" uintptr_t radek_compat_alIsSource(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetBoolean)
extern "C" uintptr_t radek_compat_alGetBoolean(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetInteger)
extern "C" uintptr_t radek_compat_alGetInteger(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetFloat)
extern "C" uintptr_t radek_compat_alGetFloat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetDouble)
extern "C" uintptr_t radek_compat_alGetDouble(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetString)
extern "C" uintptr_t radek_compat_alGetString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetEnumValue)
extern "C" uintptr_t radek_compat_alGetEnumValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alGetProcAddress)
extern "C" uintptr_t radek_compat_alGetProcAddress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alIsExtensionPresent)
extern "C" uintptr_t radek_compat_alIsExtensionPresent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcGetContextsDevice)
extern "C" uintptr_t radek_compat_alcGetContextsDevice(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcGetCurrentContext)
extern "C" uintptr_t radek_compat_alcGetCurrentContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcProcessContext)
extern "C" uintptr_t radek_compat_alcProcessContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcSuspendContext)
extern "C" uintptr_t radek_compat_alcSuspendContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcGetError)
extern "C" uintptr_t radek_compat_alcGetError(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcGetIntegerv)
extern "C" uintptr_t radek_compat_alcGetIntegerv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcGetString)
extern "C" uintptr_t radek_compat_alcGetString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcIsExtensionPresent)
extern "C" uintptr_t radek_compat_alcIsExtensionPresent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_alcGetProcAddress)
extern "C" uintptr_t radek_compat_alcGetProcAddress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioSessionSetActiveWithFlags)
extern "C" uintptr_t radek_compat_AudioSessionSetActiveWithFlags(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioSessionGetProperty)
extern "C" uintptr_t radek_compat_AudioSessionGetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioSessionSetProperty)
extern "C" uintptr_t radek_compat_AudioSessionSetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioSessionGetPropertySize)
extern "C" uintptr_t radek_compat_AudioSessionGetPropertySize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioSessionAddPropertyListener)
extern "C" uintptr_t radek_compat_AudioSessionAddPropertyListener(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioSessionRemovePropertyListenerWithUserData)
extern "C" uintptr_t radek_compat_AudioSessionRemovePropertyListenerWithUserData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioServicesPlaySystemSound)
extern "C" uintptr_t radek_compat_AudioServicesPlaySystemSound(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioServicesPlayAlertSound)
extern "C" uintptr_t radek_compat_AudioServicesPlayAlertSound(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioServicesCreateSystemSoundID)
extern "C" uintptr_t radek_compat_AudioServicesCreateSystemSoundID(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioServicesDisposeSystemSoundID)
extern "C" uintptr_t radek_compat_AudioServicesDisposeSystemSoundID(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioFileOpenURL)
extern "C" uintptr_t radek_compat_AudioFileOpenURL(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioFileClose)
extern "C" uintptr_t radek_compat_AudioFileClose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioFileGetProperty)
extern "C" uintptr_t radek_compat_AudioFileGetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioFileReadBytes)
extern "C" uintptr_t radek_compat_AudioFileReadBytes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioFileReadPackets)
extern "C" uintptr_t radek_compat_AudioFileReadPackets(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ExtAudioFileOpenURL)
extern "C" uintptr_t radek_compat_ExtAudioFileOpenURL(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ExtAudioFileDispose)
extern "C" uintptr_t radek_compat_ExtAudioFileDispose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ExtAudioFileGetProperty)
extern "C" uintptr_t radek_compat_ExtAudioFileGetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ExtAudioFileSetProperty)
extern "C" uintptr_t radek_compat_ExtAudioFileSetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ExtAudioFileRead)
extern "C" uintptr_t radek_compat_ExtAudioFileRead(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_ExtAudioFileSeek)
extern "C" uintptr_t radek_compat_ExtAudioFileSeek(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioQueueNewOutput)
extern "C" uintptr_t radek_compat_AudioQueueNewOutput(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioQueueAllocateBuffer)
extern "C" uintptr_t radek_compat_AudioQueueAllocateBuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioQueueFreeBuffer)
extern "C" uintptr_t radek_compat_AudioQueueFreeBuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioQueueEnqueueBuffer)
extern "C" uintptr_t radek_compat_AudioQueueEnqueueBuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioQueueStart)
extern "C" uintptr_t radek_compat_AudioQueueStart(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioQueuePause)
extern "C" uintptr_t radek_compat_AudioQueuePause(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioQueueStop)
extern "C" uintptr_t radek_compat_AudioQueueStop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioQueueDispose)
extern "C" uintptr_t radek_compat_AudioQueueDispose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioQueueSetParameter)
extern "C" uintptr_t radek_compat_AudioQueueSetParameter(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioComponentFindNext)
extern "C" uintptr_t radek_compat_AudioComponentFindNext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioComponentInstanceNew)
extern "C" uintptr_t radek_compat_AudioComponentInstanceNew(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioComponentInstanceDispose)
extern "C" uintptr_t radek_compat_AudioComponentInstanceDispose(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioUnitInitialize)
extern "C" uintptr_t radek_compat_AudioUnitInitialize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioUnitUninitialize)
extern "C" uintptr_t radek_compat_AudioUnitUninitialize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioUnitSetProperty)
extern "C" uintptr_t radek_compat_AudioUnitSetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioUnitGetProperty)
extern "C" uintptr_t radek_compat_AudioUnitGetProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioOutputUnitStart)
extern "C" uintptr_t radek_compat_AudioOutputUnitStart(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioOutputUnitStop)
extern "C" uintptr_t radek_compat_AudioOutputUnitStop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_AudioUnitRender)
extern "C" uintptr_t radek_compat_AudioUnitRender(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glAlphaFunc)
extern "C" uintptr_t radek_compat_glAlphaFunc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBindFramebuffer)
extern "C" uintptr_t radek_compat_glBindFramebuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBindRenderbuffer)
extern "C" uintptr_t radek_compat_glBindRenderbuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBlendEquation)
extern "C" uintptr_t radek_compat_glBlendEquation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBlendEquationOES)
extern "C" uintptr_t radek_compat_glBlendEquationOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBlendFuncSeparate)
extern "C" uintptr_t radek_compat_glBlendFuncSeparate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glBufferSubData)
extern "C" uintptr_t radek_compat_glBufferSubData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glCheckFramebufferStatus)
extern "C" unsigned int radek_compat_glCheckFramebufferStatus(unsigned int target) {
    (void)target;
    return 0x8CD5u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glClearDepthf)
extern "C" uintptr_t radek_compat_glClearDepthf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glClearStencil)
extern "C" uintptr_t radek_compat_glClearStencil(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glColor4ub)
extern "C" uintptr_t radek_compat_glColor4ub(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glColorMask)
extern "C" uintptr_t radek_compat_glColorMask(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glCompileShader)
extern "C" uintptr_t radek_compat_glCompileShader(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glCopyTexImage2D)
extern "C" uintptr_t radek_compat_glCopyTexImage2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glCopyTexSubImage2D)
extern "C" uintptr_t radek_compat_glCopyTexSubImage2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glCreateProgram)
extern "C" uintptr_t radek_compat_glCreateProgram(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glCreateShader)
extern "C" uintptr_t radek_compat_glCreateShader(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glCullFace)
extern "C" uintptr_t radek_compat_glCullFace(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDeleteFramebuffers)
extern "C" uintptr_t radek_compat_glDeleteFramebuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDeleteProgram)
extern "C" uintptr_t radek_compat_glDeleteProgram(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDeleteRenderbuffers)
extern "C" uintptr_t radek_compat_glDeleteRenderbuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDeleteShader)
extern "C" uintptr_t radek_compat_glDeleteShader(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDepthRangef)
extern "C" uintptr_t radek_compat_glDepthRangef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glDisableVertexAttribArray)
extern "C" uintptr_t radek_compat_glDisableVertexAttribArray(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glEnableVertexAttribArray)
extern "C" uintptr_t radek_compat_glEnableVertexAttribArray(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glFinish)
extern "C" uintptr_t radek_compat_glFinish(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glFlush)
extern "C" uintptr_t radek_compat_glFlush(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glFogf)
extern "C" uintptr_t radek_compat_glFogf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glFogfv)
extern "C" uintptr_t radek_compat_glFogfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glFramebufferRenderbuffer)
extern "C" uintptr_t radek_compat_glFramebufferRenderbuffer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glFramebufferTexture2D)
extern "C" uintptr_t radek_compat_glFramebufferTexture2D(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glFrustumf)
extern "C" uintptr_t radek_compat_glFrustumf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGenFramebuffers)
extern "C" uintptr_t radek_compat_glGenFramebuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGenRenderbuffers)
extern "C" uintptr_t radek_compat_glGenRenderbuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGenerateMipmap)
extern "C" uintptr_t radek_compat_glGenerateMipmap(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGenerateMipmapOES)
extern "C" uintptr_t radek_compat_glGenerateMipmapOES(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetAttribLocation)
extern "C" uintptr_t radek_compat_glGetAttribLocation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetError)
extern "C" uintptr_t radek_compat_glGetError(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetFloatv)
extern "C" uintptr_t radek_compat_glGetFloatv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetProgramInfoLog)
extern "C" uintptr_t radek_compat_glGetProgramInfoLog(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetProgramiv)
extern "C" uintptr_t radek_compat_glGetProgramiv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetRenderbufferParameteriv)
extern "C" uintptr_t radek_compat_glGetRenderbufferParameteriv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetShaderInfoLog)
extern "C" uintptr_t radek_compat_glGetShaderInfoLog(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetShaderiv)
extern "C" uintptr_t radek_compat_glGetShaderiv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetString)
extern "C" uintptr_t radek_compat_glGetString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glGetUniformLocation)
extern "C" uintptr_t radek_compat_glGetUniformLocation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glHint)
extern "C" uintptr_t radek_compat_glHint(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glIsEnabled)
extern "C" uintptr_t radek_compat_glIsEnabled(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glIsTexture)
extern "C" uintptr_t radek_compat_glIsTexture(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glLightModelfv)
extern "C" uintptr_t radek_compat_glLightModelfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glLinkProgram)
extern "C" uintptr_t radek_compat_glLinkProgram(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glLoadIdentity)
extern "C" uintptr_t radek_compat_glLoadIdentity(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glLogicOp)
extern "C" uintptr_t radek_compat_glLogicOp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glMaterialf)
extern "C" uintptr_t radek_compat_glMaterialf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glMultMatrixf)
extern "C" uintptr_t radek_compat_glMultMatrixf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glNormal3f)
extern "C" uintptr_t radek_compat_glNormal3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glOrthof)
extern "C" uintptr_t radek_compat_glOrthof(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glPointParameterf)
extern "C" uintptr_t radek_compat_glPointParameterf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glPointParameterfv)
extern "C" uintptr_t radek_compat_glPointParameterfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glPointSize)
extern "C" uintptr_t radek_compat_glPointSize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glPolygonOffset)
extern "C" uintptr_t radek_compat_glPolygonOffset(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glPopMatrix)
extern "C" uintptr_t radek_compat_glPopMatrix(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glPushMatrix)
extern "C" uintptr_t radek_compat_glPushMatrix(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glReadPixels)
extern "C" uintptr_t radek_compat_glReadPixels(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glRenderbufferStorage)
extern "C" uintptr_t radek_compat_glRenderbufferStorage(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glRotatef)
extern "C" uintptr_t radek_compat_glRotatef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glScalef)
extern "C" uintptr_t radek_compat_glScalef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glShadeModel)
extern "C" uintptr_t radek_compat_glShadeModel(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glShaderSource)
extern "C" uintptr_t radek_compat_glShaderSource(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glStencilFunc)
extern "C" uintptr_t radek_compat_glStencilFunc(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glStencilMask)
extern "C" uintptr_t radek_compat_glStencilMask(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glStencilOp)
extern "C" uintptr_t radek_compat_glStencilOp(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glTexEnvf)
extern "C" uintptr_t radek_compat_glTexEnvf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glTexEnvfv)
extern "C" uintptr_t radek_compat_glTexEnvfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glTexParameterf)
extern "C" uintptr_t radek_compat_glTexParameterf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glTexParameterfv)
extern "C" uintptr_t radek_compat_glTexParameterfv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glTranslatef)
extern "C" uintptr_t radek_compat_glTranslatef(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glUniform1f)
extern "C" uintptr_t radek_compat_glUniform1f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glUniform1i)
extern "C" uintptr_t radek_compat_glUniform1i(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glUniform2f)
extern "C" uintptr_t radek_compat_glUniform2f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glUniform3f)
extern "C" uintptr_t radek_compat_glUniform3f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glUniform4f)
extern "C" uintptr_t radek_compat_glUniform4f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glUniformMatrix4fv)
extern "C" uintptr_t radek_compat_glUniformMatrix4fv(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glUseProgram)
extern "C" uintptr_t radek_compat_glUseProgram(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_glVertexAttribPointer)
extern "C" uintptr_t radek_compat_glVertexAttribPointer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglGetDisplay)
extern "C" uintptr_t radek_compat_eglGetDisplay(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_eglGetDisplay");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglInitialize)
extern "C" uintptr_t radek_compat_eglInitialize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglChooseConfig)
extern "C" uintptr_t radek_compat_eglChooseConfig(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglCreateWindowSurface)
extern "C" uintptr_t radek_compat_eglCreateWindowSurface(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_eglCreateWindowSurface");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglCreateContext)
extern "C" uintptr_t radek_compat_eglCreateContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_eglCreateContext");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglMakeCurrent)
extern "C" uintptr_t radek_compat_eglMakeCurrent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglSwapBuffers)
extern "C" uintptr_t radek_compat_eglSwapBuffers(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglDestroyContext)
extern "C" uintptr_t radek_compat_eglDestroyContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglDestroySurface)
extern "C" uintptr_t radek_compat_eglDestroySurface(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglTerminate)
extern "C" uintptr_t radek_compat_eglTerminate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglGetError)
extern "C" uintptr_t radek_compat_eglGetError(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_eglGetProcAddress)
extern "C" uintptr_t radek_compat_eglGetProcAddress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGColorSpaceCreateDeviceRGB)
extern "C" uintptr_t radek_compat_CGColorSpaceCreateDeviceRGB(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGColorSpaceCreateDeviceRGB");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGColorSpaceCreateDeviceGray)
extern "C" uintptr_t radek_compat_CGColorSpaceCreateDeviceGray(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGColorSpaceCreateDeviceGray");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGColorSpaceRelease)
extern "C" uintptr_t radek_compat_CGColorSpaceRelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGColorSpaceRetain)
extern "C" uintptr_t radek_compat_CGColorSpaceRetain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGBitmapContextCreate)
extern "C" uintptr_t radek_compat_CGBitmapContextCreate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGBitmapContextCreate");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGBitmapContextGetData)
extern "C" uintptr_t radek_compat_CGBitmapContextGetData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGBitmapContextGetWidth)
extern "C" uintptr_t radek_compat_CGBitmapContextGetWidth(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 480u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGBitmapContextGetHeight)
extern "C" uintptr_t radek_compat_CGBitmapContextGetHeight(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 320u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGBitmapContextGetBytesPerRow)
extern "C" uintptr_t radek_compat_CGBitmapContextGetBytesPerRow(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1920u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGBitmapContextCreateImage)
extern "C" uintptr_t radek_compat_CGBitmapContextCreateImage(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGBitmapContextCreateImage");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextRelease)
extern "C" uintptr_t radek_compat_CGContextRelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextRetain)
extern "C" uintptr_t radek_compat_CGContextRetain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextClearRect)
extern "C" uintptr_t radek_compat_CGContextClearRect(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextFillRect)
extern "C" uintptr_t radek_compat_CGContextFillRect(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextDrawImage)
extern "C" uintptr_t radek_compat_CGContextDrawImage(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextTranslateCTM)
extern "C" uintptr_t radek_compat_CGContextTranslateCTM(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextScaleCTM)
extern "C" uintptr_t radek_compat_CGContextScaleCTM(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextRotateCTM)
extern "C" uintptr_t radek_compat_CGContextRotateCTM(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextSaveGState)
extern "C" uintptr_t radek_compat_CGContextSaveGState(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextRestoreGState)
extern "C" uintptr_t radek_compat_CGContextRestoreGState(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextSetRGBFillColor)
extern "C" uintptr_t radek_compat_CGContextSetRGBFillColor(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGContextSetAlpha)
extern "C" uintptr_t radek_compat_CGContextSetAlpha(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGImageGetWidth)
extern "C" uintptr_t radek_compat_CGImageGetWidth(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 480u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGImageGetHeight)
extern "C" uintptr_t radek_compat_CGImageGetHeight(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 320u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGImageGetBitsPerComponent)
extern "C" uintptr_t radek_compat_CGImageGetBitsPerComponent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 8u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGImageGetBitsPerPixel)
extern "C" uintptr_t radek_compat_CGImageGetBitsPerPixel(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 32u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGImageGetBytesPerRow)
extern "C" uintptr_t radek_compat_CGImageGetBytesPerRow(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 1920u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGImageGetAlphaInfo)
extern "C" uintptr_t radek_compat_CGImageGetAlphaInfo(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGImageGetDataProvider)
extern "C" uintptr_t radek_compat_CGImageGetDataProvider(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGImageGetColorSpace)
extern "C" uintptr_t radek_compat_CGImageGetColorSpace(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGImageRelease)
extern "C" uintptr_t radek_compat_CGImageRelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGImageRetain)
extern "C" uintptr_t radek_compat_CGImageRetain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGDataProviderCopyData)
extern "C" uintptr_t radek_compat_CGDataProviderCopyData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGDataProviderCopyData");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGDataProviderCreateWithData)
extern "C" uintptr_t radek_compat_CGDataProviderCreateWithData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_CGDataProviderCreateWithData");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGDataProviderRelease)
extern "C" uintptr_t radek_compat_CGDataProviderRelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGDataProviderRetain)
extern "C" uintptr_t radek_compat_CGDataProviderRetain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGAffineTransformMake)
extern "C" uintptr_t radek_compat_CGAffineTransformMake(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGAffineTransformMakeTranslation)
extern "C" uintptr_t radek_compat_CGAffineTransformMakeTranslation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGAffineTransformMakeScale)
extern "C" uintptr_t radek_compat_CGAffineTransformMakeScale(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGAffineTransformMakeRotation)
extern "C" uintptr_t radek_compat_CGAffineTransformMakeRotation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGAffineTransformTranslate)
extern "C" uintptr_t radek_compat_CGAffineTransformTranslate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGAffineTransformScale)
extern "C" uintptr_t radek_compat_CGAffineTransformScale(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGAffineTransformRotate)
extern "C" uintptr_t radek_compat_CGAffineTransformRotate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGAffineTransformConcat)
extern "C" uintptr_t radek_compat_CGAffineTransformConcat(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_msgSendSuper)
extern "C" uintptr_t radek_compat_objc_msgSendSuper(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_msgSendSuper_stret)
extern "C" uintptr_t radek_compat_objc_msgSendSuper_stret(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_msgSendSuper2_stret)
extern "C" uintptr_t radek_compat_objc_msgSendSuper2_stret(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_msgSend_fpret)
extern "C" uintptr_t radek_compat_objc_msgSend_fpret(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_getClass)
extern "C" uintptr_t radek_compat_objc_getClass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_objc_getClass");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_lookUpClass)
extern "C" uintptr_t radek_compat_objc_lookUpClass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_objc_lookUpClass");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_getMetaClass)
extern "C" uintptr_t radek_compat_objc_getMetaClass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_objc_getMetaClass");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_getProtocol)
extern "C" uintptr_t radek_compat_objc_getProtocol(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_allocateClassPair)
extern "C" uintptr_t radek_compat_objc_allocateClassPair(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_registerClassPair)
extern "C" uintptr_t radek_compat_objc_registerClassPair(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_retain)
extern "C" uintptr_t radek_compat_objc_retain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_release)
extern "C" uintptr_t radek_compat_objc_release(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_autorelease)
extern "C" uintptr_t radek_compat_objc_autorelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_autoreleasePoolPush)
extern "C" uintptr_t radek_compat_objc_autoreleasePoolPush(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_objc_autoreleasePoolPush");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_autoreleasePoolPop)
extern "C" uintptr_t radek_compat_objc_autoreleasePoolPop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_retainAutorelease)
extern "C" uintptr_t radek_compat_objc_retainAutorelease(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_retainAutoreleaseReturnValue)
extern "C" uintptr_t radek_compat_objc_retainAutoreleaseReturnValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_retainAutoreleasedReturnValue)
extern "C" uintptr_t radek_compat_objc_retainAutoreleasedReturnValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return a0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_storeStrong)
extern "C" uintptr_t radek_compat_objc_storeStrong(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_storeWeak)
extern "C" uintptr_t radek_compat_objc_storeWeak(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_loadWeakRetained)
extern "C" uintptr_t radek_compat_objc_loadWeakRetained(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_destroyWeak)
extern "C" uintptr_t radek_compat_objc_destroyWeak(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_getProperty)
extern "C" uintptr_t radek_compat_objc_getProperty(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_copyStruct)
extern "C" uintptr_t radek_compat_objc_copyStruct(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_sync_enter)
extern "C" uintptr_t radek_compat_objc_sync_enter(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_sync_exit)
extern "C" uintptr_t radek_compat_objc_sync_exit(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_exception_throw)
extern "C" uintptr_t radek_compat_objc_exception_throw(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_begin_catch)
extern "C" uintptr_t radek_compat_objc_begin_catch(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_end_catch)
extern "C" uintptr_t radek_compat_objc_end_catch(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sel_registerName)
extern "C" uintptr_t radek_compat_sel_registerName(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_sel_registerName");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sel_getUid)
extern "C" uintptr_t radek_compat_sel_getUid(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_sel_getUid");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_sel_getName)
extern "C" uintptr_t radek_compat_sel_getName(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_class_getName)
extern "C" uintptr_t radek_compat_class_getName(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_class_getSuperclass)
extern "C" uintptr_t radek_compat_class_getSuperclass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_class_getInstanceMethod)
extern "C" uintptr_t radek_compat_class_getInstanceMethod(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_class_getClassMethod)
extern "C" uintptr_t radek_compat_class_getClassMethod(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_class_addMethod)
extern "C" uintptr_t radek_compat_class_addMethod(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_class_replaceMethod)
extern "C" uintptr_t radek_compat_class_replaceMethod(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_class_createInstance)
extern "C" uintptr_t radek_compat_class_createInstance(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_object_getClass)
extern "C" uintptr_t radek_compat_object_getClass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_object_getClassName)
extern "C" uintptr_t radek_compat_object_getClassName(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___MPMoviePlayerController)
extern "C" uintptr_t radek_compat_OBJC_CLASS___MPMoviePlayerController(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_MPMoviePlayerController");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSDate)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSDate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSDate");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSLocale)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSLocale(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSLocale");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSNotificationCenter)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSNotificationCenter(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSNotificationCenter");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSUserDefaults)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSUserDefaults(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSUserDefaults");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIColor)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIColor(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIColor");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIDevice)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIDevice(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIDevice");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIImage)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIImage(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIImage");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIViewController)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIViewController(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIViewController");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___AVAudioPlayer)
extern "C" uintptr_t radek_compat_OBJC_CLASS___AVAudioPlayer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_AVAudioPlayer");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___AVAudioSession)
extern "C" uintptr_t radek_compat_OBJC_CLASS___AVAudioSession(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_AVAudioSession");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSArray)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSArray(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSArray");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSMutableArray)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSMutableArray(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSMutableArray");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSMutableDictionary)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSMutableDictionary(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSMutableDictionary");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSMutableString)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSMutableString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSMutableString");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSData)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSData");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSMutableData)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSMutableData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSMutableData");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSSet)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSSet(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSSet");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSMutableSet)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSMutableSet(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSMutableSet");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSFileManager)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSFileManager(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSFileManager");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSTimer)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSTimer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSTimer");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSRunLoop)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSRunLoop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSRunLoop");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSProcessInfo)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSProcessInfo(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSProcessInfo");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSValue)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSValue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSValue");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___NSError)
extern "C" uintptr_t radek_compat_OBJC_CLASS___NSError(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_NSError");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIImageView)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIImageView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIImageView");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UILabel)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UILabel(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UILabel");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIButton)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIButton(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIButton");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIScrollView)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIScrollView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIScrollView");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIAlertView)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIAlertView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIAlertView");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIActivityIndicatorView)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIActivityIndicatorView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIActivityIndicatorView");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIWebView)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIWebView(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIWebView");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIFont)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIFont(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIFont");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UITouch)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UITouch(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UITouch");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___UIEvent)
extern "C" uintptr_t radek_compat_OBJC_CLASS___UIEvent(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_UIEvent");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___CALayer)
extern "C" uintptr_t radek_compat_OBJC_CLASS___CALayer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_CALayer");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___CATransaction)
extern "C" uintptr_t radek_compat_OBJC_CLASS___CATransaction(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_CATransaction");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___CABasicAnimation)
extern "C" uintptr_t radek_compat_OBJC_CLASS___CABasicAnimation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_CABasicAnimation");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___SKPaymentQueue)
extern "C" uintptr_t radek_compat_OBJC_CLASS___SKPaymentQueue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_SKPaymentQueue");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___SKProductsRequest)
extern "C" uintptr_t radek_compat_OBJC_CLASS___SKProductsRequest(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_SKProductsRequest");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___GKLocalPlayer)
extern "C" uintptr_t radek_compat_OBJC_CLASS___GKLocalPlayer(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_GKLocalPlayer");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___CMMotionManager)
extern "C" uintptr_t radek_compat_OBJC_CLASS___CMMotionManager(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_CMMotionManager");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_CLASS___GCController)
extern "C" uintptr_t radek_compat_OBJC_CLASS___GCController(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_CLASS_$_GCController");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_METACLASS___UIViewController)
extern "C" uintptr_t radek_compat_OBJC_METACLASS___UIViewController(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_METACLASS_$_UIViewController");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OBJC_METACLASS___UIApplication)
extern "C" uintptr_t radek_compat_OBJC_METACLASS___UIApplication(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_OBJC_METACLASS_$_UIApplication");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_UIGraphicsPushContext)
extern "C" uintptr_t radek_compat_UIGraphicsPushContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_UIGraphicsPopContext)
extern "C" uintptr_t radek_compat_UIGraphicsPopContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_UIGraphicsGetCurrentContext)
extern "C" uintptr_t radek_compat_UIGraphicsGetCurrentContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_UIGraphicsGetCurrentContext");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_UIGraphicsBeginImageContext)
extern "C" uintptr_t radek_compat_UIGraphicsBeginImageContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_UIGraphicsBeginImageContextWithOptions)
extern "C" uintptr_t radek_compat_UIGraphicsBeginImageContextWithOptions(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_UIGraphicsGetImageFromCurrentImageContext)
extern "C" uintptr_t radek_compat_UIGraphicsGetImageFromCurrentImageContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_UIGraphicsGetImageFromCurrentImageContext");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_UIGraphicsEndImageContext)
extern "C" uintptr_t radek_compat_UIGraphicsEndImageContext(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_UIImagePNGRepresentation)
extern "C" uintptr_t radek_compat_UIImagePNGRepresentation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_UIImagePNGRepresentation");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_UIImageJPEGRepresentation)
extern "C" uintptr_t radek_compat_UIImageJPEGRepresentation(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_UIImageJPEGRepresentation");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_UIImageWriteToSavedPhotosAlbum)
extern "C" uintptr_t radek_compat_UIImageWriteToSavedPhotosAlbum(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_NSTemporaryDirectory)
extern "C" uintptr_t radek_compat_NSTemporaryDirectory(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_NSTemporaryDirectory");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_NSHomeDirectory)
extern "C" uintptr_t radek_compat_NSHomeDirectory(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_NSHomeDirectory");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_NSLog)
extern "C" uintptr_t radek_compat_NSLog(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_NSStringFromClass)
extern "C" uintptr_t radek_compat_NSStringFromClass(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_NSClassFromString)
extern "C" uintptr_t radek_compat_NSClassFromString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_NSStringFromSelector)
extern "C" uintptr_t radek_compat_NSStringFromSelector(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_NSSelectorFromString)
extern "C" uintptr_t radek_compat_NSSelectorFromString(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_NSPageSize)
extern "C" uintptr_t radek_compat_NSPageSize(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_async)
extern "C" uintptr_t radek_compat_dispatch_async(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_sync)
extern "C" uintptr_t radek_compat_dispatch_sync(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_after)
extern "C" uintptr_t radek_compat_dispatch_after(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_once)
extern "C" uintptr_t radek_compat_dispatch_once(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_async_f)
extern "C" uintptr_t radek_compat_dispatch_async_f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_sync_f)
extern "C" uintptr_t radek_compat_dispatch_sync_f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_once_f)
extern "C" uintptr_t radek_compat_dispatch_once_f(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_get_main_queue)
extern "C" uintptr_t radek_compat_dispatch_get_main_queue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_dispatch_get_main_queue");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_get_global_queue)
extern "C" uintptr_t radek_compat_dispatch_get_global_queue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_dispatch_get_global_queue");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_queue_create)
extern "C" uintptr_t radek_compat_dispatch_queue_create(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_dispatch_queue_create");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_release)
extern "C" uintptr_t radek_compat_dispatch_release(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_retain)
extern "C" uintptr_t radek_compat_dispatch_retain(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_time)
extern "C" uintptr_t radek_compat_dispatch_time(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_semaphore_create)
extern "C" uintptr_t radek_compat_dispatch_semaphore_create(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_dispatch_semaphore_create");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_semaphore_wait)
extern "C" uintptr_t radek_compat_dispatch_semaphore_wait(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_semaphore_signal)
extern "C" uintptr_t radek_compat_dispatch_semaphore_signal(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_group_create)
extern "C" uintptr_t radek_compat_dispatch_group_create(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_dispatch_group_create");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_group_async)
extern "C" uintptr_t radek_compat_dispatch_group_async(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_group_enter)
extern "C" uintptr_t radek_compat_dispatch_group_enter(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_group_leave)
extern "C" uintptr_t radek_compat_dispatch_group_leave(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_group_wait)
extern "C" uintptr_t radek_compat_dispatch_group_wait(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_group_notify)
extern "C" uintptr_t radek_compat_dispatch_group_notify(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__dispatch_main_q)
extern "C" uintptr_t radek_compat__dispatch_main_q(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("__dispatch_main_q");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SCNetworkReachabilityCreateWithAddress)
extern "C" uintptr_t radek_compat_SCNetworkReachabilityCreateWithAddress(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_SCNetworkReachabilityCreateWithAddress");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SCNetworkReachabilityCreateWithName)
extern "C" uintptr_t radek_compat_SCNetworkReachabilityCreateWithName(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return reinterpret_cast<uintptr_t>("_SCNetworkReachabilityCreateWithName");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SCNetworkReachabilityGetFlags)
extern "C" uintptr_t radek_compat_SCNetworkReachabilityGetFlags(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SCNetworkReachabilitySetCallback)
extern "C" uintptr_t radek_compat_SCNetworkReachabilitySetCallback(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SCNetworkReachabilityScheduleWithRunLoop)
extern "C" uintptr_t radek_compat_SCNetworkReachabilityScheduleWithRunLoop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SCNetworkReachabilityUnscheduleFromRunLoop)
extern "C" uintptr_t radek_compat_SCNetworkReachabilityUnscheduleFromRunLoop(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SCNetworkReachabilitySetDispatchQueue)
extern "C" uintptr_t radek_compat_SCNetworkReachabilitySetDispatchQueue(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SecRandomCopyBytes)
extern "C" uintptr_t radek_compat_SecRandomCopyBytes(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SecItemCopyMatching)
extern "C" uintptr_t radek_compat_SecItemCopyMatching(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SecItemAdd)
extern "C" uintptr_t radek_compat_SecItemAdd(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SecItemUpdate)
extern "C" uintptr_t radek_compat_SecItemUpdate(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_SecItemDelete)
extern "C" uintptr_t radek_compat_SecItemDelete(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CC_MD5)
extern "C" uintptr_t radek_compat_CC_MD5(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CC_SHA1)
extern "C" uintptr_t radek_compat_CC_SHA1(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CC_SHA256)
extern "C" uintptr_t radek_compat_CC_SHA256(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Unwind_DeleteException)
extern "C" uintptr_t radek_compat__Unwind_DeleteException(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Unwind_GetIP)
extern "C" uintptr_t radek_compat__Unwind_GetIP(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Unwind_SetIP)
extern "C" uintptr_t radek_compat__Unwind_SetIP(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Unwind_GetGR)
extern "C" uintptr_t radek_compat__Unwind_GetGR(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Unwind_SetGR)
extern "C" uintptr_t radek_compat__Unwind_SetGR(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Unwind_GetLanguageSpecificData)
extern "C" uintptr_t radek_compat__Unwind_GetLanguageSpecificData(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Unwind_GetRegionStart)
extern "C" uintptr_t radek_compat__Unwind_GetRegionStart(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___gxx_personality_v0)
extern "C" uintptr_t radek_compat___gxx_personality_v0(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___gcc_personality_v0)
extern "C" uintptr_t radek_compat___gcc_personality_v0(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___udivdi3)
extern "C" uint64_t radek_compat___udivdi3(uint64_t a, uint64_t b) {
    return b == 0 ? 0u : (a / b);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___umoddi3)
extern "C" uint64_t radek_compat___umoddi3(uint64_t a, uint64_t b) {
    return b == 0 ? 0u : (a % b);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___muldi3)
extern "C" uintptr_t radek_compat___muldi3(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___fixsfdi)
extern "C" uintptr_t radek_compat___fixsfdi(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___fixunsdfdi)
extern "C" uintptr_t radek_compat___fixunsdfdi(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___fixunssfdi)
extern "C" uintptr_t radek_compat___fixunssfdi(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___floatundidf)
extern "C" uintptr_t radek_compat___floatundidf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___floatundisf)
extern "C" uintptr_t radek_compat___floatundisf(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___ashldi3)
extern "C" uintptr_t radek_compat___ashldi3(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___ashrdi3)
extern "C" uintptr_t radek_compat___ashrdi3(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___lshrdi3)
extern "C" uintptr_t radek_compat___lshrdi3(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cmpdi2)
extern "C" uintptr_t radek_compat___cmpdi2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___ucmpdi2)
extern "C" uintptr_t radek_compat___ucmpdi2(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___clear_cache)
extern "C" uintptr_t radek_compat___clear_cache(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Znaj)
extern "C" uintptr_t radek_compat__Znaj(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat__Znwj)
extern "C" uintptr_t radek_compat__Znwj(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_free_exception)
extern "C" uintptr_t radek_compat___cxa_free_exception(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_rethrow)
extern "C" uintptr_t radek_compat___cxa_rethrow(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_guard_acquire)
extern "C" uintptr_t radek_compat___cxa_guard_acquire(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_guard_release)
extern "C" uintptr_t radek_compat___cxa_guard_release(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_guard_abort)
extern "C" uintptr_t radek_compat___cxa_guard_abort(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_demangle)
extern "C" uintptr_t radek_compat___cxa_demangle(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___dynamic_cast)
extern "C" uintptr_t radek_compat___dynamic_cast(uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3) {
    (void)a0; (void)a1; (void)a2; (void)a3;
    return 0u;
}
#endif


/* ========================================================================== */
/* Batch 2 (Bioshock/Angry Birds device-report inventory): CoreFoundation     */
/* queries/characters/percent-escaping/DNS, CoreGraphics rect math,           */
/* CommonCrypto HMAC (MD5/SHA1/SHA256), OSAtomic, Mach surface, C++ ABI       */
/* helpers, Blocks runtime, Objective-C property/association helpers.         */
/* ========================================================================== */

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_NEEDS_CF_RUNTIME)

// CFRange normalisation shared by the batch-2 array accessors: clamp the
// requested range to the array bounds exactly like CFArrayIsValidRange.
static inline bool radekCfValidRange(radek_CFIndex rangeLocation, radek_CFIndex rangeLength,
                                     size_t count, size_t &begin, size_t &end) {
    if (rangeLength < 0 || rangeLocation < 0) return false;
    if (static_cast<size_t>(rangeLocation) > count) return false;
    begin = static_cast<size_t>(rangeLocation);
    size_t length = static_cast<size_t>(rangeLength);
    if (begin + length > count) length = count - begin;
    end = begin + length;
    return true;
}

// Content comparison for array/dictionary values: CF strings compare by text
// (matching kCFTypeArrayCallBacks equal-callback semantics); everything else
// compares by pointer identity.
static inline bool radekCfValueEquals(const void *left, const void *right) {
    if (left == right) return true;
    const radek_CFRuntime *leftObject = radekCfConst(left);
    const radek_CFRuntime *rightObject = radekCfConst(right);
    if (leftObject != nullptr && rightObject != nullptr &&
        leftObject->kind == RadekCFKind::String && rightObject->kind == RadekCFKind::String) {
        return leftObject->text == rightObject->text;
    }
    return false;
}

#endif  // CF runtime helpers

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFArrayContainsValue)
extern "C" radek_Boolean radek_compat_CFArrayContainsValue(radek_CFArrayRef array, radek_CFRange range,
                                                           const void *value) {
    const radek_CFRuntime *object = radekCfConst(array);
    if (!radekCfIsKind(object, RadekCFKind::Array)) return 0;
    size_t begin = 0;
    size_t end = 0;
    if (!radekCfValidRange(range.location, range.length, object->elements.size(), begin, end)) return 0;
    for (size_t index = begin; index < end; ++index) {
        if (radekCfValueEquals(object->elements[index], value)) return 1;
    }
    return 0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFArrayGetFirstIndexOfValue)
extern "C" radek_CFIndex radek_compat_CFArrayGetFirstIndexOfValue(radek_CFArrayRef array, radek_CFRange range,
                                                                  const void *value) {
    const radek_CFRuntime *object = radekCfConst(array);
    if (!radekCfIsKind(object, RadekCFKind::Array)) return -1;
    size_t begin = 0;
    size_t end = 0;
    if (!radekCfValidRange(range.location, range.length, object->elements.size(), begin, end)) return -1;
    for (size_t index = begin; index < end; ++index) {
        if (radekCfValueEquals(object->elements[index], value)) return static_cast<radek_CFIndex>(index);
    }
    return -1;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFDictionaryAddValue)
extern "C" radek_Boolean radek_compat_CFDictionaryAddValue(radek_CFMutableDictionaryRef dictionary,
                                                           const void *key, const void *value) {
    radek_CFRuntime *object = radekCfMutable(dictionary);
    if (object == nullptr || object->kind != RadekCFKind::Dictionary) return 0;
    const radek_CFRuntime *keyObject = radekCfConst(key);
    const radek_CFRuntime *valueObject = radekCfConst(value);
    if (keyObject == nullptr || valueObject == nullptr) return 0;
    for (const auto &pair : object->pairs) {
        if (radekCfKeyEquals(pair.first, keyObject)) return 0;  // CFDictionaryAddValue never overwrites.
    }
    radekCfRetainInternal(keyObject);
    radekCfRetainInternal(valueObject);
    object->pairs.emplace_back(keyObject, valueObject);
    return 1;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFMakeCollectable)
extern "C" radek_CFTypeRef radek_compat_CFMakeCollectable(radek_CFTypeRef object) {
    // Garbage collection does not exist in this runtime; the documented
    // behaviour of CFMakeCollectable on non-GC builds is to hand the object
    // back unchanged.
    return object;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringAppendCharacters)
extern "C" void radek_compat_CFStringAppendCharacters(radek_CFMutableStringRef string,
                                                      const radek_UniChar *characters, radek_CFIndex count) {
    radek_CFRuntime *object = radekCfMutable(string);
    if (object == nullptr || object->kind != RadekCFKind::String) return;
    if (characters == nullptr || count <= 0) return;
    radekCfUtf16ToUtf8(reinterpret_cast<const uint16_t *>(characters), static_cast<size_t>(count),
                       object->text);
    object->charsCacheValid = false;  // Any cached UTF-16 view is now stale.
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringCreateWithCharacters)
extern "C" radek_CFStringRef radek_compat_CFStringCreateWithCharacters(radek_CFAllocatorRef allocator,
                                                                       const radek_UniChar *characters,
                                                                       radek_CFIndex count) {
    (void)allocator;
    if (characters == nullptr || count < 0) return nullptr;
    auto *object = new radek_CFRuntime();
    object->kind = RadekCFKind::String;
    radekCfUtf16ToUtf8(reinterpret_cast<const uint16_t *>(characters), static_cast<size_t>(count),
                       object->text);
    return object;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFStringGetCharactersPtr)
extern "C" const radek_UniChar *radek_compat_CFStringGetCharactersPtr(radek_CFStringRef string) {
    const radek_CFRuntime *object = radekCfConst(string);
    if (!radekCfIsKind(object, RadekCFKind::String)) return nullptr;
    if (!object->charsCacheValid) {
        object->charsCache.clear();
        radekCfUtf8ToUtf16(object->text, object->charsCache);
        object->charsCacheValid = true;
    }
    return reinterpret_cast<const radek_UniChar *>(object->charsCache.data());
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFURLCreateStringByAddingPercentEscapes)
extern "C" radek_CFStringRef radek_compat_CFURLCreateStringByAddingPercentEscapes(
    radek_CFAllocatorRef allocator, radek_CFStringRef original,
    radek_CFStringRef charactersToLeaveUnescaped, radek_CFStringRef legalURLCharactersToBeEscaped,
    radek_CFStringEncoding encoding) {
    (void)allocator;
    (void)encoding;  // The runtime stores UTF-8 internally; escapes are computed per UTF-8 byte.
    const radek_CFRuntime *source = radekCfConst(original);
    if (!radekCfIsKind(source, RadekCFKind::String)) return nullptr;
    const radek_CFRuntime *leave = radekCfConst(charactersToLeaveUnescaped);
    const radek_CFRuntime *force = radekCfConst(legalURLCharactersToBeEscaped);
    const std::string &leaveText = (leave != nullptr && leave->kind == RadekCFKind::String) ? leave->text : std::string();
    const std::string &forceText = (force != nullptr && force->kind == RadekCFKind::String) ? force->text : std::string();
    static const char *hexDigits = "0123456789ABCDEF";
    std::string escaped;
    escaped.reserve(source->text.size());
    for (unsigned char byte : source->text) {
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' ||
                                byte == '_' || byte == '~';
        const bool legalUrl = unreserved || byte == '/' || byte == ':' || byte == ',' || byte == ';' ||
                              byte == '?' || byte == '@' || byte == '&' || byte == '=' || byte == '+' ||
                              byte == '$' || byte == '#' || byte == '[' || byte == ']';
        bool mustEscape = !legalUrl;
        if (force != nullptr && forceText.find(static_cast<char>(byte)) != std::string::npos) mustEscape = true;
        if (leave != nullptr && leaveText.find(static_cast<char>(byte)) != std::string::npos) mustEscape = false;
        if (mustEscape) {
            escaped.push_back('%');
            escaped.push_back(hexDigits[byte >> 4]);
            escaped.push_back(hexDigits[byte & 0x0F]);
        } else {
            escaped.push_back(static_cast<char>(byte));
        }
    }
    auto *object = new radek_CFRuntime();
    object->kind = RadekCFKind::String;
    object->text = std::move(escaped);
    return object;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFHostCreateWithName)
extern "C" radek_CFTypeRef radek_compat_CFHostCreateWithName(radek_CFAllocatorRef allocator,
                                                             radek_CFStringRef hostname) {
    (void)allocator;
    const radek_CFRuntime *name = radekCfConst(hostname);
    if (!radekCfIsKind(name, RadekCFKind::String)) return nullptr;
    auto *object = new radek_CFRuntime();
    object->kind = RadekCFKind::Host;
    object->hostName = name->text;
    return object;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFHostStartInfoResolution)
extern "C" radek_Boolean radek_compat_CFHostStartInfoResolution(radek_CFTypeRef host, int32_t info,
                                                                void *error) {
    radek_CFRuntime *object = radekCfMutable(const_cast<radek_CFRuntime *>(host));
    if (object == nullptr || object->kind != RadekCFKind::Host) return 0;
    if (info != 0) return 0;  // kCFHostAddresses is the only resolution this runtime performs.
    // CFStreamError is {int32 domain, int32 error}; kCFStreamErrorDomainPOSIX == 2.
    auto *streamError = static_cast<int32_t *>(error);
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *results = nullptr;
    const int status = getaddrinfo(object->hostName.c_str(), nullptr, &hints, &results);
    if (status != 0 || results == nullptr) {
        object->hostAddresses.clear();
        object->hostResolved = false;
        if (streamError != nullptr) {
            streamError[0] = 2;             // kCFStreamErrorDomainPOSIX
            streamError[1] = (status != 0) ? errno : EAI_FAIL;
        }
        return 0;
    }
    object->hostAddresses.clear();
    for (struct addrinfo *entry = results; entry != nullptr; entry = entry->ai_next) {
        std::vector<uint8_t> storage(sizeof(struct sockaddr_storage), uint8_t{0});
        std::memcpy(storage.data(), entry->ai_addr, static_cast<size_t>(entry->ai_addrlen));
        object->hostAddresses.push_back(std::move(storage));
    }
    freeaddrinfo(results);
    object->hostResolved = true;
    if (streamError != nullptr) {
        streamError[0] = 0;
        streamError[1] = 0;
    }
    return 1;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CFHostGetAddressing)
extern "C" radek_CFArrayRef radek_compat_CFHostGetAddressing(radek_CFTypeRef host,
                                                             radek_Boolean *hasBeenResolved) {
    const radek_CFRuntime *object = radekCfConst(host);
    if (object == nullptr || object->kind != RadekCFKind::Host) return nullptr;
    if (hasBeenResolved != nullptr) *hasBeenResolved = object->hostResolved ? 1 : 0;
    if (!object->hostResolved) return nullptr;
    auto *array = new radek_CFRuntime();
    array->kind = RadekCFKind::Array;
    array->elements.reserve(object->hostAddresses.size());
    for (const std::vector<uint8_t> &address : object->hostAddresses) {
        auto *data = new radek_CFRuntime();
        data->kind = RadekCFKind::Data;
        data->bytes = address;
        radekCfRetainInternal(data);  // The array retains on append; balance the create reference.
        array->elements.push_back(data);
        radekCfReleaseInternal(data);
    }
    return array;
}
#endif

/* --- CoreGraphics rect math (CGRect is an HFA of four floats on arm32) ---- */

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGRectGetHeight)
extern "C" float radek_compat_CGRectGetHeight(radek_CGRect rect) { return rect.size.height; }
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGRectGetMaxX)
extern "C" float radek_compat_CGRectGetMaxX(radek_CGRect rect) {
    return rect.origin.x + rect.size.width;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGRectGetMidX)
extern "C" float radek_compat_CGRectGetMidX(radek_CGRect rect) {
    return rect.origin.x + rect.size.width * 0.5f;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGRectGetMidY)
extern "C" float radek_compat_CGRectGetMidY(radek_CGRect rect) {
    return rect.origin.y + rect.size.height * 0.5f;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGRectGetMinY)
extern "C" float radek_compat_CGRectGetMinY(radek_CGRect rect) { return rect.origin.y; }
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGRectGetWidth)
extern "C" float radek_compat_CGRectGetWidth(radek_CGRect rect) { return rect.size.width; }
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGRectIsNull)
extern "C" radek_Boolean radek_compat_CGRectIsNull(radek_CGRect rect) {
    const bool infinite = std::isinf(rect.origin.x) && std::isinf(rect.origin.y) &&
                          rect.size.width == 0.0f && rect.size.height == 0.0f;
    return infinite ? 1 : 0;  // CGRectNull is {{+inf, +inf}, {0, 0}}.
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGRectIsEmpty)
extern "C" radek_Boolean radek_compat_CGRectIsEmpty(radek_CGRect rect) {
    if (radek_compat_CGRectIsNull(rect)) return 1;
    return (rect.size.width <= 0.0f || rect.size.height <= 0.0f) ? 1 : 0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGRectIntegral)
extern "C" radek_CGRect radek_compat_CGRectIntegral(radek_CGRect rect) {
    if (radek_compat_CGRectIsNull(rect)) return rect;
    const float left = floorf(rect.origin.x);
    const float top = floorf(rect.origin.y);
    radek_CGRect integral;
    integral.origin.x = left;
    integral.origin.y = top;
    integral.size.width = ceilf(rect.origin.x + rect.size.width) - left;
    integral.size.height = ceilf(rect.origin.y + rect.size.height) - top;
    return integral;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGRectOffset)
extern "C" radek_CGRect radek_compat_CGRectOffset(radek_CGRect rect, float dx, float dy) {
    if (radek_compat_CGRectIsNull(rect)) return rect;
    rect.origin.x += dx;
    rect.origin.y += dy;
    return rect;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CGRectIntersectsRect)
extern "C" radek_Boolean radek_compat_CGRectIntersectsRect(radek_CGRect left, radek_CGRect right) {
    if (radek_compat_CGRectIsEmpty(left) || radek_compat_CGRectIsEmpty(right)) return 0;
    const float overlapX = std::min(left.origin.x + left.size.width, right.origin.x + right.size.width) -
                           std::max(left.origin.x, right.origin.x);
    const float overlapY = std::min(left.origin.y + left.size.height, right.origin.y + right.size.height) -
                           std::max(left.origin.y, right.origin.y);
    return (overlapX > 0.0f && overlapY > 0.0f) ? 1 : 0;
}
#endif

/* --- CommonCrypto HMAC: real incremental MD5/SHA1/SHA256 -------------------- */

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CCHmac) || \
    defined(RADEK_API_radek_compat_CCHmacInit) || defined(RADEK_API_radek_compat_CCHmacUpdate) || \
    defined(RADEK_API_radek_compat_CCHmacFinal)

namespace {

struct RadekDigest {
    uint32_t state[8];
    uint8_t buffer[64];
    uint64_t totalBytes;
    uint32_t buffered;
};

struct RadekDigestVtable {
    void (*initState)(uint32_t *state);
    void (*compress)(uint32_t *state, const uint8_t block[64]);
    uint32_t digestLength;
};

inline uint32_t radekRotl32(uint32_t value, unsigned bits) {
    return (value << bits) | (value >> (32u - bits));
}
inline uint32_t radekRotr32(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}
inline uint32_t radekLoadLe32(const uint8_t *bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}
inline uint32_t radekLoadBe32(const uint8_t *bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | static_cast<uint32_t>(bytes[3]);
}

/* -- MD5 (RFC 1321) -- */
const uint32_t kRadekMd5K[64] = {
    0xD76AA478u, 0xE8C7B756u, 0x242070DBu, 0xC1BDCEEEu, 0xF57C0FAFu, 0x4787C62Au, 0xA8304613u, 0xFD469501u,
    0x698098D8u, 0x8B44F7AFu, 0xFFFF5BB1u, 0x895CD7BEu, 0x6B901122u, 0xFD987193u, 0xA679438Eu, 0x49B40821u,
    0xF61E2562u, 0xC040B340u, 0x265E5A51u, 0xE9B6C7AAu, 0xD62F105Du, 0x02441453u, 0xD8A1E681u, 0xE7D3FBC8u,
    0x21E1CDE6u, 0xC33707D6u, 0xF4D50D87u, 0x455A14EDu, 0xA9E3E905u, 0xFCEFA3F8u, 0x676F02D9u, 0x8D2A4C8Au,
    0xFFFA3942u, 0x8771F681u, 0x6D9D6122u, 0xFDE5380Cu, 0xA4BEEA44u, 0x4BDECFA9u, 0xF6BB4B60u, 0xBEBFBC70u,
    0x289B7EC6u, 0xEAA127FAu, 0xD4EF3085u, 0x04881D05u, 0xD9D4D039u, 0xE6DB99E5u, 0x1FA27CF8u, 0xC4AC5665u,
    0xF4292244u, 0x432AFF97u, 0xAB9423A7u, 0xFC93A039u, 0x655B59C3u, 0x8F0CCC92u, 0xFFEFF47Du, 0x85845DD1u,
    0x6FA87E4Fu, 0xFE2CE6E0u, 0xA3014314u, 0x4E0811A1u, 0xF7537E82u, 0xBD3AF235u, 0x2AD7D2BBu, 0xEB86D391u,
};
const unsigned kRadekMd5Shift[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

void radekMd5Init(uint32_t *state) {
    state[0] = 0x67452301u;
    state[1] = 0xEFCDAB89u;
    state[2] = 0x98BADCFEu;
    state[3] = 0x10325476u;
}

void radekMd5Compress(uint32_t *state, const uint8_t block[64]) {
    uint32_t words[16];
    for (int i = 0; i < 16; ++i) words[i] = radekLoadLe32(block + 4 * i);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f = 0;
        int g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        const uint32_t temp = d;
        d = c;
        c = b;
        b = b + radekRotl32(a + f + kRadekMd5K[i] + words[g], kRadekMd5Shift[i]);
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

/* -- SHA-1 (FIPS 180-1) -- */
void radekSha1Init(uint32_t *state) {
    state[0] = 0x67452301u;
    state[1] = 0xEFCDAB89u;
    state[2] = 0x98BADCFEu;
    state[3] = 0x10325476u;
    state[4] = 0xC3D2E1F0u;
}

void radekSha1Compress(uint32_t *state, const uint8_t block[64]) {
    uint32_t words[80];
    for (int i = 0; i < 16; ++i) words[i] = radekLoadBe32(block + 4 * i);
    for (int i = 16; i < 80; ++i) {
        words[i] = radekRotl32(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f = 0;
        uint32_t k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t temp = radekRotl32(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = radekRotl32(b, 30);
        b = a;
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

/* -- SHA-256 (FIPS 180-4) -- */
const uint32_t kRadekSha256K[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

void radekSha256Init(uint32_t *state) {
    state[0] = 0x6A09E667u;
    state[1] = 0xBB67AE85u;
    state[2] = 0x3C6EF372u;
    state[3] = 0xA54FF53Au;
    state[4] = 0x510E527Fu;
    state[5] = 0x9B05688Cu;
    state[6] = 0x1F83D9ABu;
    state[7] = 0x5BE0CD19u;
}

void radekSha256Compress(uint32_t *state, const uint8_t block[64]) {
    uint32_t words[64];
    for (int i = 0; i < 16; ++i) words[i] = radekLoadBe32(block + 4 * i);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = radekRotr32(words[i - 15], 7) ^ radekRotr32(words[i - 15], 18) ^ (words[i - 15] >> 3);
        const uint32_t s1 = radekRotr32(words[i - 2], 17) ^ radekRotr32(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t bigS1 = radekRotr32(e, 6) ^ radekRotr32(e, 11) ^ radekRotr32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t temp1 = h + bigS1 + ch + kRadekSha256K[i] + words[i];
        const uint32_t bigS0 = radekRotr32(a, 2) ^ radekRotr32(a, 13) ^ radekRotr32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t temp2 = bigS0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

const RadekDigestVtable *radekDigestVtable(uint32_t algorithm) {
    switch (algorithm) {
        case RADEK_kCCHmacAlgSHA1: {
            static const RadekDigestVtable vtable = {&radekSha1Init, &radekSha1Compress, 20};
            return &vtable;
        }
        case RADEK_kCCHmacAlgMD5: {
            static const RadekDigestVtable vtable = {&radekMd5Init, &radekMd5Compress, 16};
            return &vtable;
        }
        case RADEK_kCCHmacAlgSHA256: {
            static const RadekDigestVtable vtable = {&radekSha256Init, &radekSha256Compress, 32};
            return &vtable;
        }
        default:
            return nullptr;
    }
}

void radekDigestInit(RadekDigest &digest, const RadekDigestVtable &vtable) {
    std::memset(&digest, 0, sizeof(digest));
    vtable.initState(digest.state);
}

void radekDigestUpdate(RadekDigest &digest, const RadekDigestVtable &vtable, const uint8_t *data,
                       size_t length) {
    digest.totalBytes += length;
    while (length > 0) {
        const size_t space = 64 - digest.buffered;
        const size_t chunk = length < space ? length : space;
        std::memcpy(digest.buffer + digest.buffered, data, chunk);
        digest.buffered += static_cast<uint32_t>(chunk);
        data += chunk;
        length -= chunk;
        if (digest.buffered == 64) {
            vtable.compress(digest.state, digest.buffer);
            digest.buffered = 0;
        }
    }
}

void radekDigestFinal(RadekDigest &digest, const RadekDigestVtable &vtable, uint8_t *out) {
    // MD5 pads with a LITTLE-endian 64-bit length (RFC 1321); SHA-1/SHA-256
    // pad with a big-endian one, and also serialise their state big-endian.
    const bool littleEndian = (vtable.initState == &radekMd5Init);
    const uint64_t totalBits = digest.totalBytes * 8u;
    uint8_t pad = 0x80;
    radekDigestUpdate(digest, vtable, &pad, 1);
    uint8_t zero = 0;
    while (digest.buffered != 56) radekDigestUpdate(digest, vtable, &zero, 1);
    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i) {
        lengthBytes[i] = littleEndian ? static_cast<uint8_t>(totalBits >> (8 * i))
                                      : static_cast<uint8_t>(totalBits >> (56 - 8 * i));
    }
    radekDigestUpdate(digest, vtable, lengthBytes, 8);
    for (uint32_t word = 0; word * 4 < vtable.digestLength; ++word) {
        if (littleEndian) {
            out[4 * word + 0] = static_cast<uint8_t>(digest.state[word]);
            out[4 * word + 1] = static_cast<uint8_t>(digest.state[word] >> 8);
            out[4 * word + 2] = static_cast<uint8_t>(digest.state[word] >> 16);
            out[4 * word + 3] = static_cast<uint8_t>(digest.state[word] >> 24);
        } else {
            out[4 * word + 0] = static_cast<uint8_t>(digest.state[word] >> 24);
            out[4 * word + 1] = static_cast<uint8_t>(digest.state[word] >> 16);
            out[4 * word + 2] = static_cast<uint8_t>(digest.state[word] >> 8);
            out[4 * word + 3] = static_cast<uint8_t>(digest.state[word]);
        }
    }
}

// Darwin's CCHmacContext is uint32_t ctx[96] (384 bytes). The incremental
// state below is 220 bytes, so it always fits inside caller storage.
struct RadekHmacContext {
    uint32_t magic;  // "RHMC"
    uint32_t algorithm;
    uint32_t digestLength;
    uint32_t padding;
    RadekDigest inner;
    RadekDigest outer;
};
static_assert(sizeof(RadekHmacContext) <= sizeof(radek_CCHmacContext),
              "HMAC state must fit Darwin's CCHmacContext");
const uint32_t kRadekHmacMagic = 0x52484D43u;

static inline RadekHmacContext *radekHmacContext(radek_CCHmacContext *context) {
    auto *state = reinterpret_cast<RadekHmacContext *>(context);
    return (state != nullptr && state->magic == kRadekHmacMagic) ? state : nullptr;
}

static inline void radekHmacInit(radek_CCHmacContext *context, uint32_t algorithm, const uint8_t *key,
                                 size_t keyLength) {
    const RadekDigestVtable *vtable = radekDigestVtable(algorithm);
    auto *state = reinterpret_cast<RadekHmacContext *>(context);
    if (context == nullptr || vtable == nullptr) return;
    std::memset(state, 0, sizeof(*state));
    state->magic = kRadekHmacMagic;
    state->algorithm = algorithm;
    state->digestLength = vtable->digestLength;
    uint8_t keyBlock[64];
    std::memset(keyBlock, 0, sizeof(keyBlock));
    if (keyLength > 64) {
        RadekDigest hashed;
        radekDigestInit(hashed, *vtable);
        radekDigestUpdate(hashed, *vtable, key, keyLength);
        uint8_t digest[32];
        radekDigestFinal(hashed, *vtable, digest);
        std::memcpy(keyBlock, digest, vtable->digestLength);
    } else if (key != nullptr && keyLength > 0) {
        std::memcpy(keyBlock, key, keyLength);
    }
    uint8_t padBlock[64];
    for (int i = 0; i < 64; ++i) padBlock[i] = keyBlock[i] ^ 0x36;
    radekDigestInit(state->inner, *vtable);
    radekDigestUpdate(state->inner, *vtable, padBlock, 64);
    for (int i = 0; i < 64; ++i) padBlock[i] = keyBlock[i] ^ 0x5C;
    radekDigestInit(state->outer, *vtable);
    radekDigestUpdate(state->outer, *vtable, padBlock, 64);
}

}  // namespace

#endif  // CCHmac selection

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CCHmacInit)
extern "C" void radek_compat_CCHmacInit(radek_CCHmacContext *context, radek_CCHmacAlgorithm algorithm,
                                        const void *key, uintptr_t keyLength) {
    radekHmacInit(context, algorithm, static_cast<const uint8_t *>(key), keyLength);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CCHmacUpdate)
extern "C" void radek_compat_CCHmacUpdate(radek_CCHmacContext *context, const void *data,
                                          uintptr_t dataLength) {
    RadekHmacContext *state = radekHmacContext(context);
    if (state == nullptr) return;
    const RadekDigestVtable *vtable = radekDigestVtable(state->algorithm);
    if (vtable == nullptr) return;
    radekDigestUpdate(state->inner, *vtable, static_cast<const uint8_t *>(data), dataLength);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CCHmacFinal)
extern "C" void radek_compat_CCHmacFinal(radek_CCHmacContext *context, void *macOut) {
    RadekHmacContext *state = radekHmacContext(context);
    if (state == nullptr || macOut == nullptr) return;
    const RadekDigestVtable *vtable = radekDigestVtable(state->algorithm);
    if (vtable == nullptr) return;
    uint8_t innerDigest[32];
    radekDigestFinal(state->inner, *vtable, innerDigest);
    radekDigestUpdate(state->outer, *vtable, innerDigest, vtable->digestLength);
    radekDigestFinal(state->outer, *vtable, static_cast<uint8_t *>(macOut));
    state->magic = 0;  // Context is consumed, exactly like CCHmacFinal.
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_CCHmac)
extern "C" void radek_compat_CCHmac(radek_CCHmacAlgorithm algorithm, const void *key, uintptr_t keyLength,
                                    const void *data, uintptr_t dataLength, void *macOut) {
    // Self-contained: never calls the Init/Update/Final entry points, so a
    // per-IPA build selecting only _CCHmac stays link-complete.
    radek_CCHmacContext context;
    radekHmacInit(&context, algorithm, static_cast<const uint8_t *>(key), keyLength);
    RadekHmacContext *state = radekHmacContext(&context);
    if (state == nullptr || macOut == nullptr) return;
    const RadekDigestVtable *vtable = radekDigestVtable(state->algorithm);
    if (vtable == nullptr) return;
    radekDigestUpdate(state->inner, *vtable, static_cast<const uint8_t *>(data), dataLength);
    uint8_t innerDigest[32];
    radekDigestFinal(state->inner, *vtable, innerDigest);
    radekDigestUpdate(state->outer, *vtable, innerDigest, vtable->digestLength);
    radekDigestFinal(state->outer, *vtable, static_cast<uint8_t *>(macOut));
}
#endif

/* --- OSAtomic (libkern) --------------------------------------------------- */

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OSAtomicAdd32Barrier)
extern "C" int32_t radek_compat_OSAtomicAdd32Barrier(int32_t delta, volatile int32_t *value) {
    if (value == nullptr) return 0;
    return __atomic_add_fetch(value, delta, __ATOMIC_SEQ_CST);  // Returns the new value.
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OSAtomicCompareAndSwap32Barrier)
extern "C" radek_Boolean radek_compat_OSAtomicCompareAndSwap32Barrier(int32_t oldValue, int32_t newValue,
                                                                      volatile int32_t *value) {
    if (value == nullptr) return 0;
    return __atomic_compare_exchange_n(value, &oldValue, newValue, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST)
               ? 1
               : 0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_OSAtomicCompareAndSwapPtrBarrier)
extern "C" radek_Boolean radek_compat_OSAtomicCompareAndSwapPtrBarrier(void *oldValue, void *newValue,
                                                                       void *volatile *value) {
    if (value == nullptr) return 0;
    return __atomic_compare_exchange_n(value, &oldValue, newValue, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST)
               ? 1
               : 0;
}
#endif

/* --- Mach kernel surface: ports, host info, semaphores, timing ------------ */

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_mach_host_self)
extern "C" radek_mach_port_t radek_compat_mach_host_self(void) {
    return 0x103u;  // Stable synthetic host port name; Mach IPC itself is absent.
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_mach_task_self_)
extern "C" radek_mach_port_t radek_compat_mach_task_self_(void) {
    return 0x103u;  // Stable synthetic task port name, matching mach_host_self.
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_host_page_size)
extern "C" radek_kern_return_t radek_compat_host_page_size(radek_mach_port_t host, uintptr_t *pageSize) {
    (void)host;
    if (pageSize == nullptr) return 4;  // KERN_INVALID_ARGUMENT
    const long page = sysconf(_SC_PAGESIZE);
    *pageSize = (page > 0) ? static_cast<uintptr_t>(page) : 4096u;
    return 0;  // KERN_SUCCESS
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_host_statistics)
namespace {
// Reads MemTotal / MemFree / MemAvailable from /proc/meminfo, in KiB.
bool radekReadMemInfo(long long &totalKb, long long &freeKb, long long &availableKb) {
    totalKb = freeKb = availableKb = 0;
    FILE *stream = fopen("/proc/meminfo", "r");
    if (stream == nullptr) return false;
    char line[128];
    while (fgets(line, sizeof(line), stream) != nullptr) {
        long long value = 0;
        if (sscanf(line, "MemTotal: %lld kB", &value) == 1) totalKb = value;
        if (sscanf(line, "MemFree: %lld kB", &value) == 1) freeKb = value;
        if (sscanf(line, "MemAvailable: %lld kB", &value) == 1) availableKb = value;
    }
    fclose(stream);
    return totalKb > 0;
}
}  // namespace

extern "C" radek_kern_return_t radek_compat_host_statistics(radek_mach_port_t host, int32_t flavor,
                                                            void *info, uint32_t *infoCount) {
    (void)host;
    if (info == nullptr || infoCount == nullptr) return 4;  // KERN_INVALID_ARGUMENT
    auto *words = static_cast<uint32_t *>(info);
    if (flavor == 2) {  // HOST_VM_INFO
        const uint32_t hostVmInfoCount = 23;  // sizeof(struct host_vm_info) / sizeof(natural_t)
        if (*infoCount < hostVmInfoCount) return 4;
        std::memset(words, 0, hostVmInfoCount * sizeof(uint32_t));
        long long totalKb = 0, freeKb = 0, availableKb = 0;
        if (radekReadMemInfo(totalKb, freeKb, availableKb)) {
            const long long pageSize = 4096;
            const auto toPages = [pageSize](long long kb) {
                return static_cast<uint32_t>((kb * 1024) / pageSize);
            };
            words[0] = toPages(freeKb);                              // free_count
            words[1] = (availableKb > freeKb) ? toPages(availableKb - freeKb) : 0;  // active_count
            words[2] = (totalKb > availableKb) ? toPages(totalKb - availableKb) : 0;  // inactive_count
        }
        *infoCount = hostVmInfoCount;
        return 0;  // KERN_SUCCESS
    }
    if (flavor == 3) {  // HOST_CPU_LOAD_INFO: 4 tick counters.
        if (*infoCount < 4) return 4;
        std::memset(words, 0, 4 * sizeof(uint32_t));
        *infoCount = 4;
        return 0;
    }
    return 4;  // Unsupported flavor: report the argument as invalid, like Mach does.
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_mach_wait_until)
extern "C" radek_kern_return_t radek_compat_mach_wait_until(uint64_t deadlineNanoseconds) {
    // radek mach_absolute_time runs on CLOCK_MONOTONIC nanoseconds, so the
    // deadline is directly comparable.
    struct timespec deadline;
    deadline.tv_sec = static_cast<time_t>(deadlineNanoseconds / 1000000000ull);
    deadline.tv_nsec = static_cast<long>(deadlineNanoseconds % 1000000000ull);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
    }
    return 0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_semaphore_create) || \
    defined(RADEK_API_radek_compat_semaphore_destroy) || defined(RADEK_API_radek_compat_semaphore_signal) || \
    defined(RADEK_API_radek_compat_semaphore_wait)
namespace {

struct RadekMachSemaphore {
    std::mutex mutex;
    std::condition_variable condition;
    int count = 0;
};

std::mutex &radekSemaphoreTableMutex() {
    static std::mutex tableMutex;
    return tableMutex;
}

std::unordered_map<uint32_t, std::shared_ptr<RadekMachSemaphore>> &radekSemaphoreTable() {
    static std::unordered_map<uint32_t, std::shared_ptr<RadekMachSemaphore>> table;
    return table;
}

static inline std::shared_ptr<RadekMachSemaphore> radekSemaphoreFind(uint32_t port) {
    std::lock_guard<std::mutex> lock(radekSemaphoreTableMutex());
    auto found = radekSemaphoreTable().find(port);
    return found == radekSemaphoreTable().end() ? nullptr : found->second;
}

}  // namespace
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_semaphore_create)
extern "C" radek_kern_return_t radek_compat_semaphore_create(radek_mach_port_t task,
                                                             radek_mach_port_t *semaphore, int32_t policy,
                                                             int32_t value) {
    (void)task;
    (void)policy;
    if (semaphore == nullptr) return 4;
    auto state = std::make_shared<RadekMachSemaphore>();
    state->count = value;
    static uint32_t nextPort = 0x4000u;
    std::lock_guard<std::mutex> lock(radekSemaphoreTableMutex());
    const uint32_t port = nextPort++;
    radekSemaphoreTable().emplace(port, std::move(state));
    *semaphore = port;
    return 0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_semaphore_destroy)
extern "C" radek_kern_return_t radek_compat_semaphore_destroy(radek_mach_port_t task,
                                                              radek_mach_port_t semaphore) {
    (void)task;
    std::lock_guard<std::mutex> lock(radekSemaphoreTableMutex());
    return radekSemaphoreTable().erase(semaphore) > 0 ? 0 : 4;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_semaphore_signal)
extern "C" radek_kern_return_t radek_compat_semaphore_signal(radek_mach_port_t semaphore) {
    auto state = radekSemaphoreFind(semaphore);
    if (state == nullptr) return 4;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        ++state->count;
    }
    state->condition.notify_one();
    return 0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_semaphore_wait)
extern "C" radek_kern_return_t radek_compat_semaphore_wait(radek_mach_port_t semaphore,
                                                           radek_mach_msg_timeout_t timeout) {
    auto state = radekSemaphoreFind(semaphore);
    if (state == nullptr) return 4;
    std::unique_lock<std::mutex> lock(state->mutex);
    const auto available = [&state] { return state->count > 0; };
    if (timeout == 0xFFFFFFFFu) {  // MACH_MSG_TIMEOUT_NEVER
        state->condition.wait(lock, available);
    } else {
        if (!state->condition.wait_for(lock, std::chrono::milliseconds(timeout), available)) {
            return 49;  // KERN_OPERATION_TIMED_OUT
        }
    }
    --state->count;
    return 0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_task_info)
extern "C" radek_kern_return_t radek_compat_task_info(radek_mach_port_t task, int32_t flavor, void *info,
                                                      uint32_t *infoCount) {
    (void)task;
    if (info == nullptr || infoCount == nullptr || *infoCount == 0) return 4;  // KERN_INVALID_ARGUMENT
    auto *words = static_cast<uint32_t *>(info);
    const size_t bytes = static_cast<size_t>(*infoCount) * sizeof(uint32_t);
    std::memset(words, 0, bytes);
    if (flavor == 4 || flavor == 14 || flavor == 20) {
        // TASK_BASIC_INFO / TASK_BASIC_INFO_2 / MACH_TASK_BASIC_INFO all begin
        // with {user_time, system_time, policy, suspend_count, virtual_size,
        // resident_size, ...}; fill the two sizes from the process itself.
        long vmSizeKb = -1;
        long vmRssKb = -1;
        FILE *stream = fopen("/proc/self/status", "r");
        if (stream != nullptr) {
            char line[128];
            while (fgets(line, sizeof(line), stream) != nullptr) {
                long value = 0;
                if (sscanf(line, "VmSize: %ld kB", &value) == 1) vmSizeKb = value;
                if (sscanf(line, "VmRSS: %ld kB", &value) == 1) vmRssKb = value;
            }
            fclose(stream);
        }
        // Word layout: user_time(2), system_time(2), policy(1), suspend_count(1),
        // virtual_size(1), resident_size(1).
        if (bytes >= 8 * sizeof(uint32_t)) {
            if (vmSizeKb >= 0) words[6] = static_cast<uint32_t>(vmSizeKb) * 1024u;
            if (vmRssKb >= 0) words[7] = static_cast<uint32_t>(vmRssKb) * 1024u;
        }
    }
    return 0;  // KERN_SUCCESS: the zero-filled info struct is a valid answer.
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_thread_policy_set)
extern "C" radek_kern_return_t radek_compat_thread_policy_set(uint32_t thread, int32_t flavor,
                                                              void *policy, uint32_t count) {
    (void)thread;
    (void)flavor;
    (void)policy;
    (void)count;
    // Thread QoS/priority policy has no portable bionic equivalent reachable
    // from here; Darwin returns KERN_SUCCESS for well-formed policy requests,
    // and accepting them keeps converted scheduler glue running.
    return 0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_mach_thread_np)
extern "C" uint64_t radek_compat_pthread_mach_thread_np(pthread_t thread) {
    // A stable, non-zero identifier derived from the thread handle. Mach port
    // names are opaque per-thread handles; callers only compare or log them.
    const uint64_t value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(thread));
    return (value & 0x7FFFFFFFull) != 0 ? (value & 0x7FFFFFFFull) : 1u;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_pthread_threadid_np)
extern "C" int32_t radek_compat_pthread_threadid_np(pthread_t thread, uint64_t *threadId) {
    if (threadId == nullptr) return 22;  // EINVAL
    if (thread == pthread_self()) {
        const long tid = syscall(SYS_gettid);
        *threadId = (tid > 0) ? static_cast<uint64_t>(tid) : 1u;
    } else {
        const uint64_t value = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(thread));
        *threadId = (value & 0x7FFFFFFFull) != 0 ? (value & 0x7FFFFFFFull) : 1u;
    }
    return 0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_dispatch_get_current_queue)
extern "C" uintptr_t radek_compat_dispatch_get_current_queue(void) {
    // No libdispatch is present; return one stable non-null queue token so
    // equality comparisons against dispatch_get_main_queue-style checks work.
    static const char kRadekFakeMainQueue = 0;
    return reinterpret_cast<uintptr_t>(&kRadekFakeMainQueue);
}
#endif

/* --- C / C++ ABI helpers --------------------------------------------------- */

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___assert_rtn)
extern "C" void radek_compat___assert_rtn(const char *function, const char *file, int32_t line,
                                          const char *assertion) {
    // Darwin prints exactly this format and aborts; reproduce that honestly.
    fprintf(stderr, "Assertion failed: (%s), function %s, file %s, line %d.\n",
            assertion != nullptr ? assertion : "?", function != nullptr ? function : "?",
            file != nullptr ? file : "?", static_cast<int>(line));
    fflush(stderr);
    abort();
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___cxa_call_unexpected)
extern "C" void radek_compat___cxa_call_unexpected(void *exceptionObject) {
    (void)exceptionObject;
    // An exception escaped a dynamic-exception-specification frame. The
    // documented C++ behaviour is std::terminate().
    std::terminate();
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___divmodsi4)
extern "C" radek_divmodsi4_result radek_compat___divmodsi4(int32_t numerator, int32_t denominator) {
    radek_divmodsi4_result result;
    if (denominator == 0) {
        // Division by zero traps on real hardware; return zeros as the defined
        // safe value instead of taking the process down.
        result.quotient = 0;
        result.remainder = 0;
        return result;
    }
    if (numerator == INT32_MIN && denominator == -1) {
        result.quotient = INT32_MIN;  // Saturate the only overflowing case.
        result.remainder = 0;
        return result;
    }
    result.quotient = numerator / denominator;
    result.remainder = numerator % denominator;
    return result;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___objc_personality_v0)
extern "C" int32_t __gxx_personality_v0(int32_t version, int32_t actions, uint64_t exceptionClass,
                                        void *exceptionObject, void *context);
extern "C" int32_t radek_compat___objc_personality_v0(int32_t version, int32_t actions,
                                                      uint64_t exceptionClass, uintptr_t exceptionObject,
                                                      uintptr_t context) {
    // Darwin's __objc_personality_v0 wraps the C++ personality and only
    // intercepts Objective-C exception classes. Without an Objective-C
    // runtime the correct behaviour is to delegate straight to the C++
    // personality, which this binary links.
    return __gxx_personality_v0(version, actions, exceptionClass,
                                reinterpret_cast<void *>(exceptionObject),
                                reinterpret_cast<void *>(context));
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___sincos_stret)
extern "C" radek_sincos_result radek_compat___sincos_stret(double angle) {
    radek_sincos_result result;
    result.sin = sin(angle);
    result.cos = cos(angle);
    return result;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat___sincosf_stret)
extern "C" radek_sincosf_result radek_compat___sincosf_stret(float angle) {
    radek_sincosf_result result;
    result.sin = sinf(angle);
    result.cos = cosf(angle);
    return result;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_memset_pattern16)
extern "C" void radek_compat_memset_pattern16(void *destination, const void *pattern16, uintptr_t length) {
    if (destination == nullptr || pattern16 == nullptr || length == 0) return;
    auto *cursor = static_cast<uint8_t *>(destination);
    const auto *pattern = static_cast<const uint8_t *>(pattern16);
    while (length >= 16) {
        std::memcpy(cursor, pattern, 16);
        cursor += 16;
        length -= 16;
    }
    if (length > 0) std::memcpy(cursor, pattern, length);
}
#endif

/* --- Blocks runtime: real copy/dispose semantics (libclosure-equivalent) -- */

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_Block_object_assign) || \
    defined(RADEK_API_radek_compat_Block_object_dispose)
namespace {

struct RadekBlockLiteral {
    void *isa;
    int32_t flags;
    int32_t reserved;
    void *invoke;
    void *descriptor;
};

struct RadekBlockDescriptor {
    uintptr_t reserved;
    uintptr_t size;
    void (*copyHelper)(void *dst, const void *src);
    void (*disposeHelper)(const void *src);
};

struct RadekBlockByref {
    void *isa;
    struct RadekBlockByref *forwarding;
    int32_t flags;
    int32_t size;
};

// Flags from libclosure's Block_private.h.
const int32_t kRadekBlockRefCountMask = 0xFFFF;
const int32_t kRadekBlockNeedsFree = 1 << 24;
const int32_t kRadekBlockHasCopyDispose = 1 << 25;
const int32_t kRadekBlockIsGlobal = 1 << 28;
const int32_t kRadekBlockFieldIsObject = 3;
const int32_t kRadekBlockFieldIsBlock = 7;
const int32_t kRadekBlockFieldIsByref = 8;
const int32_t kRadekBlockByrefNeedsFree = 1 << 25;  // Heap-allocated byref copy.

std::mutex &radekBlockMutex() {
    static std::mutex mutex;
    return mutex;
}

}  // namespace
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_Block_object_assign)
extern "C" void radek_compat_Block_object_assign(void *destination, const void *source, int32_t flags) {
    if (destination == nullptr || source == nullptr) return;
    auto *slot = static_cast<void **>(destination);
    switch (flags & 0x00FF) {  // BLOCK_ALL_COPY_DISPOSE_FLAGS low bits
        case kRadekBlockFieldIsObject: {
            // No Objective-C runtime exists here, so there is no retain to
            // perform; store the object pointer unchanged.
            *slot = const_cast<void *>(source);
            break;
        }
        case kRadekBlockFieldIsBlock: {
            auto *block = const_cast<RadekBlockLiteral *>(static_cast<const RadekBlockLiteral *>(source));
            if ((block->flags & kRadekBlockNeedsFree) != 0) {
                // Heap block: bump the reference count and share it.
                std::lock_guard<std::mutex> lock(radekBlockMutex());
                block->flags += 2;  // latching_incr_int: refcount += 2 per libclosure.
                *slot = block;
            } else if ((block->flags & kRadekBlockIsGlobal) != 0) {
                *slot = block;  // Global blocks are immortal.
            } else {
                // Stack block: move it to the heap and run its copy helper.
                auto *descriptor = static_cast<RadekBlockDescriptor *>(block->descriptor);
                if (descriptor == nullptr || descriptor->size < sizeof(RadekBlockLiteral)) {
                    *slot = block;
                    break;
                }
                auto *copy = static_cast<RadekBlockLiteral *>(malloc(descriptor->size));
                if (copy == nullptr) {
                    *slot = block;
                    break;
                }
                std::memcpy(copy, block, descriptor->size);
                copy->flags &= ~kRadekBlockRefCountMask;
                copy->flags |= kRadekBlockNeedsFree | 2;  // one reference, needs free.
                copy->descriptor = descriptor;
                if ((copy->flags & kRadekBlockHasCopyDispose) != 0 &&
                    descriptor->copyHelper != nullptr) {
                    descriptor->copyHelper(copy, block);
                }
                *slot = copy;
            }
            break;
        }
        case kRadekBlockFieldIsByref: {
            // __block variable: allocate the heap copy and repoint both
            // forwarding pointers at it, exactly like _Block_byref_assign_copy.
            auto *byref = const_cast<RadekBlockByref *>(static_cast<const RadekBlockByref *>(source));
            if (byref->size < static_cast<int32_t>(sizeof(RadekBlockByref))) {
                *slot = byref;
                break;
            }
            auto *copy = static_cast<RadekBlockByref *>(malloc(static_cast<size_t>(byref->size)));
            if (copy == nullptr) {
                *slot = byref;
                break;
            }
            std::memcpy(copy, byref, static_cast<size_t>(byref->size));
            copy->isa = nullptr;
            copy->forwarding = copy;
            copy->flags |= kRadekBlockByrefNeedsFree;  // Marks the copy as heap-owned.
            byref->forwarding = copy;
            *slot = copy;
            break;
        }
        default:
            *slot = const_cast<void *>(source);  // Weak/unknown: plain store.
            break;
    }
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_Block_object_dispose)
extern "C" void radek_compat_Block_object_dispose(const void *object, int32_t flags) {
    if (object == nullptr) return;
    switch (flags & 0x00FF) {
        case kRadekBlockFieldIsBlock: {
            auto *block = const_cast<RadekBlockLiteral *>(static_cast<const RadekBlockLiteral *>(object));
            if ((block->flags & kRadekBlockNeedsFree) == 0) break;
            bool freeNow = false;
            {
                std::lock_guard<std::mutex> lock(radekBlockMutex());
                block->flags -= 2;  // latching_decr_int.
                freeNow = (block->flags & kRadekBlockRefCountMask) == 0;
            }
            if (freeNow) {
                auto *descriptor = static_cast<RadekBlockDescriptor *>(block->descriptor);
                if (descriptor != nullptr && (block->flags & kRadekBlockHasCopyDispose) != 0 &&
                    descriptor->disposeHelper != nullptr) {
                    descriptor->disposeHelper(block);
                }
                free(block);
            }
            break;
        }
        case kRadekBlockFieldIsByref: {
            // Only heap copies (the NEEDS_FREE bit set by the assign path) are
            // owned allocations; stack byrefs are left alone, like libclosure.
            auto *byref = const_cast<RadekBlockByref *>(static_cast<const RadekBlockByref *>(object));
            if ((byref->flags & kRadekBlockByrefNeedsFree) != 0) free(byref);
            break;
        }
        default:
            break;  // Objects: no runtime to release with; nothing to do.
    }
}
#endif

/* --- libc++ internals the converted C++ links against ---------------------- */

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_stl_throw_length_error)
extern "C" void radek_compat_stl_throw_length_error(const char *message) {
    // Throws through the same libc++abi the converted code links against, so
    // a guest catch(const std::length_error&) catches exactly this object.
    throw std::length_error(message != nullptr ? message : "length_error");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_stl_throw_out_of_range)
extern "C" void radek_compat_stl_throw_out_of_range(const char *message) {
    throw std::out_of_range(message != nullptr ? message : "out_of_range");
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_rs_default_dtor)
extern "C" void radek_compat_rs_default_dtor(void *randomShuffleState) {
    (void)randomShuffleState;
    // std::__1::__rs::__rs_default owns no resources; its destructor is a no-op.
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_rs_default_call)
extern "C" uint32_t radek_compat_rs_default_call(void *randomShuffleState) {
    (void)randomShuffleState;
    // Random source behind std::random_shuffle. xorshift32 gives the uniform
    // draw std::random_shuffle requires without needing libc++'s private state.
    static uint32_t state = 0x9E3779B9u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_rs_get)
extern "C" uint32_t radek_compat_rs_get(void) {
    // __rs_get returns a default-constructed __rs_default (one 32-bit word).
    return 0u;
}
#endif

/* --- Objective-C runtime entry points -------------------------------------- */

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_setAssociatedObject)
namespace {
struct RadekAssociationKey {
    void *object;
    const void *key;
    bool operator==(const RadekAssociationKey &other) const {
        return object == other.object && key == other.key;
    }
};
struct RadekAssociationKeyHash {
    size_t operator()(const RadekAssociationKey &entry) const {
        return std::hash<void *>()(entry.object) ^ (std::hash<const void *>()(entry.key) * 31u);
    }
};
std::mutex &radekAssociationMutex() {
    static std::mutex mutex;
    return mutex;
}
std::unordered_map<RadekAssociationKey, void *, RadekAssociationKeyHash> &radekAssociationTable() {
    static std::unordered_map<RadekAssociationKey, void *, RadekAssociationKeyHash> table;
    return table;
}
}  // namespace

extern "C" void radek_compat_objc_setAssociatedObject(void *object, const void *key, void *value,
                                                      int32_t policy) {
    (void)policy;  // Retain/copy policies differ only in the value's memory management.
    if (object == nullptr || key == nullptr) return;
    std::lock_guard<std::mutex> lock(radekAssociationMutex());
    auto &table = radekAssociationTable();
    const RadekAssociationKey pairKey{object, key};
    if (value == nullptr) {
        table.erase(pairKey);  // Setting nil removes the association.
    } else {
        table[pairKey] = value;
    }
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_setProperty_atomic) || \
    defined(RADEK_API_radek_compat_objc_setProperty_atomic_copy) || \
    defined(RADEK_API_radek_compat_objc_setProperty_nonatomic) || \
    defined(RADEK_API_radek_compat_objc_setProperty_nonatomic_copy)
namespace {
// The ivar is a pointer-sized slot at (self + offset). Without an Objective-C
// runtime there is no retain/release to run, so the store itself IS the
// documented visible effect; the atomic variants use a sequentially
// consistent store.
static inline void radekObjcStoreProperty(void *self, uintptr_t offset, void *newValue, bool atomic) {
    if (self == nullptr) return;
    void **slot = reinterpret_cast<void **>(static_cast<char *>(self) + offset);
    if (atomic) {
        __atomic_store_n(slot, newValue, __ATOMIC_SEQ_CST);
    } else {
        *slot = newValue;
    }
}
}  // namespace
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_setProperty_atomic)
extern "C" void radek_compat_objc_setProperty_atomic(void *self, uintptr_t offset, void *newValue) {
    radekObjcStoreProperty(self, offset, newValue, true);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_setProperty_atomic_copy)
extern "C" void radek_compat_objc_setProperty_atomic_copy(void *self, uintptr_t offset, void *newValue) {
    radekObjcStoreProperty(self, offset, newValue, true);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_setProperty_nonatomic)
extern "C" void radek_compat_objc_setProperty_nonatomic(void *self, uintptr_t offset, void *newValue) {
    radekObjcStoreProperty(self, offset, newValue, false);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_objc_setProperty_nonatomic_copy)
extern "C" void radek_compat_objc_setProperty_nonatomic_copy(void *self, uintptr_t offset, void *newValue) {
    radekObjcStoreProperty(self, offset, newValue, false);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_radek_compat_NSSetUncaughtExceptionHandler)
extern "C" radek_NSUncaughtExceptionHandler radek_compat_NSGetUncaughtExceptionHandler(
    radek_NSUncaughtExceptionHandler newHandler);

extern "C" void radek_compat_NSSetUncaughtExceptionHandler(radek_NSUncaughtExceptionHandler handler) {
    // The registered handler is kept observable so tests and diagnostics can
    // confirm the store; no Objective-C runtime exists here to invoke it.
    radek_compat_NSGetUncaughtExceptionHandler(handler);
}

extern "C" radek_NSUncaughtExceptionHandler radek_compat_NSGetUncaughtExceptionHandler(
    radek_NSUncaughtExceptionHandler newHandler) {
    static radek_NSUncaughtExceptionHandler storedHandler = nullptr;
    const radek_NSUncaughtExceptionHandler previous = storedHandler;
    if (newHandler != nullptr) storedHandler = newHandler;
    return previous;
}
#endif
