// Host tests for the bounded C/POSIX/CoreFoundation compatibility shims.
//
// Everything exercised here is a real implementation body, not a resolution
// stub. The test is also run under ASan/UBSan, so it must stay leak-free:
// every CoreFoundation object created below is released.

#include "radek_ios_shims.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define CHECK(expression)                                                              \
    do {                                                                               \
        if (!(expression)) throw std::runtime_error("CHECK failed: " #expression);      \
    } while (false)

namespace {

int compareInts(const void *left, const void *right) {
    const int a = *static_cast<const int *>(left);
    const int b = *static_cast<const int *>(right);
    return (a > b) - (a < b);
}

int callVsnprintf(char *buffer, size_t size, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    const int written = radek_compat_vsnprintf(buffer, size, format, arguments);
    va_end(arguments);
    return written;
}

void testLibc() {
    void *memory = radek_compat_malloc(64);
    CHECK(memory != nullptr);
    radek_compat_memset(memory, 0xAB, 64);
    CHECK(radek_compat_memcmp(memory, std::vector<unsigned char>(64, 0xAB).data(), 64) == 0);
    void *zeroed = radek_compat_calloc(8, 4);
    CHECK(zeroed != nullptr);
    CHECK(static_cast<unsigned char *>(zeroed)[31] == 0);
    void *grown = radek_compat_realloc(memory, 4096);
    CHECK(grown != nullptr);
    radek_compat_free(grown);
    radek_compat_free(zeroed);

    char copiedString[16] = {};
    CHECK(radek_compat_strcpy(copiedString, "copy") == copiedString);
    CHECK(std::string(copiedString) == "copy");
    char boundedString[8] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    CHECK(radek_compat_strncpy(boundedString, "ok", 5) == boundedString);
    CHECK(boundedString[0] == 'o' && boundedString[1] == 'k' && boundedString[2] == '\0' && boundedString[4] == '\0');

    unsigned char copiedBytes[4] = {};
    const unsigned char sourceBytes[4] = {9, 8, 7, 6};
    CHECK(radek_compat_memcpy(copiedBytes, sourceBytes, sizeof(sourceBytes)) == copiedBytes);
    CHECK(copiedBytes[0] == 9 && copiedBytes[3] == 6);

    CHECK(radek_compat_strlen("radek") == 5);
    CHECK(radek_compat_strcmp("abc", "abc") == 0);
    CHECK(radek_compat_strcmp("abc", "abd") < 0);
    CHECK(radek_compat_strncmp("abcd", "abzz", 2) == 0);
    char *copy = radek_compat_strdup("duplicated");
    CHECK(copy != nullptr && std::string(copy) == "duplicated");
    radek_compat_free(copy);
    CHECK(radek_compat_strchr("abcdef", 'd') != nullptr);
    CHECK(*radek_compat_strchr("abcdef", 'd') == 'd');
    CHECK(radek_compat_strrchr("a.b.c", '.') != nullptr);
    CHECK(std::string(radek_compat_strstr("haystack", "stack")) == "stack");
    CHECK(radek_compat_strtol("42abc", nullptr, 10) == 42);
    CHECK(std::abs(radek_compat_strtod("2.5", nullptr) - 2.5) < 1e-9);
    CHECK(radek_compat_atoi("-17") == -17);
    CHECK(std::abs(radek_compat_atof("0.5") - 0.5) < 1e-9);
    CHECK(radek_compat_strerror(0) != nullptr);

    char small[4] = {};
    CHECK(radek_compat_strlcpy(small, "abcdef", sizeof(small)) == 6);
    CHECK(std::string(small) == "abc");
    CHECK(radek_compat_strlcpy(small, "z", sizeof(small)) == 1);
    CHECK(std::string(small) == "z");
    CHECK(radek_compat_strlcat(small, "123456", sizeof(small)) == 7);
    CHECK(std::string(small) == "z12");

    char formatted[32] = {};
    CHECK(radek_compat_snprintf(formatted, sizeof(formatted), "%d-%s", 7, "ok") == 4);
    CHECK(std::string(formatted) == "7-ok");
    CHECK(callVsnprintf(formatted, sizeof(formatted), "%s-%d", "ok", 7) == 4);
    CHECK(std::string(formatted) == "ok-7");

    CHECK(radek_compat_printf("shim-%d\n", 7) == 7);
    CHECK(radek_compat_puts("shim-puts") >= 0);
    radek_compat_srand(1234);
    const int randomValue = radek_compat_rand();
    radek_compat_srand(1234);
    CHECK(radek_compat_rand() == randomValue);

    unsigned char buffer[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    unsigned char moved[8] = {};
    radek_compat_memmove(moved, buffer, sizeof(buffer));
    CHECK(moved[7] == 8);
    CHECK(radek_compat_memchr(buffer, 5, sizeof(buffer)) == buffer + 4);

    CHECK(std::abs(radek_compat_sqrt(81.0) - 9.0) < 1e-9);
    CHECK(std::abs(radek_compat_fabs(-3.0) - 3.0) < 1e-9);
    CHECK(std::abs(radek_compat_floor(2.7) - 2.0) < 1e-9);
    CHECK(std::abs(radek_compat_ceil(2.1) - 3.0) < 1e-9);
    CHECK(std::abs(radek_compat_pow(2.0, 10.0) - 1024.0) < 1e-6);
    CHECK(std::abs(radek_compat_sin(0.0)) < 1e-12);
    CHECK(std::abs(radek_compat_cos(0.0) - 1.0) < 1e-12);
    CHECK(std::abs(radek_compat_tan(0.0)) < 1e-12);
    CHECK(std::abs(radek_compat_atan2(0.0, 1.0)) < 1e-12);
    CHECK(std::abs(radek_compat_fmod(7.0, 3.0) - 1.0) < 1e-9);
    CHECK(radek_compat_abs(-9) == 9);
    CHECK(radek_compat_labs(-900000L) == 900000L);

    int values[5] = {5, 1, 4, 2, 3};
    radek_compat_qsort(values, 5, sizeof(int), compareInts);
    CHECK(values[0] == 1 && values[4] == 5);
    int key = 4;
    CHECK(radek_compat_bsearch(&key, values, 5, sizeof(int), compareInts) != nullptr);

    CHECK(radek_compat_getpid() > 0);
    CHECK(radek_compat_setenv("RADEK_SHIM_TEST", "1", 1) == 0);
    const char *value = radek_compat_getenv("RADEK_SHIM_TEST");
    CHECK(value != nullptr && std::string(value) == "1");
    CHECK(radek_compat_unsetenv("RADEK_SHIM_TEST") == 0);
    CHECK(radek_compat_getenv("RADEK_SHIM_TEST") == nullptr);
}

void testStdio() {
    const char *path = std::getenv("TMPDIR") ? (std::string(std::getenv("TMPDIR")) + "/radek_shim_test.txt").c_str()
                                             : "/tmp/radek_shim_test.txt";
    FILE *stream = radek_compat_fopen(path, "w+");
    CHECK(stream != nullptr);
    CHECK(radek_compat_fwrite("radek", 1, 5, stream) == 5);
    CHECK(radek_compat_fputs("-", stream) >= 0);
    CHECK(radek_compat_fprintf(stream, "%d", 42) == 2);
    CHECK(radek_compat_fflush(stream) == 0);
    CHECK(radek_compat_fseek(stream, 0, SEEK_SET) == 0);
    char read[32] = {};
    CHECK(radek_compat_fread(read, 1, sizeof(read) - 1, stream) == 8);
    CHECK(std::string(read) == "radek-42");
    CHECK(radek_compat_feof(stream) != 0);
    CHECK(radek_compat_ftell(stream) == 8);
    CHECK(radek_compat_fseek(stream, 0, SEEK_SET) == 0);
    char line[16] = {};
    CHECK(radek_compat_fgets(line, sizeof(line), stream) == line);
    CHECK(std::string(line) == "radek-42");
    CHECK(radek_compat_fclose(stream) == 0);
    CHECK(radek_compat_remove(path) == 0);
}

void testTime() {
    const time_t now = radek_compat_time(nullptr);
    CHECK(now > 1500000000);

    radek_darwin_timeval wall{};
    CHECK(radek_compat_gettimeofday(&wall, nullptr) == 0);
    CHECK(wall.tv_sec > 1500000000);
    CHECK(wall.tv_usec >= 0 && wall.tv_usec < 1000000);

    struct timespec monotonic{};
    CHECK(radek_compat_clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0);

    struct timespec request{};
    request.tv_nsec = 2 * 1000 * 1000;  // 2 ms
    const struct timespec before = monotonic;
    CHECK(radek_compat_nanosleep(&request, nullptr) == 0);
    struct timespec after{};
    CHECK(radek_compat_clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    const double elapsed = static_cast<double>(after.tv_sec - before.tv_sec) +
                           static_cast<double>(after.tv_nsec - before.tv_nsec) / 1e9;
    CHECK(elapsed >= 0.001);

    time_t clockValue = 0;  // 1970-01-01T00:00:00Z
    struct tm broken{};
    CHECK(radek_compat_gmtime_r(&clockValue, &broken) != nullptr);
    CHECK(broken.tm_year == 70 && broken.tm_mon == 0 && broken.tm_mday == 1);
    struct tm localBroken{};
    CHECK(radek_compat_localtime_r(&clockValue, &localBroken) != nullptr);
    struct tm rebuild{};
    rebuild.tm_year = 100;  // 2000
    rebuild.tm_mon = 0;
    rebuild.tm_mday = 1;
    rebuild.tm_hour = 12;
    CHECK(radek_compat_mktime(&rebuild) > 0);
}

void testPthread() {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    CHECK(radek_compat_pthread_mutex_init(&mutex, nullptr) == 0);
    CHECK(radek_compat_pthread_cond_init(&condition, nullptr) == 0);

    std::atomic<bool> ready{false};
    std::atomic<bool> seen{false};
    std::thread worker([&] {
        radek_compat_pthread_mutex_lock(&mutex);
        while (!ready.load()) radek_compat_pthread_cond_wait(&condition, &mutex);
        radek_compat_pthread_mutex_unlock(&mutex);
        seen.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(radek_compat_pthread_mutex_lock(&mutex) == 0);
    ready.store(true);
    CHECK(radek_compat_pthread_cond_broadcast(&condition) == 0);
    CHECK(radek_compat_pthread_mutex_unlock(&mutex) == 0);
    CHECK(radek_compat_pthread_cond_signal(&condition) == 0);
    worker.join();
    CHECK(seen.load());

    CHECK(radek_compat_pthread_self() != 0);
    CHECK(radek_compat_pthread_cond_destroy(&condition) == 0);
    CHECK(radek_compat_pthread_mutex_destroy(&mutex) == 0);
}

struct RunLoopCall {
    std::atomic<unsigned> count{0};
    radek_CFRunLoopRef loop = nullptr;
    bool stop = false;
};

void incrementRunLoopCall(void *opaque) {
    auto *call = static_cast<RunLoopCall *>(opaque);
    call->count.fetch_add(1, std::memory_order_relaxed);
    if (call->stop) radek_compat_CFRunLoopStop(call->loop);
}

void testCoreFoundationRunLoop() {
    const radek_CFRunLoopRef current = radek_compat_CFRunLoopGetCurrent();
    CHECK(current != nullptr);
    CHECK(current == radek_compat_CFRunLoopGetCurrent());
    CHECK(current == radek_compat_CFRunLoopGetMain());
    CHECK(radek_compat_CFRunLoopGetMain() == radek_compat_CFRunLoopGetMain());
    CHECK(radek_compat_CFRetain(current) == current);
    radek_compat_CFRelease(current); // current/main loops have process/thread lifetime

    const radek_CFAllocatorRef allocator = radek_compat_CFAllocatorGetDefault();
    const radek_CFStringRef defaultMode =
        radek_compat_CFStringCreateWithCString(allocator, "default", RADEK_KCFSTRINGENCODINGUTF8);
    const radek_CFStringRef otherMode =
        radek_compat_CFStringCreateWithCString(allocator, "other", RADEK_KCFSTRINGENCODINGUTF8);
    CHECK(defaultMode != nullptr && otherMode != nullptr);

    RunLoopCall call;
    call.loop = current;
    CHECK(radek_compat_CFRunLoopPerform(current, defaultMode, incrementRunLoopCall, &call) == 1);
    CHECK(radek_compat_CFRunLoopRunInMode(defaultMode, 1.0, 1) == RADEK_KCFRUNLOOPRUNHANDLEDSOURCE);
    CHECK(call.count.load(std::memory_order_relaxed) == 1);

    CHECK(radek_compat_CFRunLoopPerform(current, otherMode, incrementRunLoopCall, &call) == 1);
    CHECK(radek_compat_CFRunLoopRunInMode(defaultMode, 0.005, 0) == RADEK_KCFRUNLOOPRUNTIMEDOUT);
    CHECK(call.count.load(std::memory_order_relaxed) == 1);
    CHECK(radek_compat_CFRunLoopRunInMode(otherMode, 1.0, 1) == RADEK_KCFRUNLOOPRUNHANDLEDSOURCE);
    CHECK(call.count.load(std::memory_order_relaxed) == 2);

    std::atomic<radek_Boolean> queued{0};
    std::thread producer([&] {
        queued.store(radek_compat_CFRunLoopPerform(current, defaultMode, incrementRunLoopCall, &call),
                     std::memory_order_release);
    });
    CHECK(radek_compat_CFRunLoopRunInMode(defaultMode, 1.0, 1) == RADEK_KCFRUNLOOPRUNHANDLEDSOURCE);
    producer.join();
    CHECK(queued.load(std::memory_order_acquire) == 1);
    CHECK(call.count.load(std::memory_order_relaxed) == 3);

    call.stop = true;
    CHECK(radek_compat_CFRunLoopPerform(current, defaultMode, incrementRunLoopCall, &call) == 1);
    CHECK(radek_compat_CFRunLoopRunInMode(defaultMode, 1.0, 0) == RADEK_KCFRUNLOOPRUNSTOPPED);
    CHECK(call.count.load(std::memory_order_relaxed) == 4);
    radek_compat_CFRunLoopWakeUp(current);
    std::thread stopper([current] {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        radek_compat_CFRunLoopStop(current);
    });
    radek_compat_CFRunLoopRun();
    stopper.join();

    radek_compat_CFRelease(defaultMode);
    radek_compat_CFRelease(otherMode);
}

void testCoreFoundationObjects() {
    radek_CFAllocatorRef allocator = radek_compat_CFAllocatorGetDefault();
    CHECK(allocator != nullptr);
    // The process-lifetime allocator ignores retain/release.
    radek_compat_CFRetain(allocator);
    radek_compat_CFRelease(allocator);

    CHECK(radek_compat_CFStringGetSystemEncoding() == RADEK_KCFSTRINGENCODINGUTF8);

    radek_CFStringRef hello = radek_compat_CFStringCreateWithCString(allocator, "hello", RADEK_KCFSTRINGENCODINGUTF8);
    CHECK(hello != nullptr);
    CHECK(radek_compat_CFStringGetLength(hello) == 5);
    CHECK(radek_compat_CFStringGetMaximumSizeForEncoding(5, RADEK_KCFSTRINGENCODINGUTF8) == 16);
    const char *raw = radek_compat_CFStringGetCStringPtr(hello, RADEK_KCFSTRINGENCODINGUTF8);
    CHECK(raw != nullptr && std::string(raw) == "hello");
    char small[3] = {};
    CHECK(radek_compat_CFStringGetCString(hello, small, sizeof(small), RADEK_KCFSTRINGENCODINGUTF8) == 0);
    char roomy[16] = {};
    CHECK(radek_compat_CFStringGetCString(hello, roomy, sizeof(roomy), RADEK_KCFSTRINGENCODINGUTF8) == 1);
    CHECK(std::string(roomy) == "hello");
    // A non-UTF-8 encoding is refused instead of silently mis-decoding.
    CHECK(radek_compat_CFStringGetCString(hello, roomy, sizeof(roomy), 0x0600) == 0);
    CHECK(radek_compat_CFStringCreateWithCString(allocator, "x", 0x0600) == nullptr);

    radek_CFStringRef other = radek_compat_CFStringCreateWithCString(allocator, "hellp", RADEK_KCFSTRINGENCODINGUTF8);
    CHECK(radek_compat_CFStringCompare(hello, other, 0) == RADEK_KCFCOMPARELESSTHAN);
    CHECK(radek_compat_CFStringCompare(other, hello, 0) == RADEK_KCFCOMPAREGREATERTHAN);
    CHECK(radek_compat_CFStringCompare(hello, hello, 0) == RADEK_KCFCOMPAREEQUALTO);

    CHECK(radek_compat_CFGetRetainCount(hello) == 1);
    radek_compat_CFRetain(hello);
    CHECK(radek_compat_CFGetRetainCount(hello) == 2);
    radek_compat_CFRelease(hello);
    CHECK(radek_compat_CFGetRetainCount(hello) == 1);

    radek_CFDataRef data = radek_compat_CFDataCreate(allocator, reinterpret_cast<const uint8_t *>("abc"), 3);
    CHECK(data != nullptr);
    CHECK(radek_compat_CFDataGetLength(data) == 3);
    CHECK(std::memcmp(radek_compat_CFDataGetBytePtr(data), "abc", 3) == 0);
    radek_CFDataRef empty = radek_compat_CFDataCreate(allocator, nullptr, 0);
    CHECK(radek_compat_CFDataGetLength(empty) == 0);
    CHECK(radek_compat_CFDataGetBytePtr(empty) == nullptr);

    radek_CFMutableArrayRef array = radek_compat_CFArrayCreateMutable(allocator, 2, nullptr);
    CHECK(array != nullptr);
    CHECK(radek_compat_CFArrayGetCount(array) == 0);
    radek_compat_CFArrayAppendValue(array, hello);
    radek_compat_CFArrayAppendValue(array, data);
    CHECK(radek_compat_CFArrayGetCount(array) == 2);
    // Appending retains, so the element survives the caller's own release.
    CHECK(radek_compat_CFGetRetainCount(hello) == 2);
    CHECK(radek_compat_CFArrayGetValueAtIndex(array, 0) == hello);
    CHECK(radek_compat_CFArrayGetValueAtIndex(array, 1) == data);
    CHECK(radek_compat_CFArrayGetValueAtIndex(array, 2) == nullptr);
    CHECK(radek_compat_CFArrayGetValueAtIndex(array, -1) == nullptr);

    radek_CFMutableDictionaryRef dictionary =
        radek_compat_CFDictionaryCreateMutable(allocator, 0, nullptr, nullptr);
    CHECK(dictionary != nullptr);
    CHECK(radek_compat_CFDictionaryGetCount(dictionary) == 0);
    radek_CFStringRef key = radek_compat_CFStringCreateWithCString(allocator, "greeting", RADEK_KCFSTRINGENCODINGUTF8);
    radek_compat_CFDictionarySetValue(dictionary, key, hello);
    CHECK(radek_compat_CFDictionaryGetCount(dictionary) == 1);
    CHECK(radek_compat_CFDictionaryGetValue(dictionary, key) == hello);
    // Replacing a key releases the previous value and keeps the count at one.
    radek_compat_CFDictionarySetValue(dictionary, key, data);
    CHECK(radek_compat_CFDictionaryGetCount(dictionary) == 1);
    CHECK(radek_compat_CFDictionaryGetValue(dictionary, key) == data);
    // The dictionary released the replaced value; the array still holds `hello`.
    CHECK(radek_compat_CFGetRetainCount(hello) == 2);
    CHECK(radek_compat_CFDictionaryGetValue(dictionary, other) == nullptr);

    radek_CFNumberRef numberObject = nullptr;
    int32_t fortyTwo = 42;
    numberObject = radek_compat_CFNumberCreate(allocator, RADEK_KCFNUMBERSINT32TYPE, &fortyTwo);
    CHECK(numberObject != nullptr);
    double asDouble = 0.0;
    CHECK(radek_compat_CFNumberGetValue(numberObject, RADEK_KCFNUMBERDOUBLETYPE, &asDouble) == 1);
    CHECK(std::abs(asDouble - 42.0) < 1e-9);
    int64_t asInt64 = 0;
    CHECK(radek_compat_CFNumberGetValue(numberObject, RADEK_KCFNUMBERSINT64TYPE, &asInt64) == 1);
    CHECK(asInt64 == 42);
    float asFloat = 0.0f;
    CHECK(radek_compat_CFNumberGetValue(numberObject, RADEK_KCFNUMBERFLOAT32TYPE, &asFloat) == 1);
    CHECK(std::abs(asFloat - 42.0f) < 1e-3f);
    CHECK(radek_compat_CFNumberGetValue(numberObject, 999u, &asDouble) == 0);
    CHECK(radek_compat_CFNumberCreate(allocator, 999u, &fortyTwo) == nullptr);

    radek_CFDateRef epoch = radek_compat_CFDateCreate(allocator, 0.0);
    radek_CFDateRef later = radek_compat_CFDateCreate(allocator, 3600.0);
    CHECK(radek_compat_CFDateGetAbsoluteTime(epoch) == 0.0);
    CHECK(std::abs(radek_compat_CFDateGetTimeIntervalSinceDate(later, epoch) - 3600.0) < 1e-9);
    CHECK(std::abs(radek_compat_CFDateGetTimeIntervalSinceDate(epoch, later) + 3600.0) < 1e-9);

    // CFAbsoluteTime 0 is 2001-01-01 00:00:00 UTC.
    const radek_CFGregorianDate gregorian = radek_compat_CFAbsoluteTimeGetGregorianDate(0.0, nullptr);
    CHECK(gregorian.year == 2001);
    CHECK(gregorian.month == 1);
    CHECK(gregorian.day == 1);
    CHECK(gregorian.hour == 0);
    CHECK(gregorian.minute == 0);
    CHECK(std::abs(gregorian.second) < 1e-6);

    // Everything created above is released exactly once; containers release
    // their elements when they die, which is why `hello` drops back to 1 above.
    radek_compat_CFRelease(array);
    radek_compat_CFRelease(dictionary);
    radek_compat_CFRelease(key);
    radek_compat_CFRelease(hello);
    radek_compat_CFRelease(other);
    radek_compat_CFRelease(data);
    radek_compat_CFRelease(empty);
    radek_compat_CFRelease(numberObject);
    radek_compat_CFRelease(epoch);
    radek_compat_CFRelease(later);
}

void testExpandedGameAndFrameworkShims() {
    // CoreFoundation expanded bundle/URL/boolean/equality shims
    CHECK(radek_compat_CFBooleanGetValue(radek_compat_CFBooleanTrue()) == 1);
    CHECK(radek_compat_CFBooleanGetValue(radek_compat_CFBooleanFalse()) == 0);
    radek_CFTypeRef mainBundle = radek_compat_CFBundleGetMainBundle();
    CHECK(mainBundle != nullptr);
    CHECK(radek_compat_CFBundleGetIdentifier(mainBundle) != nullptr);
    radek_CFTypeRef bundleUrl = radek_compat_CFBundleCopyBundleURL(mainBundle);
    CHECK(bundleUrl != nullptr);
    uint8_t urlPath[64] = {};
    CHECK(radek_compat_CFURLGetFileSystemRepresentation(bundleUrl, 1, urlPath, sizeof(urlPath)) == 1);
    CHECK(std::string(reinterpret_cast<char *>(urlPath)) == "/bundle");
    radek_CFStringRef copiedPath = radek_compat_CFURLCopyFileSystemPath(bundleUrl, 0);
    CHECK(copiedPath != nullptr);
    CHECK(radek_compat_CFEqual(bundleUrl, copiedPath) == 1);
    CHECK(radek_compat_CFHash(copiedPath) != 0);
    radek_compat_CFRelease(copiedPath);
    radek_compat_CFRelease(bundleUrl);

    // Compiler-rt 64-bit / 32-bit integer & float helpers and C++ ABI allocation
    CHECK(radek_compat___divdi3(100, -4) == -25);
    CHECK(radek_compat___divdi3(100, 0) == 0);
    CHECK(radek_compat___moddi3(103, 10) == 3);
    CHECK(radek_compat___udivdi3(100u, 4u) == 25u);
    CHECK(radek_compat___umoddi3(103u, 10u) == 3u);
    CHECK(radek_compat___divsi3(-42, 6) == -7);
    CHECK(radek_compat___modsi3(-43, 6) == -1);
    CHECK(radek_compat___udivsi3(42u, 6u) == 7u);
    CHECK(radek_compat___umodsi3(43u, 6u) == 1u);
    CHECK(radek_compat___fixdfdi(1234.75) == 1234);
    CHECK(std::abs(radek_compat___floatdidf(1234) - 1234.0) < 1e-9);
    CHECK(std::abs(radek_compat___floatdisf(1234) - 1234.0f) < 1e-5f);
    void *cppMem = radek_compat__Znwm(64);
    CHECK(cppMem != nullptr);
    radek_compat__ZdlPv(cppMem);
    void *cppArr = radek_compat__Znam(128);
    CHECK(cppArr != nullptr);
    radek_compat__ZdaPv(cppArr);
    CHECK(radek_compat___error() != nullptr);

    // OpenGL ES and OpenAL stateful shims
    unsigned int tex[2] = {0, 0};
    radek_compat_glGenTextures(2, tex);
    CHECK(tex[0] != 0 && tex[1] != 0 && tex[0] != tex[1]);
    radek_compat_glBindTexture(0x0DE1u, tex[0]);
    radek_compat_glFrontFace(0x0900u);
    radek_compat_glDrawArrays(4u, 0, 4);
    radek_compat_glDrawElements(4u, 6, 0x1403u, nullptr);
    CHECK(radek_compat_glCheckFramebufferStatusOES(0x8D40u) == 0x8CD5u);
    CHECK(radek_compat_glCheckFramebufferStatus(0x8D40u) == 0x8CD5u);
    void *alDev = radek_compat_alcOpenDevice(nullptr);
    CHECK(alDev != nullptr);
    void *alCtx = radek_compat_alcCreateContext(alDev, nullptr);
    CHECK(alCtx != nullptr);
    CHECK(radek_compat_alcMakeContextCurrent(alCtx) == 1);
    unsigned int alBuf = 0, alSrc = 0;
    radek_compat_alGenBuffers(1, &alBuf);
    radek_compat_alGenSources(1, &alSrc);
    CHECK(alBuf != 0 && alSrc != 0);

    // Verify every expanded shim function address is linked and non-null
    const void *const kExpandedShimAddresses[] = {
        reinterpret_cast<const void *>(&radek_compat_CFConstantStringClassReference),
        reinterpret_cast<const void *>(&radek_compat_CFAllocatorDefault),
        reinterpret_cast<const void *>(&radek_compat_CFBooleanTrue),
        reinterpret_cast<const void *>(&radek_compat_CFBooleanFalse),
        reinterpret_cast<const void *>(&radek_compat_CFTypeArrayCallBacks),
        reinterpret_cast<const void *>(&radek_compat_CFTypeDictionaryKeyCallBacks),
        reinterpret_cast<const void *>(&radek_compat_CFTypeDictionaryValueCallBacks),
        reinterpret_cast<const void *>(&radek_compat_CFRunLoopDefaultMode),
        reinterpret_cast<const void *>(&radek_compat_CFRunLoopCommonModes),
        reinterpret_cast<const void *>(&radek_compat_CFBundleGetMainBundle),
        reinterpret_cast<const void *>(&radek_compat_CFBundleCopyBundleURL),
        reinterpret_cast<const void *>(&radek_compat_CFBundleCopyResourcesDirectoryURL),
        reinterpret_cast<const void *>(&radek_compat_CFBundleCopyResourceURL),
        reinterpret_cast<const void *>(&radek_compat_CFBundleGetIdentifier),
        reinterpret_cast<const void *>(&radek_compat_CFBundleGetValueForInfoDictionaryKey),
        reinterpret_cast<const void *>(&radek_compat_CFURLCreateWithFileSystemPath),
        reinterpret_cast<const void *>(&radek_compat_CFURLCreateFromFileSystemRepresentation),
        reinterpret_cast<const void *>(&radek_compat_CFURLGetFileSystemRepresentation),
        reinterpret_cast<const void *>(&radek_compat_CFURLCopyFileSystemPath),
        reinterpret_cast<const void *>(&radek_compat_CFStringCreateWithBytes),
        reinterpret_cast<const void *>(&radek_compat_CFStringCreateMutable),
        reinterpret_cast<const void *>(&radek_compat_CFStringAppendCString),
        reinterpret_cast<const void *>(&radek_compat_CFStringHasPrefix),
        reinterpret_cast<const void *>(&radek_compat_CFStringHasSuffix),
        reinterpret_cast<const void *>(&radek_compat_CFStringGetIntValue),
        reinterpret_cast<const void *>(&radek_compat_CFStringGetDoubleValue),
        reinterpret_cast<const void *>(&radek_compat_CFArrayCreate),
        reinterpret_cast<const void *>(&radek_compat_CFArrayRemoveValueAtIndex),
        reinterpret_cast<const void *>(&radek_compat_CFArrayRemoveAllValues),
        reinterpret_cast<const void *>(&radek_compat_CFDictionaryCreate),
        reinterpret_cast<const void *>(&radek_compat_CFDictionaryRemoveValue),
        reinterpret_cast<const void *>(&radek_compat_CFDictionaryRemoveAllValues),
        reinterpret_cast<const void *>(&radek_compat_CFDictionaryContainsKey),
        reinterpret_cast<const void *>(&radek_compat_CFDataCreateMutable),
        reinterpret_cast<const void *>(&radek_compat_CFDataAppendBytes),
        reinterpret_cast<const void *>(&radek_compat_CFDataGetMutableBytePtr),
        reinterpret_cast<const void *>(&radek_compat_CFDataGetBytes),
        reinterpret_cast<const void *>(&radek_compat_CFBooleanGetValue),
        reinterpret_cast<const void *>(&radek_compat_CFEqual),
        reinterpret_cast<const void *>(&radek_compat_CFHash),
        reinterpret_cast<const void *>(&radek_compat_CFGetTypeID),
        reinterpret_cast<const void *>(&radek_compat_CFPreferencesCopyAppValue),
        reinterpret_cast<const void *>(&radek_compat_CFPreferencesSetAppValue),
        reinterpret_cast<const void *>(&radek_compat_CFPreferencesAppSynchronize),
        reinterpret_cast<const void *>(&radek_compat_CFUUIDCreate),
        reinterpret_cast<const void *>(&radek_compat_CFUUIDCreateString),
        reinterpret_cast<const void *>(&radek_compat_CFLocaleCopyCurrent),
        reinterpret_cast<const void *>(&radek_compat_CFLocaleCopyPreferredLanguages),
        reinterpret_cast<const void *>(&radek_compat_CFLocaleGetIdentifier),
        reinterpret_cast<const void *>(&radek_compat_CFTimeZoneCopySystem),
        reinterpret_cast<const void *>(&radek_compat_AudioSessionInitialize),
        reinterpret_cast<const void *>(&radek_compat_AudioSessionSetActive),
        reinterpret_cast<const void *>(&radek_compat_NSSearchPathForDirectoriesInDomains),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___CAEAGLLayer),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___EAGLContext),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSAutoreleasePool),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSBundle),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSDictionary),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSNumber),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSObject),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSString),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSThread),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSURL),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIAccelerometer),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIApplication),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIScreen),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIView),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIWindow),
        reinterpret_cast<const void *>(&radek_compat_OBJC_METACLASS___NSObject),
        reinterpret_cast<const void *>(&radek_compat_OBJC_METACLASS___UIView),
        reinterpret_cast<const void *>(&radek_compat_UIApplicationMain),
        reinterpret_cast<const void *>(&radek_compat__DefaultRuneLocale),
        reinterpret_cast<const void *>(&radek_compat__Unwind_SjLj_Register),
        reinterpret_cast<const void *>(&radek_compat__Unwind_SjLj_Resume),
        reinterpret_cast<const void *>(&radek_compat__Unwind_SjLj_Unregister),
        reinterpret_cast<const void *>(&radek_compat__ZSt9terminatev),
        reinterpret_cast<const void *>(&radek_compat__ZTVN10__cxxabiv117__class_type_infoE),
        reinterpret_cast<const void *>(&radek_compat__ZTVN10__cxxabiv119__pointer_type_infoE),
        reinterpret_cast<const void *>(&radek_compat__ZTVN10__cxxabiv120__si_class_type_infoE),
        reinterpret_cast<const void *>(&radek_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE),
        reinterpret_cast<const void *>(&radek_compat__ZdaPv),
        reinterpret_cast<const void *>(&radek_compat__ZdlPv),
        reinterpret_cast<const void *>(&radek_compat__Znam),
        reinterpret_cast<const void *>(&radek_compat__Znwm),
        reinterpret_cast<const void *>(&radek_compat___cxa_allocate_exception),
        reinterpret_cast<const void *>(&radek_compat___cxa_atexit),
        reinterpret_cast<const void *>(&radek_compat___cxa_begin_catch),
        reinterpret_cast<const void *>(&radek_compat___cxa_end_catch),
        reinterpret_cast<const void *>(&radek_compat___cxa_pure_virtual),
        reinterpret_cast<const void *>(&radek_compat___cxa_throw),
        reinterpret_cast<const void *>(&radek_compat___divdi3),
        reinterpret_cast<const void *>(&radek_compat___divsi3),
        reinterpret_cast<const void *>(&radek_compat___error),
        reinterpret_cast<const void *>(&radek_compat___fixdfdi),
        reinterpret_cast<const void *>(&radek_compat___floatdidf),
        reinterpret_cast<const void *>(&radek_compat___floatdisf),
        reinterpret_cast<const void *>(&radek_compat___gxx_personality_sj0),
        reinterpret_cast<const void *>(&radek_compat___maskrune),
        reinterpret_cast<const void *>(&radek_compat___moddi3),
        reinterpret_cast<const void *>(&radek_compat___modsi3),
        reinterpret_cast<const void *>(&radek_compat___stderrp),
        reinterpret_cast<const void *>(&radek_compat___stdinp),
        reinterpret_cast<const void *>(&radek_compat___stdoutp),
        reinterpret_cast<const void *>(&radek_compat___tolower),
        reinterpret_cast<const void *>(&radek_compat___toupper),
        reinterpret_cast<const void *>(&radek_compat___udivsi3),
        reinterpret_cast<const void *>(&radek_compat___umodsi3),
        reinterpret_cast<const void *>(&radek_compat__objc_empty_cache),
        reinterpret_cast<const void *>(&radek_compat__objc_empty_vtable),
        reinterpret_cast<const void *>(&radek_compat_abort),
        reinterpret_cast<const void *>(&radek_compat_acosf),
        reinterpret_cast<const void *>(&radek_compat_alBufferData),
        reinterpret_cast<const void *>(&radek_compat_alDeleteBuffers),
        reinterpret_cast<const void *>(&radek_compat_alDeleteSources),
        reinterpret_cast<const void *>(&radek_compat_alGenBuffers),
        reinterpret_cast<const void *>(&radek_compat_alGenSources),
        reinterpret_cast<const void *>(&radek_compat_alGetSourcef),
        reinterpret_cast<const void *>(&radek_compat_alGetSourcei),
        reinterpret_cast<const void *>(&radek_compat_alSource3f),
        reinterpret_cast<const void *>(&radek_compat_alSourcePlay),
        reinterpret_cast<const void *>(&radek_compat_alSourceQueueBuffers),
        reinterpret_cast<const void *>(&radek_compat_alSourceStop),
        reinterpret_cast<const void *>(&radek_compat_alSourceUnqueueBuffers),
        reinterpret_cast<const void *>(&radek_compat_alSourcef),
        reinterpret_cast<const void *>(&radek_compat_alSourcei),
        reinterpret_cast<const void *>(&radek_compat_alcCloseDevice),
        reinterpret_cast<const void *>(&radek_compat_alcCreateContext),
        reinterpret_cast<const void *>(&radek_compat_alcDestroyContext),
        reinterpret_cast<const void *>(&radek_compat_alcMakeContextCurrent),
        reinterpret_cast<const void *>(&radek_compat_alcOpenDevice),
        reinterpret_cast<const void *>(&radek_compat_asinf),
        reinterpret_cast<const void *>(&radek_compat_atan2f),
        reinterpret_cast<const void *>(&radek_compat_atanf),
        reinterpret_cast<const void *>(&radek_compat_ceilf),
        reinterpret_cast<const void *>(&radek_compat_clearerr),
        reinterpret_cast<const void *>(&radek_compat_clock),
        reinterpret_cast<const void *>(&radek_compat_close),
        reinterpret_cast<const void *>(&radek_compat_cosf),
        reinterpret_cast<const void *>(&radek_compat_coshf),
        reinterpret_cast<const void *>(&radek_compat_difftime),
        reinterpret_cast<const void *>(&radek_compat_exit),
        reinterpret_cast<const void *>(&radek_compat_expf),
        reinterpret_cast<const void *>(&radek_compat_fcntl),
        reinterpret_cast<const void *>(&radek_compat_ferror),
        reinterpret_cast<const void *>(&radek_compat_floorf),
        reinterpret_cast<const void *>(&radek_compat_fputc),
        reinterpret_cast<const void *>(&radek_compat_freopen),
        reinterpret_cast<const void *>(&radek_compat_frexp),
        reinterpret_cast<const void *>(&radek_compat_fscanf),
        reinterpret_cast<const void *>(&radek_compat_getc),
        reinterpret_cast<const void *>(&radek_compat_glActiveTexture),
        reinterpret_cast<const void *>(&radek_compat_glBindBuffer),
        reinterpret_cast<const void *>(&radek_compat_glBindFramebufferOES),
        reinterpret_cast<const void *>(&radek_compat_glBindRenderbufferOES),
        reinterpret_cast<const void *>(&radek_compat_glBindTexture),
        reinterpret_cast<const void *>(&radek_compat_glBlendFunc),
        reinterpret_cast<const void *>(&radek_compat_glBufferData),
        reinterpret_cast<const void *>(&radek_compat_glCheckFramebufferStatusOES),
        reinterpret_cast<const void *>(&radek_compat_glClear),
        reinterpret_cast<const void *>(&radek_compat_glClearColor),
        reinterpret_cast<const void *>(&radek_compat_glClientActiveTexture),
        reinterpret_cast<const void *>(&radek_compat_glColor4f),
        reinterpret_cast<const void *>(&radek_compat_glColorPointer),
        reinterpret_cast<const void *>(&radek_compat_glCompressedTexImage2D),
        reinterpret_cast<const void *>(&radek_compat_glDeleteBuffers),
        reinterpret_cast<const void *>(&radek_compat_glDeleteFramebuffersOES),
        reinterpret_cast<const void *>(&radek_compat_glDeleteRenderbuffersOES),
        reinterpret_cast<const void *>(&radek_compat_glDeleteTextures),
        reinterpret_cast<const void *>(&radek_compat_glDepthFunc),
        reinterpret_cast<const void *>(&radek_compat_glDepthMask),
        reinterpret_cast<const void *>(&radek_compat_glDisable),
        reinterpret_cast<const void *>(&radek_compat_glDisableClientState),
        reinterpret_cast<const void *>(&radek_compat_glDrawArrays),
        reinterpret_cast<const void *>(&radek_compat_glDrawElements),
        reinterpret_cast<const void *>(&radek_compat_glEnable),
        reinterpret_cast<const void *>(&radek_compat_glEnableClientState),
        reinterpret_cast<const void *>(&radek_compat_glFramebufferRenderbufferOES),
        reinterpret_cast<const void *>(&radek_compat_glFramebufferTexture2DOES),
        reinterpret_cast<const void *>(&radek_compat_glFrontFace),
        reinterpret_cast<const void *>(&radek_compat_glGenBuffers),
        reinterpret_cast<const void *>(&radek_compat_glGenFramebuffersOES),
        reinterpret_cast<const void *>(&radek_compat_glGenRenderbuffersOES),
        reinterpret_cast<const void *>(&radek_compat_glGenTextures),
        reinterpret_cast<const void *>(&radek_compat_glGetIntegerv),
        reinterpret_cast<const void *>(&radek_compat_glGetRenderbufferParameterivOES),
        reinterpret_cast<const void *>(&radek_compat_glLightfv),
        reinterpret_cast<const void *>(&radek_compat_glLineWidth),
        reinterpret_cast<const void *>(&radek_compat_glLoadMatrixf),
        reinterpret_cast<const void *>(&radek_compat_glMaterialfv),
        reinterpret_cast<const void *>(&radek_compat_glMatrixMode),
        reinterpret_cast<const void *>(&radek_compat_glNormalPointer),
        reinterpret_cast<const void *>(&radek_compat_glPixelStorei),
        reinterpret_cast<const void *>(&radek_compat_glRenderbufferStorageOES),
        reinterpret_cast<const void *>(&radek_compat_glScissor),
        reinterpret_cast<const void *>(&radek_compat_glTexCoordPointer),
        reinterpret_cast<const void *>(&radek_compat_glTexEnvi),
        reinterpret_cast<const void *>(&radek_compat_glTexImage2D),
        reinterpret_cast<const void *>(&radek_compat_glTexParameteri),
        reinterpret_cast<const void *>(&radek_compat_glTexSubImage2D),
        reinterpret_cast<const void *>(&radek_compat_glVertexPointer),
        reinterpret_cast<const void *>(&radek_compat_glViewport),
        reinterpret_cast<const void *>(&radek_compat_gmtime),
        reinterpret_cast<const void *>(&radek_compat_kEAGLColorFormatRGB565),
        reinterpret_cast<const void *>(&radek_compat_kEAGLColorFormatRGBA8),
        reinterpret_cast<const void *>(&radek_compat_kEAGLDrawablePropertyColorFormat),
        reinterpret_cast<const void *>(&radek_compat_kEAGLDrawablePropertyRetainedBacking),
        reinterpret_cast<const void *>(&radek_compat_ldexp),
        reinterpret_cast<const void *>(&radek_compat_localeconv),
        reinterpret_cast<const void *>(&radek_compat_localtime),
        reinterpret_cast<const void *>(&radek_compat_log10f),
        reinterpret_cast<const void *>(&radek_compat_logf),
        reinterpret_cast<const void *>(&radek_compat_longjmp),
        reinterpret_cast<const void *>(&radek_compat_lseek),
        reinterpret_cast<const void *>(&radek_compat_modf),
        reinterpret_cast<const void *>(&radek_compat_objc_enumerationMutation),
        reinterpret_cast<const void *>(&radek_compat_objc_msgSend),
        reinterpret_cast<const void *>(&radek_compat_objc_msgSendSuper2),
        reinterpret_cast<const void *>(&radek_compat_objc_msgSend_stret),
        reinterpret_cast<const void *>(&radek_compat_objc_setProperty),
        reinterpret_cast<const void *>(&radek_compat_pthread_create),
        reinterpret_cast<const void *>(&radek_compat_pthread_exit),
        reinterpret_cast<const void *>(&radek_compat_pthread_getschedparam),
        reinterpret_cast<const void *>(&radek_compat_pthread_join),
        reinterpret_cast<const void *>(&radek_compat_pthread_mutex_trylock),
        reinterpret_cast<const void *>(&radek_compat_pthread_mutexattr_destroy),
        reinterpret_cast<const void *>(&radek_compat_pthread_mutexattr_init),
        reinterpret_cast<const void *>(&radek_compat_pthread_mutexattr_settype),
        reinterpret_cast<const void *>(&radek_compat_pthread_setschedparam),
        reinterpret_cast<const void *>(&radek_compat_read),
        reinterpret_cast<const void *>(&radek_compat_rename),
        reinterpret_cast<const void *>(&radek_compat_sched_yield),
        reinterpret_cast<const void *>(&radek_compat_select),
        reinterpret_cast<const void *>(&radek_compat_setjmp),
        reinterpret_cast<const void *>(&radek_compat_setlocale),
        reinterpret_cast<const void *>(&radek_compat_setvbuf),
        reinterpret_cast<const void *>(&radek_compat_sinf),
        reinterpret_cast<const void *>(&radek_compat_sinhf),
        reinterpret_cast<const void *>(&radek_compat_sprintf),
        reinterpret_cast<const void *>(&radek_compat_strcasecmp),
        reinterpret_cast<const void *>(&radek_compat_strcat),
        reinterpret_cast<const void *>(&radek_compat_strcoll),
        reinterpret_cast<const void *>(&radek_compat_strcspn),
        reinterpret_cast<const void *>(&radek_compat_strftime),
        reinterpret_cast<const void *>(&radek_compat_strncat),
        reinterpret_cast<const void *>(&radek_compat_strpbrk),
        reinterpret_cast<const void *>(&radek_compat_strtok),
        reinterpret_cast<const void *>(&radek_compat_strtoul),
        reinterpret_cast<const void *>(&radek_compat_system),
        reinterpret_cast<const void *>(&radek_compat_tanf),
        reinterpret_cast<const void *>(&radek_compat_tanhf),
        reinterpret_cast<const void *>(&radek_compat_tmpfile),
        reinterpret_cast<const void *>(&radek_compat_tmpnam),
        reinterpret_cast<const void *>(&radek_compat_ungetc),
        reinterpret_cast<const void *>(&radek_compat_usleep),
        reinterpret_cast<const void *>(&radek_compat_vsprintf),
        reinterpret_cast<const void *>(&radek_compat__exit),
        reinterpret_cast<const void *>(&radek_compat_atexit),
        reinterpret_cast<const void *>(&radek_compat_sscanf),
        reinterpret_cast<const void *>(&radek_compat_putchar),
        reinterpret_cast<const void *>(&radek_compat_getchar),
        reinterpret_cast<const void *>(&radek_compat_fgetc),
        reinterpret_cast<const void *>(&radek_compat_putc),
        reinterpret_cast<const void *>(&radek_compat_rewind),
        reinterpret_cast<const void *>(&radek_compat_fileno),
        reinterpret_cast<const void *>(&radek_compat_fdopen),
        reinterpret_cast<const void *>(&radek_compat_perror),
        reinterpret_cast<const void *>(&radek_compat_tzset),
        reinterpret_cast<const void *>(&radek_compat_sleep),
        reinterpret_cast<const void *>(&radek_compat_open),
        reinterpret_cast<const void *>(&radek_compat_write),
        reinterpret_cast<const void *>(&radek_compat_unlink),
        reinterpret_cast<const void *>(&radek_compat_mkdir),
        reinterpret_cast<const void *>(&radek_compat_rmdir),
        reinterpret_cast<const void *>(&radek_compat_access),
        reinterpret_cast<const void *>(&radek_compat_getcwd),
        reinterpret_cast<const void *>(&radek_compat_chdir),
        reinterpret_cast<const void *>(&radek_compat_stat),
        reinterpret_cast<const void *>(&radek_compat_fstat),
        reinterpret_cast<const void *>(&radek_compat_lstat),
        reinterpret_cast<const void *>(&radek_compat_opendir),
        reinterpret_cast<const void *>(&radek_compat_readdir),
        reinterpret_cast<const void *>(&radek_compat_closedir),
        reinterpret_cast<const void *>(&radek_compat_mmap),
        reinterpret_cast<const void *>(&radek_compat_munmap),
        reinterpret_cast<const void *>(&radek_compat_mprotect),
        reinterpret_cast<const void *>(&radek_compat_poll),
        reinterpret_cast<const void *>(&radek_compat_pipe),
        reinterpret_cast<const void *>(&radek_compat_dup),
        reinterpret_cast<const void *>(&radek_compat_dup2),
        reinterpret_cast<const void *>(&radek_compat_fsync),
        reinterpret_cast<const void *>(&radek_compat_ftruncate),
        reinterpret_cast<const void *>(&radek_compat_truncate),
        reinterpret_cast<const void *>(&radek_compat_chmod),
        reinterpret_cast<const void *>(&radek_compat_umask),
        reinterpret_cast<const void *>(&radek_compat_getuid),
        reinterpret_cast<const void *>(&radek_compat_geteuid),
        reinterpret_cast<const void *>(&radek_compat_getgid),
        reinterpret_cast<const void *>(&radek_compat_getegid),
        reinterpret_cast<const void *>(&radek_compat_getppid),
        reinterpret_cast<const void *>(&radek_compat_sysconf),
        reinterpret_cast<const void *>(&radek_compat_sysctl),
        reinterpret_cast<const void *>(&radek_compat_sysctlbyname),
        reinterpret_cast<const void *>(&radek_compat_getpagesize),
        reinterpret_cast<const void *>(&radek_compat__setjmp),
        reinterpret_cast<const void *>(&radek_compat__longjmp),
        reinterpret_cast<const void *>(&radek_compat_sigaction),
        reinterpret_cast<const void *>(&radek_compat_signal),
        reinterpret_cast<const void *>(&radek_compat_raise),
        reinterpret_cast<const void *>(&radek_compat_kill),
        reinterpret_cast<const void *>(&radek_compat_tolower),
        reinterpret_cast<const void *>(&radek_compat_toupper),
        reinterpret_cast<const void *>(&radek_compat_isalpha),
        reinterpret_cast<const void *>(&radek_compat_isdigit),
        reinterpret_cast<const void *>(&radek_compat_isalnum),
        reinterpret_cast<const void *>(&radek_compat_isspace),
        reinterpret_cast<const void *>(&radek_compat_isupper),
        reinterpret_cast<const void *>(&radek_compat_islower),
        reinterpret_cast<const void *>(&radek_compat_isxdigit),
        reinterpret_cast<const void *>(&radek_compat_strncasecmp),
        reinterpret_cast<const void *>(&radek_compat_strspn),
        reinterpret_cast<const void *>(&radek_compat_strtok_r),
        reinterpret_cast<const void *>(&radek_compat_strtoll),
        reinterpret_cast<const void *>(&radek_compat_strtoull),
        reinterpret_cast<const void *>(&radek_compat_strtof),
        reinterpret_cast<const void *>(&radek_compat_atol),
        reinterpret_cast<const void *>(&radek_compat_atoll),
        reinterpret_cast<const void *>(&radek_compat_llabs),
        reinterpret_cast<const void *>(&radek_compat_bzero),
        reinterpret_cast<const void *>(&radek_compat_bcopy),
        reinterpret_cast<const void *>(&radek_compat_bcmp),
        reinterpret_cast<const void *>(&radek_compat_acos),
        reinterpret_cast<const void *>(&radek_compat_asin),
        reinterpret_cast<const void *>(&radek_compat_atan),
        reinterpret_cast<const void *>(&radek_compat_cosh),
        reinterpret_cast<const void *>(&radek_compat_sinh),
        reinterpret_cast<const void *>(&radek_compat_tanh),
        reinterpret_cast<const void *>(&radek_compat_exp),
        reinterpret_cast<const void *>(&radek_compat_log),
        reinterpret_cast<const void *>(&radek_compat_log10),
        reinterpret_cast<const void *>(&radek_compat_log2),
        reinterpret_cast<const void *>(&radek_compat_hypot),
        reinterpret_cast<const void *>(&radek_compat_hypotf),
        reinterpret_cast<const void *>(&radek_compat_cbrt),
        reinterpret_cast<const void *>(&radek_compat_round),
        reinterpret_cast<const void *>(&radek_compat_roundf),
        reinterpret_cast<const void *>(&radek_compat_trunc),
        reinterpret_cast<const void *>(&radek_compat_truncf),
        reinterpret_cast<const void *>(&radek_compat_lround),
        reinterpret_cast<const void *>(&radek_compat_lroundf),
        reinterpret_cast<const void *>(&radek_compat_frexpf),
        reinterpret_cast<const void *>(&radek_compat_ldexpf),
        reinterpret_cast<const void *>(&radek_compat_log2f),
        reinterpret_cast<const void *>(&radek_compat_modff),
        reinterpret_cast<const void *>(&radek_compat_powf),
        reinterpret_cast<const void *>(&radek_compat_sqrtf),
        reinterpret_cast<const void *>(&radek_compat_fabsf),
        reinterpret_cast<const void *>(&radek_compat_fmodf),
        reinterpret_cast<const void *>(&radek_compat_pthread_detach),
        reinterpret_cast<const void *>(&radek_compat_pthread_equal),
        reinterpret_cast<const void *>(&radek_compat_pthread_once),
        reinterpret_cast<const void *>(&radek_compat_pthread_cond_timedwait),
        reinterpret_cast<const void *>(&radek_compat_pthread_key_create),
        reinterpret_cast<const void *>(&radek_compat_pthread_key_delete),
        reinterpret_cast<const void *>(&radek_compat_pthread_setspecific),
        reinterpret_cast<const void *>(&radek_compat_pthread_getspecific),
        reinterpret_cast<const void *>(&radek_compat_pthread_rwlock_init),
        reinterpret_cast<const void *>(&radek_compat_pthread_rwlock_rdlock),
        reinterpret_cast<const void *>(&radek_compat_pthread_rwlock_wrlock),
        reinterpret_cast<const void *>(&radek_compat_pthread_rwlock_unlock),
        reinterpret_cast<const void *>(&radek_compat_pthread_rwlock_destroy),
        reinterpret_cast<const void *>(&radek_compat_sem_init),
        reinterpret_cast<const void *>(&radek_compat_sem_destroy),
        reinterpret_cast<const void *>(&radek_compat_sem_wait),
        reinterpret_cast<const void *>(&radek_compat_sem_trywait),
        reinterpret_cast<const void *>(&radek_compat_sem_post),
        reinterpret_cast<const void *>(&radek_compat_dlopen),
        reinterpret_cast<const void *>(&radek_compat_dlsym),
        reinterpret_cast<const void *>(&radek_compat_dlclose),
        reinterpret_cast<const void *>(&radek_compat_dlerror),
        reinterpret_cast<const void *>(&radek_compat_socket),
        reinterpret_cast<const void *>(&radek_compat_connect),
        reinterpret_cast<const void *>(&radek_compat_bind),
        reinterpret_cast<const void *>(&radek_compat_listen),
        reinterpret_cast<const void *>(&radek_compat_accept),
        reinterpret_cast<const void *>(&radek_compat_send),
        reinterpret_cast<const void *>(&radek_compat_sendto),
        reinterpret_cast<const void *>(&radek_compat_recv),
        reinterpret_cast<const void *>(&radek_compat_recvfrom),
        reinterpret_cast<const void *>(&radek_compat_setsockopt),
        reinterpret_cast<const void *>(&radek_compat_getsockopt),
        reinterpret_cast<const void *>(&radek_compat_getsockname),
        reinterpret_cast<const void *>(&radek_compat_getpeername),
        reinterpret_cast<const void *>(&radek_compat_shutdown),
        reinterpret_cast<const void *>(&radek_compat_getaddrinfo),
        reinterpret_cast<const void *>(&radek_compat_freeaddrinfo),
        reinterpret_cast<const void *>(&radek_compat_gethostbyname),
        reinterpret_cast<const void *>(&radek_compat_inet_ntop),
        reinterpret_cast<const void *>(&radek_compat_inet_pton),
        reinterpret_cast<const void *>(&radek_compat_inet_addr),
        reinterpret_cast<const void *>(&radek_compat_inet_ntoa),
        reinterpret_cast<const void *>(&radek_compat_htons),
        reinterpret_cast<const void *>(&radek_compat_htonl),
        reinterpret_cast<const void *>(&radek_compat_ntohs),
        reinterpret_cast<const void *>(&radek_compat_ntohl),
        reinterpret_cast<const void *>(&radek_compat_crc32),
        reinterpret_cast<const void *>(&radek_compat_adler32),
        reinterpret_cast<const void *>(&radek_compat_compress),
        reinterpret_cast<const void *>(&radek_compat_compress2),
        reinterpret_cast<const void *>(&radek_compat_uncompress),
        reinterpret_cast<const void *>(&radek_compat_deflateInit_),
        reinterpret_cast<const void *>(&radek_compat_deflateInit2_),
        reinterpret_cast<const void *>(&radek_compat_deflate),
        reinterpret_cast<const void *>(&radek_compat_deflateEnd),
        reinterpret_cast<const void *>(&radek_compat_deflateReset),
        reinterpret_cast<const void *>(&radek_compat_inflateInit_),
        reinterpret_cast<const void *>(&radek_compat_inflateInit2_),
        reinterpret_cast<const void *>(&radek_compat_inflate),
        reinterpret_cast<const void *>(&radek_compat_inflateEnd),
        reinterpret_cast<const void *>(&radek_compat_inflateReset),
        reinterpret_cast<const void *>(&radek_compat_gzopen),
        reinterpret_cast<const void *>(&radek_compat_gzread),
        reinterpret_cast<const void *>(&radek_compat_gzwrite),
        reinterpret_cast<const void *>(&radek_compat_gzclose),
        reinterpret_cast<const void *>(&radek_compat_alDistanceModel),
        reinterpret_cast<const void *>(&radek_compat_alDopplerFactor),
        reinterpret_cast<const void *>(&radek_compat_alDopplerVelocity),
        reinterpret_cast<const void *>(&radek_compat_alSpeedOfSound),
        reinterpret_cast<const void *>(&radek_compat_alGetError),
        reinterpret_cast<const void *>(&radek_compat_alGetSource3f),
        reinterpret_cast<const void *>(&radek_compat_alGetSourcefv),
        reinterpret_cast<const void *>(&radek_compat_alSourcefv),
        reinterpret_cast<const void *>(&radek_compat_alSourcePause),
        reinterpret_cast<const void *>(&radek_compat_alSourceRewind),
        reinterpret_cast<const void *>(&radek_compat_alListener3f),
        reinterpret_cast<const void *>(&radek_compat_alListenerf),
        reinterpret_cast<const void *>(&radek_compat_alListenerfv),
        reinterpret_cast<const void *>(&radek_compat_alListeneri),
        reinterpret_cast<const void *>(&radek_compat_alGetListenerf),
        reinterpret_cast<const void *>(&radek_compat_alGetListener3f),
        reinterpret_cast<const void *>(&radek_compat_alGetListenerfv),
        reinterpret_cast<const void *>(&radek_compat_alEnable),
        reinterpret_cast<const void *>(&radek_compat_alDisable),
        reinterpret_cast<const void *>(&radek_compat_alIsEnabled),
        reinterpret_cast<const void *>(&radek_compat_alIsBuffer),
        reinterpret_cast<const void *>(&radek_compat_alIsSource),
        reinterpret_cast<const void *>(&radek_compat_alGetBoolean),
        reinterpret_cast<const void *>(&radek_compat_alGetInteger),
        reinterpret_cast<const void *>(&radek_compat_alGetFloat),
        reinterpret_cast<const void *>(&radek_compat_alGetDouble),
        reinterpret_cast<const void *>(&radek_compat_alGetString),
        reinterpret_cast<const void *>(&radek_compat_alGetEnumValue),
        reinterpret_cast<const void *>(&radek_compat_alGetProcAddress),
        reinterpret_cast<const void *>(&radek_compat_alIsExtensionPresent),
        reinterpret_cast<const void *>(&radek_compat_alcGetContextsDevice),
        reinterpret_cast<const void *>(&radek_compat_alcGetCurrentContext),
        reinterpret_cast<const void *>(&radek_compat_alcProcessContext),
        reinterpret_cast<const void *>(&radek_compat_alcSuspendContext),
        reinterpret_cast<const void *>(&radek_compat_alcGetError),
        reinterpret_cast<const void *>(&radek_compat_alcGetIntegerv),
        reinterpret_cast<const void *>(&radek_compat_alcGetString),
        reinterpret_cast<const void *>(&radek_compat_alcIsExtensionPresent),
        reinterpret_cast<const void *>(&radek_compat_alcGetProcAddress),
        reinterpret_cast<const void *>(&radek_compat_AudioSessionSetActiveWithFlags),
        reinterpret_cast<const void *>(&radek_compat_AudioSessionGetProperty),
        reinterpret_cast<const void *>(&radek_compat_AudioSessionSetProperty),
        reinterpret_cast<const void *>(&radek_compat_AudioSessionGetPropertySize),
        reinterpret_cast<const void *>(&radek_compat_AudioSessionAddPropertyListener),
        reinterpret_cast<const void *>(&radek_compat_AudioSessionRemovePropertyListenerWithUserData),
        reinterpret_cast<const void *>(&radek_compat_AudioServicesPlaySystemSound),
        reinterpret_cast<const void *>(&radek_compat_AudioServicesPlayAlertSound),
        reinterpret_cast<const void *>(&radek_compat_AudioServicesCreateSystemSoundID),
        reinterpret_cast<const void *>(&radek_compat_AudioServicesDisposeSystemSoundID),
        reinterpret_cast<const void *>(&radek_compat_AudioFileOpenURL),
        reinterpret_cast<const void *>(&radek_compat_AudioFileClose),
        reinterpret_cast<const void *>(&radek_compat_AudioFileGetProperty),
        reinterpret_cast<const void *>(&radek_compat_AudioFileReadBytes),
        reinterpret_cast<const void *>(&radek_compat_AudioFileReadPackets),
        reinterpret_cast<const void *>(&radek_compat_ExtAudioFileOpenURL),
        reinterpret_cast<const void *>(&radek_compat_ExtAudioFileDispose),
        reinterpret_cast<const void *>(&radek_compat_ExtAudioFileGetProperty),
        reinterpret_cast<const void *>(&radek_compat_ExtAudioFileSetProperty),
        reinterpret_cast<const void *>(&radek_compat_ExtAudioFileRead),
        reinterpret_cast<const void *>(&radek_compat_ExtAudioFileSeek),
        reinterpret_cast<const void *>(&radek_compat_AudioQueueNewOutput),
        reinterpret_cast<const void *>(&radek_compat_AudioQueueAllocateBuffer),
        reinterpret_cast<const void *>(&radek_compat_AudioQueueFreeBuffer),
        reinterpret_cast<const void *>(&radek_compat_AudioQueueEnqueueBuffer),
        reinterpret_cast<const void *>(&radek_compat_AudioQueueStart),
        reinterpret_cast<const void *>(&radek_compat_AudioQueuePause),
        reinterpret_cast<const void *>(&radek_compat_AudioQueueStop),
        reinterpret_cast<const void *>(&radek_compat_AudioQueueDispose),
        reinterpret_cast<const void *>(&radek_compat_AudioQueueSetParameter),
        reinterpret_cast<const void *>(&radek_compat_AudioComponentFindNext),
        reinterpret_cast<const void *>(&radek_compat_AudioComponentInstanceNew),
        reinterpret_cast<const void *>(&radek_compat_AudioComponentInstanceDispose),
        reinterpret_cast<const void *>(&radek_compat_AudioUnitInitialize),
        reinterpret_cast<const void *>(&radek_compat_AudioUnitUninitialize),
        reinterpret_cast<const void *>(&radek_compat_AudioUnitSetProperty),
        reinterpret_cast<const void *>(&radek_compat_AudioUnitGetProperty),
        reinterpret_cast<const void *>(&radek_compat_AudioOutputUnitStart),
        reinterpret_cast<const void *>(&radek_compat_AudioOutputUnitStop),
        reinterpret_cast<const void *>(&radek_compat_AudioUnitRender),
        reinterpret_cast<const void *>(&radek_compat_glAlphaFunc),
        reinterpret_cast<const void *>(&radek_compat_glBindFramebuffer),
        reinterpret_cast<const void *>(&radek_compat_glBindRenderbuffer),
        reinterpret_cast<const void *>(&radek_compat_glBlendEquation),
        reinterpret_cast<const void *>(&radek_compat_glBlendEquationOES),
        reinterpret_cast<const void *>(&radek_compat_glBlendFuncSeparate),
        reinterpret_cast<const void *>(&radek_compat_glBufferSubData),
        reinterpret_cast<const void *>(&radek_compat_glCheckFramebufferStatus),
        reinterpret_cast<const void *>(&radek_compat_glClearDepthf),
        reinterpret_cast<const void *>(&radek_compat_glClearStencil),
        reinterpret_cast<const void *>(&radek_compat_glColor4ub),
        reinterpret_cast<const void *>(&radek_compat_glColorMask),
        reinterpret_cast<const void *>(&radek_compat_glCompileShader),
        reinterpret_cast<const void *>(&radek_compat_glCopyTexImage2D),
        reinterpret_cast<const void *>(&radek_compat_glCopyTexSubImage2D),
        reinterpret_cast<const void *>(&radek_compat_glCreateProgram),
        reinterpret_cast<const void *>(&radek_compat_glCreateShader),
        reinterpret_cast<const void *>(&radek_compat_glCullFace),
        reinterpret_cast<const void *>(&radek_compat_glDeleteFramebuffers),
        reinterpret_cast<const void *>(&radek_compat_glDeleteProgram),
        reinterpret_cast<const void *>(&radek_compat_glDeleteRenderbuffers),
        reinterpret_cast<const void *>(&radek_compat_glDeleteShader),
        reinterpret_cast<const void *>(&radek_compat_glDepthRangef),
        reinterpret_cast<const void *>(&radek_compat_glDisableVertexAttribArray),
        reinterpret_cast<const void *>(&radek_compat_glEnableVertexAttribArray),
        reinterpret_cast<const void *>(&radek_compat_glFinish),
        reinterpret_cast<const void *>(&radek_compat_glFlush),
        reinterpret_cast<const void *>(&radek_compat_glFogf),
        reinterpret_cast<const void *>(&radek_compat_glFogfv),
        reinterpret_cast<const void *>(&radek_compat_glFramebufferRenderbuffer),
        reinterpret_cast<const void *>(&radek_compat_glFramebufferTexture2D),
        reinterpret_cast<const void *>(&radek_compat_glFrustumf),
        reinterpret_cast<const void *>(&radek_compat_glGenFramebuffers),
        reinterpret_cast<const void *>(&radek_compat_glGenRenderbuffers),
        reinterpret_cast<const void *>(&radek_compat_glGenerateMipmap),
        reinterpret_cast<const void *>(&radek_compat_glGenerateMipmapOES),
        reinterpret_cast<const void *>(&radek_compat_glGetAttribLocation),
        reinterpret_cast<const void *>(&radek_compat_glGetError),
        reinterpret_cast<const void *>(&radek_compat_glGetFloatv),
        reinterpret_cast<const void *>(&radek_compat_glGetProgramInfoLog),
        reinterpret_cast<const void *>(&radek_compat_glGetProgramiv),
        reinterpret_cast<const void *>(&radek_compat_glGetRenderbufferParameteriv),
        reinterpret_cast<const void *>(&radek_compat_glGetShaderInfoLog),
        reinterpret_cast<const void *>(&radek_compat_glGetShaderiv),
        reinterpret_cast<const void *>(&radek_compat_glGetString),
        reinterpret_cast<const void *>(&radek_compat_glGetUniformLocation),
        reinterpret_cast<const void *>(&radek_compat_glHint),
        reinterpret_cast<const void *>(&radek_compat_glIsEnabled),
        reinterpret_cast<const void *>(&radek_compat_glIsTexture),
        reinterpret_cast<const void *>(&radek_compat_glLightModelfv),
        reinterpret_cast<const void *>(&radek_compat_glLinkProgram),
        reinterpret_cast<const void *>(&radek_compat_glLoadIdentity),
        reinterpret_cast<const void *>(&radek_compat_glLogicOp),
        reinterpret_cast<const void *>(&radek_compat_glMaterialf),
        reinterpret_cast<const void *>(&radek_compat_glMultMatrixf),
        reinterpret_cast<const void *>(&radek_compat_glNormal3f),
        reinterpret_cast<const void *>(&radek_compat_glOrthof),
        reinterpret_cast<const void *>(&radek_compat_glPointParameterf),
        reinterpret_cast<const void *>(&radek_compat_glPointParameterfv),
        reinterpret_cast<const void *>(&radek_compat_glPointSize),
        reinterpret_cast<const void *>(&radek_compat_glPolygonOffset),
        reinterpret_cast<const void *>(&radek_compat_glPopMatrix),
        reinterpret_cast<const void *>(&radek_compat_glPushMatrix),
        reinterpret_cast<const void *>(&radek_compat_glReadPixels),
        reinterpret_cast<const void *>(&radek_compat_glRenderbufferStorage),
        reinterpret_cast<const void *>(&radek_compat_glRotatef),
        reinterpret_cast<const void *>(&radek_compat_glScalef),
        reinterpret_cast<const void *>(&radek_compat_glShadeModel),
        reinterpret_cast<const void *>(&radek_compat_glShaderSource),
        reinterpret_cast<const void *>(&radek_compat_glStencilFunc),
        reinterpret_cast<const void *>(&radek_compat_glStencilMask),
        reinterpret_cast<const void *>(&radek_compat_glStencilOp),
        reinterpret_cast<const void *>(&radek_compat_glTexEnvf),
        reinterpret_cast<const void *>(&radek_compat_glTexEnvfv),
        reinterpret_cast<const void *>(&radek_compat_glTexParameterf),
        reinterpret_cast<const void *>(&radek_compat_glTexParameterfv),
        reinterpret_cast<const void *>(&radek_compat_glTranslatef),
        reinterpret_cast<const void *>(&radek_compat_glUniform1f),
        reinterpret_cast<const void *>(&radek_compat_glUniform1i),
        reinterpret_cast<const void *>(&radek_compat_glUniform2f),
        reinterpret_cast<const void *>(&radek_compat_glUniform3f),
        reinterpret_cast<const void *>(&radek_compat_glUniform4f),
        reinterpret_cast<const void *>(&radek_compat_glUniformMatrix4fv),
        reinterpret_cast<const void *>(&radek_compat_glUseProgram),
        reinterpret_cast<const void *>(&radek_compat_glVertexAttribPointer),
        reinterpret_cast<const void *>(&radek_compat_eglGetDisplay),
        reinterpret_cast<const void *>(&radek_compat_eglInitialize),
        reinterpret_cast<const void *>(&radek_compat_eglChooseConfig),
        reinterpret_cast<const void *>(&radek_compat_eglCreateWindowSurface),
        reinterpret_cast<const void *>(&radek_compat_eglCreateContext),
        reinterpret_cast<const void *>(&radek_compat_eglMakeCurrent),
        reinterpret_cast<const void *>(&radek_compat_eglSwapBuffers),
        reinterpret_cast<const void *>(&radek_compat_eglDestroyContext),
        reinterpret_cast<const void *>(&radek_compat_eglDestroySurface),
        reinterpret_cast<const void *>(&radek_compat_eglTerminate),
        reinterpret_cast<const void *>(&radek_compat_eglGetError),
        reinterpret_cast<const void *>(&radek_compat_eglGetProcAddress),
        reinterpret_cast<const void *>(&radek_compat_CGColorSpaceCreateDeviceRGB),
        reinterpret_cast<const void *>(&radek_compat_CGColorSpaceCreateDeviceGray),
        reinterpret_cast<const void *>(&radek_compat_CGColorSpaceRelease),
        reinterpret_cast<const void *>(&radek_compat_CGColorSpaceRetain),
        reinterpret_cast<const void *>(&radek_compat_CGBitmapContextCreate),
        reinterpret_cast<const void *>(&radek_compat_CGBitmapContextGetData),
        reinterpret_cast<const void *>(&radek_compat_CGBitmapContextGetWidth),
        reinterpret_cast<const void *>(&radek_compat_CGBitmapContextGetHeight),
        reinterpret_cast<const void *>(&radek_compat_CGBitmapContextGetBytesPerRow),
        reinterpret_cast<const void *>(&radek_compat_CGBitmapContextCreateImage),
        reinterpret_cast<const void *>(&radek_compat_CGContextRelease),
        reinterpret_cast<const void *>(&radek_compat_CGContextRetain),
        reinterpret_cast<const void *>(&radek_compat_CGContextClearRect),
        reinterpret_cast<const void *>(&radek_compat_CGContextFillRect),
        reinterpret_cast<const void *>(&radek_compat_CGContextDrawImage),
        reinterpret_cast<const void *>(&radek_compat_CGContextTranslateCTM),
        reinterpret_cast<const void *>(&radek_compat_CGContextScaleCTM),
        reinterpret_cast<const void *>(&radek_compat_CGContextRotateCTM),
        reinterpret_cast<const void *>(&radek_compat_CGContextSaveGState),
        reinterpret_cast<const void *>(&radek_compat_CGContextRestoreGState),
        reinterpret_cast<const void *>(&radek_compat_CGContextSetRGBFillColor),
        reinterpret_cast<const void *>(&radek_compat_CGContextSetAlpha),
        reinterpret_cast<const void *>(&radek_compat_CGImageGetWidth),
        reinterpret_cast<const void *>(&radek_compat_CGImageGetHeight),
        reinterpret_cast<const void *>(&radek_compat_CGImageGetBitsPerComponent),
        reinterpret_cast<const void *>(&radek_compat_CGImageGetBitsPerPixel),
        reinterpret_cast<const void *>(&radek_compat_CGImageGetBytesPerRow),
        reinterpret_cast<const void *>(&radek_compat_CGImageGetAlphaInfo),
        reinterpret_cast<const void *>(&radek_compat_CGImageGetDataProvider),
        reinterpret_cast<const void *>(&radek_compat_CGImageGetColorSpace),
        reinterpret_cast<const void *>(&radek_compat_CGImageRelease),
        reinterpret_cast<const void *>(&radek_compat_CGImageRetain),
        reinterpret_cast<const void *>(&radek_compat_CGDataProviderCopyData),
        reinterpret_cast<const void *>(&radek_compat_CGDataProviderCreateWithData),
        reinterpret_cast<const void *>(&radek_compat_CGDataProviderRelease),
        reinterpret_cast<const void *>(&radek_compat_CGDataProviderRetain),
        reinterpret_cast<const void *>(&radek_compat_CGAffineTransformMake),
        reinterpret_cast<const void *>(&radek_compat_CGAffineTransformMakeTranslation),
        reinterpret_cast<const void *>(&radek_compat_CGAffineTransformMakeScale),
        reinterpret_cast<const void *>(&radek_compat_CGAffineTransformMakeRotation),
        reinterpret_cast<const void *>(&radek_compat_CGAffineTransformTranslate),
        reinterpret_cast<const void *>(&radek_compat_CGAffineTransformScale),
        reinterpret_cast<const void *>(&radek_compat_CGAffineTransformRotate),
        reinterpret_cast<const void *>(&radek_compat_CGAffineTransformConcat),
        reinterpret_cast<const void *>(&radek_compat_objc_msgSendSuper),
        reinterpret_cast<const void *>(&radek_compat_objc_msgSendSuper_stret),
        reinterpret_cast<const void *>(&radek_compat_objc_msgSendSuper2_stret),
        reinterpret_cast<const void *>(&radek_compat_objc_msgSend_fpret),
        reinterpret_cast<const void *>(&radek_compat_objc_getClass),
        reinterpret_cast<const void *>(&radek_compat_objc_lookUpClass),
        reinterpret_cast<const void *>(&radek_compat_objc_getMetaClass),
        reinterpret_cast<const void *>(&radek_compat_objc_getProtocol),
        reinterpret_cast<const void *>(&radek_compat_objc_allocateClassPair),
        reinterpret_cast<const void *>(&radek_compat_objc_registerClassPair),
        reinterpret_cast<const void *>(&radek_compat_objc_retain),
        reinterpret_cast<const void *>(&radek_compat_objc_release),
        reinterpret_cast<const void *>(&radek_compat_objc_autorelease),
        reinterpret_cast<const void *>(&radek_compat_objc_autoreleasePoolPush),
        reinterpret_cast<const void *>(&radek_compat_objc_autoreleasePoolPop),
        reinterpret_cast<const void *>(&radek_compat_objc_retainAutorelease),
        reinterpret_cast<const void *>(&radek_compat_objc_retainAutoreleaseReturnValue),
        reinterpret_cast<const void *>(&radek_compat_objc_retainAutoreleasedReturnValue),
        reinterpret_cast<const void *>(&radek_compat_objc_storeStrong),
        reinterpret_cast<const void *>(&radek_compat_objc_storeWeak),
        reinterpret_cast<const void *>(&radek_compat_objc_loadWeakRetained),
        reinterpret_cast<const void *>(&radek_compat_objc_destroyWeak),
        reinterpret_cast<const void *>(&radek_compat_objc_getProperty),
        reinterpret_cast<const void *>(&radek_compat_objc_copyStruct),
        reinterpret_cast<const void *>(&radek_compat_objc_sync_enter),
        reinterpret_cast<const void *>(&radek_compat_objc_sync_exit),
        reinterpret_cast<const void *>(&radek_compat_objc_exception_throw),
        reinterpret_cast<const void *>(&radek_compat_objc_begin_catch),
        reinterpret_cast<const void *>(&radek_compat_objc_end_catch),
        reinterpret_cast<const void *>(&radek_compat_sel_registerName),
        reinterpret_cast<const void *>(&radek_compat_sel_getUid),
        reinterpret_cast<const void *>(&radek_compat_sel_getName),
        reinterpret_cast<const void *>(&radek_compat_class_getName),
        reinterpret_cast<const void *>(&radek_compat_class_getSuperclass),
        reinterpret_cast<const void *>(&radek_compat_class_getInstanceMethod),
        reinterpret_cast<const void *>(&radek_compat_class_getClassMethod),
        reinterpret_cast<const void *>(&radek_compat_class_addMethod),
        reinterpret_cast<const void *>(&radek_compat_class_replaceMethod),
        reinterpret_cast<const void *>(&radek_compat_class_createInstance),
        reinterpret_cast<const void *>(&radek_compat_object_getClass),
        reinterpret_cast<const void *>(&radek_compat_object_getClassName),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___MPMoviePlayerController),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSDate),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSLocale),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSNotificationCenter),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSUserDefaults),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIColor),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIDevice),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIImage),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIViewController),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___AVAudioPlayer),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___AVAudioSession),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSArray),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSMutableArray),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSMutableDictionary),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSMutableString),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSData),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSMutableData),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSSet),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSMutableSet),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSFileManager),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSTimer),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSRunLoop),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSProcessInfo),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSValue),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___NSError),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIImageView),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UILabel),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIButton),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIScrollView),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIAlertView),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIActivityIndicatorView),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIWebView),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIFont),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UITouch),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___UIEvent),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___CALayer),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___CATransaction),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___CABasicAnimation),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___SKPaymentQueue),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___SKProductsRequest),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___GKLocalPlayer),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___CMMotionManager),
        reinterpret_cast<const void *>(&radek_compat_OBJC_CLASS___GCController),
        reinterpret_cast<const void *>(&radek_compat_OBJC_METACLASS___UIViewController),
        reinterpret_cast<const void *>(&radek_compat_OBJC_METACLASS___UIApplication),
        reinterpret_cast<const void *>(&radek_compat_UIGraphicsPushContext),
        reinterpret_cast<const void *>(&radek_compat_UIGraphicsPopContext),
        reinterpret_cast<const void *>(&radek_compat_UIGraphicsGetCurrentContext),
        reinterpret_cast<const void *>(&radek_compat_UIGraphicsBeginImageContext),
        reinterpret_cast<const void *>(&radek_compat_UIGraphicsBeginImageContextWithOptions),
        reinterpret_cast<const void *>(&radek_compat_UIGraphicsGetImageFromCurrentImageContext),
        reinterpret_cast<const void *>(&radek_compat_UIGraphicsEndImageContext),
        reinterpret_cast<const void *>(&radek_compat_UIImagePNGRepresentation),
        reinterpret_cast<const void *>(&radek_compat_UIImageJPEGRepresentation),
        reinterpret_cast<const void *>(&radek_compat_UIImageWriteToSavedPhotosAlbum),
        reinterpret_cast<const void *>(&radek_compat_NSTemporaryDirectory),
        reinterpret_cast<const void *>(&radek_compat_NSHomeDirectory),
        reinterpret_cast<const void *>(&radek_compat_NSLog),
        reinterpret_cast<const void *>(&radek_compat_NSStringFromClass),
        reinterpret_cast<const void *>(&radek_compat_NSClassFromString),
        reinterpret_cast<const void *>(&radek_compat_NSStringFromSelector),
        reinterpret_cast<const void *>(&radek_compat_NSSelectorFromString),
        reinterpret_cast<const void *>(&radek_compat_NSPageSize),
        reinterpret_cast<const void *>(&radek_compat_dispatch_async),
        reinterpret_cast<const void *>(&radek_compat_dispatch_sync),
        reinterpret_cast<const void *>(&radek_compat_dispatch_after),
        reinterpret_cast<const void *>(&radek_compat_dispatch_once),
        reinterpret_cast<const void *>(&radek_compat_dispatch_async_f),
        reinterpret_cast<const void *>(&radek_compat_dispatch_sync_f),
        reinterpret_cast<const void *>(&radek_compat_dispatch_once_f),
        reinterpret_cast<const void *>(&radek_compat_dispatch_get_main_queue),
        reinterpret_cast<const void *>(&radek_compat_dispatch_get_global_queue),
        reinterpret_cast<const void *>(&radek_compat_dispatch_queue_create),
        reinterpret_cast<const void *>(&radek_compat_dispatch_release),
        reinterpret_cast<const void *>(&radek_compat_dispatch_retain),
        reinterpret_cast<const void *>(&radek_compat_dispatch_time),
        reinterpret_cast<const void *>(&radek_compat_dispatch_semaphore_create),
        reinterpret_cast<const void *>(&radek_compat_dispatch_semaphore_wait),
        reinterpret_cast<const void *>(&radek_compat_dispatch_semaphore_signal),
        reinterpret_cast<const void *>(&radek_compat_dispatch_group_create),
        reinterpret_cast<const void *>(&radek_compat_dispatch_group_async),
        reinterpret_cast<const void *>(&radek_compat_dispatch_group_enter),
        reinterpret_cast<const void *>(&radek_compat_dispatch_group_leave),
        reinterpret_cast<const void *>(&radek_compat_dispatch_group_wait),
        reinterpret_cast<const void *>(&radek_compat_dispatch_group_notify),
        reinterpret_cast<const void *>(&radek_compat__dispatch_main_q),
        reinterpret_cast<const void *>(&radek_compat_SCNetworkReachabilityCreateWithAddress),
        reinterpret_cast<const void *>(&radek_compat_SCNetworkReachabilityCreateWithName),
        reinterpret_cast<const void *>(&radek_compat_SCNetworkReachabilityGetFlags),
        reinterpret_cast<const void *>(&radek_compat_SCNetworkReachabilitySetCallback),
        reinterpret_cast<const void *>(&radek_compat_SCNetworkReachabilityScheduleWithRunLoop),
        reinterpret_cast<const void *>(&radek_compat_SCNetworkReachabilityUnscheduleFromRunLoop),
        reinterpret_cast<const void *>(&radek_compat_SCNetworkReachabilitySetDispatchQueue),
        reinterpret_cast<const void *>(&radek_compat_SecRandomCopyBytes),
        reinterpret_cast<const void *>(&radek_compat_SecItemCopyMatching),
        reinterpret_cast<const void *>(&radek_compat_SecItemAdd),
        reinterpret_cast<const void *>(&radek_compat_SecItemUpdate),
        reinterpret_cast<const void *>(&radek_compat_SecItemDelete),
        reinterpret_cast<const void *>(&radek_compat_CC_MD5),
        reinterpret_cast<const void *>(&radek_compat_CC_SHA1),
        reinterpret_cast<const void *>(&radek_compat_CC_SHA256),
        reinterpret_cast<const void *>(&radek_compat__Unwind_DeleteException),
        reinterpret_cast<const void *>(&radek_compat__Unwind_GetIP),
        reinterpret_cast<const void *>(&radek_compat__Unwind_SetIP),
        reinterpret_cast<const void *>(&radek_compat__Unwind_GetGR),
        reinterpret_cast<const void *>(&radek_compat__Unwind_SetGR),
        reinterpret_cast<const void *>(&radek_compat__Unwind_GetLanguageSpecificData),
        reinterpret_cast<const void *>(&radek_compat__Unwind_GetRegionStart),
        reinterpret_cast<const void *>(&radek_compat___gxx_personality_v0),
        reinterpret_cast<const void *>(&radek_compat___gcc_personality_v0),
        reinterpret_cast<const void *>(&radek_compat___udivdi3),
        reinterpret_cast<const void *>(&radek_compat___umoddi3),
        reinterpret_cast<const void *>(&radek_compat___muldi3),
        reinterpret_cast<const void *>(&radek_compat___fixsfdi),
        reinterpret_cast<const void *>(&radek_compat___fixunsdfdi),
        reinterpret_cast<const void *>(&radek_compat___fixunssfdi),
        reinterpret_cast<const void *>(&radek_compat___floatundidf),
        reinterpret_cast<const void *>(&radek_compat___floatundisf),
        reinterpret_cast<const void *>(&radek_compat___ashldi3),
        reinterpret_cast<const void *>(&radek_compat___ashrdi3),
        reinterpret_cast<const void *>(&radek_compat___lshrdi3),
        reinterpret_cast<const void *>(&radek_compat___cmpdi2),
        reinterpret_cast<const void *>(&radek_compat___ucmpdi2),
        reinterpret_cast<const void *>(&radek_compat___clear_cache),
        reinterpret_cast<const void *>(&radek_compat__Znaj),
        reinterpret_cast<const void *>(&radek_compat__Znwj),
        reinterpret_cast<const void *>(&radek_compat___cxa_free_exception),
        reinterpret_cast<const void *>(&radek_compat___cxa_rethrow),
        reinterpret_cast<const void *>(&radek_compat___cxa_guard_acquire),
        reinterpret_cast<const void *>(&radek_compat___cxa_guard_release),
        reinterpret_cast<const void *>(&radek_compat___cxa_guard_abort),
        reinterpret_cast<const void *>(&radek_compat___cxa_demangle),
        reinterpret_cast<const void *>(&radek_compat___dynamic_cast),
    };
    for (const void *addr : kExpandedShimAddresses) {
        CHECK(addr != nullptr);
    }
}

