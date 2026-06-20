#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/scada/node_id.h"
#include "opcua/scada/status.h"
#include "opcua/scada/variant.h"

#include <vector>

namespace opcua {
namespace scada {

class MethodService {
 public:
  virtual ~MethodService() = default;

  virtual Awaitable<Status> Call(NodeId node_id,
                                 NodeId method_id,
                                 std::vector<Variant> arguments,
                                 NodeId user_id) = 0;
};

}  // namespace scada
}  // namespace opcua (vendored)
