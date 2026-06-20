#pragma once

#include "opcua/scada/basic_types.h"

namespace opcua::scada {

struct DataChangeFilter {
  // Deadband type: Absolute, Percent.
  Double deadband_value;
};

}  // namespace opcua::scada
