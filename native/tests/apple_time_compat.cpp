#include "apple_time_compat.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

#define CHECK(expression)                                                                                   \
    do {                                                                                                     \
        if (!(expression))                                                                                  \
            throw std::runtime_error("CHECK failed: " #expression);                                         \
    } while (false)

int main() {
    const double absolute = CFAbsoluteTimeGetCurrent();
    CHECK(absolute > 700000000.0 && absolute < 1100000000.0);

    const double mediaBefore = CACurrentMediaTime();
    const uint64_t ticksBefore = mach_absolute_time();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const double mediaAfter = CACurrentMediaTime();
    const uint64_t ticksAfter = mach_absolute_time();
    CHECK(mediaAfter > mediaBefore);
    CHECK(ticksAfter > ticksBefore);

    radek_mach_timebase_info_data_t timebase{};
    CHECK(mach_timebase_info(&timebase) == 0);
    CHECK(timebase.numer == 1 && timebase.denom == 1);
    CHECK(mach_timebase_info(nullptr) == 4);
    CHECK(std::abs((mediaAfter - mediaBefore) -
                   static_cast<double>(ticksAfter - ticksBefore) / 1e9) < 0.05);

    std::cout << "Apple time API compatibility shims passed\n";
}