/* --- Batch 2 (Bioshock / Angry Birds device inventory) coverage ----------- */

void testBatch2CoreFoundation() {
    // CFArrayContainsValue / CFArrayGetFirstIndexOfValue use CF content equality.
    radek_CFMutableArrayRef array = radek_compat_CFArrayCreateMutable(nullptr, 0, nullptr);
    radek_CFStringRef first = radek_compat_CFStringCreateWithCString(nullptr, "alpha", 0x08000100u);
    radek_CFStringRef second = radek_compat_CFStringCreateWithCString(nullptr, "beta", 0x08000100u);
    CHECK(first != nullptr && second != nullptr);
    radek_compat_CFArrayAppendValue(array, first);
    radek_compat_CFArrayAppendValue(array, second);
    radek_CFStringRef twin = radek_compat_CFStringCreateWithCString(nullptr, "beta", 0x08000100u);
    const radek_CFRange all{0, 2};
    CHECK(radek_compat_CFArrayContainsValue(array, all, second) == 1);
    CHECK(radek_compat_CFArrayContainsValue(array, all, twin) == 1);   // content equality
    const radek_CFRange headOnly{0, 1};
    CHECK(radek_compat_CFArrayContainsValue(array, headOnly, twin) == 0);  // bounded range
    CHECK(radek_compat_CFArrayGetFirstIndexOfValue(array, all, twin) == 1);
    CHECK(radek_compat_CFArrayGetFirstIndexOfValue(array, headOnly, twin) == -1);
    CHECK(radek_compat_CFMakeCollectable(first) == first);

    // CFDictionaryAddValue inserts new keys but never replaces existing ones.
    radek_CFMutableDictionaryRef dictionary =
        radek_compat_CFDictionaryCreateMutable(nullptr, 0, nullptr, nullptr);
    CHECK(radek_compat_CFDictionaryAddValue(dictionary, first, second) == 1);   // "alpha"
    CHECK(radek_compat_CFDictionaryAddValue(dictionary, twin, first) == 1);     // "beta": new key
    CHECK(radek_compat_CFDictionaryAddValue(dictionary, second, twin) == 0);    // "beta" exists
    CHECK(radek_compat_CFDictionaryGetCount(dictionary) == 2);

    // CFStringCreateWithCharacters / GetCharactersPtr / AppendCharacters.
    const radek_UniChar word[] = {'h', 'e', 'l', 'l', 'o'};
    radek_CFMutableStringRef chars = const_cast<radek_CFRuntime *>(
        radek_compat_CFStringCreateWithCharacters(nullptr, word, 5));
    CHECK(chars != nullptr);
    const radek_UniChar *view = radek_compat_CFStringGetCharactersPtr(chars);
    CHECK(view != nullptr && std::memcmp(view, word, sizeof(word)) == 0);
    const radek_UniChar more[] = {'!'};
    radek_compat_CFStringAppendCharacters(chars, more, 1);
    view = radek_compat_CFStringGetCharactersPtr(chars);
    CHECK(view != nullptr);
    const radek_UniChar joined[] = {'h', 'e', 'l', 'l', 'o', '!'};
    CHECK(std::memcmp(view, joined, sizeof(joined)) == 0);

    // Percent escaping honours the forced-escape list.
    radek_CFStringRef plain = radek_compat_CFStringCreateWithCString(nullptr, "a b/c", 0x08000100u);
    radek_CFStringRef escaped = radek_compat_CFURLCreateStringByAddingPercentEscapes(
        nullptr, plain, nullptr, nullptr, 0x08000100u);
    CHECK(escaped != nullptr);
    char buffer[32] = {};
    CHECK(radek_compat_CFStringGetCString(escaped, buffer, sizeof(buffer), 0x08000100u) == 1);
    CHECK(std::string(buffer) == "a%20b/c");
    radek_CFStringRef forceSlash = radek_compat_CFStringCreateWithCString(nullptr, "/", 0x08000100u);
    radek_CFStringRef escapedForced = radek_compat_CFURLCreateStringByAddingPercentEscapes(
        nullptr, plain, nullptr, forceSlash, 0x08000100u);
    CHECK(radek_compat_CFStringGetCString(escapedForced, buffer, sizeof(buffer), 0x08000100u) == 1);
    CHECK(std::string(buffer) == "a%20b%2Fc");

    // CFHost: resolve localhost and hand back sockaddr-bearing CFData values.
    radek_CFStringRef name = radek_compat_CFStringCreateWithCString(nullptr, "localhost", 0x08000100u);
    radek_CFTypeRef host = radek_compat_CFHostCreateWithName(nullptr, name);
    CHECK(host != nullptr);
    radek_Boolean resolved = 1;
    CHECK(radek_compat_CFHostGetAddressing(host, &resolved) == nullptr && resolved == 0);
    int32_t streamError[2] = {-1, -1};
    CHECK(radek_compat_CFHostStartInfoResolution(host, 0, streamError) == 1);
    CHECK(streamError[0] == 0 && streamError[1] == 0);
    radek_CFArrayRef addresses = radek_compat_CFHostGetAddressing(host, &resolved);
    CHECK(addresses != nullptr && resolved == 1);
    CHECK(radek_compat_CFArrayGetCount(addresses) >= 1);
    const void *firstAddress = radek_compat_CFArrayGetValueAtIndex(addresses, 0);
    CHECK(radek_compat_CFDataGetLength(static_cast<radek_CFDataRef>(firstAddress)) >=
          static_cast<radek_CFIndex>(sizeof(sockaddr_in)));
    const uint8_t *addressBytes =
        radek_compat_CFDataGetBytePtr(static_cast<radek_CFDataRef>(firstAddress));
    CHECK(addressBytes != nullptr);
    const uint16_t family = *reinterpret_cast<const uint16_t *>(addressBytes);
    CHECK(family == AF_INET || family == AF_INET6);
    radek_compat_CFRelease(addresses);
    radek_compat_CFRelease(host);
    radek_compat_CFRelease(escapedForced);
    radek_compat_CFRelease(forceSlash);
    radek_compat_CFRelease(escaped);
    radek_compat_CFRelease(plain);
    radek_compat_CFRelease(chars);
    radek_compat_CFRelease(twin);
    radek_compat_CFRelease(dictionary);
    radek_compat_CFRelease(second);
    radek_compat_CFRelease(first);
    radek_compat_CFRelease(array);
    radek_compat_CFRelease(name);
}

