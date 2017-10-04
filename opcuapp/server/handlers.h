#pragma once

#include <functional>
#include <memory>
#include <opcuapp/data_value.h>
#include <opcuapp/structs.h>
#include <opcuapp/requests.h>

namespace opcua {
namespace server {

using ReadCallback = std::function<void(ReadResponse& response)>;
using ReadHandler = std::function<void(OpcUa_ReadRequest& request, const ReadCallback& callback)>;

using BrowseCallback = std::function<void(BrowseResponse& response)>;
using BrowseHandler = std::function<void(OpcUa_BrowseRequest& request, const BrowseCallback& callback)>;

using DataChangeHandler = std::function<void(const DataValue& data_value)>;
using EventHandler = std::function<void()>;

class MonitoredItem {
 public:
  virtual ~MonitoredItem() {}

  virtual void SubscribeDataChange(const DataChangeHandler& data_change_handler) = 0;
  virtual void SubscribeEvents(const EventHandler& event_handler) = 0;
};

struct CreateMonitoredItemResult {
  StatusCode status_code;
  std::shared_ptr<MonitoredItem> monitored_item;
};

using CreateMonitoredItemHandler = std::function<CreateMonitoredItemResult(ReadValueId& read_value_id)>;

} // namespace server
} // namespace opcua
