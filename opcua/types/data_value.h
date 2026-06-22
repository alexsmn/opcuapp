#pragma once

#include "opcua/types/date_time.h"
#include "opcua/types/qualifier.h"
#include "opcua/types/variant.h"

namespace opcua {

// Built-in OPC UA DataValue: a Value together with its StatusCode and source/
// server timestamps; the quality bits are held in a Qualifier. OPC UA Part 4
// §7.11 DataValue,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.11
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

inline std::ostream& operator<<(std::ostream& stream, const DataValue& v) {
  return stream << "{value: " << v.value << ", qualifier: " << v.qualifier
                << ", source_timestamp: " << v.source_timestamp
                << ", server_timestamp: " << v.server_timestamp
                << ", status_code: " << v.status_code << "}";
}

}  // namespace opcua
