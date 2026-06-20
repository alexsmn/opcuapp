#pragma once

#include "opcua/scada/method_service.h"

#include <gmock/gmock.h>

namespace opcua {
namespace scada {

class MockMethodService : public MethodService {
 public:
  MOCK_METHOD(Awaitable<Status>,
              Call,
              (NodeId node_id,
               NodeId method_id,
               std::vector<Variant> arguments,
               NodeId user_id),
              (override));
};

inline Awaitable<Status> MakeMethodCallResult(
    Status status = StatusCode::Good) {
  co_return std::move(status);
}

}  // namespace scada
}  // namespace opcua (vendored)