void testBatch2CGRect() {
    const radek_CGRect rect{{1.25f, 2.5f}, {10.0f, 20.0f}};
    CHECK(radek_compat_CGRectGetWidth(rect) == 10.0f);
    CHECK(radek_compat_CGRectGetHeight(rect) == 20.0f);
    CHECK(radek_compat_CGRectGetMinY(rect) == 2.5f);
    CHECK(radek_compat_CGRectGetMaxX(rect) == 11.25f);
    CHECK(radek_compat_CGRectGetMidX(rect) == 6.25f);
    CHECK(radek_compat_CGRectGetMidY(rect) == 12.5f);

    const radek_CGRect integral = radek_compat_CGRectIntegral(rect);
    CHECK(integral.origin.x == 1.0f && integral.origin.y == 2.0f);
    CHECK(integral.size.width == 11.0f && integral.size.height == 21.0f);

    const radek_CGRect offset = radek_compat_CGRectOffset(rect, 1.0f, -1.0f);
    CHECK(offset.origin.x == 2.25f && offset.origin.y == 1.5f);
    CHECK(offset.size.width == 10.0f && offset.size.height == 20.0f);

    const radek_CGRect overlapping{{5.0f, 5.0f}, {10.0f, 10.0f}};
    const radek_CGRect disjoint{{100.0f, 100.0f}, {5.0f, 5.0f}};
    CHECK(radek_compat_CGRectIntersectsRect(rect, overlapping) == 1);
    CHECK(radek_compat_CGRectIntersectsRect(rect, disjoint) == 0);

    CHECK(radek_compat_CGRectIsEmpty(rect) == 0);
    const radek_CGRect thin{{0.0f, 0.0f}, {0.0f, 10.0f}};
    CHECK(radek_compat_CGRectIsEmpty(thin) == 1);
    const radek_CGRect nullRect{{INFINITY, INFINITY}, {0.0f, 0.0f}};
    CHECK(radek_compat_CGRectIsNull(nullRect) == 1);
    CHECK(radek_compat_CGRectIsEmpty(nullRect) == 1);
    CHECK(radek_compat_CGRectIsNull(rect) == 0);
    CHECK(radek_compat_CGRectIntersectsRect(nullRect, overlapping) == 0);
    const radek_CGRect integralNull = radek_compat_CGRectIntegral(nullRect);
    CHECK(radek_compat_CGRectIsNull(integralNull) == 1);
}

