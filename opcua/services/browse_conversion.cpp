#include "opcua/services/browse_conversion.h"

#include <utility>

namespace opcua {

BrowseDescription ToHandWritten(const ua::BrowseDescription& description) {
  return {
      .node_id = description.node_id,
      .direction = static_cast<BrowseDirection>(description.browse_direction),
      .reference_type_id = description.reference_type_id,
      .include_subtypes = description.include_subtypes,
      .node_class_mask = description.node_class_mask,
      .result_mask = description.result_mask,
  };
}

ua::BrowseDescription ToGenerated(const BrowseDescription& description) {
  return {
      .node_id = description.node_id,
      .browse_direction =
          static_cast<ua::BrowseDirection>(description.direction),
      .reference_type_id = description.reference_type_id,
      .include_subtypes = description.include_subtypes,
      .node_class_mask = description.node_class_mask,
      .result_mask = description.result_mask,
  };
}

ReferenceDescription ToHandWritten(const ua::ReferenceDescription& reference) {
  return {
      .reference_type_id = reference.reference_type_id,
      .forward = reference.is_forward,
      .node_id = reference.node_id.node_id(),
      .node_class = static_cast<NodeClass>(reference.node_class),
      .browse_name = reference.browse_name,
      .display_name = reference.display_name,
      .type_definition = reference.type_definition.node_id(),
  };
}

ua::ReferenceDescription ToGenerated(const ReferenceDescription& reference) {
  return {
      .reference_type_id = reference.reference_type_id,
      .is_forward = reference.forward,
      .node_id = ExpandedNodeId{reference.node_id},
      .browse_name = reference.browse_name,
      .display_name = reference.display_name,
      .node_class = static_cast<ua::NodeClass>(reference.node_class),
      .type_definition = ExpandedNodeId{reference.type_definition},
  };
}

BrowseResult ToHandWritten(const ua::BrowseResult& result) {
  BrowseResult out;
  out.status_code = result.status_code.code();
  out.continuation_point = result.continuation_point;
  out.references.reserve(result.references.size());
  for (const auto& reference : result.references)
    out.references.push_back(ToHandWritten(reference));
  return out;
}

ua::BrowseResult ToGenerated(const BrowseResult& result) {
  ua::BrowseResult out;
  out.status_code = Status{result.status_code};
  out.continuation_point = result.continuation_point;
  out.references.reserve(result.references.size());
  for (const auto& reference : result.references)
    out.references.push_back(ToGenerated(reference));
  return out;
}

}  // namespace opcua
