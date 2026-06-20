#pragma once

#include "opcua/metrics/trace_id.h"

class TraceSink {
 public:
  virtual ~TraceSink() = default;

  virtual void StartSpan(const TraceSpanId& span_id,
                         std::string_view name,
                         const TraceSpanId& parent_span_id) = 0;

  virtual void EndSpan(const TraceSpanId& span_id) = 0;
};

class NoTraceSink final : public TraceSink {
 public:
  void StartSpan(const TraceSpanId& span_id,
                 std::string_view name,
                 const TraceSpanId& parent_span_id) override {}

  void EndSpan(const TraceSpanId& span_id) override {}
};