void testBatch2CCHmac() {
    // RFC 2202 test case 2, HMAC-SHA1.
    {
        const char key[] = "Jefe";
        const char data[] = "what do ya want for nothing?";
        uint8_t mac[20];
        radek_compat_CCHmac(RADEK_kCCHmacAlgSHA1, key, 4, data, 28, mac);
        const uint8_t expected[20] = {0xef, 0xfc, 0xdf, 0x6a, 0xe5, 0xeb, 0x2f, 0xa2, 0xd2, 0x74,
                                      0x16, 0xd5, 0xf1, 0x84, 0xdf, 0x9c, 0x25, 0x9a, 0x7c, 0x79};
        CHECK(std::memcmp(mac, expected, sizeof(expected)) == 0);
    }
    // RFC 4231 test case 2, HMAC-SHA256.
    {
        const char key[] = "Jefe";
        const char data[] = "what do ya want for nothing?";
        uint8_t mac[32];
        radek_compat_CCHmac(RADEK_kCCHmacAlgSHA256, key, 4, data, 28, mac);
        const uint8_t expected[32] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24,
                                      0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27,
                                      0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43};
        CHECK(std::memcmp(mac, expected, sizeof(expected)) == 0);
    }
    // RFC 2104 / classic MD5 vector: key "Jefe", same data.
    {
        const char key[] = "Jefe";
        const char data[] = "what do ya want for nothing?";
        uint8_t mac[16];
        radek_compat_CCHmac(RADEK_kCCHmacAlgMD5, key, 4, data, 28, mac);
        const uint8_t expected[16] = {0x75, 0x0c, 0x78, 0x3e, 0x6a, 0xb0, 0xb5, 0x03,
                                      0xea, 0xa8, 0x6e, 0x31, 0x0a, 0x5d, 0xb7, 0x38};
        CHECK(std::memcmp(mac, expected, sizeof(expected)) == 0);
    }
    // Incremental Init/Update/Final equals the one-shot call, and long keys
    // (> 64 bytes, forcing the key-hash path) agree between both forms.
    {
        uint8_t key[100];
        for (size_t i = 0; i < sizeof(key); ++i) key[i] = static_cast<uint8_t>(i * 7 + 1);
        const char *data = "the quick brown fox jumps over the lazy dog";
        uint8_t oneShot[20];
        radek_compat_CCHmac(RADEK_kCCHmacAlgSHA1, key, sizeof(key), data, std::strlen(data), oneShot);
        radek_CCHmacContext context;
        radek_compat_CCHmacInit(&context, RADEK_kCCHmacAlgSHA1, key, sizeof(key));
        radek_compat_CCHmacUpdate(&context, data, 10);
        radek_compat_CCHmacUpdate(&context, data + 10, std::strlen(data) - 10);
        uint8_t streamed[20];
        radek_compat_CCHmacFinal(&context, streamed);
        CHECK(std::memcmp(oneShot, streamed, sizeof(oneShot)) == 0);
    }
}

