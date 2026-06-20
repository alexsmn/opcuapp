#pragma once

#include "opcua/base/any_executor.h"
#include "opcua/scada/data_services.h"
#include "opcua/scada/logging.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace transport {
class TransportFactory;
}

class Logger;

struct DataServicesContext {
  const std::shared_ptr<Logger> logger;
  const opcua::AnyExecutor executor;
  transport::TransportFactory& transport_factory;
  opcua::scada::ServiceLogParams service_log_params;
};

using DataServicesFactoryMethod =
    std::function<bool(const DataServicesContext& context,
                       DataServices& services)>;

struct DataServicesInfo {
  std::string name;
  std::u16string display_name;
  DataServicesFactoryMethod factory_method;
  std::string default_host;
};

void RegisterDataServices(DataServicesInfo info);

using DataServicesInfoList = std::vector<DataServicesInfo>;

const DataServicesInfoList& GetDataServicesInfoList();

bool EqualDataServicesName(std::string_view name1, std::string_view name2);

#define REGISTER_DATA_SERVICES(name, display_name, factory_method,            \
                               default_host)                                  \
  static bool factory_method##_registered = [] {                              \
    RegisterDataServices({name, display_name, factory_method, default_host}); \
    return true;                                                              \
  }();

bool CreateDataServices(std::string_view name,
                        const DataServicesContext& context,
                        DataServices& services);
