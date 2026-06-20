#pragma once

#include "opcua/scada/date_time.h"
#include "opcua/scada/qualifier.h"
#include "opcua/scada/variant.h"

namespace opcua {
namespace scada {

class DataValue {
 public:
  constexpr DataValue() = default;

  constexpr DataValue(StatusCode status_code, DateTime server_timestamp)
      : server_timestamp{server_timestamp}, status_code{status_code} {
    assert(!IsGood(status_code));
  }

  template <class T>
  DataValue(T&& value,
            Qualifier qualifier,
            DateTime source_timestamp,
            DateTime server_timestamp)
      : value(std::forward<T>(value)),
        qualifier(std::move(qualifier)),
        source_timestamp(source_timestamp),
        server_timestamp(server_timestamp) {
    if (qualifier.failed())
      status_code = StatusCode::Bad;
  }

  DataValue(const DataValue&) = default;
  DataValue& operator=(const DataValue&) = default;

  DataValue(DataValue&&) = default;
  DataValue& operator=(DataValue&&) = default;

  constexpr bool is_null() const noexcept {
    return value.is_null() && qualifier.raw() == 0;
  }

  bool operator==(const DataValue& other) const {
    return source_timestamp == other.source_timestamp &&
           server_timestamp == other.server_timestamp && value == other.value &&
           qualifier == other.qualifier && status_code == other.status_code;
  }

  Variant value;
  Qualifier qualifier;
  DateTime source_timestamp;
  DateTime server_timestamp;
  StatusCode status_code = StatusCode::Good;
};

inline bool IsUpdate(const DataValue& current_data, const DataValue& new_data) {
  return current_data.source_timestamp.is_null() ||
         (current_data.source_timestamp < new_data.source_timestamp) ||
         (current_data.source_timestamp == new_data.source_timestamp &&
          current_data.server_timestamp <= new_data.server_timestamp);
}

inline std::ostream& operator<<(std::ostream& stream, const DataValue& v) {
  return stream << "{value: " << v.value << ", qualifier: " << v.qualifier
                << ", source_timestamp: " << v.source_timestamp
                << ", server_timestamp: " << v.server_timestamp
                << ", status_code: " << v.status_code << "}";
}

}  // namespace scada
}  // namespace opcua (vendored)
