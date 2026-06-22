#pragma once

#include "opcua/scada/data_value.h"
#include "opcua/scada/node_class.h"
#include "opcua/scada/read_value_id.h"
#include "opcua/scada/write_flags.h"

#include <cassert>
#include <utility>

namespace opcua {

struct WriteValue {
  NodeId node_id;
  AttributeId attribute_id = opcua::AttributeId::Value;
  Variant value;
  WriteFlags flags;

  bool operator==(const WriteValue&) const = default;
};

template <class T>
inline DataValue MakeReadResult(T&& value) {
  const auto timestamp = opcua::base::Time::Now();
  return DataValue{std::forward<T>(value), {}, timestamp, timestamp};
}

inline DataValue MakeReadResult(NodeClass node_class) {
  return MakeReadResult(static_cast<int>(node_class));
}

inline DataValue MakeReadError(StatusCode status_code) {
  assert(IsBad(status_code));
  const auto timestamp = opcua::base::Time::Now();
  return DataValue{status_code, timestamp};
}

inline std::ostream& operator<<(std::ostream& stream,
                                const WriteValue& value_id) {
  return stream << "{"
                << "node_id: " << value_id.node_id
                << ", attribute_id: " << value_id.attribute_id
                << ", value: " << value_id.value << "}";
}

}  // namespace opcua
