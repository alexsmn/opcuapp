#include "opcua/base/any_executor.h"
#include "opcua/client_session.h"
#include "opcua/scada/data_services_factory.h"
#include "opcua/scada/history_service.h"

namespace {

class UnsupportedHistoryService : public opcua::scada::HistoryService {
 public:
  opcua::Awaitable<opcua::scada::HistoryReadRawResult> HistoryReadRaw(
      opcua::scada::HistoryReadRawDetails details) override {
    co_return opcua::scada::HistoryReadRawResult{.status = opcua::scada::StatusCode::Bad};
  }

  opcua::Awaitable<opcua::scada::HistoryReadEventsResult> HistoryReadEvents(
      opcua::scada::NodeId node_id,
      opcua::base::Time from,
      opcua::base::Time to,
      opcua::scada::EventFilter filter) override {
    co_return opcua::scada::HistoryReadEventsResult{.status = opcua::scada::StatusCode::Bad};
  }
};

}  // namespace

namespace opcua {

bool CreateServices(const DataServicesContext& context,
                    DataServices& services) {
  auto session = std::make_shared<ClientSession>(context.executor,
                                                 context.transport_factory);
  auto history_service = std::make_shared<UnsupportedHistoryService>();
  services = {.session_service_ = session,
              .view_service_ = session,
              .node_management_service_ = session,
              .history_service_ = history_service,
              .attribute_service_ = session,
              .method_service_ = session,
              .monitored_item_service_ = session};
  return true;
}

}  // namespace opcua
