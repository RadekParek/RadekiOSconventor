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
 *    translated callers always over-allocate; every operation here touches only
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
 * Darwin's, and translated callers therefore always over-allocate; every
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
 * resolver can never disagree. radek/api_translation.py keeps a matching Python
 * table for host source generation, and tests/test_api_translation.py asserts
 * the two stay identical.
 */
#define RADEK_IOS_SHIM_TABLE(X)                                                        \
    X("_CFAllocatorGetDefault", radek_compat_CFAllocatorGetDefault)                    \
    X("_CFRetain", radek_compat_CFRetain)                                              \
    X("_CFRelease", radek_compat_CFRelease)                                            \
    X("_CFGetRetainCount", radek_compat_CFGetRetainCount)                              \
    X("_CFStringCreateWithCString", radek_compat_CFStringCreateWithCString)            \
    X("_CFStringGetLength", radek_compat_CFStringGetLength)                            \
    X("_CFStringGetCString", radek_compat_CFStringGetCString)                          \
    X("_CFStringGetCStringPtr", radek_compat_CFStringGetCStringPtr)                    \
    X("_CFStringGetMaximumSizeForEncoding", radek_compat_CFStringGetMaximumSizeForEncoding) \
    X("_CFStringCompare", radek_compat_CFStringCompare)                                \
    X("_CFStringGetSystemEncoding", radek_compat_CFStringGetSystemEncoding)            \
    X("_CFDataCreate", radek_compat_CFDataCreate)                                      \
    X("_CFDataGetBytePtr", radek_compat_CFDataGetBytePtr)                              \
    X("_CFDataGetLength", radek_compat_CFDataGetLength)                                \
    X("_CFArrayCreateMutable", radek_compat_CFArrayCreateMutable)                      \
    X("_CFArrayAppendValue", radek_compat_CFArrayAppendValue)                          \
    X("_CFArrayGetCount", radek_compat_CFArrayGetCount)                                \
    X("_CFArrayGetValueAtIndex", radek_compat_CFArrayGetValueAtIndex)                  \
    X("_CFDictionaryCreateMutable", radek_compat_CFDictionaryCreateMutable)            \
    X("_CFDictionarySetValue", radek_compat_CFDictionarySetValue)                      \
    X("_CFDictionaryGetValue", radek_compat_CFDictionaryGetValue)                      \
    X("_CFDictionaryGetCount", radek_compat_CFDictionaryGetCount)                      \
    X("_CFNumberCreate", radek_compat_CFNumberCreate)                                  \
    X("_CFNumberGetValue", radek_compat_CFNumberGetValue)                              \
    X("_CFDateCreate", radek_compat_CFDateCreate)                                      \
    X("_CFDateGetAbsoluteTime", radek_compat_CFDateGetAbsoluteTime)                    \
    X("_CFDateGetTimeIntervalSinceDate", radek_compat_CFDateGetTimeIntervalSinceDate)  \
    X("_CFAbsoluteTimeGetGregorianDate", radek_compat_CFAbsoluteTimeGetGregorianDate)  \
    X("_malloc", radek_compat_malloc)                                                  \
    X("_calloc", radek_compat_calloc)                                                  \
    X("_realloc", radek_compat_realloc)                                                \
    X("_free", radek_compat_free)                                                      \
    X("_memcpy", radek_compat_memcpy)                                                  \
    X("_memmove", radek_compat_memmove)                                                \
    X("_memset", radek_compat_memset)                                                  \
    X("_memcmp", radek_compat_memcmp)                                                  \
    X("_memchr", radek_compat_memchr)                                                  \
    X("_strlen", radek_compat_strlen)                                                  \
    X("_strcpy", radek_compat_strcpy)                                                  \
    X("_strncpy", radek_compat_strncpy)                                                \
    X("_strlcpy", radek_compat_strlcpy)                                                \
    X("_strlcat", radek_compat_strlcat)                                                \
    X("_strcmp", radek_compat_strcmp)                                                  \
    X("_strncmp", radek_compat_strncmp)                                                \
    X("_strdup", radek_compat_strdup)                                                  \
    X("_strchr", radek_compat_strchr)                                                  \
    X("_strrchr", radek_compat_strrchr)                                                \
    X("_strstr", radek_compat_strstr)                                                  \
    X("_strtol", radek_compat_strtol)                                                  \
    X("_strtod", radek_compat_strtod)                                                  \
    X("_atoi", radek_compat_atoi)                                                      \
    X("_atof", radek_compat_atof)                                                      \
    X("_strerror", radek_compat_strerror)                                              \
    X("_snprintf", radek_compat_snprintf)                                              \
    X("_vsnprintf", radek_compat_vsnprintf)                                            \
    X("_fopen", radek_compat_fopen)                                                    \
    X("_fclose", radek_compat_fclose)                                                  \
    X("_fread", radek_compat_fread)                                                    \
    X("_fwrite", radek_compat_fwrite)                                                  \
    X("_fputs", radek_compat_fputs)                                                    \
    X("_fgets", radek_compat_fgets)                                                    \
    X("_fflush", radek_compat_fflush)                                                  \
    X("_fprintf", radek_compat_fprintf)                                                \
    X("_printf", radek_compat_printf)                                                  \
    X("_puts", radek_compat_puts)                                                      \
    X("_remove", radek_compat_remove)                                                  \
    X("_feof", radek_compat_feof)                                                      \
    X("_ftell", radek_compat_ftell)                                                    \
    X("_fseek", radek_compat_fseek)                                                    \
    X("_time", radek_compat_time)                                                      \
    X("_gettimeofday", radek_compat_gettimeofday)                                      \
    X("_clock_gettime", radek_compat_clock_gettime)                                    \
    X("_nanosleep", radek_compat_nanosleep)                                            \
    X("_localtime_r", radek_compat_localtime_r)                                        \
    X("_gmtime_r", radek_compat_gmtime_r)                                              \
    X("_mktime", radek_compat_mktime)                                                  \
    X("_getenv", radek_compat_getenv)                                                  \
    X("_setenv", radek_compat_setenv)                                                  \
    X("_unsetenv", radek_compat_unsetenv)                                              \
    X("_getpid", radek_compat_getpid)                                                  \
    X("_qsort", radek_compat_qsort)                                                    \
    X("_bsearch", radek_compat_bsearch)                                                \
    X("_abs", radek_compat_abs)                                                        \
    X("_labs", radek_compat_labs)                                                      \
    X("_rand", radek_compat_rand)                                                      \
    X("_srand", radek_compat_srand)                                                    \
    X("_sqrt", radek_compat_sqrt)                                                      \
    X("_fabs", radek_compat_fabs)                                                      \
    X("_floor", radek_compat_floor)                                                    \
    X("_ceil", radek_compat_ceil)                                                      \
    X("_pow", radek_compat_pow)                                                        \
    X("_sin", radek_compat_sin)                                                        \
    X("_cos", radek_compat_cos)                                                        \
    X("_tan", radek_compat_tan)                                                        \
    X("_atan2", radek_compat_atan2)                                                    \
    X("_fmod", radek_compat_fmod)                                                      \
    X("_pthread_mutex_init", radek_compat_pthread_mutex_init)                          \
    X("_pthread_mutex_lock", radek_compat_pthread_mutex_lock)                          \
    X("_pthread_mutex_unlock", radek_compat_pthread_mutex_unlock)                      \
    X("_pthread_mutex_destroy", radek_compat_pthread_mutex_destroy)                    \
    X("_pthread_cond_init", radek_compat_pthread_cond_init)                            \
    X("_pthread_cond_wait", radek_compat_pthread_cond_wait)                            \
    X("_pthread_cond_signal", radek_compat_pthread_cond_signal)                        \
    X("_pthread_cond_broadcast", radek_compat_pthread_cond_broadcast)                  \
    X("_pthread_cond_destroy", radek_compat_pthread_cond_destroy)                      \
    X("_pthread_self", radek_compat_pthread_self)

#ifdef __cplusplus
}  // extern "C"
#endif