void testBatch2AtomicsAndThreads() {
    volatile int32_t value = 10;
    CHECK(radek_compat_OSAtomicAdd32Barrier(5, &value) == 15);
    CHECK(value == 15);
    CHECK(radek_compat_OSAtomicCompareAndSwap32Barrier(15, 20, &value) == 1);
    CHECK(value == 20);
    CHECK(radek_compat_OSAtomicCompareAndSwap32Barrier(15, 25, &value) == 0);  // stale expected
    CHECK(value == 20);
    int token = 0;
    int other = 0;
    void *volatile pointerSlot = &token;
    CHECK(radek_compat_OSAtomicCompareAndSwapPtrBarrier(&token, &other, &pointerSlot) == 1);
    CHECK(pointerSlot == &other);
    CHECK(radek_compat_OSAtomicCompareAndSwapPtrBarrier(&token, nullptr, &pointerSlot) == 0);  // stale
    CHECK(pointerSlot == &other);

    CHECK(radek_compat_pthread_mach_thread_np(pthread_self()) != 0);
    uint64_t threadId = 0;
    CHECK(radek_compat_pthread_threadid_np(pthread_self(), &threadId) == 0);
    CHECK(threadId != 0);
    CHECK(radek_compat_dispatch_get_current_queue() != 0);
    CHECK(radek_compat_dispatch_get_current_queue() == radek_compat_dispatch_get_current_queue());
    CHECK(radek_compat_thread_policy_set(0x103, 1, nullptr, 0) == 0);
}

