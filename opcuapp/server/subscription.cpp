#include "subscription.h"

namespace opcua {
namespace server {

void Subscription::BeginInvoke(OpcUa_CreateMonitoredItemsRequest& request, const std::function<void(OpcUa_CreateMonitoredItemsResponse& response)>& callback) {
  CreateMonitoredItemsResponse response;
  Vector<OpcUa_MonitoredItemCreateResult> results(request.NoOfItemsToCreate);
  for (size_t i = 0; i < static_cast<size_t>(request.NoOfItemsToCreate); ++i)
    CreateMonitoredItem(request.ItemsToCreate[i]).release(results[i]);
  response.NoOfResults = results.size();
  response.Results = results.release();
  callback(response);
}

void Subscription::BeginInvoke(OpcUa_DeleteMonitoredItemsRequest& request, const std::function<void(OpcUa_DeleteMonitoredItemsResponse& response)>& callback) {
  DeleteMonitoredItemsResponse response;
  Vector<OpcUa_StatusCode> results(request.NoOfMonitoredItemIds);
  for (size_t i = 0; i < static_cast<size_t>(request.NoOfMonitoredItemIds); ++i)
    results[i] = DeleteMonitoredItem(request.MonitoredItemIds[i]).code();
  response.NoOfResults = results.size();
  response.Results = results.release();
  callback(response);
}

MonitoredItemCreateResult Subscription::CreateMonitoredItem(OpcUa_MonitoredItemCreateRequest& request) {
  /*auto client_handle = request.RequestedParameters.ClientHandle;
  auto read_value_id = Convert(request.ItemToMonitor);

  auto item = monitored_item_service_.CreateMonitoredItem(read_value_id.first, read_value_id.second);
  if (!item) {
    MonitoredItemCreateResult result;
    result.StatusCode = OpcUa_Bad;
    return result;
  }

  auto item_id = next_item_id_++;
  auto& data = items_[item_id];
  data.item = std::move(item);
  data.item->set_data_change_handler([this, client_handle](const scada::DataValue& data_value) {
    OnDataChange(client_handle, data_value);
  });
  data.item->Subscribe();*/

  MonitoredItemCreateResult result;
  result.MonitoredItemId = 0; //item_id;
  result.RevisedQueueSize = request.RequestedParameters.QueueSize;
  result.RevisedSamplingInterval = request.RequestedParameters.SamplingInterval;
  result.StatusCode = OpcUa_Good;
  return result;
}

StatusCode Subscription::DeleteMonitoredItem(MonitoredItemId monitored_item_id) {
  /*auto i = items_.find(monitored_item_id);
  if (i == items_.end())
    return OpcUa_BadInvalidArgument;

  items_.erase(i);*/

  return OpcUa_Good;
}

void Subscription::OnDataChange(MonitoredItemId monitored_item_id, const DataValue& data_value) {
  auto i = items_.find(monitored_item_id);
  if (i == items_.end())
    return;

  // TODO: Notification.
}

} // namespace server
} // names