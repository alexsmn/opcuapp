#pragma once

#include "opcua/metrics/trace_id.h"

namespace opcua {
class TraceSink;

class [[nodiscard]] TraceSpan {
 public:
  TraceSpan(TraceSink& sink, std::string_view name);
  ~TraceSpan();

  TraceSpan(TraceSpan&& other) noexcept
      : sink_{other.sink_}, span_id_{other.span_id_} {
    other.sink_ = nullptr;
  }

  TraceSpan& operator=(TraceSpan&& other) noexcept {
    if (this != &other) {
      sink_ = other.sink_;
      span_id_ = other.span_id_;
      other.sink_ = nullptr;
    }
    return *this;
  }

  const TraceSpanId& span_id() const { return span_id_; }

  TraceSpan StartSpan(std::string_view name) const;

 private:
  TraceSpan(TraceSink& sink,
            const TraceSpanId& span_id,
            std::string_view name,
            const TraceSpanId& parent_span_id);

  // Null when span is moved out.
  TraceSink* sink_ = nullptr;

  TraceSpanId span_id_;
};
}  // namespace opcua (vendored)