void testBatch2MachSurface() {
    CHECK(radek_compat_mach_host_self() == 0x103u);
    CHECK(radek_compat_mach_task_self_() == 0x103u);

    uintptr_t pageSize = 0;
    CHECK(radek_compat_host_page_size(radek_compat_mach_host_self(), &pageSize) == 0);
    CHECK(pageSize >= 4096u);
    CHECK(radek_compat_host_page_size(0x103u, nullptr) == 4);

    // HOST_VM_INFO: real memory numbers parsed from /proc/meminfo.
    uint32_t vmInfo[23] = {};
    uint32_t vmCount = 23;
    CHECK(radek_compat_host_statistics(0x103u, 2, vmInfo, &vmCount) == 0);
    CHECK(vmCount == 23);
    const uint32_t freePages = vmInfo[0];
    const uint32_t activePages = vmInfo[1];
    CHECK(freePages > 0 || activePages > 0);
    uint32_t tooSmall = 2;
    CHECK(radek_compat_host_statistics(0x103u, 2, vmInfo, &tooSmall) == 4);
    uint32_t cpuLoad[4] = {1, 1, 1, 1};
    uint32_t cpuCount = 4;
    CHECK(radek_compat_host_statistics(0x103u, 3, cpuLoad, &cpuCount) == 0);

    // task_info fills the real process sizes for the basic-info flavours.
    uint32_t taskInfo[16] = {};
    uint32_t taskCount = 16;
    CHECK(radek_compat_task_info(0x103u, 20, taskInfo, &taskCount) == 0);
    CHECK(taskInfo[6] > 0);  // virtual_size from VmSize

    // mach_wait_until with a past deadline returns immediately.
    const auto before = std::chrono::steady_clock::now();
    CHECK(radek_compat_mach_wait_until(1) == 0);
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::milliseconds(50));

    // Semaphore round trip across threads, then a timed-out wait.
    radek_mach_port_t semaphore = 0;
    CHECK(radek_compat_semaphore_create(0x103u, &semaphore, 0, 0) == 0);
    CHECK(semaphore != 0);
    std::thread signaller([semaphore] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(radek_compat_semaphore_signal(semaphore) == 0);
    });
    CHECK(radek_compat_semaphore_wait(semaphore, 0xFFFFFFFFu) == 0);
    signaller.join();
    CHECK(radek_compat_semaphore_wait(semaphore, 20) == 49);  // KERN_OPERATION_TIMED_OUT
    CHECK(radek_compat_semaphore_signal(semaphore) == 0);
    CHECK(radek_compat_semaphore_wait(semaphore, 20) == 0);   // buffered signal
    CHECK(radek_compat_semaphore_destroy(0x103u, semaphore) == 0);
    CHECK(radek_compat_semaphore_destroy(0x103u, semaphore) == 4);  // already gone
}

