#include "opcua/scada/service_context.h"

#include "opcua/base/struct_writer.h"
#include "opcua/metrics/tracing.h"

#include "opcua/base/debug_util.h"

namespace opcua {
namespace scada {

struct ServiceContext::Rep {
  NodeId user_id;
  std::vector<std::string> locale_ids;
  uint64_t request_id = 0;
  TraceId trace_id;
};

// static
const std::shared_ptr<const ServiceContext::Rep> ServiceContext::kDefaultRep =
    std::make_shared<ServiceContext::Rep>();

ServiceContext::ServiceContext(const std::shared_ptr<const Rep>& rep)
    : rep_{rep} {}

const opcua::scada::NodeId& ServiceContext::user_id() const {
  return rep_->user_id;
}

uint64_t ServiceContext::request_id() const {
  return rep_->request_id;
}

const TraceId& ServiceContext::trace_id() const {
  return rep_->trace_id;
}

ServiceContext ServiceContext::with_user_id(
    const opcua::scada::NodeId& user_id) const {
  Rep rep = *rep_;
  rep.user_id = user_id;
  return ServiceContext{std::make_shared<Rep>(std::move(rep))};
}

ServiceContext ServiceContext::with_request_id(uint64_t request_id) const {
  Rep rep = *rep_;
  rep.request_id = request_id;
  return ServiceContext{std::make_shared<Rep>(std::move(rep))};
}

ServiceContext ServiceContext::with_trace_id(const TraceId& trace_id) const {
  Rep rep = *rep_;
  rep.trace_id = trace_id;
  return ServiceContext{std::make_shared<Rep>(std::move(rep))};
}

std::ostream& operator<<(std::ostream& stream, const ServiceContext& context) {
  StructWriter{stream}
      .AddField("user_id", ToString(context.rep_->user_id))
      .AddField("locale_ids", context.rep_->locale_ids)
      .AddField("trace_id", ToString(context.rep_->trace_id));
  return stream;
}

}  // namespace scada
}  // namespace opcua (vendored)
