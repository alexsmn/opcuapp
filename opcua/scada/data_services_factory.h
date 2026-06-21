#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/scada/data_services.h"
#include "opcua/scada/logging.h"

#include <memory>

namespace transport {
class TransportFactory;
}

namespace opcua {

class Logger;

// Dependencies handed to a data-services factory: the executor and transport
// used to reach a server, plus logging configuration.
struct DataServicesContext {
  const std::shared_ptr<Logger> logger;
  const AnyExecutor executor;
  transport::TransportFactory& transport_factory;
  opcua::ServiceLogParams service_log_params;
};

}  // namespace opcua (vendored)