void testBatch2CompilerAndCxx() {
    // __divmodsi4: {quotient, remainder} in r0:r1 semantics.
    const radek_divmodsi4_result split = radek_compat___divmodsi4(-7, 2);
    CHECK(split.quotient == -3 && split.remainder == -1);
    const radek_divmodsi4_result exact = radek_compat___divmodsi4(100, 10);
    CHECK(exact.quotient == 10 && exact.remainder == 0);
    const radek_divmodsi4_result saturated = radek_compat___divmodsi4(INT32_MIN, -1);
    CHECK(saturated.quotient == INT32_MIN && saturated.remainder == 0);
    const radek_divmodsi4_result guarded = radek_compat___divmodsi4(5, 0);
    CHECK(guarded.quotient == 0 && guarded.remainder == 0);

    // sincos pairs match sin/cos, including the float entry point.
    const radek_sincos_result pair = radek_compat___sincos_stret(M_PI / 4.0);
    CHECK(std::fabs(pair.sin - std::sin(M_PI / 4.0)) < 1e-12);
    CHECK(std::fabs(pair.cos - std::cos(M_PI / 4.0)) < 1e-12);
    const radek_sincosf_result floatPair = radek_compat___sincosf_stret(1.0f);
    CHECK(std::fabs(floatPair.sin - std::sin(1.0f)) < 1e-6f);
    CHECK(std::fabs(floatPair.cos - std::cos(1.0f)) < 1e-6f);

    // memset_pattern16 tiles the 16-byte pattern, including the ragged tail.
    const uint8_t pattern[16] = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
    uint8_t destination[40];
    std::memset(destination, 0, sizeof(destination));
    radek_compat_memset_pattern16(destination, pattern, sizeof(destination));
    for (size_t i = 0; i < sizeof(destination); ++i) {
        CHECK(destination[i] == pattern[i % 16]);
    }

    // STL throw helpers raise catchable libc++ exceptions.
    bool caughtLength = false;
    try {
        radek_compat_stl_throw_length_error("vector too long");
    } catch (const std::length_error &error) {
        caughtLength = std::string(error.what()) == "vector too long";
    }
    CHECK(caughtLength);
    bool caughtRange = false;
    try {
        radek_compat_stl_throw_out_of_range("index 5");
    } catch (const std::out_of_range &error) {
        caughtRange = std::string(error.what()) == "index 5";
    }
    CHECK(caughtRange);

    // std::random_shuffle support: xorshift draw changes, dtor is safe.
    const uint32_t firstDraw = radek_compat_rs_default_call(nullptr);
    const uint32_t secondDraw = radek_compat_rs_default_call(nullptr);
    CHECK(firstDraw != secondDraw);
    radek_compat_rs_default_dtor(nullptr);
    CHECK(radek_compat_rs_get() == 0u);

    // The Objective-C personality forwards to the C++ personality: an invalid
    // unwind version is rejected with _URC_FATAL_PHASE1_ERROR before any
    // context is touched, exactly like __gxx_personality_v0 does.
    CHECK(radek_compat___objc_personality_v0(99, 1, 0, 0, 0) == 3);
}

