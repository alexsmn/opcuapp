#pragma once

#include "opcua/scada/history_service.h"

#include <gmock/gmock.h>

namespace opcua {

class MockHistoryService : public HistoryService {
 public:
  MockHistoryService() {
    using namespace testing;
    ON_CALL(*this, HistoryReadRaw(_))
        .WillByDefault([](HistoryReadRawDetails) -> Awaitable<HistoryReadRawResult> {
          co_return HistoryReadRawResult{};
        });
    ON_CALL(*this, HistoryReadEvents(_, _, _, _))
        .WillByDefault([](NodeId, opcua::base::Time, opcua::base::Time,
                          EventFilter) -> Awaitable<HistoryReadEventsResult> {
          co_return HistoryReadEventsResult{};
        });
  }

  MOCK_METHOD(Awaitable<HistoryReadRawResult>,
              HistoryReadRaw,
              (HistoryReadRawDetails details),
              (override));

  MOCK_METHOD(Awaitable<HistoryReadEventsResult>,
              HistoryReadEvents,
              (NodeId node_id,
               opcua::base::Time from,
               opcua::base::Time to,
               EventFilter filter),
              (override));
};

}  // namespace opcua (vendored)
