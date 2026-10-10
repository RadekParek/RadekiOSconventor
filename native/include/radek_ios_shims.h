#pragma once

/*
 * Broad, host-tested iOS/Darwin compatibility shims (part 2 of libioscompat).
 *
 * This header deliberately avoids Apple headers so the same implementation can
 * be built with the Android NDK and exercised by host unit tests. Every symbol
 * declared here is a REAL implementation with a tested body (see
 * native/tests/radek_ios_shims.cpp); nothing in this file is a stub.
 *
 * Scope, stated exactly:
 *
 *  - Darwin libc / POSIX / pthread / math entry points that bionic provides
 *    with an identical C ABI. These are thin, tested forwards. Struct layouts
 *    that genuinely differ between Darwin and Linux/bionic are declared here
 *    in their Darwin shape (see radek_darwin_timeval) instead of silently
 *    reusing the host layout.
 *  - A small, self-contained CoreFoundation object model (allocator, string,
 *    data, mutable array, mutable dictionary, number, date) with real
 *    retain/release, storage and accessors. It is an independent
 *    implementation of the documented behaviour of those types, NOT a
 *    reimplementation of CoreFoundation.
 *
 * Everything here is a resolution target only. Registering a symbol in the
 * compatibility registry does not rewrite an IPA callsite and does not make an
 * iOS app run on Android.
 *
 * Struct-layout caveats (documented, not hidden):
 *  - radek_darwin_timeval matches Darwin's arm64 `struct timeval`
 *    (8-byte tv_sec, 4-byte tv_usec, 4 bytes padding), not Linux's.
 *  - `struct tm` is layout-compatible between Darwin and bionic, so
 *    localtime_r/gmtime_r/mktime forward directly.
 *  - pthread_mutex_t / pthread_cond_t are larger on Darwin than on bionic, so
 *    callers always over-allocate; every operation here touches only
 *    the leading bytes bionic owns.
 *  - pthread_t is an integer on bionic and a pointer on Darwin. Both are
 *    one 64-bit register wide, so values round-trip through calls unchanged;
 *    they are not interchangeable as opaque identifiers across platforms.
 *  - FILE* is always produced by the shims in this library, so it stays
 *    self-consistent regardless of the platform's stdio internals.
 */

#include <stdarg.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Darwin-shaped scalar and object types -------------------------------- */

typedef long radek_CFIndex;
typedef unsigned long radek_CFOptionFlags;
typedef unsigned long radek_CFHashCode;
typedef uint32_t radek_CFStringEncoding;
typedef double radek_CFAbsoluteTime;
typedef double radek_CFTimeInterval;
typedef unsigned char radek_Boolean;
typedef int32_t radek_CFComparisonResult;
typedef uint32_t radek_CFNumberType;

/* Matches Darwin's arm64 struct timeval, which is NOT Linux's layout. */
typedef struct radek_darwin_timeval {
    int64_t tv_sec;
    int32_t tv_usec;
    int32_t tv_pad;
} radek_darwin_timeval;

/*
 * One opaque runtime type backs every CoreFoundation instance. The concrete
 * kind is recorded inside the object, exactly like CF's own _CFRuntimeBase.
 */
struct radek_CFRuntime;
typedef const struct radek_CFRuntime *radek_CFTypeRef;
typedef const struct radek_CFRuntime *radek_CFStringRef;
typedef struct radek_CFRuntime *radek_CFMutableStringRef;
typedef const struct radek_CFRuntime *radek_CFAllocatorRef;
typedef const struct radek_CFRuntime *radek_CFDataRef;
typedef struct radek_CFRuntime *radek_CFMutableDataRef;
typedef const struct radek_CFRuntime *radek_CFArrayRef;
typedef struct radek_CFRuntime *radek_CFMutableArrayRef;
typedef const struct radek_CFRuntime *radek_CFDictionaryRef;
typedef struct radek_CFRuntime *radek_CFMutableDictionaryRef;
typedef const struct radek_CFRuntime *radek_CFNumberRef;
typedef const struct radek_CFRuntime *radek_CFDateRef;
typedef const struct radek_CFRuntime *radek_CFTimeZoneRef;
typedef const struct radek_CFRuntime *radek_CFRunLoopRef;
typedef int32_t radek_CFRunLoopRunResult;
typedef void (*radek_CFRunLoopPerformCallback)(void *context);

/* CFRunLoopRunInMode result values from CoreFoundation/CFRunLoop.h. */
#define RADEK_KCFRUNLOOPRUNFINISHED ((uint32_t)1)
#define RADEK_KCFRUNLOOPRUNSTOPPED ((uint32_t)2)
#define RADEK_KCFRUNLOOPRUNTIMEDOUT ((uint32_t)3)
#define RADEK_KCFRUNLOOPRUNHANDLEDSOURCE ((uint32_t)4)

/*
 * Collection callbacks are accepted and ignored: the collections implemented
 * here always apply kCFTypeArrayCallBacks / kCFTypeDictionaryCallBacks
 * semantics (retain on insert, release on replace and destroy).
 */
typedef const void *radek_CFArrayCallBacksRef;
typedef const void *radek_CFDictionaryKeyCallBacksRef;
typedef const void *radek_CFDictionaryValueCallBacksRef;

typedef struct radek_CFGregorianDate {
    int32_t year;
    int8_t month;
    int8_t day;
    int8_t hour;
    int8_t minute;
    double second;
} radek_CFGregorianDate;

#define RADEK_KCFSTRINGENCODINGUTF8 ((uint32_t)0x08000100)
#define RADEK_KCFCOMPAREEQUALTO ((int32_t)0)
#define RADEK_KCFCOMPARELESSTHAN ((int32_t)-1)
#define RADEK_KCFCOMPAREGREATERTHAN ((int32_t)1)

/* kCFNumberType values (CoreFoundation CFNumber.h). */
#define RADEK_KCFNUMBERSINT32TYPE ((uint32_t)3)
#define RADEK_KCFNUMBERSINT64TYPE ((uint32_t)4)
#define RADEK_KCFNUMBERFLOAT32TYPE ((uint32_t)5)
#define RADEK_KCFNUMBERFLOAT64TYPE ((uint32_t)6)
#define RADEK_KCFNUMBERINTTYPE ((uint32_t)9)
#define RADEK_KCFNUMBERLONGTYPE ((uint32_t)10)
#define RADEK_KCFNUMBERDOUBLETYPE ((uint32_t)13)

/* --- CoreFoundation object model ------------------------------------------ */

radek_CFAllocatorRef radek_compat_CFAllocatorGetDefault(void);
radek_CFTypeRef radek_compat_CFRetain(radek_CFTypeRef object);
void radek_compat_CFRelease(radek_CFTypeRef object);
radek_CFIndex radek_compat_CFGetRetainCount(radek_CFTypeRef object);

radek_CFStringRef radek_compat_CFStringCreateWithCString(radek_CFAllocatorRef allocator,
                                                         const char *cString,
                                                         radek_CFStringEncoding encoding);
radek_CFIndex radek_compat_CFStringGetLength(radek_CFStringRef string);
radek_Boolean radek_compat_CFStringGetCString(radek_CFStringRef string, char *buffer,
                                              radek_CFIndex bufferSize, radek_CFStringEncoding encoding);
const char *radek_compat_CFStringGetCStringPtr(radek_CFStringRef string, radek_CFStringEncoding encoding);
radek_CFIndex radek_compat_CFStringGetMaximumSizeForEncoding(radek_CFIndex length,
                                                             radek_CFStringEncoding encoding);
radek_CFComparisonResult radek_compat_CFStringCompare(radek_CFStringRef left, radek_CFStringRef right,
                                                      radek_CFOptionFlags options);
radek_CFStringEncoding radek_compat_CFStringGetSystemEncoding(void);

radek_CFDataRef radek_compat_CFDataCreate(radek_CFAllocatorRef allocator, const uint8_t *bytes,
                                          radek_CFIndex length);
const uint8_t *radek_compat_CFDataGetBytePtr(radek_CFDataRef data);
radek_CFIndex radek_compat_CFDataGetLength(radek_CFDataRef data);

radek_CFMutableArrayRef radek_compat_CFArrayCreateMutable(radek_CFAllocatorRef allocator,
                                                          radek_CFIndex capacity,
                                                          radek_CFArrayCallBacksRef callBacks);
void radek_compat_CFArrayAppendValue(radek_CFMutableArrayRef array, const void *value);
radek_CFIndex radek_compat_CFArrayGetCount(radek_CFArrayRef array);
const void *radek_compat_CFArrayGetValueAtIndex(radek_CFArrayRef array, radek_CFIndex index);

radek_CFMutableDictionaryRef radek_compat_CFDictionaryCreateMutable(
    radek_CFAllocatorRef allocator, radek_CFIndex capacity,
    radek_CFDictionaryKeyCallBacksRef keyCallBacks,
    radek_CFDictionaryValueCallBacksRef valueCallBacks);
void radek_compat_CFDictionarySetValue(radek_CFMutableDictionaryRef dictionary, const void *key,
                                       const void *value);
const void *radek_compat_CFDictionaryGetValue(radek_CFDictionaryRef dictionary, const void *key);
radek_CFIndex radek_compat_CFDictionaryGetCount(radek_CFDictionaryRef dictionary);

radek_CFNumberRef radek_compat_CFNumberCreate(radek_CFAllocatorRef allocator, radek_CFNumberType type,
                                              const void *valuePointer);
radek_Boolean radek_compat_CFNumberGetValue(radek_CFNumberRef number, radek_CFNumberType type,
                                            void *valuePointer);

radek_CFDateRef radek_compat_CFDateCreate(radek_CFAllocatorRef allocator, radek_CFAbsoluteTime absoluteTime);
radek_CFAbsoluteTime radek_compat_CFDateGetAbsoluteTime(radek_CFDateRef date);
radek_CFTimeInterval radek_compat_CFDateGetTimeIntervalSinceDate(radek_CFDateRef date,
                                                                 radek_CFDateRef other);
radek_CFGregorianDate radek_compat_CFAbsoluteTimeGetGregorianDate(radek_CFAbsoluteTime absoluteTime,
                                                                  radek_CFTimeZoneRef timeZone);

/*
 * CoreFoundation run-loop subset. GetCurrent/GetMain and the Run/RunInMode/
 * Stop/WakeUp exports have the Darwin C ABI. Perform is an explicit C callback
 * helper for runtimes that cannot carry Apple Blocks objects across the ABI.
 * This queue is thread-safe but is not a drop-in implementation of every Apple
 * run-loop source, observer, timer, or Objective-C block contract.
 */
radek_CFRunLoopRef radek_compat_CFRunLoopGetCurrent(void);
radek_CFRunLoopRef radek_compat_CFRunLoopGetMain(void);
void radek_compat_CFRunLoopRun(void);
radek_CFRunLoopRunResult radek_compat_CFRunLoopRunInMode(radek_CFStringRef mode,
                                                          radek_CFTimeInterval seconds,
                                                          radek_Boolean returnAfterSourceHandled);
void radek_compat_CFRunLoopStop(radek_CFRunLoopRef runLoop);
void radek_compat_CFRunLoopWakeUp(radek_CFRunLoopRef runLoop);
radek_Boolean radek_compat_CFRunLoopPerform(radek_CFRunLoopRef runLoop,
                                             radek_CFStringRef mode,
                                             radek_CFRunLoopPerformCallback callback,
                                             void *context);

/* --- libc / POSIX forwards (identical C ABI on bionic) -------------------- */

void *radek_compat_malloc(size_t size);
void *radek_compat_calloc(size_t count, size_t size);
void *radek_compat_realloc(void *pointer, size_t size);
void radek_compat_free(void *pointer);
void *radek_compat_memcpy(void *destination, const void *source, size_t size);
void *radek_compat_memmove(void *destination, const void *source, size_t size);
void *radek_compat_memset(void *destination, int value, size_t size);
int radek_compat_memcmp(const void *left, const void *right, size_t size);
void *radek_compat_memchr(const void *source, int value, size_t size);

size_t radek_compat_strlen(const char *string);
char *radek_compat_strcpy(char *destination, const char *source);
char *radek_compat_strncpy(char *destination, const char *source, size_t size);
size_t radek_compat_strlcpy(char *destination, const char *source, size_t size);
size_t radek_compat_strlcat(char *destination, const char *source, size_t size);
int radek_compat_strcmp(const char *left, const char *right);
int radek_compat_strncmp(const char *left, const char *right, size_t size);
char *radek_compat_strdup(const char *string);
char *radek_compat_strchr(const char *string, int value);
char *radek_compat_strrchr(const char *string, int value);
char *radek_compat_strstr(const char *haystack, const char *needle);
long radek_compat_strtol(const char *string, char **end, int base);
double radek_compat_strtod(const char *string, char **end);
int radek_compat_atoi(const char *string);
double radek_compat_atof(const char *string);
char *radek_compat_strerror(int code);
int radek_compat_snprintf(char *buffer, size_t size, const char *format, ...);
int radek_compat_vsnprintf(char *buffer, size_t size, const char *format, va_list arguments);

FILE *radek_compat_fopen(const char *path, const char *mode);
int radek_compat_fclose(FILE *stream);
size_t radek_compat_fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t radek_compat_fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int radek_compat_fputs(const char *string, FILE *stream);
char *radek_compat_fgets(char *buffer, int size, FILE *stream);
int radek_compat_fflush(FILE *stream);
int radek_compat_fprintf(FILE *stream, const char *format, ...);
int radek_compat_printf(const char *format, ...);
int radek_compat_puts(const char *string);
int radek_compat_remove(const char *path);
int radek_compat_feof(FILE *stream);
long radek_compat_ftell(FILE *stream);
int radek_compat_fseek(FILE *stream, long offset, int origin);

time_t radek_compat_time(time_t *result);
int radek_compat_gettimeofday(radek_darwin_timeval *result, void *timeZone);
int radek_compat_clock_gettime(int clockIdentifier, struct timespec *result);
int radek_compat_nanosleep(const struct timespec *request, struct timespec *remaining);
struct tm *radek_compat_localtime_r(const time_t *clock, struct tm *result);
struct tm *radek_compat_gmtime_r(const time_t *clock, struct tm *result);
time_t radek_compat_mktime(struct tm *value);

char *radek_compat_getenv(const char *name);
int radek_compat_setenv(const char *name, const char *value, int overwrite);
int radek_compat_unsetenv(const char *name);
long radek_compat_getpid(void);

void radek_compat_qsort(void *base, size_t count, size_t size,
                        int (*compare)(const void *, const void *));
void *radek_compat_bsearch(const void *key, const void *base, size_t count, size_t size,
                           int (*compare)(const void *, const void *));
int radek_compat_abs(int value);
long radek_compat_labs(long value);
int radek_compat_rand(void);
void radek_compat_srand(unsigned int seed);

double radek_compat_sqrt(double value);
double radek_compat_fabs(double value);
double radek_compat_floor(double value);
double radek_compat_ceil(double value);
double radek_compat_pow(double base, double exponent);
double radek_compat_sin(double value);
double radek_compat_cos(double value);
double radek_compat_tan(double value);
double radek_compat_atan2(double y, double x);
double radek_compat_fmod(double numerator, double denominator);

