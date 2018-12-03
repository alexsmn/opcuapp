#pragma once

#include <opcuapp/data_value.h>
#include <opcuapp/requests.h>
#include <opcuapp/structs.h>
#include <opcuapp/variant.h>
#include <opcuapp/vector.h>
#include <functional>
#include <memory>

namespace opcua {
namespace server {

using ReadCallback = std::function<void(ReadResponse&& response)>;
using ReadHandler = std::function<void(OpcUa_ReadRequest& request,
                                       const ReadCallback& callback)>;

using BrowseCallback = std::function<void(BrowseResponse&& response)>;
using BrowseHandler = std::function<void(OpcUa_BrowseRequest& request,
                                         const BrowseCallback& callback)>;

using TranslateBrowsePathsToNodeIdsCallback =
    std::function<void(TranslateBrowsePathsToNodeIdsResponse&& response)>;
using TranslateBrowsePathsToNodeIdsHandler =
    std::function<void(OpcUa_TranslateBrowsePathsToNodeIdsRequest& request,
                       const TranslateBrowsePathsToNodeIdsCallback& callback)>;

using DataChangeHandler = std::function<void(DataValue&& data_value)>;
using EventHandler = std::function<void(Vector<OpcUa_Variant>&& event_fields)>;

class MonitoredItem {
 public:
  virtual ~MonitoredItem() {}

  virtual void SubscribeDataChange(
      const DataChangeHandler& data_change_handler) = 0;
  virtual void SubscribeEvents(const EventFilter& filter,
                               const EventHandler& event_handler) = 0;
};

struct CreateMonitoredItemResult {
  StatusCode status_code;
  std::shared_ptr<MonitoredItem> monitored_item;
};

using CreateMonitoredItemHandler =
    std::function<CreateMonitoredItemResult(ReadValueId&& read_value_id)>;

struct SessionHandlers {
  ReadHandler read_handler_;
  BrowseHandler browse_handler_;
  TranslateBrowsePathsToNodeIdsHandler
      translate_browse_paths_to_node_ids_handler_;
  CreateMonitoredItemHandler create_monitored_item_handler_;
};

}  // namespace server
}  // namespace opcua
