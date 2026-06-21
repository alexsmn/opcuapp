#pragma once

#include "opcua/scada/view_service.h"

#include <gmock/gmock.h>

namespace opcua {
namespace scada {

class MockViewService : public ViewService {
 public:
  MOCK_METHOD((Awaitable<StatusOr<std::vector<BrowseResult>>>),
              Browse,
              (opcua::scada::ServiceContext context,
               std::vector<BrowseDescription> inputs),
              (override));

  MOCK_METHOD((Awaitable<StatusOr<std::vector<BrowsePathResult>>>),
              TranslateBrowsePaths,
              (std::vector<BrowsePath> browse_paths),
              (override));
};

}  // namespace scada
}  // namespace opcua (vendored)
