#pragma once

#include <chrono>

namespace opcua {
using Clock = std::chrono::steady_clock;
// Renamed from `Duration` to avoid a clash with the OPC UA `Duration`
// (base::TimeDelta) once opcua::scada:: flattens into opcua::.
using SteadyDuration = Clock::duration;
using TimePoint = Clock::time_point;
}  // namespace opcua (vendored)