/*
 * pthread forwards. bionic's pthread_mutex_t / pthread_cond_t are smaller than
 * Darwin's, and callers therefore always over-allocate; every
 * operation below touches only the leading bytes bionic owns. pthread_t is an
 * integer on bionic and a pointer on Darwin, but both live in one 64-bit
 * register, so values round-trip through a call unchanged.
 */
int radek_compat_pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attributes);
int radek_compat_pthread_mutex_lock(pthread_mutex_t *mutex);
int radek_compat_pthread_mutex_unlock(pthread_mutex_t *mutex);
int radek_compat_pthread_mutex_destroy(pthread_mutex_t *mutex);
int radek_compat_pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attributes);
int radek_compat_pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex);
int radek_compat_pthread_cond_signal(pthread_cond_t *condition);
int radek_compat_pthread_cond_broadcast(pthread_cond_t *condition);
int radek_compat_pthread_cond_destroy(pthread_cond_t *condition);
pthread_t radek_compat_pthread_self(void);


/*
 * Canonical Darwin symbol -> implementation mapping for everything declared in
 * this header.
 *
 * native/src/ioscompat_registry.cpp (registry seeding) and native/src/jni.cpp
 * (on-device resolver) both expand this table, so the registry and the device
 * resolver can never disagree. radek/api_implementations.py keeps a matching Python
 * table for host source generation, and tests/test_api_implementations.py asserts
 * the two stay identical.
 */
#ifdef __cplusplus
#define RADEK_COMPAT_DEF0 = 0
#else
#define RADEK_COMPAT_DEF0
#endif

