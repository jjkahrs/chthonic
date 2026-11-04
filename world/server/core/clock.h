// Time types. Logic takes `now` as a parameter instead of reading a clock, so tests drive time without sleeping.
#pragma once
#include <chrono>

namespace chthonic {

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;
using Millis = std::chrono::milliseconds;
using SysTime = std::chrono::system_clock::time_point;

inline double seconds_between(TimePoint a, TimePoint b) { return std::chrono::duration<double>(b - a).count(); }

} // namespace chthonic
