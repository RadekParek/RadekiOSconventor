#include "apple_time_compat.h"

#include <limits>
#include <time.h>

namespace {
#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_CFAbsoluteTimeGetCurrent) || \
    defined(RADEK_API_CACurrentMediaTime) || defined(RADEK_API_mach_absolute_time)
constexpr uint64_t kNanosecondsPerSecond = UINT64_C(1000000000);
bool readClock(clockid_t clock, timespec &value) {
    return clock_gettime(clock, &value) == 0 && value.tv_sec >= 0 && value.tv_nsec >= 0 &&
           static_cast<uint64_t>(value.tv_nsec) < kNanosecondsPerSecond;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_CFAbsoluteTimeGetCurrent)
constexpr uint64_t kUnixToCFAbsoluteTime = UINT64_C(978307200);
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_mach_absolute_time)
uint64_t toNanoseconds(const timespec &value) {
    const auto seconds = static_cast<uint64_t>(value.tv_sec);
    if (seconds > std::numeric_limits<uint64_t>::max() / kNanosecondsPerSecond)
        return 0;
    const uint64_t wholeSeconds = seconds * kNanosecondsPerSecond;
    const uint64_t nanoseconds = static_cast<uint64_t>(value.tv_nsec);
    if (nanoseconds > std::numeric_limits<uint64_t>::max() - wholeSeconds)
        return 0;
    return wholeSeconds + nanoseconds;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_mach_timebase_info)
constexpr int32_t kKernSuccess = 0;
constexpr int32_t kKernInvalidArgument = 4;
#endif
} // namespace

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_CFAbsoluteTimeGetCurrent)
extern "C" double CFAbsoluteTimeGetCurrent(void) {
    timespec now{};
    if (!readClock(CLOCK_REALTIME, now))
        return 0.0;
    return static_cast<double>(now.tv_sec) - static_cast<double>(kUnixToCFAbsoluteTime) +
           static_cast<double>(now.tv_nsec) / static_cast<double>(kNanosecondsPerSecond);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_CACurrentMediaTime)
extern "C" double CACurrentMediaTime(void) {
    timespec now{};
    if (!readClock(CLOCK_MONOTONIC, now))
        return 0.0;
    return static_cast<double>(now.tv_sec) +
           static_cast<double>(now.tv_nsec) / static_cast<double>(kNanosecondsPerSecond);
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_mach_absolute_time)
extern "C" uint64_t mach_absolute_time(void) {
    timespec now{};
    return readClock(CLOCK_MONOTONIC, now) ? toNanoseconds(now) : 0;
}
#endif

#if !defined(RADEK_API_REPLACEMENTS_ONLY) || defined(RADEK_API_mach_timebase_info)
extern "C" int32_t mach_timebase_info(radek_mach_timebase_info_data_t *info) {
    if (!info)
        return kKernInvalidArgument;
    // mach_absolute_time() is expressed in nanoseconds, so one tick is one ns.
    info->numer = 1;
    info->denom = 1;
    return kKernSuccess;
}
#endif
