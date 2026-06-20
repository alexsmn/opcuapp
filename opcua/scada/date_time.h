#pragma once

#include "opcua/base/time/time.h"

#include <ostream>
#include <string>

namespace opcua::scada {

using DateTime = opcua::base::Time;
using Duration = opcua::base::TimeDelta;

}  // namespace opcua::scada

std::string ToString(opcua::scada::DateTime time);
std::u16string ToString16(opcua::scada::DateTime time);
