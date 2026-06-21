#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/scada/event.h"
#include "opcua/scada/history_types.h"

namespace opcua {

class HistoryService {
 public:
  virtual ~HistoryService() = default;

  virtual Awaitable<HistoryReadRawResult> HistoryReadRaw(
      HistoryReadRawDetails details) = 0;

  virtual Awaitable<HistoryReadEventsResult> HistoryReadEvents(
      NodeId node_id,
      opcua::base::Time from,
      opcua::base::Time to,
      EventFilter filter) = 0;
};

}  // namespace opcua (vendored)
