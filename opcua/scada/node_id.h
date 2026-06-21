#pragma once

#include "opcua/scada/basic_types.h"
#include "opcua/scada/shared_value.h"
#include "opcua/scada/string.h"

#include <cassert>
#include <memory>
#include <ostream>
#include <string_view>
#include <variant>

namespace opcua {

enum class NodeIdType { Numeric, String, Opaque };

using NumericId = uint32_t;

class NodeId {
 public:
  constexpr NodeId() noexcept = default;
  constexpr NodeId(NumericId numeric_id,
                   NamespaceIndex namespace_index = 0) noexcept;
  NodeId(String string_id, NamespaceIndex namespace_index);
  NodeId(ByteString opaque_id, NamespaceIndex namespace_index);

  NodeId(const NodeId& source) = default;
  NodeId& operator=(const NodeId& source) = default;

  // The move source becomes null.
  NodeId(NodeId&& source) noexcept;
  NodeId& operator=(NodeId&& source) noexcept;

  constexpr NodeIdType type() const noexcept {
    return static_cast<NodeIdType>(identifier_.index());
  }

  constexpr bool is_null() const noexcept;
  constexpr bool is_namespace_only() const noexcept;
  constexpr bool is_numeric() const noexcept {
    return type() == NodeIdType::Numeric;
  }
  constexpr bool is_string() const noexcept {
    return type() == NodeIdType::String;
  }

  constexpr NamespaceIndex namespace_index() const noexcept {
    return namespace_index_;
  }
  void set_namespace_index(NamespaceIndex index) noexcept {
    namespace_index_ = index;
  }

  constexpr NumericId numeric_id() const;
  const String& string_id() const;
  const ByteString& opaque_id() const;

  auto operator<=>(const NodeId&) const = default;

  String ToString() const;
  static NodeId FromString(std::string_view string);

 private:
  std::variant<NumericId, SharedValue<String>, SharedValue<ByteString>>
      identifier_;

  NamespaceIndex namespace_index_ = 0;
};

inline constexpr NodeId::NodeId(NumericId numeric_id,
                                NamespaceIndex namespace_index) noexcept
    : identifier_{numeric_id}, namespace_index_{namespace_index} {}

inline constexpr bool NodeId::is_null() const noexcept {
  if (namespace_index_ != 0)
    return false;
  if (const auto* numeric_id = std::get_if<NumericId>(&identifier_))
    return *numeric_id == 0;
  return false;
}

inline constexpr bool NodeId::is_namespace_only() const noexcept {
  if (const auto* numeric_id = std::get_if<NumericId>(&identifier_))
    return *numeric_id == 0;
  return false;
}

inline constexpr NumericId NodeId::numeric_id() const {
  assert(type() == NodeIdType::Numeric);
  return std::get<NumericId>(identifier_);
}

inline constexpr bool operator==(const NodeId& a, NumericId b) noexcept {
  return a.namespace_index() == 0 && a.type() == NodeIdType::Numeric &&
         a.numeric_id() == b;
}

inline constexpr bool operator!=(const NodeId& a, NumericId b) noexcept {
  return !(a == b);
}

inline constexpr bool operator==(NumericId a, const NodeId& b) noexcept {
  return b == a;
}

inline constexpr bool operator!=(NumericId a, const NodeId& b) noexcept {
  return !operator==(a, b);
}

inline std::ostream& operator<<(std::ostream& stream, const NodeId& node_id) {
  return stream << "\"" << node_id.ToString() << "\"";
}


}  // (re-open opcua after external block)
namespace std {

template <>
struct hash<opcua::NodeId> {
  std::size_t operator()(const opcua::NodeId& node_id) const noexcept;
};

}  // namespace std
namespace opcua {
}  // namespace opcua (vendored)
