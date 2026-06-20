#pragma once

#include "opcua/scada/aggregate_filter.h"
#include "opcua/scada/data_change_filter.h"
#include "opcua/scada/event_filter.h"

#include <memory>
#include <optional>

namespace opcua::scada {

using MonitoringFilter = std::
    variant<std::monostate, DataChangeFilter, EventFilter, AggregateFilter>;

struct MonitoringParameters {
  bool is_null() const {
    return !sampling_interval.has_value() && filter.index() == 0 &&
           !queue_size.has_value();
  }

  MonitoringParameters& set_filter(MonitoringFilter filter) {
    this->filter = std::move(filter);
    return *this;
  }

  std::optional<Duration> sampling_interval;

  MonitoringFilter filter;

  std::optional<size_t> queue_size;
};
}  // namespace opcua::scada