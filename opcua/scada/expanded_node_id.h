#pragma once

#include "opcua/scada/node_id.h"
#include "opcua/scada/string.h"

#include <ostream>

namespace opcua {

class ExpandedNodeId {
 public:
  ExpandedNodeId() {}
  ExpandedNodeId(NodeId node_id) : node_id_{std::move(node_id)} {}
  ExpandedNodeId(NodeId node_id, String namespace_uri, unsigned server_index);

  constexpr bool is_null() const noexcept { return node_id_.is_null(); }

  bool operator==(const ExpandedNodeId& other) const;
  bool operator!=(const ExpandedNodeId& other) const {
    return !operator==(other);
  }

  const NodeId& node_id() const { return node_id_; }
  const String& namespace_uri() const { return namespace_uri_; }
  unsigned server_index() const { return server_index_; }

 private:
  NodeId node_id_;
  String namespace_uri_;
  unsigned server_index_ = 0;
};

inline ExpandedNodeId::ExpandedNodeId(NodeId node_id,
                                      String namespace_uri,
                                      unsigned server_index)
    : node_id_{std::move(node_id)},
      namespace_uri_{std::move(namespace_uri)},
      server_index_{server_index} {}

inline bool ExpandedNodeId::operator==(const ExpandedNodeId& other) const {
  return node_id_ == other.node_id_ && namespace_uri_ == other.namespace_uri_ &&
         server_index_ == other.server_index_;
}

inline std::ostream& operator<<(std::ostream& stream,
                                const ExpandedNodeId& expanded_node_id) {
  return stream << "{node_id: " << expanded_node_id.node_id() << ", "
                << "namespace_uri: \"" << expanded_node_id.namespace_uri()
                << "\", "
                << "server_index: " << expanded_node_id.server_index() << "}";
}

}  // namespace opcua (vendored)
