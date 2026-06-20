#include "opcua/metrics/tracing.h"

#include "opcua/metrics/trace_sink.h"

#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <cassert>

namespace opcua {
namespace {

TraceSpanId GenerateTraceSpanId() {
  boost::uuids::random_generator gen;
  auto uuid = gen();
  return boost::uuids::to_string(uuid);
}

}  // namespace

// TraceSpan

TraceSpan::TraceSpan(TraceSink& sink, std::string_view name)
    : TraceSpan{sink, /*span_id=*/GenerateTraceSpanId(), name,
                /*parent_span_id=*/{}} {}

TraceSpan::TraceSpan(TraceSink& sink,
                     const TraceSpanId& span_id,
                     std::string_view name,
                     const TraceSpanId& parent_span_id)
    : sink_{&sink}, span_id_{span_id} {
  sink.StartSpan(span_id, name, parent_span_id);
}

TraceSpan::~TraceSpan() {
  if (sink_) {
    sink_->EndSpan(span_id_);
  }
}

TraceSpan TraceSpan::StartSpan(std::string_view name) const {
  assert(sink_);
  return TraceSpan{*sink_, /*span_id=*/GenerateTraceSpanId(), name,
                   /*parent_span_id*/ span_id_};
}
}  // namespace opcua (vendored)
