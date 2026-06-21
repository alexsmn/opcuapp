#pragma once

#include "opcua/scada/data_services.h"

namespace opcua {
namespace data_services {

template <typename Service>
std::shared_ptr<Service> Unowned(Service& service) {
  return std::shared_ptr<Service>{std::shared_ptr<void>{}, &service};
}

inline DataServices FromUnownedServices(const opcua::services& services) {
  return DataServices::FromUnownedServices(services);
}

}  // namespace data_services
}  // namespace opcua (vendored)
