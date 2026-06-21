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

// OPC UA HistoryUpdate service (the write counterpart of HistoryService). OPC
// UA Part 4 §5.10.5 HistoryUpdate,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/5.10.5
class HistoryUpdateService {
 public:
  virtual ~HistoryUpdateService() = default;

  virtual Awaitable<HistoryUpdateResult> HistoryUpdateData(
      UpdateDataDetails details) = 0;
};

}  // namespace opcua
