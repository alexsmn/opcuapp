#pragma once

#include "opcua/scada/status.h"

#include <functional>

namespace opcua {

using StatusCallback = std::function<void(Status&&)>;
using MultiStatusCallback =
    std::function<void(Status&&, std::vector<StatusCode>&&)>;

}  // namespace opcua (vendored)
