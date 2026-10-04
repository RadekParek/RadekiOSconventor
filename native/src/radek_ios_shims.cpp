#include "radek_ios_shims.h"

/*
 * Broad, host-tested iOS/Darwin compatibility shims.
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

#include <cstring>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>


#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_NEEDS_CF_RUNTIME)
#define RADEK_CF_RUNTIME 1
#endif

#ifdef RADEK_CF_RUNTIME

#include <atomic>
#include <string>
#include <utility>
#include <vector>

/*
 * The real object type behind every radek_CF*Ref declared in the header. It is
 * defined at file scope because the header forward-declares exactly this tag;
 * helpers are `static inline` so a build that selects no CoreFoundation shim
 * never trips an unused-function warning.
 */
enum class RadekCFKind : uint32_t {
    Allocator = 1,
    String = 2,
    Data = 3,
    Array = 4,
    Dictionary = 5,
    Number = 6,
    Date = 7,
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

    ~radek_CFRuntime();
};

static inline void radekCfReleaseInternal(const radek_CFRuntime *object);
static inline void radekCfRetainInternal(const radek_CFRuntime *object);

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

static inline void radekCfRetainInternal(const radek_CFRuntime *object) {
    if (object == nullptr || object->kind == RadekCFKind::Allocator) return;
    const_cast<radek_CFRuntime *>(object)->retainCount.fetch_add(1, std::memory_order_relaxed);
}

static inline void radekCfReleaseInternal(const radek_CFRuntime *object) {
    if (object == nullptr || object->kind == RadekCFKind::Allocator) return;
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
