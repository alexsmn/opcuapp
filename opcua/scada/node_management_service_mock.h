#pragma once

#include "opcua/scada/node_management_service.h"

#include <gmock/gmock.h>

namespace opcua {
namespace scada {

class MockNodeManagementService : public NodeManagementService {
 public:
  MOCK_METHOD((Awaitable<StatusOr<std::vector<AddNodesResult>>>),
              AddNodes,
              (std::vector<AddNodesItem> inputs),
              (override));

  MOCK_METHOD((Awaitable<StatusOr<std::vector<StatusCode>>>),
              DeleteNodes,
              (std::vector<DeleteNodesItem> inputs),
              (override));

  MOCK_METHOD((Awaitable<StatusOr<std::vector<StatusCode>>>),
              AddReferences,
              (std::vector<AddReferencesItem> inputs),
              (override));

  MOCK_METHOD((Awaitable<StatusOr<std::vector<StatusCode>>>),
              DeleteReferences,
              (std::vector<DeleteReferencesItem> inputs),
              (override));
};

}  // namespace scada
}  // namespace opcua (vendored)