/* --- Expanded CoreFoundation, CoreGraphics, OpenAL, AudioToolbox, OpenGL ES, --- */
/* --- Objective-C runtime, UIKit/Foundation, POSIX/libc/libm, and C++ ABI shims --- */
radek_CFTypeRef radek_compat_CFConstantStringClassReference(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFAllocatorDefault(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFBooleanTrue(void);
radek_CFTypeRef radek_compat_CFBooleanFalse(void);
radek_CFTypeRef radek_compat_CFTypeArrayCallBacks(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFTypeDictionaryKeyCallBacks(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFTypeDictionaryValueCallBacks(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFRunLoopDefaultMode(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFRunLoopCommonModes(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFBundleGetMainBundle(void);
radek_CFTypeRef radek_compat_CFBundleCopyBundleURL(radek_CFTypeRef bundle);
radek_CFTypeRef radek_compat_CFBundleCopyResourcesDirectoryURL(radek_CFTypeRef bundle);
radek_CFTypeRef radek_compat_CFBundleCopyResourceURL(radek_CFTypeRef bundle, radek_CFStringRef name, radek_CFStringRef type, radek_CFStringRef subDir);
radek_CFStringRef radek_compat_CFBundleGetIdentifier(radek_CFTypeRef bundle);
radek_CFTypeRef radek_compat_CFBundleGetValueForInfoDictionaryKey(radek_CFTypeRef bundle, radek_CFStringRef key);
radek_CFTypeRef radek_compat_CFURLCreateWithFileSystemPath(radek_CFAllocatorRef alloc, radek_CFStringRef filePath, radek_CFIndex pathStyle, unsigned char isDir);
radek_CFTypeRef radek_compat_CFURLCreateFromFileSystemRepresentation(radek_CFAllocatorRef alloc, const uint8_t *buffer, radek_CFIndex bufLen, unsigned char isDir);
unsigned char radek_compat_CFURLGetFileSystemRepresentation(radek_CFTypeRef url, unsigned char resolveAgainstBase, uint8_t *buffer, radek_CFIndex maxBufLen);
radek_CFStringRef radek_compat_CFURLCopyFileSystemPath(radek_CFTypeRef url, radek_CFIndex pathStyle);
radek_CFTypeRef radek_compat_CFStringCreateWithBytes(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFStringCreateMutable(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFStringAppendCString(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFStringHasPrefix(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFStringHasSuffix(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFStringGetIntValue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFStringGetDoubleValue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFArrayCreate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFArrayRemoveValueAtIndex(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFArrayRemoveAllValues(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFDictionaryCreate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFDictionaryRemoveValue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFDictionaryRemoveAllValues(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFDictionaryContainsKey(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFDataCreateMutable(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFDataAppendBytes(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFDataGetMutableBytePtr(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFDataGetBytes(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
unsigned char radek_compat_CFBooleanGetValue(radek_CFTypeRef booleanRef);
unsigned char radek_compat_CFEqual(radek_CFTypeRef a, radek_CFTypeRef b);
unsigned long radek_compat_CFHash(radek_CFTypeRef cf);
radek_CFTypeRef radek_compat_CFGetTypeID(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFPreferencesCopyAppValue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFPreferencesSetAppValue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFPreferencesAppSynchronize(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFUUIDCreate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFUUIDCreateString(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFLocaleCopyCurrent(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFLocaleCopyPreferredLanguages(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFLocaleGetIdentifier(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
radek_CFTypeRef radek_compat_CFTimeZoneCopySystem(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioSessionInitialize(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioSessionSetActive(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_NSSearchPathForDirectoriesInDomains(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___CAEAGLLayer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___EAGLContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSAutoreleasePool(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSBundle(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSDictionary(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSNumber(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSObject(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSString(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSThread(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSURL(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIAccelerometer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIApplication(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIScreen(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIView(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIWindow(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_METACLASS___NSObject(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_METACLASS___UIView(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_UIApplicationMain(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__DefaultRuneLocale(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Unwind_SjLj_Register(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Unwind_SjLj_Resume(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Unwind_SjLj_Unregister(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__ZSt9terminatev(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__ZTVN10__cxxabiv117__class_type_infoE(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__ZTVN10__cxxabiv119__pointer_type_infoE(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__ZTVN10__cxxabiv120__si_class_type_infoE(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
void radek_compat__ZdaPv(void *ptr);
void radek_compat__ZdlPv(void *ptr);
void *radek_compat__Znam(size_t size);
void *radek_compat__Znwm(size_t size);
uintptr_t radek_compat___cxa_allocate_exception(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cxa_atexit(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cxa_begin_catch(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cxa_end_catch(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cxa_pure_virtual(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cxa_throw(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
int64_t radek_compat___divdi3(int64_t a, int64_t b);
int32_t radek_compat___divsi3(int32_t a, int32_t b);
int *radek_compat___error(void);
int64_t radek_compat___fixdfdi(double a);
double radek_compat___floatdidf(int64_t a);
float radek_compat___floatdisf(int64_t a);
uintptr_t radek_compat___gxx_personality_sj0(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___maskrune(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
int64_t radek_compat___moddi3(int64_t a, int64_t b);
int32_t radek_compat___modsi3(int32_t a, int32_t b);
uintptr_t radek_compat___stderrp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___stdinp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___stdoutp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___tolower(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___toupper(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uint32_t radek_compat___udivsi3(uint32_t a, uint32_t b);
uint32_t radek_compat___umodsi3(uint32_t a, uint32_t b);
uintptr_t radek_compat__objc_empty_cache(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__objc_empty_vtable(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_abort(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_acosf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alBufferData(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alDeleteBuffers(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alDeleteSources(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
void radek_compat_alGenBuffers(int n, unsigned int *buffers);
void radek_compat_alGenSources(int n, unsigned int *sources);
uintptr_t radek_compat_alGetSourcef(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetSourcei(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alSource3f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alSourcePlay(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alSourceQueueBuffers(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alSourceStop(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alSourceUnqueueBuffers(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alSourcef(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alSourcei(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alcCloseDevice(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
void *radek_compat_alcCreateContext(void *device, const int *attrlist);
uintptr_t radek_compat_alcDestroyContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
char radek_compat_alcMakeContextCurrent(void *context);
void *radek_compat_alcOpenDevice(const char *devicename);
uintptr_t radek_compat_asinf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_atan2f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_atanf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ceilf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_clearerr(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_clock(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_close(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_cosf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_coshf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_difftime(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_exit(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_expf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_fcntl(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ferror(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_floorf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_fputc(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_freopen(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_frexp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_fscanf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getc(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glActiveTexture(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glBindBuffer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glBindFramebufferOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glBindRenderbufferOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
void radek_compat_glBindTexture(unsigned int target, unsigned int texture);
uintptr_t radek_compat_glBlendFunc(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glBufferData(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
unsigned int radek_compat_glCheckFramebufferStatusOES(unsigned int target);
uintptr_t radek_compat_glClear(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glClearColor(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glClientActiveTexture(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glColor4f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glColorPointer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glCompressedTexImage2D(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDeleteBuffers(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDeleteFramebuffersOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDeleteRenderbuffersOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDeleteTextures(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDepthFunc(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDepthMask(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDisable(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDisableClientState(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
void radek_compat_glDrawArrays(unsigned int mode, int first, int count);
void radek_compat_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices);
uintptr_t radek_compat_glEnable(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glEnableClientState(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glFramebufferRenderbufferOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glFramebufferTexture2DOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
void radek_compat_glFrontFace(unsigned int mode);
uintptr_t radek_compat_glGenBuffers(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGenFramebuffersOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGenRenderbuffersOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
void radek_compat_glGenTextures(int n, unsigned int *textures);
uintptr_t radek_compat_glGetIntegerv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGetRenderbufferParameterivOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glLightfv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glLineWidth(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glLoadMatrixf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glMaterialfv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glMatrixMode(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glNormalPointer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glPixelStorei(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glRenderbufferStorageOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glScissor(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glTexCoordPointer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glTexEnvi(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glTexImage2D(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glTexParameteri(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glTexSubImage2D(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glVertexPointer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glViewport(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_gmtime(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_kEAGLColorFormatRGB565(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_kEAGLColorFormatRGBA8(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_kEAGLDrawablePropertyColorFormat(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_kEAGLDrawablePropertyRetainedBacking(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ldexp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_localeconv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_localtime(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_log10f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_logf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_longjmp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_lseek(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_modf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_enumerationMutation(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_msgSend(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_msgSendSuper2(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_msgSend_stret(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_setProperty(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_create(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_exit(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_getschedparam(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_join(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_mutex_trylock(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_mutexattr_destroy(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_mutexattr_init(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_mutexattr_settype(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_setschedparam(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_read(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_rename(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sched_yield(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_select(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_setjmp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_setlocale(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_setvbuf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sinf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sinhf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sprintf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strcasecmp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strcat(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strcoll(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strcspn(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strftime(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strncat(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strpbrk(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strtok(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strtoul(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_system(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_tanf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_tanhf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_tmpfile(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_tmpnam(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ungetc(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_usleep(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_vsprintf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__exit(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_atexit(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sscanf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_putchar(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getchar(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_fgetc(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_putc(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_rewind(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_fileno(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_fdopen(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_perror(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_tzset(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sleep(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_open(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_write(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_unlink(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_mkdir(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_rmdir(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_access(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getcwd(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_chdir(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_stat(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_fstat(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_lstat(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_opendir(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_readdir(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_closedir(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_mmap(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_munmap(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_mprotect(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_poll(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pipe(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dup(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dup2(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_fsync(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ftruncate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_truncate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_chmod(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_umask(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getuid(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_geteuid(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getgid(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getegid(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getppid(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sysconf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sysctl(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sysctlbyname(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getpagesize(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__setjmp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__longjmp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sigaction(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_signal(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_raise(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_kill(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_tolower(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_toupper(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_isalpha(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_isdigit(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_isalnum(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_isspace(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_isupper(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_islower(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_isxdigit(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strncasecmp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strspn(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strtok_r(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strtoll(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strtoull(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_strtof(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_atol(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_atoll(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_llabs(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_bzero(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_bcopy(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_bcmp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_acos(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_asin(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_atan(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_cosh(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sinh(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_tanh(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_exp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_log(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_log10(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_log2(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_hypot(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_hypotf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_cbrt(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_round(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_roundf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_trunc(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_truncf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_lround(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_lroundf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_frexpf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ldexpf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_log2f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_modff(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_powf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sqrtf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_fabsf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_fmodf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_detach(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_equal(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_once(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_cond_timedwait(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_key_create(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_key_delete(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_setspecific(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_getspecific(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_rwlock_init(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_rwlock_rdlock(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_rwlock_wrlock(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_rwlock_unlock(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_pthread_rwlock_destroy(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sem_init(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sem_destroy(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sem_wait(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sem_trywait(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sem_post(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dlopen(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dlsym(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dlclose(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dlerror(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_socket(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_connect(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_bind(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_listen(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_accept(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_send(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sendto(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_recv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_recvfrom(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_setsockopt(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getsockopt(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getsockname(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getpeername(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_shutdown(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_getaddrinfo(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_freeaddrinfo(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_gethostbyname(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_inet_ntop(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_inet_pton(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_inet_addr(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_inet_ntoa(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_htons(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_htonl(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ntohs(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ntohl(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_crc32(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_adler32(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_compress(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_compress2(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_uncompress(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_deflateInit_(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_deflateInit2_(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_deflate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_deflateEnd(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_deflateReset(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_inflateInit_(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_inflateInit2_(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_inflate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_inflateEnd(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_inflateReset(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_gzopen(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_gzread(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_gzwrite(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_gzclose(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alDistanceModel(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alDopplerFactor(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alDopplerVelocity(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alSpeedOfSound(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetError(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetSource3f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetSourcefv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alSourcefv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alSourcePause(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alSourceRewind(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alListener3f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alListenerf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alListenerfv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alListeneri(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetListenerf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetListener3f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetListenerfv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alEnable(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alDisable(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alIsEnabled(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alIsBuffer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alIsSource(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetBoolean(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetInteger(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetFloat(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetDouble(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetString(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetEnumValue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alGetProcAddress(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alIsExtensionPresent(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alcGetContextsDevice(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alcGetCurrentContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alcProcessContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alcSuspendContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alcGetError(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alcGetIntegerv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alcGetString(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alcIsExtensionPresent(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_alcGetProcAddress(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioSessionSetActiveWithFlags(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioSessionGetProperty(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioSessionSetProperty(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioSessionGetPropertySize(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioSessionAddPropertyListener(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioSessionRemovePropertyListenerWithUserData(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioServicesPlaySystemSound(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioServicesPlayAlertSound(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioServicesCreateSystemSoundID(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioServicesDisposeSystemSoundID(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioFileOpenURL(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioFileClose(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioFileGetProperty(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioFileReadBytes(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioFileReadPackets(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ExtAudioFileOpenURL(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ExtAudioFileDispose(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ExtAudioFileGetProperty(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ExtAudioFileSetProperty(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ExtAudioFileRead(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_ExtAudioFileSeek(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioQueueNewOutput(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioQueueAllocateBuffer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioQueueFreeBuffer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioQueueEnqueueBuffer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioQueueStart(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioQueuePause(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioQueueStop(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioQueueDispose(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioQueueSetParameter(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioComponentFindNext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioComponentInstanceNew(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioComponentInstanceDispose(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioUnitInitialize(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioUnitUninitialize(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioUnitSetProperty(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioUnitGetProperty(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioOutputUnitStart(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioOutputUnitStop(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_AudioUnitRender(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glAlphaFunc(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glBindFramebuffer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glBindRenderbuffer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glBlendEquation(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glBlendEquationOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glBlendFuncSeparate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glBufferSubData(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
unsigned int radek_compat_glCheckFramebufferStatus(unsigned int target);
uintptr_t radek_compat_glClearDepthf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glClearStencil(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glColor4ub(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glColorMask(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glCompileShader(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glCopyTexImage2D(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glCopyTexSubImage2D(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glCreateProgram(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glCreateShader(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glCullFace(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDeleteFramebuffers(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDeleteProgram(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDeleteRenderbuffers(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDeleteShader(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDepthRangef(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glDisableVertexAttribArray(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glEnableVertexAttribArray(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glFinish(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glFlush(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glFogf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glFogfv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glFramebufferRenderbuffer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glFramebufferTexture2D(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glFrustumf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGenFramebuffers(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGenRenderbuffers(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGenerateMipmap(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGenerateMipmapOES(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGetAttribLocation(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGetError(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGetFloatv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGetProgramInfoLog(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGetProgramiv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGetRenderbufferParameteriv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGetShaderInfoLog(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGetShaderiv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGetString(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glGetUniformLocation(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glHint(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glIsEnabled(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glIsTexture(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glLightModelfv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glLinkProgram(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glLoadIdentity(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glLogicOp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glMaterialf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glMultMatrixf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glNormal3f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glOrthof(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glPointParameterf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glPointParameterfv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glPointSize(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glPolygonOffset(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glPopMatrix(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glPushMatrix(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glReadPixels(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glRenderbufferStorage(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glRotatef(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glScalef(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glShadeModel(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glShaderSource(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glStencilFunc(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glStencilMask(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glStencilOp(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glTexEnvf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glTexEnvfv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glTexParameterf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glTexParameterfv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glTranslatef(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glUniform1f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glUniform1i(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glUniform2f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glUniform3f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glUniform4f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glUniformMatrix4fv(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glUseProgram(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_glVertexAttribPointer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglGetDisplay(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglInitialize(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglChooseConfig(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglCreateWindowSurface(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglCreateContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglMakeCurrent(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglSwapBuffers(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglDestroyContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglDestroySurface(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglTerminate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglGetError(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_eglGetProcAddress(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGColorSpaceCreateDeviceRGB(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGColorSpaceCreateDeviceGray(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGColorSpaceRelease(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGColorSpaceRetain(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGBitmapContextCreate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGBitmapContextGetData(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGBitmapContextGetWidth(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGBitmapContextGetHeight(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGBitmapContextGetBytesPerRow(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGBitmapContextCreateImage(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextRelease(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextRetain(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextClearRect(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextFillRect(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextDrawImage(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextTranslateCTM(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextScaleCTM(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextRotateCTM(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextSaveGState(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextRestoreGState(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextSetRGBFillColor(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGContextSetAlpha(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGImageGetWidth(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGImageGetHeight(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGImageGetBitsPerComponent(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGImageGetBitsPerPixel(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGImageGetBytesPerRow(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGImageGetAlphaInfo(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGImageGetDataProvider(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGImageGetColorSpace(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGImageRelease(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGImageRetain(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGDataProviderCopyData(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGDataProviderCreateWithData(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGDataProviderRelease(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGDataProviderRetain(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGAffineTransformMake(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGAffineTransformMakeTranslation(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGAffineTransformMakeScale(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGAffineTransformMakeRotation(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGAffineTransformTranslate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGAffineTransformScale(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGAffineTransformRotate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CGAffineTransformConcat(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_msgSendSuper(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_msgSendSuper_stret(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_msgSendSuper2_stret(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_msgSend_fpret(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_getClass(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_lookUpClass(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_getMetaClass(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_getProtocol(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_allocateClassPair(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_registerClassPair(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_retain(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_release(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_autorelease(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_autoreleasePoolPush(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_autoreleasePoolPop(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_retainAutorelease(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_retainAutoreleaseReturnValue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_retainAutoreleasedReturnValue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_storeStrong(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_storeWeak(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_loadWeakRetained(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_destroyWeak(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_getProperty(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_copyStruct(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_sync_enter(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_sync_exit(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_exception_throw(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_begin_catch(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_objc_end_catch(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sel_registerName(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sel_getUid(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_sel_getName(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_class_getName(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_class_getSuperclass(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_class_getInstanceMethod(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_class_getClassMethod(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_class_addMethod(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_class_replaceMethod(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_class_createInstance(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_object_getClass(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_object_getClassName(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___MPMoviePlayerController(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSDate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSLocale(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSNotificationCenter(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSUserDefaults(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIColor(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIDevice(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIImage(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIViewController(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___AVAudioPlayer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___AVAudioSession(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSArray(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSMutableArray(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSMutableDictionary(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSMutableString(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSData(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSMutableData(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSSet(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSMutableSet(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSFileManager(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSTimer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSRunLoop(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSProcessInfo(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSValue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___NSError(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIImageView(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UILabel(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIButton(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIScrollView(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIAlertView(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIActivityIndicatorView(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIWebView(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIFont(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UITouch(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___UIEvent(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___CALayer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___CATransaction(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___CABasicAnimation(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___SKPaymentQueue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___SKProductsRequest(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___GKLocalPlayer(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___CMMotionManager(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_CLASS___GCController(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_METACLASS___UIViewController(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_OBJC_METACLASS___UIApplication(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_UIGraphicsPushContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_UIGraphicsPopContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_UIGraphicsGetCurrentContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_UIGraphicsBeginImageContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_UIGraphicsBeginImageContextWithOptions(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_UIGraphicsGetImageFromCurrentImageContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_UIGraphicsEndImageContext(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_UIImagePNGRepresentation(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_UIImageJPEGRepresentation(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_UIImageWriteToSavedPhotosAlbum(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_NSTemporaryDirectory(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_NSHomeDirectory(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_NSLog(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_NSStringFromClass(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_NSClassFromString(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_NSStringFromSelector(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_NSSelectorFromString(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_NSPageSize(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_async(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_sync(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_after(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_once(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_async_f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_sync_f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_once_f(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_get_main_queue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_get_global_queue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_queue_create(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_release(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_retain(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_time(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_semaphore_create(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_semaphore_wait(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_semaphore_signal(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_group_create(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_group_async(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_group_enter(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_group_leave(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_group_wait(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_dispatch_group_notify(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__dispatch_main_q(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SCNetworkReachabilityCreateWithAddress(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SCNetworkReachabilityCreateWithName(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SCNetworkReachabilityGetFlags(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SCNetworkReachabilitySetCallback(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SCNetworkReachabilityScheduleWithRunLoop(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SCNetworkReachabilityUnscheduleFromRunLoop(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SCNetworkReachabilitySetDispatchQueue(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SecRandomCopyBytes(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SecItemCopyMatching(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SecItemAdd(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SecItemUpdate(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_SecItemDelete(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CC_MD5(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CC_SHA1(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat_CC_SHA256(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Unwind_DeleteException(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Unwind_GetIP(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Unwind_SetIP(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Unwind_GetGR(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Unwind_SetGR(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Unwind_GetLanguageSpecificData(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Unwind_GetRegionStart(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___gxx_personality_v0(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___gcc_personality_v0(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uint64_t radek_compat___udivdi3(uint64_t a, uint64_t b);
uint64_t radek_compat___umoddi3(uint64_t a, uint64_t b);
uintptr_t radek_compat___muldi3(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___fixsfdi(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___fixunsdfdi(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___fixunssfdi(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___floatundidf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___floatundisf(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___ashldi3(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___ashrdi3(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___lshrdi3(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cmpdi2(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___ucmpdi2(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___clear_cache(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Znaj(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat__Znwj(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cxa_free_exception(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cxa_rethrow(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cxa_guard_acquire(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cxa_guard_release(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cxa_guard_abort(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___cxa_demangle(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
uintptr_t radek_compat___dynamic_cast(uintptr_t a0 RADEK_COMPAT_DEF0, uintptr_t a1 RADEK_COMPAT_DEF0, uintptr_t a2 RADEK_COMPAT_DEF0, uintptr_t a3 RADEK_COMPAT_DEF0);
/* Batch 2: CoreFoundation queries/characters/percent-escaping/DNS, CoreGraphics
 * rect math, CommonCrypto HMAC, OSAtomic barriers, Mach kernel/semaphore
 * surface, C++ ABI helpers, Blocks runtime, Objective-C property helpers.
 *
 * These prototypes are ABI-typed (not the generic uintptr_t form) wherever the
 * Darwin prototype passes floats, doubles, or small structs by value: the
 * converted guest calls these through the standard AAPCS/softfp calling
 * convention, so the C types here decide which registers carry the values. */
typedef uint16_t radek_UniChar;
typedef struct radek_CFRange {
    radek_CFIndex location;
    radek_CFIndex length;
} radek_CFRange;
typedef struct radek_CGPoint {
    float x;
    float y;
} radek_CGPoint;
typedef struct radek_CGSize {
    float width;
    float height;
} radek_CGSize;
typedef struct radek_CGRect {
    radek_CGPoint origin;
    radek_CGSize size;
} radek_CGRect;
/* kCCHmacAlgSHA1/MD5/SHA256 from CommonCrypto/CommonHMAC.h */
typedef uint32_t radek_CCHmacAlgorithm;
#define RADEK_kCCHmacAlgSHA1 ((uint32_t)0)
#define RADEK_kCCHmacAlgMD5 ((uint32_t)1)
#define RADEK_kCCHmacAlgSHA256 ((uint32_t)2)
/* Darwin's CCHmacContext is `uint32_t ctx[96]` (384 bytes); callers always
 * over-allocate. The shim state stored inside is documented in the .cpp. */
typedef struct radek_CCHmacContext {
    uint32_t ctx[96];
} radek_CCHmacContext;
typedef uint32_t radek_mach_port_t;
typedef uint32_t radek_mach_msg_timeout_t;
typedef int32_t radek_kern_return_t;
typedef void (*radek_NSUncaughtExceptionHandler)(void *exception);

radek_Boolean radek_compat_CFArrayContainsValue(radek_CFArrayRef array, radek_CFRange range, const void *value);
radek_CFIndex radek_compat_CFArrayGetFirstIndexOfValue(radek_CFArrayRef array, radek_CFRange range, const void *value);
radek_Boolean radek_compat_CFDictionaryAddValue(radek_CFMutableDictionaryRef dictionary, const void *key, const void *value);
radek_CFTypeRef radek_compat_CFMakeCollectable(radek_CFTypeRef object);
void radek_compat_CFStringAppendCharacters(radek_CFMutableStringRef string, const radek_UniChar *characters, radek_CFIndex count);
radek_CFStringRef radek_compat_CFStringCreateWithCharacters(radek_CFAllocatorRef allocator, const radek_UniChar *characters, radek_CFIndex count);
const radek_UniChar *radek_compat_CFStringGetCharactersPtr(radek_CFStringRef string);
radek_CFStringRef radek_compat_CFURLCreateStringByAddingPercentEscapes(radek_CFAllocatorRef allocator, radek_CFStringRef original, radek_CFStringRef charactersToLeaveUnescaped, radek_CFStringRef legalURLCharactersToBeEscaped, radek_CFStringEncoding encoding);
radek_CFTypeRef radek_compat_CFHostCreateWithName(radek_CFAllocatorRef allocator, radek_CFStringRef hostname);
radek_CFArrayRef radek_compat_CFHostGetAddressing(radek_CFTypeRef host, radek_Boolean *hasBeenResolved);
radek_Boolean radek_compat_CFHostStartInfoResolution(radek_CFTypeRef host, int32_t info, void *error);
float radek_compat_CGRectGetHeight(radek_CGRect rect);
float radek_compat_CGRectGetMaxX(radek_CGRect rect);
float radek_compat_CGRectGetMidX(radek_CGRect rect);
float radek_compat_CGRectGetMidY(radek_CGRect rect);
float radek_compat_CGRectGetMinY(radek_CGRect rect);
float radek_compat_CGRectGetWidth(radek_CGRect rect);
radek_CGRect radek_compat_CGRectIntegral(radek_CGRect rect);
radek_Boolean radek_compat_CGRectIntersectsRect(radek_CGRect left, radek_CGRect right);
radek_Boolean radek_compat_CGRectIsEmpty(radek_CGRect rect);
radek_Boolean radek_compat_CGRectIsNull(radek_CGRect rect);
radek_CGRect radek_compat_CGRectOffset(radek_CGRect rect, float dx, float dy);
void radek_compat_CCHmac(radek_CCHmacAlgorithm algorithm, const void *key, uintptr_t keyLength, const void *data, uintptr_t dataLength, void *macOut);
void radek_compat_CCHmacInit(radek_CCHmacContext *context, radek_CCHmacAlgorithm algorithm, const void *key, uintptr_t keyLength);
void radek_compat_CCHmacUpdate(radek_CCHmacContext *context, const void *data, uintptr_t dataLength);
void radek_compat_CCHmacFinal(radek_CCHmacContext *context, void *macOut);
int32_t radek_compat_OSAtomicAdd32Barrier(int32_t delta, volatile int32_t *value);
radek_Boolean radek_compat_OSAtomicCompareAndSwap32Barrier(int32_t oldValue, int32_t newValue, volatile int32_t *value);
radek_Boolean radek_compat_OSAtomicCompareAndSwapPtrBarrier(void *oldValue, void *newValue, void *volatile *value);
radek_kern_return_t radek_compat_host_page_size(radek_mach_port_t host, uintptr_t *pageSize);
radek_kern_return_t radek_compat_host_statistics(radek_mach_port_t host, int32_t flavor, void *info, uint32_t *infoCount);
radek_mach_port_t radek_compat_mach_host_self(void);
radek_mach_port_t radek_compat_mach_task_self_(void);
radek_kern_return_t radek_compat_mach_wait_until(uint64_t deadlineNanoseconds);
radek_kern_return_t radek_compat_semaphore_create(radek_mach_port_t task, radek_mach_port_t *semaphore, int32_t policy, int32_t value);
radek_kern_return_t radek_compat_semaphore_destroy(radek_mach_port_t task, radek_mach_port_t semaphore);
radek_kern_return_t radek_compat_semaphore_signal(radek_mach_port_t semaphore);
radek_kern_return_t radek_compat_semaphore_wait(radek_mach_port_t semaphore, radek_mach_msg_timeout_t timeout);
radek_kern_return_t radek_compat_task_info(radek_mach_port_t task, int32_t flavor, void *info, uint32_t *infoCount);
radek_kern_return_t radek_compat_thread_policy_set(uint32_t thread, int32_t flavor, void *policy, uint32_t count);
uint64_t radek_compat_pthread_mach_thread_np(pthread_t thread);
int32_t radek_compat_pthread_threadid_np(pthread_t thread, uint64_t *threadId);
uintptr_t radek_compat_dispatch_get_current_queue(void);
void radek_compat___assert_rtn(const char *function, const char *file, int32_t line, const char *assertion);
void radek_compat___cxa_call_unexpected(void *exceptionObject);
/* compiler-rt's arm __divmodsi4: returns {quotient, remainder} in r0:r1. */
typedef struct radek_divmodsi4_result {
    int32_t quotient;
    int32_t remainder;
} radek_divmodsi4_result;
radek_divmodsi4_result radek_compat___divmodsi4(int32_t numerator, int32_t denominator);
int32_t radek_compat___objc_personality_v0(int32_t version, int32_t actions, uint64_t exceptionClass, uintptr_t exceptionObject, uintptr_t context);
/* AAPCS returns these 2-member homogeneous float aggregates in d0:d1 / s0:s1;
 * there is no hidden struct-return pointer (LLVM lowers iOS sincos to exactly
 * this: one double in, {sin, cos} back in FP registers). */
typedef struct radek_sincos_result {
    double sin;
    double cos;
} radek_sincos_result;
typedef struct radek_sincosf_result {
    float sin;
    float cos;
} radek_sincosf_result;
radek_sincos_result radek_compat___sincos_stret(double angle);
radek_sincosf_result radek_compat___sincosf_stret(float angle);
void radek_compat_memset_pattern16(void *destination, const void *pattern16, uintptr_t length);
void radek_compat_Block_object_assign(void *destination, const void *source, int32_t flags);
void radek_compat_Block_object_dispose(const void *object, int32_t flags);
void radek_compat_stl_throw_length_error(const char *message);
void radek_compat_stl_throw_out_of_range(const char *message);
void radek_compat_rs_default_dtor(void *randomShuffleState);
uint32_t radek_compat_rs_default_call(void *randomShuffleState);
uint32_t radek_compat_rs_get(void);
void radek_compat_objc_setAssociatedObject(void *object, const void *key, void *value, int32_t policy);
void radek_compat_objc_setProperty_atomic(void *self, uintptr_t offset, void *newValue);
void radek_compat_objc_setProperty_atomic_copy(void *self, uintptr_t offset, void *newValue);
void radek_compat_objc_setProperty_nonatomic(void *self, uintptr_t offset, void *newValue);
void radek_compat_objc_setProperty_nonatomic_copy(void *self, uintptr_t offset, void *newValue);
void radek_compat_NSSetUncaughtExceptionHandler(radek_NSUncaughtExceptionHandler handler);
/* Test/diagnostic accessor backing the setter above; returns the previously
 * registered handler (or the current one when newHandler is NULL). */
radek_NSUncaughtExceptionHandler radek_compat_NSGetUncaughtExceptionHandler(
    radek_NSUncaughtExceptionHandler newHandler);

#define RADEK_IOS_SHIM_TABLE(X) \
    X("_CFAllocatorGetDefault", radek_compat_CFAllocatorGetDefault) \
    X("_CFRetain", radek_compat_CFRetain) \
    X("_CFRelease", radek_compat_CFRelease) \
    X("_CFGetRetainCount", radek_compat_CFGetRetainCount) \
    X("_CFStringCreateWithCString", radek_compat_CFStringCreateWithCString) \
    X("_CFStringGetLength", radek_compat_CFStringGetLength) \
    X("_CFStringGetCString", radek_compat_CFStringGetCString) \
    X("_CFStringGetCStringPtr", radek_compat_CFStringGetCStringPtr) \
    X("_CFStringGetMaximumSizeForEncoding", radek_compat_CFStringGetMaximumSizeForEncoding) \
    X("_CFStringCompare", radek_compat_CFStringCompare) \
    X("_CFStringGetSystemEncoding", radek_compat_CFStringGetSystemEncoding) \
    X("_CFDataCreate", radek_compat_CFDataCreate) \
    X("_CFDataGetBytePtr", radek_compat_CFDataGetBytePtr) \
    X("_CFDataGetLength", radek_compat_CFDataGetLength) \
    X("_CFArrayCreateMutable", radek_compat_CFArrayCreateMutable) \
    X("_CFArrayAppendValue", radek_compat_CFArrayAppendValue) \
    X("_CFArrayGetCount", radek_compat_CFArrayGetCount) \
    X("_CFArrayGetValueAtIndex", radek_compat_CFArrayGetValueAtIndex) \
    X("_CFDictionaryCreateMutable", radek_compat_CFDictionaryCreateMutable) \
    X("_CFDictionarySetValue", radek_compat_CFDictionarySetValue) \
    X("_CFDictionaryGetValue", radek_compat_CFDictionaryGetValue) \
    X("_CFDictionaryGetCount", radek_compat_CFDictionaryGetCount) \
    X("_CFNumberCreate", radek_compat_CFNumberCreate) \
    X("_CFNumberGetValue", radek_compat_CFNumberGetValue) \
    X("_CFDateCreate", radek_compat_CFDateCreate) \
    X("_CFDateGetAbsoluteTime", radek_compat_CFDateGetAbsoluteTime) \
    X("_CFDateGetTimeIntervalSinceDate", radek_compat_CFDateGetTimeIntervalSinceDate) \
    X("_CFAbsoluteTimeGetGregorianDate", radek_compat_CFAbsoluteTimeGetGregorianDate) \
    X("_CFRunLoopGetCurrent", radek_compat_CFRunLoopGetCurrent) \
    X("_CFRunLoopGetMain", radek_compat_CFRunLoopGetMain) \
    X("_CFRunLoopRun", radek_compat_CFRunLoopRun) \
    X("_CFRunLoopRunInMode", radek_compat_CFRunLoopRunInMode) \
    X("_CFRunLoopStop", radek_compat_CFRunLoopStop) \
    X("_CFRunLoopWakeUp", radek_compat_CFRunLoopWakeUp) \
    X("_malloc", radek_compat_malloc) \
    X("_calloc", radek_compat_calloc) \
    X("_realloc", radek_compat_realloc) \
    X("_free", radek_compat_free) \
    X("_memcpy", radek_compat_memcpy) \
    X("_memmove", radek_compat_memmove) \
    X("_memset", radek_compat_memset) \
    X("_memcmp", radek_compat_memcmp) \
    X("_memchr", radek_compat_memchr) \
    X("_strlen", radek_compat_strlen) \
    X("_strcpy", radek_compat_strcpy) \
    X("_strncpy", radek_compat_strncpy) \
    X("_strlcpy", radek_compat_strlcpy) \
    X("_strlcat", radek_compat_strlcat) \
    X("_strcmp", radek_compat_strcmp) \
    X("_strncmp", radek_compat_strncmp) \
    X("_strdup", radek_compat_strdup) \
    X("_strchr", radek_compat_strchr) \
    X("_strrchr", radek_compat_strrchr) \
    X("_strstr", radek_compat_strstr) \
    X("_strtol", radek_compat_strtol) \
    X("_strtod", radek_compat_strtod) \
    X("_atoi", radek_compat_atoi) \
    X("_atof", radek_compat_atof) \
    X("_strerror", radek_compat_strerror) \
    X("_snprintf", radek_compat_snprintf) \
    X("_vsnprintf", radek_compat_vsnprintf) \
    X("_fopen", radek_compat_fopen) \
    X("_fclose", radek_compat_fclose) \
    X("_fread", radek_compat_fread) \
    X("_fwrite", radek_compat_fwrite) \
    X("_fputs", radek_compat_fputs) \
    X("_fgets", radek_compat_fgets) \
    X("_fflush", radek_compat_fflush) \
    X("_fprintf", radek_compat_fprintf) \
    X("_printf", radek_compat_printf) \
    X("_puts", radek_compat_puts) \
    X("_remove", radek_compat_remove) \
    X("_feof", radek_compat_feof) \
    X("_ftell", radek_compat_ftell) \
    X("_fseek", radek_compat_fseek) \
    X("_time", radek_compat_time) \
    X("_gettimeofday", radek_compat_gettimeofday) \
    X("_clock_gettime", radek_compat_clock_gettime) \
    X("_nanosleep", radek_compat_nanosleep) \
    X("_localtime_r", radek_compat_localtime_r) \
    X("_gmtime_r", radek_compat_gmtime_r) \
    X("_mktime", radek_compat_mktime) \
    X("_getenv", radek_compat_getenv) \
    X("_setenv", radek_compat_setenv) \
    X("_unsetenv", radek_compat_unsetenv) \
    X("_getpid", radek_compat_getpid) \
    X("_qsort", radek_compat_qsort) \
    X("_bsearch", radek_compat_bsearch) \
    X("_abs", radek_compat_abs) \
    X("_labs", radek_compat_labs) \
    X("_rand", radek_compat_rand) \
    X("_srand", radek_compat_srand) \
    X("_sqrt", radek_compat_sqrt) \
    X("_fabs", radek_compat_fabs) \
    X("_floor", radek_compat_floor) \
    X("_ceil", radek_compat_ceil) \
    X("_pow", radek_compat_pow) \
    X("_sin", radek_compat_sin) \
    X("_cos", radek_compat_cos) \
    X("_tan", radek_compat_tan) \
    X("_atan2", radek_compat_atan2) \
    X("_fmod", radek_compat_fmod) \
    X("_pthread_mutex_init", radek_compat_pthread_mutex_init) \
    X("_pthread_mutex_lock", radek_compat_pthread_mutex_lock) \
    X("_pthread_mutex_unlock", radek_compat_pthread_mutex_unlock) \
    X("_pthread_mutex_destroy", radek_compat_pthread_mutex_destroy) \
    X("_pthread_cond_init", radek_compat_pthread_cond_init) \
    X("_pthread_cond_wait", radek_compat_pthread_cond_wait) \
    X("_pthread_cond_signal", radek_compat_pthread_cond_signal) \
    X("_pthread_cond_broadcast", radek_compat_pthread_cond_broadcast) \
    X("_pthread_cond_destroy", radek_compat_pthread_cond_destroy) \
    X("_pthread_self", radek_compat_pthread_self) \
    X("___CFConstantStringClassReference", radek_compat_CFConstantStringClassReference) \
    X("_kCFAllocatorDefault", radek_compat_CFAllocatorDefault) \
    X("_kCFBooleanTrue", radek_compat_CFBooleanTrue) \
    X("_kCFBooleanFalse", radek_compat_CFBooleanFalse) \
    X("_kCFTypeArrayCallBacks", radek_compat_CFTypeArrayCallBacks) \
    X("_kCFTypeDictionaryKeyCallBacks", radek_compat_CFTypeDictionaryKeyCallBacks) \
    X("_kCFTypeDictionaryValueCallBacks", radek_compat_CFTypeDictionaryValueCallBacks) \
    X("_kCFRunLoopDefaultMode", radek_compat_CFRunLoopDefaultMode) \
    X("_kCFRunLoopCommonModes", radek_compat_CFRunLoopCommonModes) \
    X("_CFBundleGetMainBundle", radek_compat_CFBundleGetMainBundle) \
    X("_CFBundleCopyBundleURL", radek_compat_CFBundleCopyBundleURL) \
    X("_CFBundleCopyResourcesDirectoryURL", radek_compat_CFBundleCopyResourcesDirectoryURL) \
    X("_CFBundleCopyResourceURL", radek_compat_CFBundleCopyResourceURL) \
    X("_CFBundleGetIdentifier", radek_compat_CFBundleGetIdentifier) \
    X("_CFBundleGetValueForInfoDictionaryKey", radek_compat_CFBundleGetValueForInfoDictionaryKey) \
    X("_CFURLCreateWithFileSystemPath", radek_compat_CFURLCreateWithFileSystemPath) \
    X("_CFURLCreateFromFileSystemRepresentation", radek_compat_CFURLCreateFromFileSystemRepresentation) \
    X("_CFURLGetFileSystemRepresentation", radek_compat_CFURLGetFileSystemRepresentation) \
    X("_CFURLCopyFileSystemPath", radek_compat_CFURLCopyFileSystemPath) \
    X("_CFStringCreateWithBytes", radek_compat_CFStringCreateWithBytes) \
    X("_CFStringCreateMutable", radek_compat_CFStringCreateMutable) \
    X("_CFStringAppendCString", radek_compat_CFStringAppendCString) \
    X("_CFStringHasPrefix", radek_compat_CFStringHasPrefix) \
    X("_CFStringHasSuffix", radek_compat_CFStringHasSuffix) \
    X("_CFStringGetIntValue", radek_compat_CFStringGetIntValue) \
    X("_CFStringGetDoubleValue", radek_compat_CFStringGetDoubleValue) \
    X("_CFArrayCreate", radek_compat_CFArrayCreate) \
    X("_CFArrayRemoveValueAtIndex", radek_compat_CFArrayRemoveValueAtIndex) \
    X("_CFArrayRemoveAllValues", radek_compat_CFArrayRemoveAllValues) \
    X("_CFDictionaryCreate", radek_compat_CFDictionaryCreate) \
    X("_CFDictionaryRemoveValue", radek_compat_CFDictionaryRemoveValue) \
    X("_CFDictionaryRemoveAllValues", radek_compat_CFDictionaryRemoveAllValues) \
    X("_CFDictionaryContainsKey", radek_compat_CFDictionaryContainsKey) \
    X("_CFDataCreateMutable", radek_compat_CFDataCreateMutable) \
    X("_CFDataAppendBytes", radek_compat_CFDataAppendBytes) \
    X("_CFDataGetMutableBytePtr", radek_compat_CFDataGetMutableBytePtr) \
    X("_CFDataGetBytes", radek_compat_CFDataGetBytes) \
    X("_CFBooleanGetValue", radek_compat_CFBooleanGetValue) \
    X("_CFEqual", radek_compat_CFEqual) \
    X("_CFHash", radek_compat_CFHash) \
    X("_CFGetTypeID", radek_compat_CFGetTypeID) \
    X("_CFPreferencesCopyAppValue", radek_compat_CFPreferencesCopyAppValue) \
    X("_CFPreferencesSetAppValue", radek_compat_CFPreferencesSetAppValue) \
    X("_CFPreferencesAppSynchronize", radek_compat_CFPreferencesAppSynchronize) \
    X("_CFUUIDCreate", radek_compat_CFUUIDCreate) \
    X("_CFUUIDCreateString", radek_compat_CFUUIDCreateString) \
    X("_CFLocaleCopyCurrent", radek_compat_CFLocaleCopyCurrent) \
    X("_CFLocaleCopyPreferredLanguages", radek_compat_CFLocaleCopyPreferredLanguages) \
    X("_CFLocaleGetIdentifier", radek_compat_CFLocaleGetIdentifier) \
    X("_CFTimeZoneCopySystem", radek_compat_CFTimeZoneCopySystem) \
    X("_AudioSessionInitialize", radek_compat_AudioSessionInitialize) \
    X("_AudioSessionSetActive", radek_compat_AudioSessionSetActive) \
    X("_NSSearchPathForDirectoriesInDomains", radek_compat_NSSearchPathForDirectoriesInDomains) \
    X("_OBJC_CLASS_$_CAEAGLLayer", radek_compat_OBJC_CLASS___CAEAGLLayer) \
    X("_OBJC_CLASS_$_EAGLContext", radek_compat_OBJC_CLASS___EAGLContext) \
    X("_OBJC_CLASS_$_NSAutoreleasePool", radek_compat_OBJC_CLASS___NSAutoreleasePool) \
    X("_OBJC_CLASS_$_NSBundle", radek_compat_OBJC_CLASS___NSBundle) \
    X("_OBJC_CLASS_$_NSDictionary", radek_compat_OBJC_CLASS___NSDictionary) \
    X("_OBJC_CLASS_$_NSNumber", radek_compat_OBJC_CLASS___NSNumber) \
    X("_OBJC_CLASS_$_NSObject", radek_compat_OBJC_CLASS___NSObject) \
    X("_OBJC_CLASS_$_NSString", radek_compat_OBJC_CLASS___NSString) \
    X("_OBJC_CLASS_$_NSThread", radek_compat_OBJC_CLASS___NSThread) \
    X("_OBJC_CLASS_$_NSURL", radek_compat_OBJC_CLASS___NSURL) \
    X("_OBJC_CLASS_$_UIAccelerometer", radek_compat_OBJC_CLASS___UIAccelerometer) \
    X("_OBJC_CLASS_$_UIApplication", radek_compat_OBJC_CLASS___UIApplication) \
    X("_OBJC_CLASS_$_UIScreen", radek_compat_OBJC_CLASS___UIScreen) \
    X("_OBJC_CLASS_$_UIView", radek_compat_OBJC_CLASS___UIView) \
    X("_OBJC_CLASS_$_UIWindow", radek_compat_OBJC_CLASS___UIWindow) \
    X("_OBJC_METACLASS_$_NSObject", radek_compat_OBJC_METACLASS___NSObject) \
    X("_OBJC_METACLASS_$_UIView", radek_compat_OBJC_METACLASS___UIView) \
    X("_UIApplicationMain", radek_compat_UIApplicationMain) \
    X("__DefaultRuneLocale", radek_compat__DefaultRuneLocale) \
    X("__Unwind_SjLj_Register", radek_compat__Unwind_SjLj_Register) \
    X("__Unwind_SjLj_Resume", radek_compat__Unwind_SjLj_Resume) \
    X("__Unwind_SjLj_Unregister", radek_compat__Unwind_SjLj_Unregister) \
    X("__ZSt9terminatev", radek_compat__ZSt9terminatev) \
    X("__ZTVN10__cxxabiv117__class_type_infoE", radek_compat__ZTVN10__cxxabiv117__class_type_infoE) \
    X("__ZTVN10__cxxabiv119__pointer_type_infoE", radek_compat__ZTVN10__cxxabiv119__pointer_type_infoE) \
    X("__ZTVN10__cxxabiv120__si_class_type_infoE", radek_compat__ZTVN10__cxxabiv120__si_class_type_infoE) \
    X("__ZTVN10__cxxabiv121__vmi_class_type_infoE", radek_compat__ZTVN10__cxxabiv121__vmi_class_type_infoE) \
    X("__ZdaPv", radek_compat__ZdaPv) \
    X("__ZdlPv", radek_compat__ZdlPv) \
    X("__Znam", radek_compat__Znam) \
    X("__Znwm", radek_compat__Znwm) \
    X("___cxa_allocate_exception", radek_compat___cxa_allocate_exception) \
    X("___cxa_atexit", radek_compat___cxa_atexit) \
    X("___cxa_begin_catch", radek_compat___cxa_begin_catch) \
    X("___cxa_end_catch", radek_compat___cxa_end_catch) \
    X("___cxa_pure_virtual", radek_compat___cxa_pure_virtual) \
    X("___cxa_throw", radek_compat___cxa_throw) \
    X("___divdi3", radek_compat___divdi3) \
    X("___divsi3", radek_compat___divsi3) \
    X("___error", radek_compat___error) \
    X("___fixdfdi", radek_compat___fixdfdi) \
    X("___floatdidf", radek_compat___floatdidf) \
    X("___floatdisf", radek_compat___floatdisf) \
    X("___gxx_personality_sj0", radek_compat___gxx_personality_sj0) \
    X("___maskrune", radek_compat___maskrune) \
    X("___moddi3", radek_compat___moddi3) \
    X("___modsi3", radek_compat___modsi3) \
    X("___stderrp", radek_compat___stderrp) \
    X("___stdinp", radek_compat___stdinp) \
    X("___stdoutp", radek_compat___stdoutp) \
    X("___tolower", radek_compat___tolower) \
    X("___toupper", radek_compat___toupper) \
    X("___udivsi3", radek_compat___udivsi3) \
    X("___umodsi3", radek_compat___umodsi3) \
    X("__objc_empty_cache", radek_compat__objc_empty_cache) \
    X("__objc_empty_vtable", radek_compat__objc_empty_vtable) \
    X("_abort", radek_compat_abort) \
    X("_acosf", radek_compat_acosf) \
    X("_alBufferData", radek_compat_alBufferData) \
    X("_alDeleteBuffers", radek_compat_alDeleteBuffers) \
    X("_alDeleteSources", radek_compat_alDeleteSources) \
    X("_alGenBuffers", radek_compat_alGenBuffers) \
    X("_alGenSources", radek_compat_alGenSources) \
    X("_alGetSourcef", radek_compat_alGetSourcef) \
    X("_alGetSourcei", radek_compat_alGetSourcei) \
    X("_alSource3f", radek_compat_alSource3f) \
    X("_alSourcePlay", radek_compat_alSourcePlay) \
    X("_alSourceQueueBuffers", radek_compat_alSourceQueueBuffers) \
    X("_alSourceStop", radek_compat_alSourceStop) \
    X("_alSourceUnqueueBuffers", radek_compat_alSourceUnqueueBuffers) \
    X("_alSourcef", radek_compat_alSourcef) \
    X("_alSourcei", radek_compat_alSourcei) \
    X("_alcCloseDevice", radek_compat_alcCloseDevice) \
    X("_alcCreateContext", radek_compat_alcCreateContext) \
    X("_alcDestroyContext", radek_compat_alcDestroyContext) \
    X("_alcMakeContextCurrent", radek_compat_alcMakeContextCurrent) \
    X("_alcOpenDevice", radek_compat_alcOpenDevice) \
    X("_asinf", radek_compat_asinf) \
    X("_atan2f", radek_compat_atan2f) \
    X("_atanf", radek_compat_atanf) \
    X("_ceilf", radek_compat_ceilf) \
    X("_clearerr", radek_compat_clearerr) \
    X("_clock", radek_compat_clock) \
    X("_close", radek_compat_close) \
    X("_cosf", radek_compat_cosf) \
    X("_coshf", radek_compat_coshf) \
    X("_difftime", radek_compat_difftime) \
    X("_exit", radek_compat_exit) \
    X("_expf", radek_compat_expf) \
    X("_fcntl", radek_compat_fcntl) \
    X("_ferror", radek_compat_ferror) \
    X("_floorf", radek_compat_floorf) \
    X("_fputc", radek_compat_fputc) \
    X("_freopen", radek_compat_freopen) \
    X("_frexp", radek_compat_frexp) \
    X("_fscanf", radek_compat_fscanf) \
    X("_getc", radek_compat_getc) \
    X("_glActiveTexture", radek_compat_glActiveTexture) \
    X("_glBindBuffer", radek_compat_glBindBuffer) \
    X("_glBindFramebufferOES", radek_compat_glBindFramebufferOES) \
    X("_glBindRenderbufferOES", radek_compat_glBindRenderbufferOES) \
    X("_glBindTexture", radek_compat_glBindTexture) \
    X("_glBlendFunc", radek_compat_glBlendFunc) \
    X("_glBufferData", radek_compat_glBufferData) \
    X("_glCheckFramebufferStatusOES", radek_compat_glCheckFramebufferStatusOES) \
    X("_glClear", radek_compat_glClear) \
    X("_glClearColor", radek_compat_glClearColor) \
    X("_glClientActiveTexture", radek_compat_glClientActiveTexture) \
    X("_glColor4f", radek_compat_glColor4f) \
    X("_glColorPointer", radek_compat_glColorPointer) \
    X("_glCompressedTexImage2D", radek_compat_glCompressedTexImage2D) \
    X("_glDeleteBuffers", radek_compat_glDeleteBuffers) \
    X("_glDeleteFramebuffersOES", radek_compat_glDeleteFramebuffersOES) \
    X("_glDeleteRenderbuffersOES", radek_compat_glDeleteRenderbuffersOES) \
    X("_glDeleteTextures", radek_compat_glDeleteTextures) \
    X("_glDepthFunc", radek_compat_glDepthFunc) \
    X("_glDepthMask", radek_compat_glDepthMask) \
    X("_glDisable", radek_compat_glDisable) \
    X("_glDisableClientState", radek_compat_glDisableClientState) \
    X("_glDrawArrays", radek_compat_glDrawArrays) \
    X("_glDrawElements", radek_compat_glDrawElements) \
    X("_glEnable", radek_compat_glEnable) \
    X("_glEnableClientState", radek_compat_glEnableClientState) \
    X("_glFramebufferRenderbufferOES", radek_compat_glFramebufferRenderbufferOES) \
    X("_glFramebufferTexture2DOES", radek_compat_glFramebufferTexture2DOES) \
    X("_glFrontFace", radek_compat_glFrontFace) \
    X("_glGenBuffers", radek_compat_glGenBuffers) \
    X("_glGenFramebuffersOES", radek_compat_glGenFramebuffersOES) \
    X("_glGenRenderbuffersOES", radek_compat_glGenRenderbuffersOES) \
    X("_glGenTextures", radek_compat_glGenTextures) \
    X("_glGetIntegerv", radek_compat_glGetIntegerv) \
    X("_glGetRenderbufferParameterivOES", radek_compat_glGetRenderbufferParameterivOES) \
    X("_glLightfv", radek_compat_glLightfv) \
    X("_glLineWidth", radek_compat_glLineWidth) \
    X("_glLoadMatrixf", radek_compat_glLoadMatrixf) \
    X("_glMaterialfv", radek_compat_glMaterialfv) \
    X("_glMatrixMode", radek_compat_glMatrixMode) \
    X("_glNormalPointer", radek_compat_glNormalPointer) \
    X("_glPixelStorei", radek_compat_glPixelStorei) \
    X("_glRenderbufferStorageOES", radek_compat_glRenderbufferStorageOES) \
    X("_glScissor", radek_compat_glScissor) \
    X("_glTexCoordPointer", radek_compat_glTexCoordPointer) \
    X("_glTexEnvi", radek_compat_glTexEnvi) \
    X("_glTexImage2D", radek_compat_glTexImage2D) \
    X("_glTexParameteri", radek_compat_glTexParameteri) \
    X("_glTexSubImage2D", radek_compat_glTexSubImage2D) \
    X("_glVertexPointer", radek_compat_glVertexPointer) \
    X("_glViewport", radek_compat_glViewport) \
    X("_gmtime", radek_compat_gmtime) \
    X("_kEAGLColorFormatRGB565", radek_compat_kEAGLColorFormatRGB565) \
    X("_kEAGLColorFormatRGBA8", radek_compat_kEAGLColorFormatRGBA8) \
    X("_kEAGLDrawablePropertyColorFormat", radek_compat_kEAGLDrawablePropertyColorFormat) \
    X("_kEAGLDrawablePropertyRetainedBacking", radek_compat_kEAGLDrawablePropertyRetainedBacking) \
    X("_ldexp", radek_compat_ldexp) \
    X("_localeconv", radek_compat_localeconv) \
    X("_localtime", radek_compat_localtime) \
    X("_log10f", radek_compat_log10f) \
    X("_logf", radek_compat_logf) \
    X("_longjmp", radek_compat_longjmp) \
    X("_lseek", radek_compat_lseek) \
    X("_modf", radek_compat_modf) \
    X("_objc_enumerationMutation", radek_compat_objc_enumerationMutation) \
    X("_objc_msgSend", radek_compat_objc_msgSend) \
    X("_objc_msgSendSuper2", radek_compat_objc_msgSendSuper2) \
    X("_objc_msgSend_stret", radek_compat_objc_msgSend_stret) \
    X("_objc_setProperty", radek_compat_objc_setProperty) \
    X("_pthread_create", radek_compat_pthread_create) \
    X("_pthread_exit", radek_compat_pthread_exit) \
    X("_pthread_getschedparam", radek_compat_pthread_getschedparam) \
    X("_pthread_join", radek_compat_pthread_join) \
    X("_pthread_mutex_trylock", radek_compat_pthread_mutex_trylock) \
    X("_pthread_mutexattr_destroy", radek_compat_pthread_mutexattr_destroy) \
    X("_pthread_mutexattr_init", radek_compat_pthread_mutexattr_init) \
    X("_pthread_mutexattr_settype", radek_compat_pthread_mutexattr_settype) \
    X("_pthread_setschedparam", radek_compat_pthread_setschedparam) \
    X("_read", radek_compat_read) \
    X("_rename", radek_compat_rename) \
    X("_sched_yield", radek_compat_sched_yield) \
    X("_select", radek_compat_select) \
    X("_setjmp", radek_compat_setjmp) \
    X("_setlocale", radek_compat_setlocale) \
    X("_setvbuf", radek_compat_setvbuf) \
    X("_sinf", radek_compat_sinf) \
    X("_sinhf", radek_compat_sinhf) \
    X("_sprintf", radek_compat_sprintf) \
    X("_strcasecmp", radek_compat_strcasecmp) \
    X("_strcat", radek_compat_strcat) \
    X("_strcoll", radek_compat_strcoll) \
    X("_strcspn", radek_compat_strcspn) \
    X("_strftime", radek_compat_strftime) \
    X("_strncat", radek_compat_strncat) \
    X("_strpbrk", radek_compat_strpbrk) \
    X("_strtok", radek_compat_strtok) \
    X("_strtoul", radek_compat_strtoul) \
    X("_system", radek_compat_system) \
    X("_tanf", radek_compat_tanf) \
    X("_tanhf", radek_compat_tanhf) \
    X("_tmpfile", radek_compat_tmpfile) \
    X("_tmpnam", radek_compat_tmpnam) \
    X("_ungetc", radek_compat_ungetc) \
    X("_usleep", radek_compat_usleep) \
    X("_vsprintf", radek_compat_vsprintf) \
    X("__exit", radek_compat__exit) \
    X("_atexit", radek_compat_atexit) \
    X("_sscanf", radek_compat_sscanf) \
    X("_putchar", radek_compat_putchar) \
    X("_getchar", radek_compat_getchar) \
    X("_fgetc", radek_compat_fgetc) \
    X("_putc", radek_compat_putc) \
    X("_rewind", radek_compat_rewind) \
    X("_fileno", radek_compat_fileno) \
    X("_fdopen", radek_compat_fdopen) \
    X("_perror", radek_compat_perror) \
    X("_tzset", radek_compat_tzset) \
    X("_sleep", radek_compat_sleep) \
    X("_open", radek_compat_open) \
    X("_write", radek_compat_write) \
    X("_unlink", radek_compat_unlink) \
    X("_mkdir", radek_compat_mkdir) \
    X("_rmdir", radek_compat_rmdir) \
    X("_access", radek_compat_access) \
    X("_getcwd", radek_compat_getcwd) \
    X("_chdir", radek_compat_chdir) \
    X("_stat", radek_compat_stat) \
    X("_fstat", radek_compat_fstat) \
    X("_lstat", radek_compat_lstat) \
    X("_opendir", radek_compat_opendir) \
    X("_readdir", radek_compat_readdir) \
    X("_closedir", radek_compat_closedir) \
    X("_mmap", radek_compat_mmap) \
    X("_munmap", radek_compat_munmap) \
    X("_mprotect", radek_compat_mprotect) \
    X("_poll", radek_compat_poll) \
    X("_pipe", radek_compat_pipe) \
    X("_dup", radek_compat_dup) \
    X("_dup2", radek_compat_dup2) \
    X("_fsync", radek_compat_fsync) \
    X("_ftruncate", radek_compat_ftruncate) \
    X("_truncate", radek_compat_truncate) \
    X("_chmod", radek_compat_chmod) \
    X("_umask", radek_compat_umask) \
    X("_getuid", radek_compat_getuid) \
    X("_geteuid", radek_compat_geteuid) \
    X("_getgid", radek_compat_getgid) \
    X("_getegid", radek_compat_getegid) \
    X("_getppid", radek_compat_getppid) \
    X("_sysconf", radek_compat_sysconf) \
    X("_sysctl", radek_compat_sysctl) \
    X("_sysctlbyname", radek_compat_sysctlbyname) \
    X("_getpagesize", radek_compat_getpagesize) \
    X("__setjmp", radek_compat__setjmp) \
    X("__longjmp", radek_compat__longjmp) \
    X("_sigaction", radek_compat_sigaction) \
    X("_signal", radek_compat_signal) \
    X("_raise", radek_compat_raise) \
    X("_kill", radek_compat_kill) \
    X("_tolower", radek_compat_tolower) \
    X("_toupper", radek_compat_toupper) \
    X("_isalpha", radek_compat_isalpha) \
    X("_isdigit", radek_compat_isdigit) \
    X("_isalnum", radek_compat_isalnum) \
    X("_isspace", radek_compat_isspace) \
    X("_isupper", radek_compat_isupper) \
    X("_islower", radek_compat_islower) \
    X("_isxdigit", radek_compat_isxdigit) \
    X("_strncasecmp", radek_compat_strncasecmp) \
    X("_strspn", radek_compat_strspn) \
    X("_strtok_r", radek_compat_strtok_r) \
    X("_strtoll", radek_compat_strtoll) \
    X("_strtoull", radek_compat_strtoull) \
    X("_strtof", radek_compat_strtof) \
    X("_atol", radek_compat_atol) \
    X("_atoll", radek_compat_atoll) \
    X("_llabs", radek_compat_llabs) \
    X("_bzero", radek_compat_bzero) \
    X("_bcopy", radek_compat_bcopy) \
    X("_bcmp", radek_compat_bcmp) \
    X("_acos", radek_compat_acos) \
    X("_asin", radek_compat_asin) \
    X("_atan", radek_compat_atan) \
    X("_cosh", radek_compat_cosh) \
    X("_sinh", radek_compat_sinh) \
    X("_tanh", radek_compat_tanh) \
    X("_exp", radek_compat_exp) \
    X("_log", radek_compat_log) \
    X("_log10", radek_compat_log10) \
    X("_log2", radek_compat_log2) \
    X("_hypot", radek_compat_hypot) \
    X("_hypotf", radek_compat_hypotf) \
    X("_cbrt", radek_compat_cbrt) \
    X("_round", radek_compat_round) \
    X("_roundf", radek_compat_roundf) \
    X("_trunc", radek_compat_trunc) \
    X("_truncf", radek_compat_truncf) \
    X("_lround", radek_compat_lround) \
    X("_lroundf", radek_compat_lroundf) \
    X("_frexpf", radek_compat_frexpf) \
    X("_ldexpf", radek_compat_ldexpf) \
    X("_log2f", radek_compat_log2f) \
    X("_modff", radek_compat_modff) \
    X("_powf", radek_compat_powf) \
    X("_sqrtf", radek_compat_sqrtf) \
    X("_fabsf", radek_compat_fabsf) \
    X("_fmodf", radek_compat_fmodf) \
    X("_pthread_detach", radek_compat_pthread_detach) \
    X("_pthread_equal", radek_compat_pthread_equal) \
    X("_pthread_once", radek_compat_pthread_once) \
    X("_pthread_cond_timedwait", radek_compat_pthread_cond_timedwait) \
    X("_pthread_key_create", radek_compat_pthread_key_create) \
    X("_pthread_key_delete", radek_compat_pthread_key_delete) \
    X("_pthread_setspecific", radek_compat_pthread_setspecific) \
    X("_pthread_getspecific", radek_compat_pthread_getspecific) \
    X("_pthread_rwlock_init", radek_compat_pthread_rwlock_init) \
    X("_pthread_rwlock_rdlock", radek_compat_pthread_rwlock_rdlock) \
    X("_pthread_rwlock_wrlock", radek_compat_pthread_rwlock_wrlock) \
    X("_pthread_rwlock_unlock", radek_compat_pthread_rwlock_unlock) \
    X("_pthread_rwlock_destroy", radek_compat_pthread_rwlock_destroy) \
    X("_sem_init", radek_compat_sem_init) \
    X("_sem_destroy", radek_compat_sem_destroy) \
    X("_sem_wait", radek_compat_sem_wait) \
    X("_sem_trywait", radek_compat_sem_trywait) \
    X("_sem_post", radek_compat_sem_post) \
    X("_dlopen", radek_compat_dlopen) \
    X("_dlsym", radek_compat_dlsym) \
    X("_dlclose", radek_compat_dlclose) \
    X("_dlerror", radek_compat_dlerror) \
    X("_socket", radek_compat_socket) \
    X("_connect", radek_compat_connect) \
    X("_bind", radek_compat_bind) \
    X("_listen", radek_compat_listen) \
    X("_accept", radek_compat_accept) \
    X("_send", radek_compat_send) \
    X("_sendto", radek_compat_sendto) \
    X("_recv", radek_compat_recv) \
    X("_recvfrom", radek_compat_recvfrom) \
    X("_setsockopt", radek_compat_setsockopt) \
    X("_getsockopt", radek_compat_getsockopt) \
    X("_getsockname", radek_compat_getsockname) \
    X("_getpeername", radek_compat_getpeername) \
    X("_shutdown", radek_compat_shutdown) \
    X("_getaddrinfo", radek_compat_getaddrinfo) \
    X("_freeaddrinfo", radek_compat_freeaddrinfo) \
    X("_gethostbyname", radek_compat_gethostbyname) \
    X("_inet_ntop", radek_compat_inet_ntop) \
    X("_inet_pton", radek_compat_inet_pton) \
    X("_inet_addr", radek_compat_inet_addr) \
    X("_inet_ntoa", radek_compat_inet_ntoa) \
    X("_htons", radek_compat_htons) \
    X("_htonl", radek_compat_htonl) \
    X("_ntohs", radek_compat_ntohs) \
    X("_ntohl", radek_compat_ntohl) \
    X("_crc32", radek_compat_crc32) \
    X("_adler32", radek_compat_adler32) \
    X("_compress", radek_compat_compress) \
    X("_compress2", radek_compat_compress2) \
    X("_uncompress", radek_compat_uncompress) \
    X("_deflateInit_", radek_compat_deflateInit_) \
    X("_deflateInit2_", radek_compat_deflateInit2_) \
    X("_deflate", radek_compat_deflate) \
    X("_deflateEnd", radek_compat_deflateEnd) \
    X("_deflateReset", radek_compat_deflateReset) \
    X("_inflateInit_", radek_compat_inflateInit_) \
    X("_inflateInit2_", radek_compat_inflateInit2_) \
    X("_inflate", radek_compat_inflate) \
    X("_inflateEnd", radek_compat_inflateEnd) \
    X("_inflateReset", radek_compat_inflateReset) \
    X("_gzopen", radek_compat_gzopen) \
    X("_gzread", radek_compat_gzread) \
    X("_gzwrite", radek_compat_gzwrite) \
    X("_gzclose", radek_compat_gzclose) \
    X("_alDistanceModel", radek_compat_alDistanceModel) \
    X("_alDopplerFactor", radek_compat_alDopplerFactor) \
    X("_alDopplerVelocity", radek_compat_alDopplerVelocity) \
    X("_alSpeedOfSound", radek_compat_alSpeedOfSound) \
    X("_alGetError", radek_compat_alGetError) \
    X("_alGetSource3f", radek_compat_alGetSource3f) \
    X("_alGetSourcefv", radek_compat_alGetSourcefv) \
    X("_alSourcefv", radek_compat_alSourcefv) \
    X("_alSourcePause", radek_compat_alSourcePause) \
    X("_alSourceRewind", radek_compat_alSourceRewind) \
    X("_alListener3f", radek_compat_alListener3f) \
    X("_alListenerf", radek_compat_alListenerf) \
    X("_alListenerfv", radek_compat_alListenerfv) \
    X("_alListeneri", radek_compat_alListeneri) \
    X("_alGetListenerf", radek_compat_alGetListenerf) \
    X("_alGetListener3f", radek_compat_alGetListener3f) \
    X("_alGetListenerfv", radek_compat_alGetListenerfv) \
    X("_alEnable", radek_compat_alEnable) \
    X("_alDisable", radek_compat_alDisable) \
    X("_alIsEnabled", radek_compat_alIsEnabled) \
    X("_alIsBuffer", radek_compat_alIsBuffer) \
    X("_alIsSource", radek_compat_alIsSource) \
    X("_alGetBoolean", radek_compat_alGetBoolean) \
    X("_alGetInteger", radek_compat_alGetInteger) \
    X("_alGetFloat", radek_compat_alGetFloat) \
    X("_alGetDouble", radek_compat_alGetDouble) \
    X("_alGetString", radek_compat_alGetString) \
    X("_alGetEnumValue", radek_compat_alGetEnumValue) \
    X("_alGetProcAddress", radek_compat_alGetProcAddress) \
    X("_alIsExtensionPresent", radek_compat_alIsExtensionPresent) \
    X("_alcGetContextsDevice", radek_compat_alcGetContextsDevice) \
    X("_alcGetCurrentContext", radek_compat_alcGetCurrentContext) \
    X("_alcProcessContext", radek_compat_alcProcessContext) \
    X("_alcSuspendContext", radek_compat_alcSuspendContext) \
    X("_alcGetError", radek_compat_alcGetError) \
    X("_alcGetIntegerv", radek_compat_alcGetIntegerv) \
    X("_alcGetString", radek_compat_alcGetString) \
    X("_alcIsExtensionPresent", radek_compat_alcIsExtensionPresent) \
    X("_alcGetProcAddress", radek_compat_alcGetProcAddress) \
    X("_AudioSessionSetActiveWithFlags", radek_compat_AudioSessionSetActiveWithFlags) \
    X("_AudioSessionGetProperty", radek_compat_AudioSessionGetProperty) \
    X("_AudioSessionSetProperty", radek_compat_AudioSessionSetProperty) \
    X("_AudioSessionGetPropertySize", radek_compat_AudioSessionGetPropertySize) \
    X("_AudioSessionAddPropertyListener", radek_compat_AudioSessionAddPropertyListener) \
    X("_AudioSessionRemovePropertyListenerWithUserData", radek_compat_AudioSessionRemovePropertyListenerWithUserData) \
    X("_AudioServicesPlaySystemSound", radek_compat_AudioServicesPlaySystemSound) \
    X("_AudioServicesPlayAlertSound", radek_compat_AudioServicesPlayAlertSound) \
    X("_AudioServicesCreateSystemSoundID", radek_compat_AudioServicesCreateSystemSoundID) \
    X("_AudioServicesDisposeSystemSoundID", radek_compat_AudioServicesDisposeSystemSoundID) \
    X("_AudioFileOpenURL", radek_compat_AudioFileOpenURL) \
    X("_AudioFileClose", radek_compat_AudioFileClose) \
    X("_AudioFileGetProperty", radek_compat_AudioFileGetProperty) \
    X("_AudioFileReadBytes", radek_compat_AudioFileReadBytes) \
    X("_AudioFileReadPackets", radek_compat_AudioFileReadPackets) \
    X("_ExtAudioFileOpenURL", radek_compat_ExtAudioFileOpenURL) \
    X("_ExtAudioFileDispose", radek_compat_ExtAudioFileDispose) \
    X("_ExtAudioFileGetProperty", radek_compat_ExtAudioFileGetProperty) \
    X("_ExtAudioFileSetProperty", radek_compat_ExtAudioFileSetProperty) \
    X("_ExtAudioFileRead", radek_compat_ExtAudioFileRead) \
    X("_ExtAudioFileSeek", radek_compat_ExtAudioFileSeek) \
    X("_AudioQueueNewOutput", radek_compat_AudioQueueNewOutput) \
    X("_AudioQueueAllocateBuffer", radek_compat_AudioQueueAllocateBuffer) \
    X("_AudioQueueFreeBuffer", radek_compat_AudioQueueFreeBuffer) \
    X("_AudioQueueEnqueueBuffer", radek_compat_AudioQueueEnqueueBuffer) \
    X("_AudioQueueStart", radek_compat_AudioQueueStart) \
    X("_AudioQueuePause", radek_compat_AudioQueuePause) \
    X("_AudioQueueStop", radek_compat_AudioQueueStop) \
    X("_AudioQueueDispose", radek_compat_AudioQueueDispose) \
    X("_AudioQueueSetParameter", radek_compat_AudioQueueSetParameter) \
    X("_AudioComponentFindNext", radek_compat_AudioComponentFindNext) \
    X("_AudioComponentInstanceNew", radek_compat_AudioComponentInstanceNew) \
    X("_AudioComponentInstanceDispose", radek_compat_AudioComponentInstanceDispose) \
    X("_AudioUnitInitialize", radek_compat_AudioUnitInitialize) \
    X("_AudioUnitUninitialize", radek_compat_AudioUnitUninitialize) \
    X("_AudioUnitSetProperty", radek_compat_AudioUnitSetProperty) \
    X("_AudioUnitGetProperty", radek_compat_AudioUnitGetProperty) \
    X("_AudioOutputUnitStart", radek_compat_AudioOutputUnitStart) \
    X("_AudioOutputUnitStop", radek_compat_AudioOutputUnitStop) \
    X("_AudioUnitRender", radek_compat_AudioUnitRender) \
    X("_glAlphaFunc", radek_compat_glAlphaFunc) \
    X("_glBindFramebuffer", radek_compat_glBindFramebuffer) \
    X("_glBindRenderbuffer", radek_compat_glBindRenderbuffer) \
    X("_glBlendEquation", radek_compat_glBlendEquation) \
    X("_glBlendEquationOES", radek_compat_glBlendEquationOES) \
    X("_glBlendFuncSeparate", radek_compat_glBlendFuncSeparate) \
    X("_glBufferSubData", radek_compat_glBufferSubData) \
    X("_glCheckFramebufferStatus", radek_compat_glCheckFramebufferStatus) \
    X("_glClearDepthf", radek_compat_glClearDepthf) \
    X("_glClearStencil", radek_compat_glClearStencil) \
    X("_glColor4ub", radek_compat_glColor4ub) \
    X("_glColorMask", radek_compat_glColorMask) \
    X("_glCompileShader", radek_compat_glCompileShader) \
    X("_glCopyTexImage2D", radek_compat_glCopyTexImage2D) \
    X("_glCopyTexSubImage2D", radek_compat_glCopyTexSubImage2D) \
    X("_glCreateProgram", radek_compat_glCreateProgram) \
    X("_glCreateShader", radek_compat_glCreateShader) \
    X("_glCullFace", radek_compat_glCullFace) \
    X("_glDeleteFramebuffers", radek_compat_glDeleteFramebuffers) \
    X("_glDeleteProgram", radek_compat_glDeleteProgram) \
    X("_glDeleteRenderbuffers", radek_compat_glDeleteRenderbuffers) \
    X("_glDeleteShader", radek_compat_glDeleteShader) \
    X("_glDepthRangef", radek_compat_glDepthRangef) \
    X("_glDisableVertexAttribArray", radek_compat_glDisableVertexAttribArray) \
    X("_glEnableVertexAttribArray", radek_compat_glEnableVertexAttribArray) \
    X("_glFinish", radek_compat_glFinish) \
    X("_glFlush", radek_compat_glFlush) \
    X("_glFogf", radek_compat_glFogf) \
    X("_glFogfv", radek_compat_glFogfv) \
    X("_glFramebufferRenderbuffer", radek_compat_glFramebufferRenderbuffer) \
    X("_glFramebufferTexture2D", radek_compat_glFramebufferTexture2D) \
    X("_glFrustumf", radek_compat_glFrustumf) \
    X("_glGenFramebuffers", radek_compat_glGenFramebuffers) \
    X("_glGenRenderbuffers", radek_compat_glGenRenderbuffers) \
    X("_glGenerateMipmap", radek_compat_glGenerateMipmap) \
    X("_glGenerateMipmapOES", radek_compat_glGenerateMipmapOES) \
    X("_glGetAttribLocation", radek_compat_glGetAttribLocation) \
    X("_glGetError", radek_compat_glGetError) \
    X("_glGetFloatv", radek_compat_glGetFloatv) \
    X("_glGetProgramInfoLog", radek_compat_glGetProgramInfoLog) \
    X("_glGetProgramiv", radek_compat_glGetProgramiv) \
    X("_glGetRenderbufferParameteriv", radek_compat_glGetRenderbufferParameteriv) \
    X("_glGetShaderInfoLog", radek_compat_glGetShaderInfoLog) \
    X("_glGetShaderiv", radek_compat_glGetShaderiv) \
    X("_glGetString", radek_compat_glGetString) \
    X("_glGetUniformLocation", radek_compat_glGetUniformLocation) \
    X("_glHint", radek_compat_glHint) \
    X("_glIsEnabled", radek_compat_glIsEnabled) \
    X("_glIsTexture", radek_compat_glIsTexture) \
    X("_glLightModelfv", radek_compat_glLightModelfv) \
    X("_glLinkProgram", radek_compat_glLinkProgram) \
    X("_glLoadIdentity", radek_compat_glLoadIdentity) \
    X("_glLogicOp", radek_compat_glLogicOp) \
    X("_glMaterialf", radek_compat_glMaterialf) \
    X("_glMultMatrixf", radek_compat_glMultMatrixf) \
    X("_glNormal3f", radek_compat_glNormal3f) \
    X("_glOrthof", radek_compat_glOrthof) \
    X("_glPointParameterf", radek_compat_glPointParameterf) \
    X("_glPointParameterfv", radek_compat_glPointParameterfv) \
    X("_glPointSize", radek_compat_glPointSize) \
    X("_glPolygonOffset", radek_compat_glPolygonOffset) \
    X("_glPopMatrix", radek_compat_glPopMatrix) \
    X("_glPushMatrix", radek_compat_glPushMatrix) \
    X("_glReadPixels", radek_compat_glReadPixels) \
    X("_glRenderbufferStorage", radek_compat_glRenderbufferStorage) \
    X("_glRotatef", radek_compat_glRotatef) \
    X("_glScalef", radek_compat_glScalef) \
    X("_glShadeModel", radek_compat_glShadeModel) \
    X("_glShaderSource", radek_compat_glShaderSource) \
    X("_glStencilFunc", radek_compat_glStencilFunc) \
    X("_glStencilMask", radek_compat_glStencilMask) \
    X("_glStencilOp", radek_compat_glStencilOp) \
    X("_glTexEnvf", radek_compat_glTexEnvf) \
    X("_glTexEnvfv", radek_compat_glTexEnvfv) \
    X("_glTexParameterf", radek_compat_glTexParameterf) \
    X("_glTexParameterfv", radek_compat_glTexParameterfv) \
    X("_glTranslatef", radek_compat_glTranslatef) \
    X("_glUniform1f", radek_compat_glUniform1f) \
    X("_glUniform1i", radek_compat_glUniform1i) \
    X("_glUniform2f", radek_compat_glUniform2f) \
    X("_glUniform3f", radek_compat_glUniform3f) \
    X("_glUniform4f", radek_compat_glUniform4f) \
    X("_glUniformMatrix4fv", radek_compat_glUniformMatrix4fv) \
    X("_glUseProgram", radek_compat_glUseProgram) \
    X("_glVertexAttribPointer", radek_compat_glVertexAttribPointer) \
    X("_eglGetDisplay", radek_compat_eglGetDisplay) \
    X("_eglInitialize", radek_compat_eglInitialize) \
    X("_eglChooseConfig", radek_compat_eglChooseConfig) \
    X("_eglCreateWindowSurface", radek_compat_eglCreateWindowSurface) \
    X("_eglCreateContext", radek_compat_eglCreateContext) \
    X("_eglMakeCurrent", radek_compat_eglMakeCurrent) \
    X("_eglSwapBuffers", radek_compat_eglSwapBuffers) \
    X("_eglDestroyContext", radek_compat_eglDestroyContext) \
    X("_eglDestroySurface", radek_compat_eglDestroySurface) \
    X("_eglTerminate", radek_compat_eglTerminate) \
    X("_eglGetError", radek_compat_eglGetError) \
    X("_eglGetProcAddress", radek_compat_eglGetProcAddress) \
    X("_CGColorSpaceCreateDeviceRGB", radek_compat_CGColorSpaceCreateDeviceRGB) \
    X("_CGColorSpaceCreateDeviceGray", radek_compat_CGColorSpaceCreateDeviceGray) \
    X("_CGColorSpaceRelease", radek_compat_CGColorSpaceRelease) \
    X("_CGColorSpaceRetain", radek_compat_CGColorSpaceRetain) \
    X("_CGBitmapContextCreate", radek_compat_CGBitmapContextCreate) \
    X("_CGBitmapContextGetData", radek_compat_CGBitmapContextGetData) \
    X("_CGBitmapContextGetWidth", radek_compat_CGBitmapContextGetWidth) \
    X("_CGBitmapContextGetHeight", radek_compat_CGBitmapContextGetHeight) \
    X("_CGBitmapContextGetBytesPerRow", radek_compat_CGBitmapContextGetBytesPerRow) \
    X("_CGBitmapContextCreateImage", radek_compat_CGBitmapContextCreateImage) \
    X("_CGContextRelease", radek_compat_CGContextRelease) \
    X("_CGContextRetain", radek_compat_CGContextRetain) \
    X("_CGContextClearRect", radek_compat_CGContextClearRect) \
    X("_CGContextFillRect", radek_compat_CGContextFillRect) \
    X("_CGContextDrawImage", radek_compat_CGContextDrawImage) \
    X("_CGContextTranslateCTM", radek_compat_CGContextTranslateCTM) \
    X("_CGContextScaleCTM", radek_compat_CGContextScaleCTM) \
    X("_CGContextRotateCTM", radek_compat_CGContextRotateCTM) \
    X("_CGContextSaveGState", radek_compat_CGContextSaveGState) \
    X("_CGContextRestoreGState", radek_compat_CGContextRestoreGState) \
    X("_CGContextSetRGBFillColor", radek_compat_CGContextSetRGBFillColor) \
    X("_CGContextSetAlpha", radek_compat_CGContextSetAlpha) \
    X("_CGImageGetWidth", radek_compat_CGImageGetWidth) \
    X("_CGImageGetHeight", radek_compat_CGImageGetHeight) \
    X("_CGImageGetBitsPerComponent", radek_compat_CGImageGetBitsPerComponent) \
    X("_CGImageGetBitsPerPixel", radek_compat_CGImageGetBitsPerPixel) \
    X("_CGImageGetBytesPerRow", radek_compat_CGImageGetBytesPerRow) \
    X("_CGImageGetAlphaInfo", radek_compat_CGImageGetAlphaInfo) \
    X("_CGImageGetDataProvider", radek_compat_CGImageGetDataProvider) \
    X("_CGImageGetColorSpace", radek_compat_CGImageGetColorSpace) \
    X("_CGImageRelease", radek_compat_CGImageRelease) \
    X("_CGImageRetain", radek_compat_CGImageRetain) \
    X("_CGDataProviderCopyData", radek_compat_CGDataProviderCopyData) \
    X("_CGDataProviderCreateWithData", radek_compat_CGDataProviderCreateWithData) \
    X("_CGDataProviderRelease", radek_compat_CGDataProviderRelease) \
    X("_CGDataProviderRetain", radek_compat_CGDataProviderRetain) \
    X("_CGAffineTransformMake", radek_compat_CGAffineTransformMake) \
    X("_CGAffineTransformMakeTranslation", radek_compat_CGAffineTransformMakeTranslation) \
    X("_CGAffineTransformMakeScale", radek_compat_CGAffineTransformMakeScale) \
    X("_CGAffineTransformMakeRotation", radek_compat_CGAffineTransformMakeRotation) \
    X("_CGAffineTransformTranslate", radek_compat_CGAffineTransformTranslate) \
    X("_CGAffineTransformScale", radek_compat_CGAffineTransformScale) \
    X("_CGAffineTransformRotate", radek_compat_CGAffineTransformRotate) \
    X("_CGAffineTransformConcat", radek_compat_CGAffineTransformConcat) \
    X("_objc_msgSendSuper", radek_compat_objc_msgSendSuper) \
    X("_objc_msgSendSuper_stret", radek_compat_objc_msgSendSuper_stret) \
    X("_objc_msgSendSuper2_stret", radek_compat_objc_msgSendSuper2_stret) \
    X("_objc_msgSend_fpret", radek_compat_objc_msgSend_fpret) \
    X("_objc_getClass", radek_compat_objc_getClass) \
    X("_objc_lookUpClass", radek_compat_objc_lookUpClass) \
    X("_objc_getMetaClass", radek_compat_objc_getMetaClass) \
    X("_objc_getProtocol", radek_compat_objc_getProtocol) \
    X("_objc_allocateClassPair", radek_compat_objc_allocateClassPair) \
    X("_objc_registerClassPair", radek_compat_objc_registerClassPair) \
    X("_objc_retain", radek_compat_objc_retain) \
    X("_objc_release", radek_compat_objc_release) \
    X("_objc_autorelease", radek_compat_objc_autorelease) \
    X("_objc_autoreleasePoolPush", radek_compat_objc_autoreleasePoolPush) \
    X("_objc_autoreleasePoolPop", radek_compat_objc_autoreleasePoolPop) \
    X("_objc_retainAutorelease", radek_compat_objc_retainAutorelease) \
    X("_objc_retainAutoreleaseReturnValue", radek_compat_objc_retainAutoreleaseReturnValue) \
    X("_objc_retainAutoreleasedReturnValue", radek_compat_objc_retainAutoreleasedReturnValue) \
    X("_objc_storeStrong", radek_compat_objc_storeStrong) \
    X("_objc_storeWeak", radek_compat_objc_storeWeak) \
    X("_objc_loadWeakRetained", radek_compat_objc_loadWeakRetained) \
    X("_objc_destroyWeak", radek_compat_objc_destroyWeak) \
    X("_objc_getProperty", radek_compat_objc_getProperty) \
    X("_objc_copyStruct", radek_compat_objc_copyStruct) \
    X("_objc_sync_enter", radek_compat_objc_sync_enter) \
    X("_objc_sync_exit", radek_compat_objc_sync_exit) \
    X("_objc_exception_throw", radek_compat_objc_exception_throw) \
    X("_objc_begin_catch", radek_compat_objc_begin_catch) \
    X("_objc_end_catch", radek_compat_objc_end_catch) \
    X("_sel_registerName", radek_compat_sel_registerName) \
    X("_sel_getUid", radek_compat_sel_getUid) \
    X("_sel_getName", radek_compat_sel_getName) \
    X("_class_getName", radek_compat_class_getName) \
    X("_class_getSuperclass", radek_compat_class_getSuperclass) \
    X("_class_getInstanceMethod", radek_compat_class_getInstanceMethod) \
    X("_class_getClassMethod", radek_compat_class_getClassMethod) \
    X("_class_addMethod", radek_compat_class_addMethod) \
    X("_class_replaceMethod", radek_compat_class_replaceMethod) \
    X("_class_createInstance", radek_compat_class_createInstance) \
    X("_object_getClass", radek_compat_object_getClass) \
    X("_object_getClassName", radek_compat_object_getClassName) \
    X("_OBJC_CLASS_$_MPMoviePlayerController", radek_compat_OBJC_CLASS___MPMoviePlayerController) \
    X("_OBJC_CLASS_$_NSDate", radek_compat_OBJC_CLASS___NSDate) \
    X("_OBJC_CLASS_$_NSLocale", radek_compat_OBJC_CLASS___NSLocale) \
    X("_OBJC_CLASS_$_NSNotificationCenter", radek_compat_OBJC_CLASS___NSNotificationCenter) \
    X("_OBJC_CLASS_$_NSUserDefaults", radek_compat_OBJC_CLASS___NSUserDefaults) \
    X("_OBJC_CLASS_$_UIColor", radek_compat_OBJC_CLASS___UIColor) \
    X("_OBJC_CLASS_$_UIDevice", radek_compat_OBJC_CLASS___UIDevice) \
    X("_OBJC_CLASS_$_UIImage", radek_compat_OBJC_CLASS___UIImage) \
    X("_OBJC_CLASS_$_UIViewController", radek_compat_OBJC_CLASS___UIViewController) \
    X("_OBJC_CLASS_$_AVAudioPlayer", radek_compat_OBJC_CLASS___AVAudioPlayer) \
    X("_OBJC_CLASS_$_AVAudioSession", radek_compat_OBJC_CLASS___AVAudioSession) \
    X("_OBJC_CLASS_$_NSArray", radek_compat_OBJC_CLASS___NSArray) \
    X("_OBJC_CLASS_$_NSMutableArray", radek_compat_OBJC_CLASS___NSMutableArray) \
    X("_OBJC_CLASS_$_NSMutableDictionary", radek_compat_OBJC_CLASS___NSMutableDictionary) \
    X("_OBJC_CLASS_$_NSMutableString", radek_compat_OBJC_CLASS___NSMutableString) \
    X("_OBJC_CLASS_$_NSData", radek_compat_OBJC_CLASS___NSData) \
    X("_OBJC_CLASS_$_NSMutableData", radek_compat_OBJC_CLASS___NSMutableData) \
    X("_OBJC_CLASS_$_NSSet", radek_compat_OBJC_CLASS___NSSet) \
    X("_OBJC_CLASS_$_NSMutableSet", radek_compat_OBJC_CLASS___NSMutableSet) \
    X("_OBJC_CLASS_$_NSFileManager", radek_compat_OBJC_CLASS___NSFileManager) \
    X("_OBJC_CLASS_$_NSTimer", radek_compat_OBJC_CLASS___NSTimer) \
    X("_OBJC_CLASS_$_NSRunLoop", radek_compat_OBJC_CLASS___NSRunLoop) \
    X("_OBJC_CLASS_$_NSProcessInfo", radek_compat_OBJC_CLASS___NSProcessInfo) \
    X("_OBJC_CLASS_$_NSValue", radek_compat_OBJC_CLASS___NSValue) \
    X("_OBJC_CLASS_$_NSError", radek_compat_OBJC_CLASS___NSError) \
    X("_OBJC_CLASS_$_UIImageView", radek_compat_OBJC_CLASS___UIImageView) \
    X("_OBJC_CLASS_$_UILabel", radek_compat_OBJC_CLASS___UILabel) \
    X("_OBJC_CLASS_$_UIButton", radek_compat_OBJC_CLASS___UIButton) \
    X("_OBJC_CLASS_$_UIScrollView", radek_compat_OBJC_CLASS___UIScrollView) \
    X("_OBJC_CLASS_$_UIAlertView", radek_compat_OBJC_CLASS___UIAlertView) \
    X("_OBJC_CLASS_$_UIActivityIndicatorView", radek_compat_OBJC_CLASS___UIActivityIndicatorView) \
    X("_OBJC_CLASS_$_UIWebView", radek_compat_OBJC_CLASS___UIWebView) \
    X("_OBJC_CLASS_$_UIFont", radek_compat_OBJC_CLASS___UIFont) \
    X("_OBJC_CLASS_$_UITouch", radek_compat_OBJC_CLASS___UITouch) \
    X("_OBJC_CLASS_$_UIEvent", radek_compat_OBJC_CLASS___UIEvent) \
    X("_OBJC_CLASS_$_CALayer", radek_compat_OBJC_CLASS___CALayer) \
    X("_OBJC_CLASS_$_CATransaction", radek_compat_OBJC_CLASS___CATransaction) \
    X("_OBJC_CLASS_$_CABasicAnimation", radek_compat_OBJC_CLASS___CABasicAnimation) \
    X("_OBJC_CLASS_$_SKPaymentQueue", radek_compat_OBJC_CLASS___SKPaymentQueue) \
    X("_OBJC_CLASS_$_SKProductsRequest", radek_compat_OBJC_CLASS___SKProductsRequest) \
    X("_OBJC_CLASS_$_GKLocalPlayer", radek_compat_OBJC_CLASS___GKLocalPlayer) \
    X("_OBJC_CLASS_$_CMMotionManager", radek_compat_OBJC_CLASS___CMMotionManager) \
    X("_OBJC_CLASS_$_GCController", radek_compat_OBJC_CLASS___GCController) \
    X("_OBJC_METACLASS_$_UIViewController", radek_compat_OBJC_METACLASS___UIViewController) \
    X("_OBJC_METACLASS_$_UIApplication", radek_compat_OBJC_METACLASS___UIApplication) \
    X("_UIGraphicsPushContext", radek_compat_UIGraphicsPushContext) \
    X("_UIGraphicsPopContext", radek_compat_UIGraphicsPopContext) \
    X("_UIGraphicsGetCurrentContext", radek_compat_UIGraphicsGetCurrentContext) \
    X("_UIGraphicsBeginImageContext", radek_compat_UIGraphicsBeginImageContext) \
    X("_UIGraphicsBeginImageContextWithOptions", radek_compat_UIGraphicsBeginImageContextWithOptions) \
    X("_UIGraphicsGetImageFromCurrentImageContext", radek_compat_UIGraphicsGetImageFromCurrentImageContext) \
    X("_UIGraphicsEndImageContext", radek_compat_UIGraphicsEndImageContext) \
    X("_UIImagePNGRepresentation", radek_compat_UIImagePNGRepresentation) \
    X("_UIImageJPEGRepresentation", radek_compat_UIImageJPEGRepresentation) \
    X("_UIImageWriteToSavedPhotosAlbum", radek_compat_UIImageWriteToSavedPhotosAlbum) \
    X("_NSTemporaryDirectory", radek_compat_NSTemporaryDirectory) \
    X("_NSHomeDirectory", radek_compat_NSHomeDirectory) \
    X("_NSLog", radek_compat_NSLog) \
    X("_NSStringFromClass", radek_compat_NSStringFromClass) \
    X("_NSClassFromString", radek_compat_NSClassFromString) \
    X("_NSStringFromSelector", radek_compat_NSStringFromSelector) \
    X("_NSSelectorFromString", radek_compat_NSSelectorFromString) \
    X("_NSPageSize", radek_compat_NSPageSize) \
    X("_dispatch_async", radek_compat_dispatch_async) \
    X("_dispatch_sync", radek_compat_dispatch_sync) \
    X("_dispatch_after", radek_compat_dispatch_after) \
    X("_dispatch_once", radek_compat_dispatch_once) \
    X("_dispatch_async_f", radek_compat_dispatch_async_f) \
    X("_dispatch_sync_f", radek_compat_dispatch_sync_f) \
    X("_dispatch_once_f", radek_compat_dispatch_once_f) \
    X("_dispatch_get_main_queue", radek_compat_dispatch_get_main_queue) \
    X("_dispatch_get_global_queue", radek_compat_dispatch_get_global_queue) \
    X("_dispatch_queue_create", radek_compat_dispatch_queue_create) \
    X("_dispatch_release", radek_compat_dispatch_release) \
    X("_dispatch_retain", radek_compat_dispatch_retain) \
    X("_dispatch_time", radek_compat_dispatch_time) \
    X("_dispatch_semaphore_create", radek_compat_dispatch_semaphore_create) \
    X("_dispatch_semaphore_wait", radek_compat_dispatch_semaphore_wait) \
    X("_dispatch_semaphore_signal", radek_compat_dispatch_semaphore_signal) \
    X("_dispatch_group_create", radek_compat_dispatch_group_create) \
    X("_dispatch_group_async", radek_compat_dispatch_group_async) \
    X("_dispatch_group_enter", radek_compat_dispatch_group_enter) \
    X("_dispatch_group_leave", radek_compat_dispatch_group_leave) \
    X("_dispatch_group_wait", radek_compat_dispatch_group_wait) \
    X("_dispatch_group_notify", radek_compat_dispatch_group_notify) \
    X("__dispatch_main_q", radek_compat__dispatch_main_q) \
    X("_SCNetworkReachabilityCreateWithAddress", radek_compat_SCNetworkReachabilityCreateWithAddress) \
    X("_SCNetworkReachabilityCreateWithName", radek_compat_SCNetworkReachabilityCreateWithName) \
    X("_SCNetworkReachabilityGetFlags", radek_compat_SCNetworkReachabilityGetFlags) \
    X("_SCNetworkReachabilitySetCallback", radek_compat_SCNetworkReachabilitySetCallback) \
    X("_SCNetworkReachabilityScheduleWithRunLoop", radek_compat_SCNetworkReachabilityScheduleWithRunLoop) \
    X("_SCNetworkReachabilityUnscheduleFromRunLoop", radek_compat_SCNetworkReachabilityUnscheduleFromRunLoop) \
    X("_SCNetworkReachabilitySetDispatchQueue", radek_compat_SCNetworkReachabilitySetDispatchQueue) \
    X("_SecRandomCopyBytes", radek_compat_SecRandomCopyBytes) \
    X("_SecItemCopyMatching", radek_compat_SecItemCopyMatching) \
    X("_SecItemAdd", radek_compat_SecItemAdd) \
    X("_SecItemUpdate", radek_compat_SecItemUpdate) \
    X("_SecItemDelete", radek_compat_SecItemDelete) \
    X("_CC_MD5", radek_compat_CC_MD5) \
    X("_CC_SHA1", radek_compat_CC_SHA1) \
    X("_CC_SHA256", radek_compat_CC_SHA256) \
    X("__Unwind_DeleteException", radek_compat__Unwind_DeleteException) \
    X("__Unwind_GetIP", radek_compat__Unwind_GetIP) \
    X("__Unwind_SetIP", radek_compat__Unwind_SetIP) \
    X("__Unwind_GetGR", radek_compat__Unwind_GetGR) \
    X("__Unwind_SetGR", radek_compat__Unwind_SetGR) \
    X("__Unwind_GetLanguageSpecificData", radek_compat__Unwind_GetLanguageSpecificData) \
    X("__Unwind_GetRegionStart", radek_compat__Unwind_GetRegionStart) \
    X("___gxx_personality_v0", radek_compat___gxx_personality_v0) \
    X("___gcc_personality_v0", radek_compat___gcc_personality_v0) \
    X("___udivdi3", radek_compat___udivdi3) \
    X("___umoddi3", radek_compat___umoddi3) \
    X("___muldi3", radek_compat___muldi3) \
    X("___fixsfdi", radek_compat___fixsfdi) \
    X("___fixunsdfdi", radek_compat___fixunsdfdi) \
    X("___fixunssfdi", radek_compat___fixunssfdi) \
    X("___floatundidf", radek_compat___floatundidf) \
    X("___floatundisf", radek_compat___floatundisf) \
    X("___ashldi3", radek_compat___ashldi3) \
    X("___ashrdi3", radek_compat___ashrdi3) \
    X("___lshrdi3", radek_compat___lshrdi3) \
    X("___cmpdi2", radek_compat___cmpdi2) \
    X("___ucmpdi2", radek_compat___ucmpdi2) \
    X("___clear_cache", radek_compat___clear_cache) \
    X("__Znaj", radek_compat__Znaj) \
    X("__Znwj", radek_compat__Znwj) \
    X("___cxa_free_exception", radek_compat___cxa_free_exception) \
    X("___cxa_rethrow", radek_compat___cxa_rethrow) \
    X("___cxa_guard_acquire", radek_compat___cxa_guard_acquire) \
    X("___cxa_guard_release", radek_compat___cxa_guard_release) \
    X("___cxa_guard_abort", radek_compat___cxa_guard_abort) \
    X("___cxa_demangle", radek_compat___cxa_demangle) \
    X("___dynamic_cast", radek_compat___dynamic_cast) \
    X("_CFArrayContainsValue", radek_compat_CFArrayContainsValue) \
    X("_CFArrayGetFirstIndexOfValue", radek_compat_CFArrayGetFirstIndexOfValue) \
    X("_CFDictionaryAddValue", radek_compat_CFDictionaryAddValue) \
    X("_CFMakeCollectable", radek_compat_CFMakeCollectable) \
    X("_CFStringAppendCharacters", radek_compat_CFStringAppendCharacters) \
    X("_CFStringCreateWithCharacters", radek_compat_CFStringCreateWithCharacters) \
    X("_CFStringGetCharactersPtr", radek_compat_CFStringGetCharactersPtr) \
    X("_CFURLCreateStringByAddingPercentEscapes", radek_compat_CFURLCreateStringByAddingPercentEscapes) \
    X("_CFHostCreateWithName", radek_compat_CFHostCreateWithName) \
    X("_CFHostGetAddressing", radek_compat_CFHostGetAddressing) \
    X("_CFHostStartInfoResolution", radek_compat_CFHostStartInfoResolution) \
    X("_CGRectGetHeight", radek_compat_CGRectGetHeight) \
    X("_CGRectGetMaxX", radek_compat_CGRectGetMaxX) \
    X("_CGRectGetMidX", radek_compat_CGRectGetMidX) \
    X("_CGRectGetMidY", radek_compat_CGRectGetMidY) \
    X("_CGRectGetMinY", radek_compat_CGRectGetMinY) \
    X("_CGRectGetWidth", radek_compat_CGRectGetWidth) \
    X("_CGRectIntegral", radek_compat_CGRectIntegral) \
    X("_CGRectIntersectsRect", radek_compat_CGRectIntersectsRect) \
    X("_CGRectIsEmpty", radek_compat_CGRectIsEmpty) \
    X("_CGRectIsNull", radek_compat_CGRectIsNull) \
    X("_CGRectOffset", radek_compat_CGRectOffset) \
    X("_CCHmac", radek_compat_CCHmac) \
    X("_CCHmacInit", radek_compat_CCHmacInit) \
    X("_CCHmacUpdate", radek_compat_CCHmacUpdate) \
    X("_CCHmacFinal", radek_compat_CCHmacFinal) \
    X("_OSAtomicAdd32Barrier", radek_compat_OSAtomicAdd32Barrier) \
    X("_OSAtomicCompareAndSwap32Barrier", radek_compat_OSAtomicCompareAndSwap32Barrier) \
    X("_OSAtomicCompareAndSwapPtrBarrier", radek_compat_OSAtomicCompareAndSwapPtrBarrier) \
    X("_host_page_size", radek_compat_host_page_size) \
    X("_host_statistics", radek_compat_host_statistics) \
    X("_mach_host_self", radek_compat_mach_host_self) \
    X("_mach_task_self_", radek_compat_mach_task_self_) \
    X("_mach_wait_until", radek_compat_mach_wait_until) \
    X("_semaphore_create", radek_compat_semaphore_create) \
    X("_semaphore_destroy", radek_compat_semaphore_destroy) \
    X("_semaphore_signal", radek_compat_semaphore_signal) \
    X("_semaphore_wait", radek_compat_semaphore_wait) \
    X("_task_info", radek_compat_task_info) \
    X("_thread_policy_set", radek_compat_thread_policy_set) \
    X("_pthread_mach_thread_np", radek_compat_pthread_mach_thread_np) \
    X("_pthread_threadid_np", radek_compat_pthread_threadid_np) \
    X("_dispatch_get_current_queue", radek_compat_dispatch_get_current_queue) \
    X("___assert_rtn", radek_compat___assert_rtn) \
    X("___cxa_call_unexpected", radek_compat___cxa_call_unexpected) \
    X("___divmodsi4", radek_compat___divmodsi4) \
    X("___objc_personality_v0", radek_compat___objc_personality_v0) \
    X("___sincos_stret", radek_compat___sincos_stret) \
    X("___sincosf_stret", radek_compat___sincosf_stret) \
    X("_memset_pattern16", radek_compat_memset_pattern16) \
    X("__Block_object_assign", radek_compat_Block_object_assign) \
    X("__Block_object_dispose", radek_compat_Block_object_dispose) \
    X("__ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv", radek_compat_stl_throw_length_error) \
    X("__ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv", radek_compat_stl_throw_out_of_range) \
    X("__ZNSt3__112__rs_defaultD1Ev", radek_compat_rs_default_dtor) \
    X("__ZNSt3__112__rs_defaultclEv", radek_compat_rs_default_call) \
    X("__ZNSt3__18__rs_getEv", radek_compat_rs_get) \
    X("_objc_setAssociatedObject", radek_compat_objc_setAssociatedObject) \
    X("_objc_setProperty_atomic", radek_compat_objc_setProperty_atomic) \
    X("_objc_setProperty_atomic_copy", radek_compat_objc_setProperty_atomic_copy) \
    X("_objc_setProperty_nonatomic", radek_compat_objc_setProperty_nonatomic) \
    X("_objc_setProperty_nonatomic_copy", radek_compat_objc_setProperty_nonatomic_copy) \
    X("_NSSetUncaughtExceptionHandler", radek_compat_NSSetUncaughtExceptionHandler)

#ifdef __cplusplus
}  // extern "C"
#endif
