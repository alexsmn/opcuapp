#pragma once

#include "opcua/base/time/time.h"

#include <ostream>
#include <string>

namespace opcua {

// Built-in OPC UA DateTime: a UTC instant, here aliased to base::Time. OPC UA
// Part 3 §8.11 DateTime (encoding: Part 6 §5.1.4),
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.11
using DateTime = opcua::base::Time;
// OPC UA Duration: a time interval in milliseconds (a Double subtype), here
// aliased to base::TimeDelta. OPC UA Part 3 §8.13 Duration,
// https://reference.opcfoundation.org/Core/Part3/v105/docs/8.13
using Duration = opcua::base::TimeDelta;

std::string ToString(opcua::DateTime time);
std::u16string ToString16(opcua::DateTime time);
}  // namespace opcua
