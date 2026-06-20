#include "opcua/scada/aggregate_filter.h"

#include "opcua/base/struct_writer.h"

#include "opcua/base/debug_util.h"

namespace opcua::scada {

std::ostream& operator<<(std::ostream& stream, const AggregateFilter& filter) {
  StructWriter{stream}
      .AddField("start_time", filter.start_time)
      .AddField("interval", filter.interval)
      .AddField("aggregate_type", ToString(filter.aggregate_type));
  return stream;
}

}  // namespace opcua::scada
