#pragma once

#include "opcua/scada/status.h"

#include <functional>

namespace opcua {
namespace scada {

using StatusCallback = std::function<void(Status&&)>;
using MultiStatusCallback =
    std::function<void(Status&&, std::vector<StatusCode>&&)>;

}  // namespace scada
}  // namespace opcua (vendored)
