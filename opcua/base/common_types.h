#pragma once

#include <chrono>

namespace opcua {
using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;
using TimePoint = Clock::time_point;
}  // namespace opcua (vendored)