void testBatch2BlocksRuntime() {
    static bool copyRan = false;
    static bool disposeRan = false;

    // Stack block -> heap copy via _Block_object_assign (flags 7).
    struct RadekTestBlock {
        void *isa;
        int32_t flags;
        int32_t reserved;
        void *invoke;
        void *descriptor;
    };
    struct RadekTestDescriptor {
        uintptr_t reserved;
        uintptr_t size;
        void (*copyHelper)(void *, const void *);
        void (*disposeHelper)(const void *);
    };
    copyRan = disposeRan = false;
    RadekTestDescriptor descriptor;
    descriptor.reserved = 0;
    descriptor.size = sizeof(RadekTestBlock);
    descriptor.copyHelper = [](void *, const void *) { copyRan = true; };
    descriptor.disposeHelper = [](const void *) { disposeRan = true; };
    alignas(16) char stackBlockStorage[sizeof(RadekTestBlock)];
    auto *stackBlock = reinterpret_cast<RadekTestBlock *>(stackBlockStorage);
    stackBlock->isa = nullptr;
    stackBlock->flags = (1 << 25);  // BLOCK_HAS_COPY_DISPOSE
    stackBlock->reserved = 0;
    stackBlock->invoke = reinterpret_cast<void *>(0x1234);
    stackBlock->descriptor = &descriptor;

    void *copiedSlot = nullptr;
    radek_compat_Block_object_assign(&copiedSlot, stackBlock, 7);
    CHECK(copiedSlot != nullptr && copiedSlot != stackBlock);
    CHECK(copyRan);
    auto *heapBlock = static_cast<RadekTestBlock *>(copiedSlot);
    CHECK(heapBlock->invoke == reinterpret_cast<void *>(0x1234));
    CHECK((heapBlock->flags & (1 << 24)) != 0);  // BLOCK_NEEDS_FREE

    void *secondSlot = nullptr;
    radek_compat_Block_object_assign(&secondSlot, heapBlock, 7);  // heap block: share + refcount
    CHECK(secondSlot == heapBlock);
    radek_compat_Block_object_dispose(heapBlock, 7);  // refcount down, still alive
    CHECK(!disposeRan);
    radek_compat_Block_object_dispose(heapBlock, 7);  // final release
    CHECK(disposeRan);

    // __block byref: assign copies to the heap and repoints forwarding.
    struct RadekTestByref {
        void *isa;
        void *forwarding;
        int32_t flags;
        int32_t size;
        int64_t payload;
    };
    alignas(16) char byrefStorage[sizeof(RadekTestByref)];
    auto *stackByref = reinterpret_cast<RadekTestByref *>(byrefStorage);
    stackByref->isa = nullptr;
    stackByref->forwarding = stackByref;
    stackByref->flags = 0;
    stackByref->size = static_cast<int32_t>(sizeof(RadekTestByref));
    stackByref->payload = 0x55AA;

    void *byrefSlot = nullptr;
    radek_compat_Block_object_assign(&byrefSlot, stackByref, 8);
    CHECK(byrefSlot != nullptr && byrefSlot != stackByref);
    auto *heapByref = static_cast<RadekTestByref *>(byrefSlot);
    CHECK(heapByref->payload == 0x55AA);
    CHECK(heapByref->forwarding == heapByref);
    CHECK(stackByref->forwarding == heapByref);
    radek_compat_Block_object_dispose(heapByref, 8);  // NEEDS_FREE: frees the copy
    radek_compat_Block_object_dispose(stackByref, 8);  // stack byref: safe no-op

    // Plain object slot store (flags 3) without an Objective-C runtime.
    int objectToken = 0;
    void *objectSlot = nullptr;
    radek_compat_Block_object_assign(&objectSlot, &objectToken, 3);
    CHECK(objectSlot == &objectToken);
    radek_compat_Block_object_dispose(&objectToken, 3);
}

void testBatch2ObjectiveCHelpers() {
    // Property setters write the ivar slot at (self + offset).
    char self[64] = {};
    radek_compat_objc_setProperty_nonatomic(self, 16, &self[0]);
    CHECK(*reinterpret_cast<void **>(self + 16) == &self[0]);
    radek_compat_objc_setProperty_atomic(self, 24, &self[8]);
    CHECK(*reinterpret_cast<void **>(self + 24) == &self[8]);
    radek_compat_objc_setProperty_atomic_copy(self, 24, nullptr);
    CHECK(*reinterpret_cast<void **>(self + 24) == nullptr);
    radek_compat_objc_setProperty_nonatomic_copy(self, 16, nullptr);
    CHECK(*reinterpret_cast<void **>(self + 16) == nullptr);

    // Associated-object storage accepts set, overwrite, and nil-removal.
    int owner = 0;
    int valueOne = 1;
    int valueTwo = 2;
    radek_compat_objc_setAssociatedObject(&owner, &valueOne, &valueOne, 0x301);
    radek_compat_objc_setAssociatedObject(&owner, &valueOne, &valueTwo, 0x301);
    radek_compat_objc_setAssociatedObject(&owner, &valueOne, nullptr, 0x301);

    // Uncaught exception handler storage round-trips through the getter.
    auto handler = [](void *) {};
    CHECK(radek_compat_NSGetUncaughtExceptionHandler(nullptr) == nullptr);
    radek_compat_NSSetUncaughtExceptionHandler(handler);
    CHECK(radek_compat_NSGetUncaughtExceptionHandler(nullptr) == handler);
    radek_compat_NSSetUncaughtExceptionHandler(nullptr);
    CHECK(radek_compat_NSGetUncaughtExceptionHandler(nullptr) == handler);  // nil keeps the old one
}

bool childDiesWithSignal(void (*body)(), int expectedSignal) {
    const pid_t child = fork();
    if (child == 0) {
        body();
        _exit(0);  // Reached only if the body failed to die.
    }
    CHECK(child > 0);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    return WIFSIGNALED(status) && WTERMSIG(status) == expectedSignal;
}

void assertRtnBody() {
    radek_compat___assert_rtn("fn", "file.c", 42, "1 == 2");
}

void callUnexpectedBody() {
    radek_compat___cxa_call_unexpected(nullptr);
}

void testBatch2TerminalHelpers() {
    CHECK(childDiesWithSignal(&assertRtnBody, SIGABRT));
    CHECK(childDiesWithSignal(&callUnexpectedBody, SIGABRT));
}

}  // namespace

int main() {
    testExpandedGameAndFrameworkShims();
    testLibc();
    testStdio();
    testTime();
    testPthread();
    testCoreFoundationObjects();
    testCoreFoundationRunLoop();
    testBatch2CoreFoundation();
    testBatch2CGRect();
    testBatch2CCHmac();
    testBatch2AtomicsAndThreads();
    testBatch2MachSurface();
    testBatch2CompilerAndCxx();
    testBatch2BlocksRuntime();
    testBatch2ObjectiveCHelpers();
    testBatch2TerminalHelpers();
    std::cout << "Bounded C/POSIX/CoreFoundation compatibility shims passed\n";
    return 0;
}
