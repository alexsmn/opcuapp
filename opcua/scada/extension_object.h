#pragma once

#include "opcua/scada/expanded_node_id.h"

#include <any>
#include <ostream>

namespace opcua {
namespace scada {

class ExtensionObject {
 public:
  ExtensionObject() {}

  ExtensionObject(ExpandedNodeId data_type_id, std::any value)
      : data_type_id_{std::move(data_type_id)}, value_{std::move(value)} {}

  const ExpandedNodeId& data_type_id() const { return data_type_id_; }

  ExtensionObject(const ExtensionObject&) = default;
  ExtensionObject& operator=(const ExtensionObject&) = default;

  ExtensionObject(ExtensionObject&& source) noexcept
      : data_type_id_{std::move(source.data_type_id_)},
        value_{std::move(source.value_)} {}

  ExtensionObject& operator=(ExtensionObject&& source) noexcept {
    if (&value_ != &source.value_) {
      data_type_id_ = std::move(source.data_type_id_);
      value_ = std::move(source.value_);
    }
    return *this;
  }

  bool operator==(const ExtensionObject& other) const { return false; }

  /*template<class T>
  static ExtensionObject Encode(ExpandedNodeId data_type_id, T&& object) {
    return ExtensionObject{std::any{std::move(object)}};
  }*/

  std::any& value() { return value_; }
  const std::any& value() const { return value_; }

 private:
  ExpandedNodeId data_type_id_;
  std::any value_;
};

inline std::ostream& operator<<(std::ostream& stream,
                                const ExtensionObject& extension_object) {
  return stream << "{data_type_id: " << extension_object.data_type_id() << "}";
}

}  // namespace scada
}  // namespace opcua (vendored)
