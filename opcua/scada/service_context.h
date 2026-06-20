#pragma once

#include "opcua/metrics/trace_id.h"
#include "opcua/scada/node_id.h"

#include <memory>
#include <ostream>

namespace opcua::scada {

class [[nodiscard]] ServiceContext {
 public:
  ServiceContext() = default;

  ServiceContext(const ServiceContext&) = default;
  ServiceContext& operator=(const ServiceContext&) = default;

  const opcua::scada::NodeId& user_id() const;
  uint64_t request_id() const;
  const TraceId& trace_id() const;

  ServiceContext with_user_id(const opcua::scada::NodeId& user_id) const;
  ServiceContext with_request_id(uint64_t request_id) const;
  ServiceContext with_trace_id(const TraceId& trace_id) const;

  friend std::ostream& operator<<(std::ostream& stream,
                                  const ServiceContext& context);

 private:
  struct Rep;

  explicit ServiceContext(const std::shared_ptr<const Rep>& rep);

  std::shared_ptr<const Rep> rep_ = kDefaultRep;

  static const std::shared_ptr<const Rep> kDefaultRep;
};

}  // namespace opcua::scada
