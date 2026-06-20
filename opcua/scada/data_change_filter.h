#pragma once

#include "opcua/scada/basic_types.h"

namespace opcua {
namespace scada {

struct DataChangeFilter {
  // Deadband type: Absolute, Percent.
  Double deadband_value;
};

}  // namespace scada
}  // namespace opcua (vendored)
