#pragma once

#include "opcua/base/awaitable.h"
#include "opcua/scada/data_value.h"
#include "opcua/scada/node_class.h"
#include "opcua/scada/read_value_id.h"
#include "opcua/scada/service_context.h"
#include "opcua/scada/status_callback.h"
#include "opcua/scada/status_or.h"
#include "opcua/scada/write_flags.h"

#include <cassert>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace opcua {
namespace scada {

class ServiceContext;

struct WriteValue {
  NodeId node_id;
  AttributeId attribute_id = opcua::scada::AttributeId::Value;
  Variant value;
  WriteFlags flags;

  bool operator==(const WriteValue&) const = default;
};

class AttributeService {
 public:
  virtual ~AttributeService() = default;

  virtual Awaitable<StatusOr<std::vector<DataValue>>> Read(
      ServiceContext context,
      std::shared_ptr<const std::vector<ReadValueId>> inputs) = 0;

  virtual Awaitable<StatusOr<std::vector<StatusCode>>> Write(
      ServiceContext context,
      std::shared_ptr<const std::vector<WriteValue>> inputs) = 0;
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

inline Awaitable<DataValue> Read(AttributeService& attribute_service,
                                 opcua::scada::ServiceContext context,
                                 ReadValueId input) {
  auto inputs = std::make_shared<std::vector<ReadValueId>>(1, std::move(input));
  auto results =
      co_await attribute_service.Read(std::move(context), std::move(inputs));
  if (!results.ok())
    co_return MakeReadError(results.status().code());
  assert(results->size() == 1);
  co_return std::move(results->front());
}

inline Awaitable<Status> Write(AttributeService& attribute_service,
                               opcua::scada::ServiceContext context,
                               WriteValue input) {
  auto inputs = std::make_shared<std::vector<WriteValue>>(1, std::move(input));
  auto results =
      co_await attribute_service.Write(std::move(context), std::move(inputs));
  if (!results.ok())
    co_return results.status();
  assert(results->size() == 1);
  co_return Status{results->front()};
}

inline std::ostream& operator<<(std::ostream& stream,
                                const WriteValue& value_id) {
  return stream << "{"
                << "node_id: " << value_id.node_id
                << ", attribute_id: " << value_id.attribute_id
                << ", value: " << value_id.value << "}";
}

}  // namespace scada
}  // namespace opcua (vendored)
