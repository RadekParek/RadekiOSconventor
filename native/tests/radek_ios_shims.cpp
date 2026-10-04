// Host tests for the broad iOS/Darwin compatibility shims.
//
// Everything exercised here is a real implementation body, not a resolution
// stub. The test is also run under ASan/UBSan, so it must stay leak-free:
// every CoreFoundation object created below is released.

#include "radek_ios_shims.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
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
    CHECK(radek_compat_fputs("radek", stream) >= 0);
    CHECK(radek_compat_fprintf(stream, "-%d", 42) == 3);
    CHECK(radek_compat_fflush(stream) == 0);
    CHECK(radek_compat_fseek(stream, 0, SEEK_SET) == 0);
    char read[32] = {};
    CHECK(radek_compat_fread(read, 1, sizeof(read) - 1, stream) == 8);
    CHECK(std::string(read) == "radek-42");
    CHECK(radek_compat_ftell(stream) == 8);
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

}  // namespace

int main() {
    testLibc();
    testStdio();
    testTime();
    testPthread();
    testCoreFoundationObjects();
    std::cout << "Broad iOS compatibility shims passed\n";
    return 0;
}
