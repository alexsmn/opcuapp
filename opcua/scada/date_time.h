#pragma once

#include "opcua/base/time/time.h"

#include <ostream>
#include <string>

namespace opcua {

using DateTime = opcua::base::Time;
using Duration = opcua::base::TimeDelta;


std::string ToString(opcua::DateTime time);
std::u16string ToString16(opcua::DateTime time);
}  // namespace opcua (vendored)
