#pragma once

#include "opcua/scada/event.h"
#include "opcua/scada/history_types.h"

namespace opcua {

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
